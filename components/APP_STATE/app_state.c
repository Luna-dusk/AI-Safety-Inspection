#include <time.h>
#include "app_state.h"

static const char *TAG = "APP_STATE";

/* 上电第一件事：判断时间到底可不可信 */
bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now > 1704067200;
}

esp_err_t time_sync(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    /* 初始化（cfg.start=true，所以这里同时启动了同步） */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 阻塞等，最多 10 s（依赖 cfg.wait_for_sync == true） */
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "校时超时，时间仍不可信");
        esp_netif_sntp_deinit();
        return err;
    }

    time_t now = 0;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "校时成功: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    esp_netif_sntp_deinit();   /* 不需要持续同步就关掉，省内存和 socket */
    return ESP_OK;
}