#include "chat.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ai_frames.h"
#include "transport.h"
#include "my_client.h"
#include "i2s.h"
#include "afe_sr.h"
#include "key.h"
#include "lcd_ui.h"
#include "wifi.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"         
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "AI_CHAT";

/* ===================== 文件级 static =====================
 * 一次只有一个会话在跑，全部单实例。12KB 的解析器别放任务栈（栈 8192，坑 9）。 */
typedef enum {
    CHAT_NONE = 0,
    CHAT_DONE,                 /* 收到服务器 done 事件 */
    CHAT_ERR,                  /* 收到服务器 err 事件 */
    CHAT_ABORT,                /* 用户按键/外部抢占中断 */
} chat_result_t;

typedef struct {
    volatile chat_result_t result;
    uint16_t resid;                                  /* 不满 128 样本的尾字节 */
    uint8_t  resid_buf[AUDIO_FRAME_BYTES];
} chat_ctx_t;

static ai_frames_t       s_frames;              /* 下行帧解析器（约 12 KB BSS） */
static chat_ctx_t        s_ctx;
static transport_state_t s_tstate;              /* 每轮 memset 后复用 */
static uint8_t          *s_ring;                /* 480KB PSRAM 上行缓冲；NULL = 直传模式 */
static char              s_session_id[37];
static int               s_turn;                /* 会话内轮次，从 1 起（A.3.1） */
static bool              s_session_live;        /* 本 SR_LISTENING 会话建过连接 */
static TaskHandle_t      s_chat_handle;
static char              s_prompt_req[32];      /* 待播提示音文件名，"" = 无请求 */
static ai_chat_event_cb_t s_event_cb;
static void             *s_event_cb_ctx;
static bool              s_spiffs_ready;
static volatile bool     s_abort_external;      /* ai_chat_abort() 置位：收尾不播兜底 */
static uint32_t          s_dropped_bytes;       /* 缓冲不够被丢的上行字节（观测用） */

/* ===================== 本地兜底播报（A.8 预录 PCM） ===================== */

static void spiffs_ensure(void)
{
    if (s_spiffs_ready) return;
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/storage",
        .partition_label        = "storage",       /* partitions-16MiB.csv 的 storage 区 */
        .max_files              = 2,
        .format_if_mount_failed = false,           /* 绝不格式化用户分区 */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    s_spiffs_ready = (err == ESP_OK);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "spiffs mount failed (%s), prompts fall back to beep",
                 esp_err_to_name(err));
}

/* 播放本地 PCM（16k/16bit/mono 裸流）。文件缺失退化为 200ms 提示音：
 * "失败必须让用户听见"（FR-6 原则）不能依赖录音文件是否就位。 */
static void play_prompt_file(const char *name)
{
    static uint8_t buf[2048];                   /* static：不吃任务栈 */
    spiffs_ensure();

    FILE *f = NULL;
    if (s_spiffs_ready) {
        char path[64];
        snprintf(path, sizeof(path), "/storage/%s", name);
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        ESP_LOGW(TAG, "prompt %s missing, play beep", name);
        static int16_t beep[1024];
        const int total = I2S_SAMPLE_RATE / 5;  /* 200 ms = 3200 采样 */
        for (int off = 0; off < total; off += 1024) {
            int n = (total - off < 1024) ? (total - off) : 1024;
            for (int i = 0; i < n; ++i) {
                int t = off + i;
                /* 首尾 10ms 线性淡入淡出（与 replay 同思路），防爆音 */
                int env = t < 160 ? t : (total - 1 - t < 160 ? total - 1 - t : 160);
                beep[i] = (int16_t)(6000.0f * env / 160 *
                                    sinf(2.0f * (float)M_PI * 440.0f * t / I2S_SAMPLE_RATE));
            }
            i2s_spk_write(beep, (size_t)n);
        }
        i2s_spk_write_silence(6);
        return;
    }

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (i2s_spk_write((const int16_t *)buf, n / 2) != ESP_OK) {
            ESP_LOGW(TAG, "spk write failed during prompt");
            break;
        }
    }
    fclose(f);
    i2s_spk_write_silence(6);
}

void ai_chat_request_prompt(const char *name)
{
    if (name == NULL || s_chat_handle == NULL) return;
    strlcpy(s_prompt_req, name, sizeof(s_prompt_req));   /* 并发请求取最后者，可接受 */
    xTaskNotifyGive(s_chat_handle);
}

