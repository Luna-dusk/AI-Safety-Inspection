/**
 * @file wifi_provision.c  SoftAP + Captive Portal 网页配网
 *
 * 流程：wifi 任务调用 wifi_provision_run()（阻塞）进入配网：
 *   1. 切 APSTA，开热点 "AI巡检-XXXXXX"（MAC 后 3 字节，开放无密码）；
 *   2. 启动 UDP:53 DNS 劫持（所有域名都解析到设备）+ httpd 配网页；
 *   3. 手机连热点后 captive portal 自动弹页（404 统一 302 回首页）；
 *   4. /save 把凭证写入 NVS 并触发 STA 试连，/status 供页面轮询结果；
 *   5. GOT_IP 钩子触发后延时 3s 关门户/热点，LED 转常亮。
 *
 * 已知限制：
 *   - 重配网成功后 ESP-NOW 不会自动恢复（其任务 30s 超时自删，重启恢复）；
 *   - 服务器地址/密钥仍为编译期宏（AI_CHAT/my_client.h），不在本模块范围。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#include "wifi.h"
#include "wifi_provision.h"
#include "LED.h"

static const char *TAG = "wifi_prov";

/* 内嵌配网页面（BSP CMakeLists EMBED_TXTFILES 注入，txt 模式自带结尾 0）*/
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[]   asm("_binary_portal_html_end");

#define PROV_NVS_NS     "wifi_prov"
#define DNS_PORT        53
#define AP_CHANNEL      1
#define AP_MAX_CONN     4
#define CLOSE_DELAY_US  (3ULL * 1000000)  /* 联网成功后留 3s 让成功页刷完再关门户 */
#define LED_BLINK_MS    250

/* ---- portal 运行状态（wifi 任务 / httpd 任务 / 事件任务三方访问）---- */
typedef enum {
    PROV_WAIT,        /* 页面就绪，等用户提交 */
    PROV_CONNECTING,  /* 已提交，STA 试连中 */
    PROV_OK,          /* 试连成功 */
    PROV_ERR,         /* 试连失败（密码错误等），可在网页重试 */
} prov_state_t;

static volatile prov_state_t s_state = PROV_WAIT;
static volatile bool s_portal_active = false;
static volatile bool s_prov_requested = false;

static httpd_handle_t s_httpd = NULL;
static SemaphoreHandle_t s_done_sem = NULL;
static esp_timer_handle_t s_close_timer = NULL;  /* GOT_IP 后延时关门户 */
static esp_timer_handle_t s_led_timer = NULL;    /* 配网中 LED 快闪 */
static volatile TaskHandle_t s_dns_task = NULL;
static volatile bool s_dns_run = false;
static uint32_t s_dns_ip = 0;                    /* 劫持应答指向的设备 IP（网络序）*/

/* ============================ 凭证存取（NVS） ============================ */

esp_err_t wifi_prov_read(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = ssid_len;
    err = nvs_get_str(h, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(h, "pass", pass, &len);
    }
    nvs_close(h);
    return err;
}

esp_err_t wifi_prov_write(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PROV_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool wifi_prov_has_credentials(void)
{
    char ssid[WIFI_PROV_SSID_MAX + 1];
    char pass[WIFI_PROV_PASS_MAX + 1];
    return wifi_prov_read(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;
}

/* ============================ 配网请求（跨任务） ============================ */

bool wifi_provision_requested(void)
{
    return s_prov_requested;
}

bool wifi_provision_take_request(void)
{
    if (s_prov_requested) {
        s_prov_requested = false;
        return true;
    }
    return false;
}

void wifi_provision_request(void)
{
    ESP_LOGI(TAG, "provisioning requested (BOOT long press)");
    s_prov_requested = true;
}

/* ============================ portal 内部实现 ============================ */

/**
 * @brief  其余路径一律 302 回首页：手机 captive portal 探测路径被引到配网页
 */
static esp_err_t err_404_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const char *s = (s_state == PROV_OK) ? "ok"
                  : (s_state == PROV_ERR) ? "err" : "connecting";
    char body[24];
    snprintf(body, sizeof(body), "{\"s\":\"%s\"}", s);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, strlen(body));
}

/**
 * @brief  JSON 转义 SSID 中的 " \ 和控制字符（中文 UTF-8 原样透传）
 */
static size_t json_escape(const char *in, size_t in_len, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 7 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(out + o, out_len - o, "\\u%04X", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

/**
 * @brief  GET /scan：阻塞扫描周边 AP，返回 [{s,r,l}] JSON（ssid/rssi/加密）
 * @note   APSTA 下扫描走 STA 接口，热点不中断；一次阻塞约 1.5s
 */
static esp_err_t scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) {
        num = 20;   /* 页面只展示最强的 20 个，控制 JSON 大小 */
    }
    if (num == 0) {
        esp_wifi_clear_ap_list();   /* 释放驱动侧扫描内存 */
        return httpd_resp_sendstr(req, "[]");
    }
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return httpd_resp_sendstr(req, "[]");
    }
    esp_wifi_scan_get_ap_records(&num, recs);

    char esc[(WIFI_PROV_SSID_MAX + 1) * 6];
    char item[sizeof(esc) + 48];
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < num; i++) {
        char ssid[WIFI_PROV_SSID_MAX + 1] = {0};   /* 32 字节 SSID 可能不带 0 */
        memcpy(ssid, recs[i].ssid, WIFI_PROV_SSID_MAX);
        json_escape(ssid, strlen(ssid), esc, sizeof(esc));
        snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"r\":%d,\"l\":%s}",
                 i ? "," : "", esc, recs[i].rssi,
                 recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);   /* 结束 chunked */
    free(recs);
    return ESP_OK;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief  urlencoded 解码（%XX 与 +），就地输出 0 结尾字符串
 */
