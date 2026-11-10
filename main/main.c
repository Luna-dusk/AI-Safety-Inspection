#include <stdio.h>
#include "i2s.h"
#include "wifi.h"
#include "afe_sr.h"
#include "multinet_cmd.h"
#include "myesp_now.h"
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

void app_main(void)
{
    /* 1. 硬件层最先：装 I2S0(喇叭)/I2S1(双麦) 驱动 */
    init_i2s();

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

    ESP_LOGI(TAG, "app_main done, all tasks running");
}
