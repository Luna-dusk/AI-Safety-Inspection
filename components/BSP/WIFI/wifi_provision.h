#ifndef __WIFI_PROVISION_H
#define __WIFI_PROVISION_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define WIFI_PROV_SSID_MAX 32   /* SSID 最大字节数（UTF-8 中文按字节算，不含结尾 0）*/
#define WIFI_PROV_PASS_MAX 64   /* 密码最大字节数，不含结尾 0 */

/**
 * @brief  从 NVS（命名空间 "wifi_prov"）读取已保存的 WiFi 凭证
 * @param  ssid/pass 输出缓冲区，长度须 >= 对应 *_MAX + 1
 * @return ESP_OK 找到；ESP_ERR_NVS_NOT_FOUND 无凭证；其它为 NVS 错误
 */
esp_err_t wifi_prov_read(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/**
 * @brief  保存 WiFi 凭证到 NVS（配网页 /save 端点写入）
 * @param  pass 开放网络传空串即可
 */
esp_err_t wifi_prov_write(const char *ssid, const char *pass);

/**
 * @brief  是否已有可用凭证（快速探测，不搬运数据）
 */
bool wifi_prov_has_credentials(void);

/**
 * @brief  是否有未处理的配网请求（只查看，不消费）
 * @note   wifi 任务等待循环里轮询用
 */
bool wifi_provision_requested(void);

/**
 * @brief  取走并清除配网请求（读取即消费）
 * @return true=本次取到了请求
 */
bool wifi_provision_take_request(void);

/**
 * @brief  请求进入配网模式（长按 BOOT 触发，非阻塞只置请求位）
 * @note   实际切 AP/起门户统一由 wifi 任务执行，避免多任务改 WiFi 状态
 */
void wifi_provision_request(void);

/**
 * @brief  运行配网门户（阻塞）：APSTA + SoftAP + DNS 劫持 + 配网页
 * @return ESP_OK 配网完成（STA 已拿到 IP，门户自动收尾）
 * @note   只能由 wifi 任务调用；门户会在联网成功 3s 后自动关闭热点
 */
esp_err_t wifi_provision_run(void);

/* ---- 内部钩子：仅由 wifi.c 事件回调调用 ---- */
void wifi_provision_notify_connected(void);
void wifi_provision_notify_disconnected(int reason);

#endif
