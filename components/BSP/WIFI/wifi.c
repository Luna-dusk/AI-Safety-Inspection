#include "wifi.h"
#include "wifi_provision.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <string.h>
#include <fcntl.h>

static EventGroupHandle_t wifi_event_group;
const static char *TAG="wifi";

static uint8_t retry_num = 0;          /* 退避步数：第 n 次断连等约 2^n 秒，封顶 30s */
static bool s_sta_configured = false;  /* STA 是否已有凭证配置（事件回调据此决定是否重连）*/
static esp_timer_handle_t s_retry_timer = NULL;

static void net_diag_task(void *arg);   /* 首次联网诊断任务，定义在文件尾 */

/**
 * @brief  退避定时器回调：到点重连（STA 有配置且尚未连上才动手）
 */
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_sta_configured &&
        !(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT)) {
        ESP_LOGI(TAG, "backoff reconnect...");
        wifi_connect();
    }
}

/**
 * @brief  安排一次指数退避重连：1s→2s→4s→8s→16s→30s 封顶，无限重试
 * @note   不再沿用"重试 100 次后置 FAIL 位永久放弃"的旧逻辑——设备离线
 *         直到重启在产品上不可接受；事件回调里也不允许阻塞延时，
 *         统一走 esp_timer 一次性定时器。
 */
static void schedule_reconnect(void)
{
    uint32_t delay_ms = 1000UL << (retry_num > 5 ? 5 : retry_num);
    if (delay_ms > 30000) {
        delay_ms = 30000;
    }
    retry_num++;
    if (s_retry_timer == NULL) {
        return;
    }
    esp_timer_stop(s_retry_timer);   /* 上一次还没到期又断连：重置退避窗口 */
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    ESP_LOGI(TAG, "reconnect in %lu ms", (unsigned long)delay_ms);
}

/**
 * @brief  WiFi / IP 事件统一处理
 * @param  arg        未使用
 * @param  event_base 事件基（WIFI_EVENT 或 IP_EVENT）
 * @param  event_id   事件编号
 * @param  event_data 事件数据
 *
 * 三种事件：
 *   - WIFI_EVENT_STA_START        : 驱动就绪；已有凭证配置才发起连接
 *                                   （配网模式下 STA 尚无配置，不动）；
 *   - WIFI_EVENT_STA_DISCONNECTED : 清 CONNECTED 位、通知配网模块试连结果，
 *                                   并按指数退避安排重连（无限重试不放弃）；
 *   - IP_EVENT_STA_GOT_IP         : 置 CONNECTED 位、退避计数清零、
 *                                   关闭省电，并通知配网模块联网成功。
 *
 * @note esp_wifi_set_ps(WIFI_PS_NONE) 会明显增加功耗，
 *       与低功耗预算是冲突的，属于"保实时性"的有意取舍。
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (s_sta_configured) {
            ESP_LOGI(TAG, "WiFi started, connecting to AP...");
            wifi_connect();
        }
        else {
            ESP_LOGI(TAG, "WiFi started, no credentials yet");
        }

    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *wifi_event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", wifi_event->reason);
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_provision_notify_disconnected(wifi_event->reason);
        schedule_reconnect();

    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        //event->ip_info.ip为获取的ip
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_num=0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电，减少延迟 */
        wifi_provision_notify_connected();

        /* 首次联网跑一次连通性诊断（"连上网但出不去"时定位卡在哪一跳） */
        static bool diag_done = false;
        if (!diag_done) {
            diag_done = true;
            if (xTaskCreate(net_diag_task, "net_diag", 4096, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "create net_diag task failed");
            }
        }
    }
}

/**
 * @brief  创建 WiFi 事件组并启动连接任务
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复调用；
 *         ESP_ERR_NO_MEM 事件组或任务创建失败
 *
 * 先建事件组再创建任务，是为了让调用方拿到句柄后立刻就能
 * xEventGroupWaitBits，不会因为任务跑太快而错过 CONNECTED 位。
 * 失败时会把已创建的事件组回滚删除。
 */