/* IDLE 轮询里调：MultiNet 本地回复 / 其它模块的兜底请求 */
static void prompt_request_check(void)
{
    if (ulTaskNotifyTake(pdTRUE, 0) == 0 || s_prompt_req[0] == '\0') return;

    /* MultiNet 命中后 capture 由 fetch 任务关闭；这里再关一次是幂等的，
     * 兜住"通知先于 capture_enabled=false"的竞态窗口，自播不回灌 */
    afe_sr_set_capture_enabled(false);
    play_prompt_file(s_prompt_req);
    s_prompt_req[0] = '\0';
    afe_sr_set_capture_enabled(true);
    lcd_ui_set_status(LCD_STATUS_LISTENING);
}

/* ===================== 下行：JSON 事件与音频播放 ===================== */

static void handle_json(const char *str, uint16_t len, chat_ctx_t *c)
{
    cJSON *root = cJSON_ParseWithLength(str, len);   /* payload 不保证 0 结尾，坑 7 */
    if (root == NULL) {
        ESP_LOGW(TAG, "bad json event (%u bytes)", (unsigned)len);
        return;
    }
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "t"));
    if (t == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(t, "asr") == 0 || strcmp(t, "reply") == 0) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        ESP_LOGI(TAG, "%s: %s", t, text ? text : "");
        if (s_event_cb) {
            s_event_cb(t[0] == 'a' ? AI_CHAT_EVT_ASR : AI_CHAT_EVT_REPLY,
                       text ? text : "", s_event_cb_ctx);
        }
    } else if (strcmp(t, "action") == 0) {
        ESP_LOGI(TAG, "action event (FR-8 二期，仅透传)");
        if (s_event_cb) s_event_cb(AI_CHAT_EVT_ACTION, str, s_event_cb_ctx);
    } else if (strcmp(t, "done") == 0) {
        c->result = CHAT_DONE;      /* 之后继续读完尾巴音频，不提前断流（坑 6） */
    } else if (strcmp(t, "err") == 0) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *msg  = cJSON_GetObjectItem(root, "msg");
        const char *msg_str = cJSON_IsString(msg) ? msg->valuestring : "?";
        ESP_LOGE(TAG, "server err code=%d msg=%s",
                 cJSON_IsNumber(code) ? (int)code->valuedouble : -1,
                 msg_str ? msg_str : "?");
        c->result = CHAT_ERR;
        if (s_event_cb) s_event_cb(AI_CHAT_EVT_ERR, msg_str, s_event_cb_ctx);
    }
    cJSON_Delete(root);                              /* 所有路径都销毁，防泄漏 */
}

static void on_frame(ai_payload_type_t type, const uint8_t *payload, uint16_t len, void *ctx)
{
    chat_ctx_t *c = ctx;

    if (type == AI_PT_JSON) {
        if (c->result != CHAT_ABORT) handle_json((const char *)payload, len, c);
        return;
    }
    if (type != AI_PT_AUDIO || c->result == CHAT_ABORT) return;   /* 打断后不再播 */

    /* 音频统一 memcpy 进拼装缓冲，再按 128 样本整块播：
     * ① 不赌 payload 对齐（帧头 3 字节是奇数，坑 8）；
     * ② 不赌服务器块恰好 128 样本整数倍；③ 残帧统一在字节层拼接（A.9 #6） */
    const uint8_t *p = payload;
    uint16_t n = len;
    if (c->resid > 0) {
        uint16_t take = (uint16_t)(AUDIO_FRAME_BYTES - c->resid);
        if (take > n) take = n;
        memcpy(c->resid_buf + c->resid, p, take);
        c->resid += take;
        p += take;
        n -= take;
        if (c->resid == AUDIO_FRAME_BYTES) {
            if (key_consume_wake_request()) {        /* 半双工：打断只能靠按键（A.9 #2） */
                tx_abort();
                c->result = CHAT_ABORT;
                return;
            }
            i2s_spk_write((const int16_t *)c->resid_buf, AUDIO_FRAME_SAMPLES);
            c->resid = 0;
        }
    }
    while (n >= AUDIO_FRAME_BYTES) {
        if (key_consume_wake_request()) {
            tx_abort();
            c->result = CHAT_ABORT;
            return;
        }
        i2s_spk_write((const int16_t *)p, AUDIO_FRAME_SAMPLES);
        p += AUDIO_FRAME_BYTES;
        n -= AUDIO_FRAME_BYTES;
    }
    if (n > 0) {                                     /* 不足 256B 存残帧 */
        memcpy(c->resid_buf, p, n);
        c->resid = n;
    }
}

