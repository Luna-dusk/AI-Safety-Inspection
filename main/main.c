#include <stdio.h>
#include <string.h>
#include "i2s.h"
#include "wifi.h"
#include "afe_sr.h"
#include "multinet_cmd.h"
#include "myesp_now.h"
#include "fall_det.h"
#include "PWR.h"
#include "chat.h"
#include "my_client.h"
#include "lcd_ui.h"
#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "MAIN";

static QueueHandle_t s_pcm_queue = NULL;

/** @brief 打一行内部 RAM 余量（静默降级只能看数字定位） */
static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "heap[%s]: internal free=%u largest=%u dma=%u psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/**
 * @brief  本地命令命中后的回复回调（MultiNet 命令执行完触发）
 *
 * 跑在 afe_fetch 任务上下文，且在 capture_enabled=false 之前：这里不能播 I2S
 * （阻塞写会拖死识别管线），也不能恢复采集（会被立刻关回去）。只转交请求，
 * 由 ai_chat 任务统一播放，播完再恢复采集。
 */
static void local_cmd_reply_cb(int cmd_id)
{
    ESP_LOGI(TAG, "local cmd %d reply -> prompt", cmd_id);
    /* TODO(录音): 为每条命令配专属提示音后按 cmd_id 选择文件 */
    ai_chat_request_prompt(PROMPT_CMD_OK);
}

/**
 * @brief  一次性校时任务：等 WiFi 起来 → SNTP 同步 → 退出
 *
 * 单独建任务：`time_sync()` 内部 `esp_netif_sntp_sync_wait()` 最长阻塞 10 秒，
 * 不能放在 app_main 或 lcd_ui 任务里。优先级 4 低于所有音频/UI 任务。
 *
 * 状态栏 HH:MM 由 lcd_ui 任务自己从 localtime 取（见 lcd_ui.c: clock_tick），
 * 这里不用回写界面。
 */
static void time_sync_task(void *arg)
{
    (void)arg;

    /* ⚠ 必须轮询等：事件组在第 5 步 esp_create_wifi_task() 里才创建，本任务
     * 是 3.5 步，早一拍就拿到 NULL → 跳过等待 → 立刻 SNTP 必超时 → 永远 --:--。*/
    bool online = false;
    for (int i = 0; i < 120; i++) {              /* 最多等 60s */
        EventGroupHandle_t ev = wifi_get_event_group();
        if (ev != NULL && (xEventGroupGetBits(ev) & WIFI_CONNECTED_BIT)) {
            online = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!online) {
        ESP_LOGW(TAG, "等 WiFi 超时，暂不校时（状态栏保持 --:--）");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));             /* 等 DHCP/DNS 就绪 */

    /* 三路尝试：SNTP → 服务器 Date 头 → 再 SNTP
     * （热点/企业网常拦 UDP 123；服务器也可能还没起来，所以两边各留一次机会）*/
    if (!time_is_valid() && time_sync() != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 不可用（UDP 123 可能被网络拦了），改用服务器 Date 头");
        time_sync_http(SERVER_BASE "/healthz");
    }
    if (!time_is_valid()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        time_sync();
    }

    if (time_is_valid()) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        ESP_LOGI(TAG, "时钟就绪：%04d-%02d-%02d %02d:%02d（状态栏开始走）",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
        vTaskDelete(NULL);
        return;
    }

    /* 还没成：每 2 分钟再试一次，最多 10 次，之后放弃并留日志。
     * 不做常驻重试任务——网络恢复后用户重启一次即可。*/
    ESP_LOGE(TAG, "校时全部失败，进入慢重试");
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(120000));
        time_sync();
        if (!time_is_valid()) {
            time_sync_http(SERVER_BASE "/healthz");
        }
        if (time_is_valid()) {
            ESP_LOGI(TAG, "慢重试成功，时钟就绪");
            break;
        }
    }
    if (!time_is_valid()) {
        ESP_LOGE(TAG, "放弃校时：状态栏保持 --:--（检查网络能否访问 NTP 或 %s）", SERVER_BASE);
    }
    vTaskDelete(NULL);
}