esp_err_t esp_create_wifi_task(void)
{
    if (wifi_event_group != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Publish the event group before the caller starts waiting for an IP.
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ret = xTaskCreate(wifi_init_sta, "wifi_init_sta", 6144, NULL, 5, NULL);
    if (ret != pdPASS) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief  WiFi 主状态循环（独立任务，栈 6144）
 * @param  pvParameters 未使用
 *
 * 流程：NVS 初始化（必要时擦除重建）-> esp_netif（STA + AP）->
 *       esp_wifi_init -> RAM 存储（凭证由 wifi_prov 的 NVS 键管理）->
 *       注册事件回调 -> esp_wifi_start。之后循环按序处理：
 *         ① 无凭证或长按 BOOT 请求配网 -> 运行 SoftAP 网页门户（阻塞至联网）；
 *         ② 有凭证且未连接 -> 从 NVS 读凭证发起连接
 *            （失败由事件回调指数退避重试，1s→30s 无限重试）；
 *         ③ 已连接 -> 1s 保活轮询，配网请求随时可打断回到 ①。
 *
 * 两个关键顺序：
 *   - 事件回调必须在 esp_wifi_start() 之前注册，否则会漏掉 STA_START 事件；
 *   - esp_create_wifi_task 先建事件组再建本任务，等待方不会错过 CONNECTED 位。
 *
 * @note 凭证持久化在 wifi_provision 的 NVS 命名空间，驱动侧
 *       WIFI_STORAGE_RAM 不再隐式落盘；连接成功后任务不退出，
 *       保活轮询支撑运行中长按重配网。
 */
void wifi_init_sta(void *pvParameters)
{
    (void)pvParameters;
    // 初始化 NVS（WiFi 驱动依赖 NVS 存储校准数据；配网凭证也存这里）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();   /* 配网热点用，平时不激活 */
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    /* 凭证持久化改由 wifi_prov 自己的 NVS 键管理，驱动不再隐式落盘 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* 必须显式进 STA 模式：默认 NULL 模式下 esp_wifi_start 不拉起任何接口，
     * 有凭证直连路径的 esp_wifi_connect 会失败且没有 STA_START/重试事件，
     * 设备会永远静默离线（2026-11-08 真机踩坑） */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Register before starting Wi-Fi so WIFI_EVENT_STA_START cannot be missed.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL));

    const esp_timer_create_args_t retry_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_start());
    ret = esp_wifi_set_country_code("CN", true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set country failed: %s", esp_err_to_name(ret));
    }

    char ssid[WIFI_PROV_SSID_MAX + 1] = {0};
    char pass[WIFI_PROV_PASS_MAX + 1] = {0};

    while (1)
    {
        esp_err_t creds = wifi_prov_read(ssid, sizeof(ssid), pass, sizeof(pass));

        /* ① 无凭证 / 长按 BOOT 请求配网：跑门户直到联网成功才返回 */
        if (wifi_provision_take_request() || creds != ESP_OK)
        {
            if (creds != ESP_OK) {
                ESP_LOGI(TAG, "no credentials in NVS, entering provisioning mode");
            }
            wifi_provision_run();
            continue;
        }

        /* ② 有凭证且未连接：发起连接（配网成功后已连上则跳过，避免无谓重连） */
        if (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT))
        {
            ESP_LOGI(TAG, "Connecting to AP: %s", ssid);
            wifi_sta_apply_credentials(ssid, pass);
        }

        /* ③ 等待联网 / 配网请求（1s 轮询；退避重试由事件回调驱动） */
        while (!(xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) &&
               !wifi_provision_requested())
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (wifi_provision_requested()) {
            continue;
        }

        /* ④ 已连接：保活轮询，长按 BOOT 随时打断进入配网 */
        ESP_LOGI(TAG, "Connected to AP: %s", ssid);
        while (!wifi_provision_requested())
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}

/**
 * @brief  用给定凭证配置 STA 并发起连接
 * @param  ssid 以 0 结尾；pass 可为空串（开放网络）
 * @return ESP_OK / 配置失败错误码
 *
 * 若 STA 当前已连接（如配网门户改密重试场景），先断开再连，
 * 保证拿到新的 GOT_IP 事件——门户收尾依赖它触发。
 */