static size_t url_decode(const char *in, size_t in_len, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_len; i++) {
        char c = in[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < in_len &&
                   hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (char)((hexval(in[i + 1]) << 4) | hexval(in[i + 2]));
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = 0;
    return o;
}

/**
 * @brief  从 urlencoded body 提取 name 参数（ssid=..&pass=..）
 */
static bool url_param(const char *body, const char *name, char *out, size_t out_len)
{
    size_t nlen = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            url_decode(v, vlen, out, out_len);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

/**
 * @brief  POST /save：校验凭证 -> 写 NVS -> STA 试连（portal 保持在线）
 * @note   连接结果经 /status 轮询反馈；失败可改密码重复提交
 */
static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request length");
        return ESP_OK;
    }
    char body[513] = {0};
    int recvd = 0;
    while (recvd < req->content_len) {
        int n = httpd_req_recv(req, body + recvd, req->content_len - recvd);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read body failed");
            return ESP_OK;
        }
        recvd += n;
    }

    char ssid[WIFI_PROV_SSID_MAX + 1] = {0};
    char pass[WIFI_PROV_PASS_MAX + 1] = {0};
    url_param(body, "ssid", ssid, sizeof(ssid));
    url_param(body, "pass", pass, sizeof(pass));

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > WIFI_PROV_SSID_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid");
        return ESP_OK;
    }
    if (pass_len > 0 && (pass_len < 8 || pass_len > WIFI_PROV_PASS_MAX)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid password length (8-64, empty for open network)");
        return ESP_OK;
    }

    esp_err_t err = wifi_prov_write(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
        return ESP_OK;
    }
    /* 立即用新凭证试连；APSTA 下热点不掉线，结果经 /status 反馈 */
    wifi_sta_apply_credentials(ssid, pass);
    s_state = PROV_CONNECTING;
    ESP_LOGI(TAG, "got credentials ssid=\"%s\", connecting...", ssid);

    static const char resp[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>连接中</title></head>"
        "<body style=\"font-family:sans-serif;text-align:center;padding-top:40px\">"
        "<p id=\"m\" style=\"font-size:18px\">正在连接 WiFi…</p>"
        "<script>var m=document.getElementById('m');"
        "var t=setInterval(async()=>{try{"
        "var s=(await(await fetch('/status')).json()).s;"
        "if(s=='ok'){clearInterval(t);m.textContent='连接成功！设备已联网，热点即将关闭';}"
        "else if(s=='err'){clearInterval(t);m.textContent='连接失败：请回到上一页检查密码后重试';}"
        "}catch(e){}},1000);</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, resp, sizeof(resp) - 1);
}

/**
 * @brief  极简 DNS 劫持：任何 A 查询都应答"本机地址"
 * @note   手机 captive portal 探测（captive.apple.com 等）由此被引到配网页。
 *         退出靠 s_dns_run=false + 一条唤醒包（recvfrom 无法被 close 打断）。
 */
static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns bind :%d failed", DNS_PORT);
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive dns on :%d", DNS_PORT);

    /* DNS 答案段：名字指针指回问题段(0xC00C), TYPE=A, CLASS=IN, TTL=60, RDLEN=4 */
    static const uint8_t dns_ans[16] = {
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 60, 0x00, 0x04, 0, 0, 0, 0
    };
    /* 收发缓冲用 static：lwip socket 调用栈开销大，放任务栈里 4KB 都会溢出；
     * 本任务全系统唯一实例，static 无并发问题 */
    static uint8_t rx[512];
    static uint8_t tx[512 + sizeof(dns_ans)];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    while (s_dns_run) {
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) {
            continue;   /* 不足一个 DNS 头 */
        }
        memcpy(tx, rx, n);          /* 头 + 问题段原样回 */
        tx[2] = 0x81;               /* QR=响应，保留 RD */
        tx[3] = 0x80;               /* RA=1，RCODE=0 */
        tx[6] = 0; tx[7] = 1;       /* ANCOUNT=1 */
        tx[8] = 0; tx[9] = 0; tx[10] = 0; tx[11] = 0;
        memcpy(tx + n, dns_ans, sizeof(dns_ans));
        memcpy(tx + n + 12, &s_dns_ip, 4);   /* RDATA = 本机 IP */
        sendto(sock, tx, n + sizeof(dns_ans), 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void led_blink_cb(void *arg)
{
    (void)arg;
    LED_TOGGLE();
}

/**
 * @brief  关闭门户：httpd/DNS/LED 全部回收，切回纯 STA（热点随之关闭）
 * @note   由 s_close_timer（联网成功 +3s）或启动失败路径调用；
 *         STA 已连接时切 STA-only 不掉线。
 */
static void portal_stop(void)
{
    esp_timer_stop(s_close_timer);
    esp_timer_stop(s_led_timer);
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    /* 唤醒阻塞在 recvfrom 的 DNS 任务，等它自行退出并 close 套接字 */
    if (s_dns_run) {
        s_dns_run = false;
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0) {
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(DNS_PORT),
                .sin_addr.s_addr = s_dns_ip,
            };
            sendto(sock, "x", 1, 0, (struct sockaddr *)&dst, sizeof(dst));
            close(sock);
        }
        int wait_ms = 1000;
        while (s_dns_task != NULL && wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            wait_ms -= 20;
        }
    }
    LED(1);   /* 联网成功 -> LED 常亮 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal_active = false;
    if (s_done_sem) {
        xSemaphoreGive(s_done_sem);
    }
}

static void portal_close_cb(void *arg)
{
    (void)arg;
    portal_stop();
}

/**
 * @brief  启动热点 + DNS 劫持 + httpd 配网页
 * @return ESP_OK 成功；失败时已回滚 WiFi 模式
 */
