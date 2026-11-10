#include "myesp_now.h"

static const char *TAG = "ESP_NOW";

/* 对端 peer 信息（extern 声明在 myesp_now.h）。
 * MAC 改成另一台设备的 STA MAC：两台各自上电后用日志里打印的
 * "self MAC" 互相抄。channel=0 表示跟随本机当前信道——两台连同一个
 * AP 时信道自动一致，路由器换信道也会自动跟随，不要硬编码。 */
esp_now_peer_info_t peer = {
    .peer_addr = {0xC4, 0xDD, 0x57, 0x12, 0x34, 0x56},  // TODO: 换成对端真实 MAC
    .channel = 0,      // 0 = 跟随当前信道
    .ifidx = WIFI_IF_STA,
    .encrypt = false,  // TODO: 控制帧加密待接入（esp_now_set_pmk + lmk）
};

static QueueHandle_t espnow_queue = NULL;

/* ══════════ 回调（运行在 WiFi 任务上下文：只拷贝、只投递、绝不阻塞） ══════════ */

static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL || espnow_queue == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    espnow_event_t evt = {0};
    evt.id = ESPNOW_SEND_CB;
    memcpy(evt.info.send_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    evt.info.send_cb.status = status;

    /* timeout=0：WiFi 任务里不允许阻塞，队列满就丢事件。
     * 丢的只是"发送结果通知"，不影响链路本身。 */
    if (xQueueSend(espnow_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "send cb queue full, drop event");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (recv_info == NULL || recv_info->src_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }
    if (espnow_queue == NULL) {
        return;   /* 队列还没建好（初始化竞态），直接丢 */
    }

    espnow_event_t evt = {0};
    evt.id = ESPNOW_RECV_CB;
    memcpy(evt.info.recv_cb.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    /* 深拷贝出 WiFi 任务上下文。当前每包 malloc 在低频控制帧下够用；
     * 接音频流（~25 包/s）时应换成静态缓冲池。 */
    evt.info.recv_cb.data = malloc(len);
    if (evt.info.recv_cb.data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(evt.info.recv_cb.data, data, len);
    evt.info.recv_cb.data_len = len;

    /* timeout=0：队列满立刻丢包（实时流丢包是正确策略，阻塞才是灾难） */
    if (xQueueSend(espnow_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "recv cb queue full, drop %d bytes", len);
        free(evt.info.recv_cb.data);
    }
}

/* ══════════ 事件消费：链路就绪 + 循环处理收发事件 ══════════ */

static void espnow_task(void *arg)
{
    /* 1. 等 WiFi 拿到 IP：此时信道已与对端（同一 AP）一致，
     *    esp_wifi_start 也必然已完成，esp_now_init 才能成功。
     *    30s 连不上就放弃（离线优先：语音等其它功能不受影响）。 */
    EventBits_t bits = xEventGroupWaitBits(wifi_get_event_group(),
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,   /* 不清除位 */
                                           pdFALSE,   /* 任一位即返回 */
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi not connected in 30s, espnow link NOT ready");
        vTaskDelete(NULL);
        return;
    }

    /* 2. esp_now_init + 注册回调（必须在 esp_wifi_start 之后） */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(example_espnow_recv_cb));

    /* 3. 打印自身 MAC（对方要把这个填进 peer.peer_addr）并挂 peer */
    uint8_t self_mac[ESP_NOW_ETH_ALEN];
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    ESP_LOGI(TAG, "self MAC: " MACSTR " (fill this into the OTHER board's peer_addr)",
             MAC2STR(self_mac));

    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add peer failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "peer added: " MACSTR " (channel=%d)", MAC2STR(peer.peer_addr), peer.channel);
    ESP_LOGI(TAG, "espnow link ready");

    /* 4. 循环消费收发事件 */
    espnow_event_t evt;
    for (;;) {
        if (xQueueReceive(espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (evt.id == ESPNOW_SEND_CB) {
            ESP_LOGI(TAG, "send to " MACSTR " -> %s",
                     MAC2STR(evt.info.send_cb.mac_addr),
                     (evt.info.send_cb.status == ESP_NOW_SEND_SUCCESS) ? "OK" : "FAIL");
        } else if (evt.id == ESPNOW_RECV_CB) {
            /* TODO: 按协议分发（心跳 / 告警 / 音频包），当前只打日志 */
            ESP_LOGI(TAG, "recv %d bytes from " MACSTR,
                     evt.info.recv_cb.data_len, MAC2STR(evt.info.recv_cb.mac_addr));
            free(evt.info.recv_cb.data);
            evt.info.recv_cb.data = NULL;
        }
    }
}

/* ══════════ 对外接口 ══════════ */

esp_err_t my_esp_now_init(void)
{
    if (espnow_queue != NULL) {
        return ESP_OK;   /* 幂等：重复调用直接成功 */
    }

    espnow_queue = xQueueCreate(16, sizeof(espnow_event_t));
    if (espnow_queue == NULL) {
        ESP_LOGE(TAG, "create espnow queue failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(espnow_task, "espnow", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create espnow task failed");
        vQueueDelete(espnow_queue);
        espnow_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