esp_err_t wifi_sta_apply_credentials(const char *ssid, const char *pass)
{
    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass ? pass : "", sizeof(wifi_config.sta.password));
    /* 有密码按 WPA/WPA2 起（WPA3 由 sdkconfig 自动协商），无密码即开放网络 */
    wifi_config.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    s_sta_configured = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set sta config failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
        esp_wifi_disconnect();   /* 触发新一轮 DISCONNECTED -> 连接 */
    }
    wifi_connect();
    return ESP_OK;
}

/* ============================ 联网连通性诊断 ============================ */

#define DIAG_SERVER_IP ""   /* 与 AI_CHAT/my_client.h 的 SERVER_BASE 保持一致 */

/**
 * @brief  非阻塞 TCP 探测：区分"秒拒(路由通/端口关)"与"超时(包被丢弃)"
 *
 * 网关(手机)秒拒 + 服务器超时  -> 设备到手机是通的，手机没转发客户端流量
 * 两个都超时                   -> 设备到手机链路本身不通（子网冲突/ARP 等）
 */
static void diag_tcp_probe(const char *label, const char *ip_str, uint16_t port)
{
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_aton(ip_str, &dst.sin_addr);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "[diag] %s socket 创建失败", label);
        return;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&dst, sizeof(dst));
    if (r == 0) {
        ESP_LOGI(TAG, "[diag] %s(%s:%u) 连接成功", label, ip_str, (unsigned)port);
    } else {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
        if (select(s + 1, NULL, &wset, NULL, &tv) > 0) {
            int so_err = 0;
            socklen_t elen = sizeof(so_err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &elen);
            if (so_err == 0) {
                ESP_LOGI(TAG, "[diag] %s(%s:%u) 连接成功", label, ip_str, (unsigned)port);
            } else {
                ESP_LOGW(TAG, "[diag] %s(%s:%u) 拒绝/不可达 errno=%d（路由通，端口没开或被拒）",
                         label, ip_str, (unsigned)port, so_err);
            }
        } else {
            ESP_LOGW(TAG, "[diag] %s(%s:%u) 4s 超时（包被静默丢弃）", label, ip_str, (unsigned)port);
        }
    }
    close(s);
}

/**
 * @brief  首次联网后自动跑一次：打印 ip/掩码/网关，探测网关与 AI 服务器
 */
static void net_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));   /* 等 DHCP 路由稳定 */

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = { 0 };
    if (sta != NULL && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        ESP_LOGI(TAG, "[diag] ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
        char gw_str[16];
        snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&info.gw));
        diag_tcp_probe("网关(手机)", gw_str, 80);
    } else {
        ESP_LOGW(TAG, "[diag] 获取 STA 网络信息失败");
    }
    diag_tcp_probe("AI服务器", DIAG_SERVER_IP, 80);
    ESP_LOGI(TAG, "[diag] 诊断完成");
    vTaskDelete(NULL);
}

/**
 * @brief  发起一次 esp_wifi_connect()，失败只打日志不重试
 *
 * 重试节奏由 DISCONNECTED 事件里的退避定时器驱动，本函数保持无状态。
 */
void wifi_connect()
{
    esp_err_t ret=esp_wifi_connect();
    if(ret==ESP_OK)
    {
        ESP_LOGI(TAG,"wifi connect...");
    }
    else
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s (0x%x)", esp_err_to_name(ret), (unsigned int)ret);
    }
}

/**
 * @brief  取得 WiFi 事件组句柄，供其它模块等待联网就绪
 * @return 事件组句柄；若 esp_create_wifi_task() 尚未调用则返回 NULL
 *
 * 上层典型用法：
 *   xEventGroupWaitBits(wifi_get_event_group(), WIFI_CONNECTED_BIT, ...)
 */
EventGroupHandle_t wifi_get_event_group(void)
{
    return wifi_event_group;
}