/* ===================== 上行 ===================== */

static void make_session_id(char out[37])            /* A.3.1：每次唤醒生成新 UUID */
{
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    snprintf(out, 37, "%08lx-%04x-%04x-%04x-%04x%08lx",
             (unsigned long)r[0],
             (unsigned)(r[1] >> 16), (unsigned)(r[1] & 0xffff),
             (unsigned)(r[2] >> 16), (unsigned)(r[2] & 0xffff),
             (unsigned long)r[3]);
}

/**
 * @brief  上行循环：掏空 pcm_queue → 环形缓冲 → 凑 512B 发一包，遇 END 结束
 * @return true 上行完整（含尾包）；false 网络写失败
 *
 * "先掏空再写"是防丢音的关键：tx_write 阻塞在 TCP 背压上时，pcm_queue
 * （约 1s 容量）是唯一还在进数据的缓冲，写之前把能拿的都拿进 480KB ring，
 * 抗抖动窗口 = ring 剩余 + 1s 队列，≈15s（A.3.1 单次上限），再往后丢弃并计数。
 * s_ring 为 NULL（PSRAM 分配失败）时降级为逐帧 256B 直传，只损失凑包效率。
 */
static bool upload_loop(QueueHandle_t q, audio_frame_t *pending)
{
    audio_frame_t fr;
    bool have = (pending != NULL);                   /* 建会话前取出的首帧 */
    if (have) fr = *pending;
    size_t fill = 0;

    for (;;) {
        while (have || xQueueReceive(q, &fr, 0) == pdTRUE) {
            have = false;
            if (fr.type == AUDIO_FRAME_END) {
                if (s_ring && fill > 0 && tx_write(s_ring, fill) != ESP_OK) return false;
                if (s_dropped_bytes > 0)
                    ESP_LOGW(TAG, "upload dropped %u bytes (ring full, 15s cap)",
                             (unsigned)s_dropped_bytes);
                return true;
            }
            if (s_ring) {
                if (fill + fr.bytes <= AI_RING_BYTES) {
                    memcpy(s_ring + fill, fr.data, fr.bytes);
                    fill += fr.bytes;
                } else {
                    s_dropped_bytes += fr.bytes;     /* 15s 上限到顶（A.3.1） */
                }
            } else if (tx_write(fr.data, fr.bytes) != ESP_OK) {
                return false;
            }
        }
        if (s_ring) {
            size_t pkt = fill & ~(size_t)(AI_UPLOAD_PKT_BYTES - 1);   /* 512 向下取整 */
            if (pkt > 0) {
                if (tx_write(s_ring, pkt) != ESP_OK) return false;
                memmove(s_ring, s_ring + pkt, fill - pkt);
                fill -= pkt;
            }
        }
        vTaskDelay(1);                               /* 让出 CPU，防饿死低优先级任务 */
    }
}

/* ===================== 会话与主循环 ===================== */

/* AFE 侧 5 分钟空闲回 SR_IDLE 的下降沿：同步 DELETE 服务器会话（A.3.3）。
 * 失败不重试——顶多服务器多留一个空会话。 */
static void session_idle_check(void)
{
    static bool prev_in_session;
    bool now = afe_sr_in_session();
    if (prev_in_session && !now && s_session_live) {
        session_delete(s_session_id);
        s_session_live = false;
        s_turn = 0;
        s_session_id[0] = '\0';
    }
    prev_in_session = now;
}

