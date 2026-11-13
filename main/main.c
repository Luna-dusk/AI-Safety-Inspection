#include <stdio.h>
#include "i2s.h"
#include "wifi.h"
#include "afe_sr.h"
#include "multinet_cmd.h"
#include "myesp_now.h"
#include "fall_det.h"
#include "PWR.h"
#include "chat.h"
#include "my_client.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

static QueueHandle_t s_pcm_queue = NULL;

/**
 * @brief  本地命令命中后的回复回调（MultiNet 命令执行完触发）
 *
 * 注意时序：本回调在 multinet_cmd_exec() 里被调用，发生在 fetch 任务
 * 置 capture_enabled=false **之前**，且跑在 afe_fetch 任务上下文里，
 * 所以这里绝不能直接播 I2S（阻塞写会拖死识别管线），也不能恢复采集
 * （会被立刻关回去）。只转交请求，由 ai_chat 任务统一播放，
 * 播完再恢复采集——REPLY_PLACEHOLDER_MS 假延时已移除（附录 A.9 #3）。
 */
static void local_cmd_reply_cb(int cmd_id)
{
    ESP_LOGI(TAG, "local cmd %d reply -> prompt", cmd_id);
    /* TODO(录音): 为每条命令配专属提示音后按 cmd_id 选择文件 */
    ai_chat_request_prompt(PROMPT_CMD_OK);
}

/**
 * @brief  跌倒/传感器事件回调（FR-3）
 *
 * 跑在 fall_det 任务上下文（优先级 8）——只做轻活，绝不播 I2S、
 * 不发网络（拖死采集任务就漏真跌倒）。告警转发（ESP-NOW / 企业微信）
 * 接入时在本函数里投队列/置标志，由对应任务消费。
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
    /* 0. 电源自锁：**必须是第一行业务代码**（mzx 分支 PWR 模块）。
     *    K1 松手后主开关栅极只靠 C18(22µF) 撑 2.2s（τ=R11×C18=100k×22µF）；
     *    下面 I2S/WiFi/AFE 初始化加起来几百毫秒，晚了会"上电→掉电→重启"
     *    死循环，而且掉得太快串口都抓不到。这里失败说明 2.2s 后必然断电，
     *    所以只记日志、继续跑（至少串口能看到根因）。 */
    if (pwr_init() != ESP_OK) {
        ESP_LOGE(TAG, "pwr_init failed -> 2.2s 后必然断电，检查 GPIO40/GPIO18");
    }

    /* 传感器尚未接线期间静音 i2c.master 驱动日志：每帧 NACK 会刷三行、把有用
     * 信息全淹掉（fall_det 自己的 L1/L2/L3 汇总日志仍然保留）。
     * 接上 MPU6050/QMC5883L 之后删掉这一行，恢复 I2C 错误可见性。 */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    /* 1. 硬件层最先：装 I2S0(喇叭)/I2S1(双麦) 驱动 */
    init_i2s();

    //测试音频模块
    //spk_selftest_start();

    /* 2. WiFi 独立任务连接，不阻塞启动序列 */
    esp_create_wifi_task();

    /* 3. ESP-NOW：内部自己等 WiFi 就绪，连不上也不拖累下面 */
    if (my_esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "espnow init failed");
    }

    /* 4. 语音链路：建队列 → 建实例（顺带初始化 LED/MultiNet/按键）
     *    → 起 feed/fetch 双任务。注意 key_init/multinet_cmd_init
     *    都已由 afe_sr_start/afe_init 内部调用，这里不要重复调。 */
    s_pcm_queue = xQueueCreate(PCM_QUEUE_LEN, sizeof(audio_frame_t));
    if (s_pcm_queue == NULL) {
        ESP_LOGE(TAG, "create pcm queue failed");
    }
    if (afe_init() != ESP_OK) {
        ESP_LOGE(TAG, "afe init failed");
    }
    if (afe_sr_start(s_pcm_queue) != ESP_OK) {
        ESP_LOGE(TAG, "afe sr start failed");
    }

    /* 5. pcm_queue 消费者 = AI_CHAT 任务（栈 8192，附录 A.9 #5）：
     *    上传/播放/兜底/会话管理全在里面。没有它第一句话说完设备就永久卡死。 */
    multinet_cmd_set_reply_cb(local_cmd_reply_cb);
    if (xTaskCreate(ai_chat_task, "ai_chat", 8192, s_pcm_queue, 12, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create ai_chat task failed");
    }

    /* 6. 跌倒检测（FR-3）：内部自建 50Hz 任务（核 1，优先级 8），
     *    初始化含最长 ~12s 的零偏校准，全部在任务里做，不阻塞启动序列。
     *    传感器不在也不报错——恢复阶梯持续重试并回调 FAULT。 */
    if (fall_det_start(fall_det_evt_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "fall detect start failed");
    }

    ESP_LOGI(TAG, "app_main done, all tasks running");
}