/**
 * @brief  对话文本上屏：把 ASR / AI 回复文本接到 LCD 对话屏
 *
 * 数据来源（server → ai_frames → chat.c:handle_json → 本回调）：
 *   {"t":"asr",  "text":"..."}  → 用户说的话
 *   {"t":"reply","text":"..."}  → AI 回复
 * 跑在 ai_chat 任务里，所以这里**只能**调 lcd_ui_chat_*（内部入队），
 * 绝不能碰 lv_*（LVGL 非线程安全）。
 *
 * LCD_CHAT_AUTO_SHOW：说话后是否自动切到对话屏（关掉就只能点卡片进去看）。
 */
#define LCD_CHAT_AUTO_SHOW 1

static void on_chat_event(ai_chat_event_t ev, const char *text, void *ctx)
{
    (void)ctx;
    if (text == NULL) {
        return;
    }
    switch (ev) {
    case AI_CHAT_EVT_ASR:
        ESP_LOGI(TAG, "chat text [user] %u bytes", (unsigned)strlen(text));
        lcd_ui_chat_add(LCD_CHAT_USER, text);
#if LCD_CHAT_AUTO_SHOW
        lcd_ui_chat_show();
#endif
        break;
    case AI_CHAT_EVT_REPLY:
        ESP_LOGI(TAG, "chat text [ai] %u bytes", (unsigned)strlen(text));
        lcd_ui_chat_add(LCD_CHAT_AI, text);
        break;
    default:
        break;
    }
}

/**
 * @brief  跌倒/传感器事件回调（FR-3）
 *
 * 跑在 fall_det 任务上下文（优先级 8）——只做轻活，不播 I2S、不发网络
 * （拖死采集任务就漏真跌倒）。告警转发要在这里只投队列/置标志，由别的任务消费。
 */
static void fall_det_evt_cb(fall_det_event_t evt, float pitch, float roll, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case FALL_DET_EVT_FALL:
        ESP_LOGW(TAG, ">>> FALL confirmed: pitch=%.1f roll=%.1f", pitch, roll);
        /* TODO(告警): ESP-NOW 广播配对设备 + 播本地告警音 + 上报服务器 */
        break;
    case FALL_DET_EVT_SENSOR_FAULT:
        ESP_LOGE(TAG, ">>> IMU sensor fault (L3)");
        /* TODO(告警): FR-10 每日自检事件上报 */
        break;
    case FALL_DET_EVT_SENSOR_RECOVER:
        ESP_LOGI(TAG, ">>> IMU sensor recovered");
        break;
    }
}