static esp_err_t portal_start(void)
{
    /* 热点名固定 "ESP配网服务"（用户指定；同场多设备时再考虑加 MAC 后缀） */
    char ap_ssid[32] = "ESP配网服务";

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;   /* 配网热点不设密码，仅按需短暂开启 */

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* 劫持应答指向的 AP 地址（默认 192.168.4.1，按实际 netif 取） */
    uint8_t ip_fallback[4] = { 192, 168, 4, 1 };
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info = { 0 };
    if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &info) == ESP_OK &&
        info.ip.addr != 0) {
        s_dns_ip = info.ip.addr;   /* 网络序 */
    } else {
        memcpy(&s_dns_ip, ip_fallback, sizeof(ip_fallback));
    }

    if (s_led_timer == NULL) {
        const esp_timer_create_args_t led_args = {
            .callback = led_blink_cb,
            .name = "prov_led",
        };
        ESP_ERROR_CHECK(esp_timer_create(&led_args, &s_led_timer));
    }
    LED_Init();   /* wifi 先于 afe 启动时 LED 可能还未初始化，重复初始化无害 */
    esp_timer_start_periodic(s_led_timer, LED_BLINK_MS * 1000);

    s_dns_run = true;
    if (xTaskCreate(dns_server_task, "prov_dns", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create dns task failed");
        esp_timer_stop(s_led_timer);
        esp_wifi_set_mode(WIFI_MODE_STA);
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size = 8192;        /* /scan 跑阻塞扫描 + JSON 组包 */
    httpd_cfg.lru_purge_enable = true;
    err = httpd_start(&s_httpd, &httpd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t uri_index  = { .uri = "/",      .method = HTTP_GET,  .handler = index_handler };
    static const httpd_uri_t uri_scan   = { .uri = "/scan",  .method = HTTP_GET,  .handler = scan_handler };
    static const httpd_uri_t uri_save   = { .uri = "/save",  .method = HTTP_POST, .handler = save_handler };
    static const httpd_uri_t uri_status = { .uri = "/status",.method = HTTP_GET,  .handler = status_handler };
    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_scan);
    httpd_register_uri_handler(s_httpd, &uri_save);
    httpd_register_uri_handler(s_httpd, &uri_status);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, err_404_redirect);

    ESP_LOGI(TAG, "provisioning portal up: ssid=\"%s\", page http://192.168.4.1", ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_provision_run(void)
{
    if (s_portal_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinary();
    }
    if (s_close_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = portal_close_cb,
            .name = "prov_close",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_close_timer));
    }

    s_prov_requested = false;   /* 门户开着时新来的请求无意义，入口处清掉 */
    s_state = PROV_WAIT;
    s_portal_active = true;

    esp_err_t err = portal_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal start failed: %s", esp_err_to_name(err));
        portal_stop();
        LED(0);
        return err;
    }

    ESP_LOGI(TAG, "waiting for user provisioning...");
    xSemaphoreTake(s_done_sem, portMAX_DELAY);   /* portal_stop 成功收尾后释放 */
    ESP_LOGI(TAG, "provisioning done, portal closed");
    return ESP_OK;
}

/* ============================ 事件钩子（wifi.c 调用） ============================ */

void wifi_provision_notify_connected(void)
{
    if (!s_portal_active || s_state != PROV_CONNECTING) {
        return;
    }
    s_state = PROV_OK;
    esp_timer_start_once(s_close_timer, CLOSE_DELAY_US);
}

void wifi_provision_notify_disconnected(int reason)
{
    if (!s_portal_active || s_state != PROV_CONNECTING) {
        return;
    }
    s_state = PROV_ERR;
    ESP_LOGW(TAG, "try-connect failed reason=%d, retry via web page", reason);
}
