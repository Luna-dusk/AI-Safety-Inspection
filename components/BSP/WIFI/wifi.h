#ifndef __WIFI_H
#define __WIFI_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#define WIFI_CONNECTED_BIT BIT(0) //WiFi 连接成功标志位
#define WIFI_FAIL_BIT      BIT(1) //兼容保留：重试已改为无限指数退避，不再置位
#define CONFIG_ESPNOW_CHANNEL 0
/**
 * @brief  创建 WiFi 事件组并启动 STA 连接任务
 * @return ESP_OK / ESP_ERR_INVALID_STATE(重复调用) / ESP_ERR_NO_MEM
 */
esp_err_t esp_create_wifi_task(void);
/**
 * @brief  WiFi 主状态循环（由 esp_create_wifi_task 内部创建）
 * @param  pvParameters 未使用
 *
 * 无凭证自动进配网、有凭证连接、连接后轮询配网请求，详见 wifi.c。
 */
void wifi_init_sta(void *pvParameters);
/**
 * @brief  发起一次 esp_wifi_connect()，失败只打日志
 */
void wifi_connect(void);
/**
 * @brief  用给定凭证配置 STA 并发起连接（wifi 任务与配网 /save 共用）
 * @param  ssid 以 0 结尾；pass 可为空串（开放网络）
 * @return ESP_OK / 配置失败错误码
 */
esp_err_t wifi_sta_apply_credentials(const char *ssid, const char *pass);

// 获取 WiFi 事件组句柄，供其他模块等待连接就绪
/**
 * @brief  取得 WiFi 事件组句柄，用于等待 WIFI_CONNECTED_BIT
 * @return 句柄；未初始化时返回 NULL
 */
EventGroupHandle_t wifi_get_event_group(void);
#endif