void app_main(void)
{
    /* 0. 电源自锁：**必须是第一行业务代码**。K1 松手后栅极只靠 C18 撑 2.2s，
     *    晚了就是"上电→掉电→重启"死循环（HANDOFF 坑 34）。
     *    这里失败 = 2.2s 后必然断电，所以只记日志、继续跑，好歹留下根因。 */
    if (pwr_init() != ESP_OK) {
        ESP_LOGE(TAG, "pwr_init failed -> 2.2s 后必然断电，检查 GPIO40/GPIO18");
    }

    /* 固件指纹（编译时刻）。放 pwr_init 之后，是为了不占用 2.2s 电源窗口。*/
    ESP_LOGI(TAG, "===== app_main @ %s %s =====", __DATE__, __TIME__);

    /* 传感器未接线期间静音 i2c.master 驱动日志（每帧 NACK 刷三行，会淹掉
     * fall_det 的 L1/L2/L3 汇总）。接上 MPU6050/QMC5883L 后删掉这一行。 */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    /* 1. PCM 队列：33KB 且要求连续，内部 RAM 给不起（HANDOFF 坑 31）。
     *    存储放 PSRAM、控制块留内部；前提是本队列**从不在 ISR 里访问**。*/
    log_heap("boot");
    {
        static StaticQueue_t s_pcm_queue_struct;   /* 控制块：必须在内部 RAM */
        void *storage = heap_caps_malloc(PCM_QUEUE_LEN * sizeof(audio_frame_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage != NULL) {
            s_pcm_queue = xQueueCreateStatic(PCM_QUEUE_LEN, sizeof(audio_frame_t),
                                             (uint8_t *)storage, &s_pcm_queue_struct);
        }
        if (s_pcm_queue == NULL) {
            /* PSRAM 不可用（未焊/初始化失败）才退回内部 RAM */
            ESP_LOGW(TAG, "pcm queue PSRAM 存储失败 -> 退回内部 RAM");
            s_pcm_queue = xQueueCreate(PCM_QUEUE_LEN, sizeof(audio_frame_t));
        }
        if (s_pcm_queue == NULL) {
            ESP_LOGE(TAG, "pcm queue 彻底失败 -> 语音链路不可用（配网/对讲仍可跑）");
        } else {
            ESP_LOGI(TAG, "pcm queue: %d 帧 × %d B，存储于 %s",
                     PCM_QUEUE_LEN, (int)sizeof(audio_frame_t),
                     (storage != NULL) ? "PSRAM" : "内部RAM");
        }
    }
    log_heap("pcm_queue");

    /* ── 3. 屏幕：**紧跟着队列初始化**，趁内部堆还完整时抢（HANDOFF 坑 31/3b）。
     *    排在 WiFi/esp-sr/AFE 之后会连 11KB 都拿不到（碎片化）→ 屏幕全黑。
     * 优先级 8：低于 afe_feed(21)/afe_fetch(20)/ai_chat(12)——UI 掉帧没人察觉，
     * 音频掉帧就是爆音。栈 8192 与 ai_chat 同规格。 */
    log_heap("before_lcd");
    lcd_ui_init();
    if (xTaskCreate(lcd_ui_task, "lcd_ui", 8192, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create lcd_ui task failed");
    }
    log_heap("after_lcd");


    /* 3.5 校时：联网后一次性 SNTP（会阻塞 10s，所以丢给独立任务） */
    if (xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create time_sync task failed");
    }

    /* 4. 硬件层：装 I2S0(喇叭)/I2S1(双麦) 驱动 */
    init_i2s();

    //测试音频模块
    //spk_selftest_start();

    /* 5. WiFi 独立任务连接，不阻塞启动序列 */
    esp_create_wifi_task();

    /* 6. ESP-NOW：内部自己等 WiFi 就绪，连不上也不拖累下面 */
    if (my_esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "espnow init failed");
    }

    /* 7. 语音链路：建实例（顺带初始化 LED/MultiNet/按键）→ 起 feed/fetch 双任务。
     *    key_init/multinet_cmd_init 已由 afe_sr_start/afe_init 内部调用，别重复调。*/
    /*    队列已在第 1 步建好：内存次序原因，不能挪到这里。*/
    if (afe_init() != ESP_OK) {
        ESP_LOGE(TAG, "afe init failed");
    }
    if (s_pcm_queue != NULL) {
        if (afe_sr_start(s_pcm_queue) != ESP_OK) {
            ESP_LOGE(TAG, "afe sr start failed");
        }
    } else {
        ESP_LOGE(TAG, "skip afe_sr_start: no pcm queue");
    }

    /* 8. pcm_queue 消费者 = AI_CHAT 任务（栈 8192）：上传/播放/兜底/会话管理。
     *    ❗队列为 NULL 时**绝不能**再建它：arg=NULL 会在 xQueueReceive 里断言，
     *      表现就是看门狗复位循环（HANDOFF 坑 31）。*/
    multinet_cmd_set_reply_cb(local_cmd_reply_cb);
    ai_chat_set_event_cb(on_chat_event, NULL);      /* ASR/回复文本 → 对话屏 */
    if (s_pcm_queue != NULL) {
        if (xTaskCreate(ai_chat_task, "ai_chat", 8192, s_pcm_queue, 12, NULL) != pdPASS) {
            ESP_LOGE(TAG, "create ai_chat task failed");
        }
    } else {
        ESP_LOGE(TAG, "skip ai_chat task: no pcm queue");
    }

    /* 9. 跌倒检测（FR-3）：内部自建 50Hz 任务（核 1，优先级 8），含最长 ~12s
     *    的零偏校准，全在任务里做，不阻塞启动序列。传感器不在也不报错。*/
    if (fall_det_start(fall_det_evt_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "fall detect start failed");
    }

    ESP_LOGI(TAG, "app_main done, all tasks running");
}
