#include "transport.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "my_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "TRANSPORT";

/* ===================== 模式分派（v2，附录 B）=====================
 *
 * 对 CHAT 层透明：session_start 优先 WS，失败降级 v1 HTTP；本轮内
 * tx_write / tx_read_loop / tx_end / tx_abort 按 s_mode 各走各的实现。
 * 连续 3 轮 WS 失败 → 暂停 WS 10 分钟（防止服务器没部署 /ws/v1 时
 * 每轮都白等握手超时）；期间纯 HTTP，到期自动恢复尝试。
 */
typedef enum { TP_HTTP = 0, TP_WS } tp_mode_t;
static tp_mode_t s_mode = TP_HTTP;

/* ---- HTTP（v1）状态 ---- */
static esp_http_client_handle_t tx_c = NULL;
static volatile bool s_abort;         /* tx_abort() 置位；session_start() 清零 */
static bool s_in_flight;              /* 连接已 open：配对 close，防重复派发断连事件 */
static size_t s_bytes_tx;             /* 本轮已上行字节数（session_start 清零） */
static TickType_t s_open_tick;        /* open 时刻：首帧超时的计时起点 */

/* ---- WS（v2）状态 ---- */
#define WS_BIT_CONNECTED (1 << 0)
#define WS_BIT_FAILED    (1 << 1)     /* 建连期间出错/被拒：让 ws_ensure 早退，别干等满超时 */

static esp_websocket_client_handle_t s_ws;
static StreamBufferHandle_t s_ws_sb;  /* 下行字节管道：WS 事件任务 → 本任务（chat） */
static EventGroupHandle_t   s_ws_ev;
static volatile bool s_ws_helloed;    /* 本条连接已发过 hello（断线重连后须重发） */
static volatile bool s_ws_poison;     /* 下行管道写不进（播放端卡死）→ 连接作废重建 */
static volatile bool s_ws_turn_done;  /* tx_turn_finished() 置位：读到 done/err，本轮终结 */
static int      s_ws_fail_streak;     /* 连续失败计数（成功一轮清零） */
static TickType_t s_ws_disabled_until;/* 非零 = 暂停 WS 直至此刻 */

static const esp_http_client_config_t tx_config = {
    .url               = SERVER_BASE SERVER_PATH_CHAT,
    .method            = HTTP_METHOD_POST,
    .timeout_ms        = AI_HTTP_TIMEOUT_MS,
    .keep_alive_enable = true,
};

/* ===================================================== WS 事件回调 ===
 * 只做一件事：把下行字节搬进管道。播放/JSON 解析绝不在回调里做
 * （A.9 硬约束 1：i2s_spk_write 只属于 chat 任务）。
 * 管道满 2s 还没人取 → 毒化连接（丢字节会错位帧流，宁可整轮作废）。
 * 注意 esp_websocket_client 大消息会分多次回调（payload_offset 递增），
 * 这里按字节流喂——ai_frames 解析器不关心消息边界，天然免疫分片。
 */
static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_helloed = false;                   /* 每条新连接都要重新 hello */
        xEventGroupSetBits(s_ws_ev, WS_BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        xEventGroupClearBits(s_ws_ev, WS_BIT_CONNECTED);
        if (s_ws != NULL)                       /* 建连尝试失败：通知等待者别干等 */
            xEventGroupSetBits(s_ws_ev, WS_BIT_FAILED);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (s_ws_poison || e->data_len <= 0) break;
        /* 只收 binary(0x2)/continuation(0x0)；text 服务器不发，ping/pong/close 不入管道 */
        if (e->op_code == 0x01 || e->op_code == 0x08 ||
            e->op_code == 0x09 || e->op_code == 0x0A) break;
        if (xStreamBufferSend(s_ws_sb, e->data_ptr, (size_t)e->data_len,
                              pdMS_TO_TICKS(2000)) != (size_t)e->data_len) {
            s_ws_poison = true;
            ESP_LOGW(TAG, "ws downstream stalled 2s, connection poisoned");
        }
        break;
    default:
        break;
    }
}

