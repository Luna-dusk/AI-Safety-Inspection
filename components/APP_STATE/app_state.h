#ifndef _APP_STATE_H_
#define _APP_STATE_H_

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_sntp.h"
#include "wifi.h"
#include <sys/time.h>
#include "stdlib.h"

bool time_is_valid(void);

/** 用 SNTP(UDP/123) 校时，最长阻塞 10s。需要联网。 */
esp_err_t time_sync(void);

/**
 * @brief 用 HTTP 响应的 `Date` 头校时（SNTP 兜底方案）
 *
 * 手机热点/企业网经常丢弃 UDP 123，此时 `time_sync()` 必然超时。
 * 但 HTTP 通路是通的（对话本身就走它），而每个 HTTP 响应都带 `Date` 头，
 * 精度到秒，对状态栏时钟完全够用。
 *
 * @param url  任意可 GET 的地址（本项目用 SERVER_BASE "/healthz"）
 * @return ESP_OK 表示已 settimeofday()；失败会打 WARN 说明卡在哪一步
 */
esp_err_t time_sync_http(const char *url);

#endif // 