/* 一轮完整对话：建会话 → 上行 → 下行 → 收尾。问候轮（greeting）没有上行。 */
static void do_turn(QueueHandle_t q, audio_frame_t *first, bool greeting)
{
    esp_err_t err = ESP_FAIL;

    memset(&s_tstate, 0, sizeof(s_tstate));
    s_ctx.result = CHAT_NONE;

    make_session_id(s_session_id);
    s_turn++;
    /* 问候轮在此关采集防自播回灌；正常轮【绝不能】在这里关——
     * 正常轮 do_turn 从首帧就开始（流式上传），说话还在继续，
     * 此时关采集会让 fetch 丢弃后续语音、永远等不到 END 帧，
     * upload_loop 死等 → 无回复且设备失聪（2026-11-08 真机踩坑）。
     * 正常轮的采集关闭由 fetch 任务在 utterance end 时完成，
     * 播放期间的防回灌由 prompt/TTS 播放路径各自幂等关闭。 */
    if (greeting) {
        afe_sr_set_capture_enabled(false);
    }

    if (session_start(s_session_id, s_turn, greeting) == ESP_OK) {
        s_session_live = true;
        s_tstate.status = 1;
        if (greeting) {
            err = tx_read_loop(&s_frames, &s_ctx, &s_tstate);
        } else if (upload_loop(q, first)) {
            err = tx_read_loop(&s_frames, &s_ctx, &s_tstate);
        }
    } else {
        ESP_LOGW(TAG, "session_start failed (no network / server down)");
    }

    /* ===== 收尾：唯一一份，所有路径必经（漏了 capture 开不回，设备永久失聪）===== */
    tx_end();
    s_tstate.status = 2;
    bool aborted = (s_ctx.result == CHAT_ABORT) || s_abort_external;
    if (!aborted && (err != ESP_OK || s_ctx.result == CHAT_ERR)) {
        /* A.3.4：失败播兜底、不自动重试。问候轮失败降级本地问候，不喊"网络不好" */
        play_prompt_file(greeting ? PROMPT_GREETING : PROMPT_NET_ERR);
    }
    i2s_spk_write_silence(6);              /* 把尾巴推出 TX DMA，成功路径也无害 */
    afe_sr_set_capture_enabled(true);      /* 半双工解除 */
    lcd_ui_set_status(LCD_STATUS_LISTENING);

    ESP_LOGI(TAG, "turn %d %s: first_frame=%ums rx=%u tx=%u",
             s_turn, err == ESP_OK ? "ok" : esp_err_to_name(err),
             (unsigned)s_tstate.first_frame_ms,
             (unsigned)s_tstate.bytes_rx, (unsigned)s_tstate.bytes_tx);
    s_dropped_bytes = 0;
    s_ctx.result = CHAT_NONE;
    s_abort_external = false;
    if (s_event_cb) s_event_cb(AI_CHAT_EVT_DONE, NULL, s_event_cb_ctx);
}

void ai_chat_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    s_chat_handle = xTaskGetCurrentTaskHandle();
    ai_frames_init(&s_frames, on_frame, &s_ctx);

    s_ring = heap_caps_malloc(AI_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        ESP_LOGW(TAG, "PSRAM ring alloc failed, degrade to direct 256B upload");
    } else {
        ESP_LOGI(TAG, "upload ring %u KB (PSRAM)", (unsigned)(AI_RING_BYTES / 1024));
    }

    /* 开机问候（FR-9）：等网 10s，成了走服务器 TTS，不成走本地占位音。
     * 本期"每次开机问候"；"每天首次"需 SNTP+NVS，二期（坑 16）。 */
    EventGroupHandle_t wifi_ev = wifi_get_event_group();
    EventBits_t bits = 0;
    if (wifi_ev != NULL)
        bits = xEventGroupWaitBits(wifi_ev, WIFI_CONNECTED_BIT,
                                   pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        if (afe_sr_in_session()) {
            /* 用户在问候前就已唤醒开口（联网慢/手快时必现）：跳过问候，
             * 否则问候轮关采集会把用户第一句话吞掉 */
            ESP_LOGI(TAG, "user already in session at boot, skip greeting");
        } else {
            do_turn(q, NULL, true);
        }
    } else {
        ESP_LOGW(TAG, "no wifi at boot, local greeting");
        afe_sr_set_capture_enabled(false);
        play_prompt_file(PROMPT_GREETING);
        i2s_spk_write_silence(6);
        afe_sr_set_capture_enabled(true);
    }

    for (;;) {                                             /* ===== IDLE ===== */
        audio_frame_t fr;
        if (xQueueReceive(q, &fr, pdMS_TO_TICKS(200)) != pdTRUE) {
            session_idle_check();
            prompt_request_check();
            continue;
        }
        if (fr.type == AUDIO_FRAME_DATA)
            do_turn(q, &fr, false);                        /* 首帧已取出，交给 do_turn */
    }
}

void ai_chat_set_event_cb(ai_chat_event_cb_t cb, void *ctx)
{
    s_event_cb = cb;
    s_event_cb_ctx = ctx;
}

void ai_chat_abort(void)
{
    s_abort_external = true;               /* 抢占不是故障：收尾不播兜底话术 */
    s_ctx.result = CHAT_ABORT;
    tx_abort();
}
