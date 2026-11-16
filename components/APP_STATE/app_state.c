#include <stdio.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "esp_http_client.h"
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

/* ===================== HTTP Date 头校时（SNTP 兜底）===================== */

static volatile bool s_http_got_date;
static time_t        s_http_time;

/** Howard Hinnant 的 days_from_civil：把 UTC 年月日时分秒换成 epoch 秒。
 *  不用 strptime/timegm —— 前者依赖 locale，后者在 newlib 里要额外确认。*/
static time_t utc_to_epoch(int year, int mon, int day, int h, int mi, int sec)
{
    int y = year - (mon <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(mon + (mon > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)day - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60 + sec;
}

static int month_from_name(const char *m)
{
    static const char *names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(m, names[i], 3) == 0) {
            return i + 1;
        }
    }
    return 0;
}

/** 解析 RFC 1123: "Fri, 14 Nov 2026 08:42:07 GMT" */
static bool parse_http_date(const char *v, time_t *out)
{
    char wd[8] = {0}, mon[8] = {0};
    int day = 0, year = 0, h = 0, mi = 0, sec = 0;
    if (sscanf(v, "%7s %d %7s %d %d:%d:%d", wd, &day, mon, &year, &h, &mi, &sec) != 7) {
        return false;
    }
    int m = month_from_name(mon);
    if (m == 0 || year < 2024 || day < 1 || day > 31) {
        return false;
    }
    *out = utc_to_epoch(year, m, day, h, mi, sec);
    return true;
}

static esp_err_t http_date_events(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key != NULL && e->header_value != NULL) {
        if (strcasecmp(e->header_key, "Date") == 0) {
            time_t t;
            if (parse_http_date(e->header_value, &t)) {
                s_http_time = t;
                s_http_got_date = true;
            } else {
                ESP_LOGW(TAG, "Date 头解析失败: %s", e->header_value);
            }
        }
    }
    return ESP_OK;
}

esp_err_t time_sync_http(const char *url)
{
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_http_got_date = false;

    esp_http_client_config_t cfg = {
        .url                 = url,
        .method              = HTTP_METHOD_GET,
        .timeout_ms          = 8000,
        .disable_auto_redirect = true,
        .event_handler       = http_date_events,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGW(TAG, "HTTP 校时: client init 失败");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP 校时请求失败: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_http_got_date) {
        ESP_LOGW(TAG, "HTTP 校时: 响应里没有可用的 Date 头");
        return ESP_ERR_NOT_FOUND;
    }

    struct timeval tv = { .tv_sec = s_http_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "用服务器 Date 头校时成功");
    return ESP_OK;
}
