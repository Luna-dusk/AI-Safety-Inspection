#include "transport.h"

#include <stdio.h>
#include "esp_http_client.h"
#include "my_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TRANSPORT";

static esp_http_client_handle_t tx_c = NULL;
static volatile bool s_abort;         /* tx_abort() 置位；session_start() 清零 */
static bool s_in_flight;              /* 连接已 open：配对 close，防重复派发断连事件 */
static size_t s_bytes_tx;             /* 本轮已上行字节数（session_start 清零） */
static TickType_t s_open_tick;        /* open 时刻：首帧超时的计时起点 */

static const esp_http_client_config_t tx_config = {
    .url               = SERVER_BASE SERVER_PATH_CHAT,
    .method            = HTTP_METHOD_POST,
    .timeout_ms        = AI_HTTP_TIMEOUT_MS,
    .keep_alive_enable = true,
};

/**
 * @brief  手工 chunk 封帧：写 "<hex长度>\r\n" + 数据 + "\r\n"
 *
 * IDF 不会代劳（esp_http_client_write 是裸写，esp_http_client.c:1664），
 * 不封帧服务器会把 PCM 当 chunk 长度行解析，第一包就乱。
 */
static esp_err_t send_chunk(esp_http_client_handle_t c, const void *data, size_t len)
{
    char head[16];
    int hlen = snprintf(head, sizeof(head), "%zx\r\n", len);
    if (hlen <= 0 || esp_http_client_write(c, head, hlen) != hlen) return ESP_FAIL;
    if (len > 0) {
        const char *p = data;
        while (len > 0) {
            int w = esp_http_client_write(c, p, len);
            if (w <= 0) return ESP_FAIL;      /* 写不动：TCP 背压/断连，交上层 */
            p += w;  len -= w;                /* 阻塞在这里 = 免费流控（A.9 硬约束 3） */
        }
    }
    if (esp_http_client_write(c, "\r\n", 2) != 2) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t session_start(const char *session_id, int turn_index, bool greeting)
{
    if (tx_c == NULL) {
        tx_c = esp_http_client_init(&tx_config);   /* 句柄跨轮复用，进程内只建一次 */
        if (tx_c == NULL) {
            ESP_LOGE(TAG, "esp_http_client_init failed");
            return ESP_FAIL;
        }
    }

    s_abort = false;
    s_bytes_tx = 0;
    s_open_tick = xTaskGetTickCount();

    esp_http_client_set_header(tx_c, "Content-Type", "audio/L16;rate=16000;channels=1");
    esp_http_client_set_header(tx_c, "X-Device-Id", DEVICE_ID);          /* A.3.1 要求 */
    if (DEVICE_TOKEN_DEFAULT[0])                                         /* 空 token 不发 */
        esp_http_client_set_header(tx_c, "X-Device-Token", DEVICE_TOKEN_DEFAULT);
    esp_http_client_set_header(tx_c, "X-Session-Id", session_id);
    esp_http_client_set_header(tx_c, "X-Greeting", greeting ? "1" : "0");
    static char turn_buf[8];   /* set_header 内部会 strdup 拷贝（http_header.c:111），
                                  static 只是让读代码的人不误会生命周期 */
    snprintf(turn_buf, sizeof(turn_buf), "%d", turn_index);
    esp_http_client_set_header(tx_c, "X-Turn-Index", turn_buf);          /* 每轮覆盖 */

    /* write_len = -1 → 请求头带 Transfer-Encoding: chunked（esp_http_client.c:1518） */
    esp_err_t err = esp_http_client_open(tx_c, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_close(tx_c);
        s_in_flight = false;
        return ESP_FAIL;
    }
    s_in_flight = true;
    return ESP_OK;
}

esp_err_t tx_write(const void *data, size_t len_bytes)
{
    if (tx_c == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
    if (s_abort) return ESP_FAIL;             /* 中断请求已发出，不再写 */
    if (len_bytes == 0) return ESP_OK;
    if (send_chunk(tx_c, data, len_bytes) != ESP_OK) return ESP_FAIL;
    s_bytes_tx += len_bytes;
    return ESP_OK;
}

esp_err_t tx_read_loop(ai_frames_t *frames, void *ctx, transport_state_t *state)
{
    if (tx_c == NULL || !s_in_flight) return ESP_ERR_INVALID_STATE;
    (void)ctx;                                 /* ctx 已随 ai_frames_init 存进解析器 */

    /* 1. 上行终止块：告诉服务器"我说完了"（fetch_headers 只读不写，:1410）。
          问候轮 body 为空：这一步就是全部上行。 */
    if (esp_http_client_write(tx_c, "0\r\n\r\n", 5) != 5) {
        ESP_LOGE(TAG, "send last-chunk failed");
        return ESP_FAIL;
    }

    /* 2. 等响应头。服务器收完上行才开始处理，这里阻塞是正常的；
          timeout_ms=AI_HTTP_TIMEOUT_MS 兜底，不会永久卡死 */
    int64_t hdr = esp_http_client_fetch_headers(tx_c);
    if (hdr < 0) {
        ESP_LOGE(TAG, "fetch_headers failed (timeout or reset)");
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(tx_c);
    if (status != 200) {
        ESP_LOGE(TAG, "server status %d", status);
        return ESP_FAIL;
    }
    if (state) {
        state->status = 1;
        state->bytes_tx = s_bytes_tx;
    }

    /* 3. 拉循环：read 主动拉（绝不用事件回调收数据，A.9 硬约束 1），喂解析器 */
    static uint8_t rbuf[AI_READ_BUF_SIZE];     /* static：不吃 8KB 任务栈 */
    TickType_t t0 = s_open_tick;
    for (;;) {
        if (s_abort) return ESP_ERR_TIMEOUT;   /* 外部要求中断（用户按键/仲裁抢占） */

        /* A.3.4 首帧兜底双保险：服务器先回 200 头再装死时，靠这里跳出 */
        if (state && state->first_frame_ms == 0 &&
            (xTaskGetTickCount() - t0) > pdMS_TO_TICKS(AI_FIRST_FRAME_TIMEOUT_MS + 2000)) {
            ESP_LOGE(TAG, "no first frame within %d ms", AI_FIRST_FRAME_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        int n = esp_http_client_read(tx_c, (char *)rbuf, sizeof(rbuf));
        if (n < 0) {
            ESP_LOGE(TAG, "read error");
            return ESP_FAIL;
        }
        if (n == 0) break;                     /* 终止块已收 / 对端关闭：流自然结束 */

        if (state) {
            state->bytes_rx += (size_t)n;
            if (state->first_frame_ms == 0)    /* 首帧打点：验收 A.7/A.4.5 要这个数 */
                state->first_frame_ms =
                    (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        }
        if (ai_frames_feed(frames, rbuf, (size_t)n) < 0) {
            ESP_LOGE(TAG, "frame stream corrupted");
            return ESP_FAIL;                   /* 帧边界已乱，唯一处理是断连 */
        }
    }
    return ESP_OK;
}

void tx_end(void)
{
    if (tx_c != NULL && s_in_flight) {
        esp_http_client_close(tx_c);           /* 只断连接；client 保留给下一轮复用 */
    }
    s_in_flight = false;
}

void tx_abort(void)
{
    s_abort = true;    /* 读/写循环在检查点自行退出；连接由 tx_end() 统一收 */
}

esp_err_t session_delete(const char *session_id)
{
    char url[80];
    snprintf(url, sizeof(url), "%s%s/%s", SERVER_BASE, SERVER_PATH_CHAT, session_id);
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_DELETE,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_http_client_set_header(c, "X-Device-Id", DEVICE_ID);
    esp_err_t err = esp_http_client_perform(c);    /* 一次性：连、发、收、断 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "session delete failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "session %s deleted (http %d)",
                 session_id, esp_http_client_get_status_code(c));
    }
    esp_http_client_cleanup(c);                    /* 一次性客户端必须销毁，防泄漏 */
    return err;
}