/** WS 是否允许尝试（开关 + 失败暂停窗口） */
static bool ws_usable(void)
{
    if (!AI_TRANSPORT_WS) return false;
    if (s_ws_disabled_until != 0) {
        if (xTaskGetTickCount() < s_ws_disabled_until) return false;
        s_ws_disabled_until = 0;                /* 暂停期满，恢复尝试 */
        s_ws_fail_streak = 0;
        ESP_LOGI(TAG, "ws retry window re-opened");
    }
    return true;
}

/** 确保有可用连接：无则（重）建。断线/毒化的旧连接先销毁——
 *  毒化连接里的在途字节属于上一轮，漏进下一轮会播出陈旧音频。 */
static esp_err_t ws_ensure(void)
{
    if (s_ws != NULL && !s_ws_poison &&
        (xEventGroupGetBits(s_ws_ev) & WS_BIT_CONNECTED)) {
        return ESP_OK;                          /* 热连接直接复用（v2 的核心收益） */
    }
    if (s_ws != NULL) {                         /* 半死连接收尸 */
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_ws_helloed = false;
    }
    if (s_ws_sb == NULL) s_ws_sb = xStreamBufferCreate(AI_WS_DOWN_BUF_BYTES, 1);
    if (s_ws_ev == NULL) s_ws_ev = xEventGroupCreate();
    if (s_ws_sb == NULL || s_ws_ev == NULL) {
        ESP_LOGE(TAG, "ws os resource alloc failed");
        return ESP_FAIL;
    }
    xStreamBufferReset(s_ws_sb);
    s_ws_poison = false;
    xEventGroupClearBits(s_ws_ev, WS_BIT_CONNECTED | WS_BIT_FAILED);

    /* 自动重连关闭：连接死了就等下一轮 ws_ensure 显式重建，
     * 状态机比"后台悄悄重连"好推理（重连后必须重发 hello）。 */
    esp_websocket_client_config_t cfg = {
        .uri                    = SERVER_WS_URL,
        .buffer_size            = 4096,
        .network_timeout_ms     = AI_WS_CONNECT_WAIT_MS,
        .disable_auto_reconnect = true,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) return ESP_FAIL;
    /* 1.8.0 起注册函数叫 esp_websocket_register_events（无 _client） */
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));

    EventBits_t bits = xEventGroupWaitBits(
        s_ws_ev, WS_BIT_CONNECTED | WS_BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(AI_WS_CONNECT_WAIT_MS));
    if (!(bits & WS_BIT_CONNECTED)) {
        ESP_LOGW(TAG, "ws connect/handshake failed (server has /ws/v1?)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void ws_note_fail(void)
{
    if (++s_ws_fail_streak >= 3) {
        s_ws_disabled_until = xTaskGetTickCount() + pdMS_TO_TICKS(10 * 60 * 1000);
        ESP_LOGW(TAG, "ws failed %d times, paused 10 min (v1 HTTP only)", s_ws_fail_streak);
    }
}

/** 发一条 text 控制消息（hello/turn_start/greeting/utterance_end） */
static esp_err_t ws_send_text_(const char *text)
{
    int n = strlen(text);
    if (esp_websocket_client_send_text(s_ws, text, n,
                                       pdMS_TO_TICKS(AI_WS_SEND_TIMEOUT_MS)) != n) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** WS 轮次开启：清管道 → hello（新连接才需要）→ turn_start/greeting */
static esp_err_t ws_turn_open(int turn_index, bool greeting)
{
    /* 上一轮若有残余字节（理论不该有），现在清最安全：此刻无写者 */
    if (xStreamBufferReset(s_ws_sb) != pdPASS) return ESP_FAIL;

    if (!s_ws_helloed) {
        char hello[96];
        snprintf(hello, sizeof(hello),
                 "{\"t\":\"hello\",\"device\":\"%s\",\"token\":\"%s\"}",
                 DEVICE_ID, DEVICE_TOKEN_DEFAULT);
        if (ws_send_text_(hello) != ESP_OK) return ESP_FAIL;
        s_ws_helloed = true;                    /* welcome 帧到达后由解析器无害丢弃 */
    }
    char ev[40];
    if (greeting) {
        strlcpy(ev, "{\"t\":\"greeting\"}", sizeof(ev));
    } else {
        snprintf(ev, sizeof(ev), "{\"t\":\"turn_start\",\"turn\":%d}", turn_index);
    }
    return ws_send_text_(ev);
}

/* ===================================================== v1 HTTP 实现 === */

/**
 * @brief  手工 chunk 封帧：写 "<hex长度>\r\n" + 数据 + "\r\n"
 *
 * IDF 不会代劳（esp_http_client_write 是裸写，esp_http_client.c:1664），
 * 不封帧服务器会把 PCM 当 chunk 长度行解析，第一包就乱。
 */
static esp_err_t send_chunk(esp_http_client_handle_t c, const void *data, size_t len)
{
    char head[16];
    int hlen = snprintf(head, sizeof(head), "%zx\r\n", len);
    if (hlen <= 0 || esp_http_client_write(c, head, hlen) != hlen) return ESP_FAIL;
    if (len > 0) {
        const char *p = data;
        while (len > 0) {
            int w = esp_http_client_write(c, p, len);
            if (w <= 0) return ESP_FAIL;      /* 写不动：TCP 背压/断连，交上层 */
            p += w;  len -= w;                /* 阻塞在这里 = 免费流控（A.9 硬约束 3） */
        }
    }
    if (esp_http_client_write(c, "\r\n", 2) != 2) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t http_session_start(const char *session_id, int turn_index, bool greeting)
{
    if (tx_c == NULL) {
        tx_c = esp_http_client_init(&tx_config);   /* 句柄跨轮复用，进程内只建一次 */
        if (tx_c == NULL) {
            ESP_LOGE(TAG, "esp_http_client_init failed");
            return ESP_FAIL;
        }
    }

    esp_http_client_set_header(tx_c, "Content-Type", "audio/L16;rate=16000;channels=1");
    esp_http_client_set_header(tx_c, "X-Device-Id", DEVICE_ID);          /* A.3.1 要求 */
    if (DEVICE_TOKEN_DEFAULT[0])                                         /* 空 token 不发 */
        esp_http_client_set_header(tx_c, "X-Device-Token", DEVICE_TOKEN_DEFAULT);
    esp_http_client_set_header(tx_c, "X-Session-Id", session_id);
    esp_http_client_set_header(tx_c, "X-Greeting", greeting ? "1" : "0");
    static char turn_buf[8];   /* set_header 内部会 strdup 拷贝（http_header.c:111），
                                  static 只是让读代码的人不误会生命周期 */
    snprintf(turn_buf, sizeof(turn_buf), "%d", turn_index);
    esp_http_client_set_header(tx_c, "X-Turn-Index", turn_buf);          /* 每轮覆盖 */

    /* write_len = -1 → 请求头带 Transfer-Encoding: chunked（esp_http_client.c:1518） */
    esp_err_t err = esp_http_client_open(tx_c, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_close(tx_c);
        s_in_flight = false;
        return ESP_FAIL;
    }
    s_in_flight = true;
    return ESP_OK;
}

static esp_err_t http_tx_write(const void *data, size_t len_bytes)
{
    if (tx_c == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
    if (s_abort) return ESP_FAIL;             /* 中断请求已发出，不再写 */
    if (len_bytes == 0) return ESP_OK;
    if (send_chunk(tx_c, data, len_bytes) != ESP_OK) return ESP_FAIL;
    s_bytes_tx += len_bytes;
    return ESP_OK;
}

static esp_err_t http_read_loop(ai_frames_t *frames, void *ctx, transport_state_t *state)
{
    if (tx_c == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
    (void)ctx;                                 /* ctx 已随 ai_frames_init 存进解析器 */

    /* 1. 上行终止块：告诉服务器"我说完了"（fetch_headers 只读不写，:1410）。
          问候轮 body 为空：这一步就是全部上行。 */
    if (esp_http_client_write(tx_c, "0\r\n\r\n", 5) != 5) {
        ESP_LOGE(TAG, "send last-chunk failed");
        return ESP_FAIL;
    }

    /* 2. 等响应头。服务器收完上行才开始处理，这里阻塞是正常的；
          timeout_ms=AI_HTTP_TIMEOUT_MS 兜底，不会永久卡死 */
    int64_t hdr = esp_http_client_fetch_headers(tx_c);
    if (hdr < 0) {
        ESP_LOGE(TAG, "fetch_headers failed (timeout or reset)");
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(tx_c);
    if (status != 200) {
        ESP_LOGE(TAG, "server status %d", status);
        return ESP_FAIL;
    }
    if (state) {
        state->status = 1;
        state->bytes_tx = s_bytes_tx;
    }

    /* 3. 拉循环：read 主动拉（绝不用事件回调收数据，A.9 硬约束 1），喂解析器 */
    static uint8_t rbuf[AI_READ_BUF_SIZE];     /* static：不吃 8KB 任务栈 */
    TickType_t t0 = s_open_tick;
    for (;;) {
        if (s_abort) return ESP_ERR_TIMEOUT;   /* 外部要求中断（用户按键/仲裁抢占） */

        /* A.3.4 首帧兜底双保险：服务器先回 200 头再装死时，靠这里跳出 */
        if (state && state->first_frame_ms == 0 &&
            (xTaskGetTickCount() - t0) > pdMS_TO_TICKS(AI_FIRST_FRAME_TIMEOUT_MS + 2000)) {
            ESP_LOGE(TAG, "no first frame within %d ms", AI_FIRST_FRAME_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        int n = esp_http_client_read(tx_c, (char *)rbuf, sizeof(rbuf));
        if (n < 0) {
            ESP_LOGE(TAG, "read error");
            return ESP_FAIL;
        }
        if (n == 0) break;                     /* 终止块已收 / 对端关闭：流自然结束 */

        if (state) {
            state->bytes_rx += (size_t)n;
            if (state->first_frame_ms == 0)    /* 首帧打点：验收 A.7/A.4.5 要这个数 */
                state->first_frame_ms =
                    (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        }
        if (ai_frames_feed(frames, rbuf, (size_t)n) < 0) {
            ESP_LOGE(TAG, "frame stream corrupted");
            return ESP_FAIL;                   /* 帧边界已乱，唯一处理是断连 */
        }
    }
    return ESP_OK;
}

/* ===================================================== 公共入口（分派）=== */

esp_err_t session_start(const char *session_id, int turn_index, bool greeting)
{
    s_abort = false;
    s_bytes_tx = 0;
    s_open_tick = xTaskGetTickCount();

    bool ws_ok = false;
    if (ws_usable()) {
        ws_ok = (ws_ensure() == ESP_OK) &&
                (ws_turn_open(turn_index, greeting) == ESP_OK);
        if (!ws_ok) ws_note_fail();
    }
    if (ws_ok) {
        s_mode = TP_WS;
        s_ws_turn_done = false;
        s_in_flight = true;
        ESP_LOGI(TAG, "turn on ws (%s)", greeting ? "greeting" : "voice");
        return ESP_OK;
    }
    s_mode = TP_HTTP;                          /* 降级：v1 轮次制，行为与旧版完全一致 */
    return http_session_start(session_id, turn_index, greeting);
}

esp_err_t tx_write(const void *data, size_t len_bytes)
{
    if (s_abort) return ESP_FAIL;              /* 两种模式共用：中断后不再写 */
    if (len_bytes == 0) return ESP_OK;

    if (s_mode == TP_WS) {
        if (s_ws == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
        /* send_bin 阻塞到写进 socket = 免费流控（与 HTTP 的写背压同语义） */
        if (esp_websocket_client_send_bin(s_ws, data, len_bytes,
                                          pdMS_TO_TICKS(AI_WS_SEND_TIMEOUT_MS)) < 0)
            return ESP_FAIL;
        s_bytes_tx += len_bytes;
        return ESP_OK;
    }
    return http_tx_write(data, len_bytes);
}

esp_err_t tx_read_loop(ai_frames_t *frames, void *ctx, transport_state_t *state)
{
    if (s_mode != TP_WS) {
        return http_read_loop(frames, ctx, state);
    }
    if (s_ws == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
    (void)ctx;

    /* 1. 上行结束信号（对应 v1 的终止块）。问候轮多发一次无害：
     *    服务器侧 stream 为 None 时直接忽略。 */
    if (ws_send_text_("{\"t\":\"utterance_end\"}") != ESP_OK) {
        ESP_LOGE(TAG, "send utterance_end failed");
        return ESP_FAIL;
    }
    if (state) state->status = 1;

    /* 2. 拉循环：从字节管道读（事件回调只负责搬运，A.9 硬约束 1）。
     *    退出条件：done/err 已到（s_ws_turn_done）/ 被中断 / 断连 / 首帧超时。
     *    done 事件在全部音频帧之后（TCP 保序），退出时音频必然已播完。 */
    static uint8_t rbuf[AI_READ_BUF_SIZE];     /* static：不吃 8KB 任务栈 */
    TickType_t t0 = s_open_tick;
    for (;;) {
        if (s_abort) return ESP_ERR_TIMEOUT;
        if (s_ws_turn_done) return ESP_OK;
        if (!(xEventGroupGetBits(s_ws_ev) & WS_BIT_CONNECTED)) {
            ESP_LOGE(TAG, "ws disconnected mid-turn");
            return ESP_FAIL;
        }
        /* A.3.4 首帧兜底（WS 的 t0 从 turn_start 起算，不含建连——比 v1 更严） */
        if (state && state->first_frame_ms == 0 &&
            (xTaskGetTickCount() - t0) > pdMS_TO_TICKS(AI_FIRST_FRAME_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "no first frame within %d ms", AI_FIRST_FRAME_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        size_t n = xStreamBufferReceive(s_ws_sb, rbuf, sizeof(rbuf), pdMS_TO_TICKS(100));
        if (n == 0) continue;

        if (state) {
            state->bytes_rx += n;
            if (state->first_frame_ms == 0)
                state->first_frame_ms =
                    (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            state->bytes_tx = s_bytes_tx;
        }
        if (ai_frames_feed(frames, rbuf, n) < 0) {
            ESP_LOGE(TAG, "frame stream corrupted");
            return ESP_FAIL;                   /* 帧边界已乱：作废连接（tx_end 判定） */
        }
    }
}

void tx_end(void)
{
    if (s_mode == TP_WS) {
        bool clean = s_ws_turn_done && !s_ws_poison;
        if (s_ws != NULL && !clean) {
            /* 非正常收尾（中断/超时/断连/毒化）：连接里可能有上一轮的在途
             * 数据，保留会漏进下一轮——整条作废，下一轮 ws_ensure 重建 */
            ESP_LOGI(TAG, "ws turn ended abnormally, dropping connection");
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
            s_ws_helloed = false;
        } else {
            /* 正常收尾：连接保留复用；残余字节清掉（理论为空，防御性） */
            xStreamBufferReset(s_ws_sb);
            s_ws_fail_streak = 0;              /* 成功一轮，失败计数清零 */
        }
        s_in_flight = false;
        return;
    }
    if (tx_c != NULL && s_in_flight) {
        esp_http_client_close(tx_c);           /* 只断连接；client 保留给下一轮复用 */
    }
    s_in_flight = false;
}

void tx_abort(void)
{
    s_abort = true;    /* 读/写循环在检查点自行退出；连接由 tx_end() 统一收 */
}

void tx_turn_finished(void)
{
    s_ws_turn_done = true;   /* WS 读循环的退出信号；HTTP 模式无人消费，无害 */
}

esp_err_t session_delete(const char *session_id)
{
    if (s_mode == TP_WS) {
        /* v2 语义：服务器会话按 device 常驻（断线重连恢复上下文是特性），
         * 空闲清理由服务端 SESSION_TTL_MS 兜底，设备不再发 DELETE。 */
        ESP_LOGI(TAG, "ws mode: server session kept (ttl on server)");
        return ESP_OK;
    }
    char url[80];
    snprintf(url, sizeof(url), "%s%s/%s", SERVER_BASE, SERVER_PATH_CHAT, session_id);
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_DELETE,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_http_client_set_header(c, "X-Device-Id", DEVICE_ID);
    esp_err_t err = esp_http_client_perform(c);    /* 一次性：连、发、收、断 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "session delete failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "session %s deleted (http %d)",
                 session_id, esp_http_client_get_status_code(c));
    }
    esp_http_client_cleanup(c);                    /* 一次性客户端必须销毁，防泄漏 */
    return err;
}
