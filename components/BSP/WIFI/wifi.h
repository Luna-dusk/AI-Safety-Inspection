#ifndef __WIFI_H
#define __WIFI_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"
#include "esp_err.h"


// WiFi 凭据保存在本地未跟踪的 wifi_credentials.h 中
// 首次使用：cp wifi_credentials.h.example wifi_credentials.h 后填写
#include "wifi_credentials.h"

#define WIFI_CONNECTED_BIT BIT(0) //WiFi 连接成功标志位
#define WIFI_FAIL_BIT      BIT(1) //WiFi 连接失败标志位

esp_err_t esp_create_wifi_task(void);
void wifi_init_sta(void *pvParameters);
void wifi_connect(void);

// 获取 WiFi 事件组句柄，供其他模块等待连接就绪 
EventGroupHandle_t wifi_get_event_group(void);
#endif
