#ifndef TRANSPORT_H
#define TRANSPORT_H

/*
 * Transport：HTTP chunked 传输层（设备 ↔ 服务器唯一网络 IO 层，附录 A.3）。
 *
 * 三个已核实的 IDF 5.4 源码事实（README 4.1，别再凭直觉写）：
 *   esp_http_client_write()         裸写，不做 chunk 分帧（esp_http_client.c:1664）
 *   esp_http_client_open(client,-1) 只是把请求头换成 Transfer-Encoding: chunked（:1518）
 *   esp_http_client_fetch_headers() 只读响应，绝不发终止块（:1410）
 * → 上行 chunk 的封帧（send_chunk）与终止块（"0\r\n\r\n"）必须手工做。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include "ai_frames.h"

typedef struct {
    uint32_t first_frame_ms;   /* open → 收到第一个下行字节的毫秒数（A.4.5 设备段打点） */
    int status;                /* 0=未开始，1=进行中，2=结束 */
    size_t bytes_rx;
    size_t bytes_tx;
} transport_state_t;

/**
 * @brief  发起一轮会话：建连 + 发请求头（chunked POST /api/v1/chat）
 * @param  session_id 本轮会话 UUID（每次唤醒换新，A.3.1）
 * @param  turn_index 会话内第几轮，从 1 开始
 * @param  greeting   true = 开机问候轮（X-Greeting: 1，上行 body 为空）
 * @return ESP_OK 连接已建立，随后可 tx_write；ESP_FAIL 建连/发送失败
 * @note   成功后必须以 tx_read_loop()（或失败路径的 tx_end()）收尾，否则连接悬挂。
 */
esp_err_t session_start(const char *session_id, int turn_index, bool greeting);

/**
 * @brief  写一包上行 PCM（手工封成 chunk；512 B/包由 CHAT 层拼好，A.3.1）
 * @return ESP_OK；ESP_FAIL 写超时/断连；ESP_ERR_INVALID_STATE 未先 session_start
 */
esp_err_t tx_write(const void *data, size_t len_bytes);

/**
 * @brief  下行循环：发上行终止块 → 等响应头校验 200 → 循环 read 喂解析器
 * @return ESP_OK 流自然结束；ESP_FAIL 传输/帧流错误；ESP_ERR_TIMEOUT 被 tx_abort 中断
 * @note   必须在 ai_chat 任务上下文调用：回调里会做 I2S 播放，
 *         绝不能放进 HTTP 事件回调（附录 A.9 硬约束 1）。
 */
esp_err_t tx_read_loop(ai_frames_t *frames, void *ctx, transport_state_t *state);

/** @brief 正常收尾：关闭本轮连接（client 句柄保留给下一轮复用）。幂等。 */
void tx_end(void);

/** @brief 异步请求中断：置标志，tx_read_loop/tx_write 在检查点自行退出，连接由 tx_end 收 */
void tx_abort(void);

/**
 * @brief  删除服务器侧会话（A.3.3：5 分钟空闲后让服务器停止计费，A.4.4 坑 4）
 * @return ESP_OK（含 4xx，会话不存在也算删干净）；其它为网络失败
 * @note   一次性独立连接，失败不重试（顶多服务器多留一个空会话）。
 */
esp_err_t session_delete(const char *session_id);

#endif /* TRANSPORT_H */
