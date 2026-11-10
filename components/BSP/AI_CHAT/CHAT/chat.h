#ifndef CHAT_H
#define CHAT_H

/*
 * CHAT：AI_CHAT 编排层（附录 A.2/A.3 设备侧状态机）。
 *
 * 数据流：
 *   上行  pcm_queue(DATA/END 帧) → 480KB PSRAM 环形缓冲 → 512B/包 tx_write
 *         （先掏空队列再写网络：tx_write 阻塞期间队列是唯一缓冲，顺序反了丢音更多）
 *   下行  tx_read_loop → ai_frames 回调 → 残帧拼接 → i2s_spk_write → I2S0
 *   收尾  6 块静音 → afe_sr_set_capture_enabled(true)
 *         收尾代码唯一一份、所有路径必经——漏了设备永久失聪（README 5.2 生死规则）
 *
 * 优先级仲裁（A.2 告警 > 对讲 > AI 对话）：对外只交出 ai_chat_abort()，
 * 本期没有调用方，留给 ESP_NOW/告警模块。
 */

#include <stdbool.h>

/** 透传给 UI/上层的事件（LCD 还是 stub，先留挂点） */
typedef enum {
    AI_CHAT_EVT_ASR,      /* text = 识别出的用户话（只上屏，不发声，A.3.2） */
    AI_CHAT_EVT_REPLY,    /* text = AI 回复文本 */
    AI_CHAT_EVT_ACTION,   /* FR-8 二次确认：本期只透传，不执行 */
    AI_CHAT_EVT_ERR,      /* text = err.msg */
    AI_CHAT_EVT_DONE,
} ai_chat_event_t;

typedef void (*ai_chat_event_cb_t)(ai_chat_event_t ev, const char *text, void *ctx);

/** 注册事件回调（可为 NULL 取消）。任意任务、任意时刻可调。 */
void ai_chat_set_event_cb(ai_chat_event_cb_t cb, void *ctx);

/**
 * @brief  AI_CHAT 主任务。arg = pcm_queue 句柄（main.c 创建后传入）。
 * @note   栈必须 8192（附录 A.9 #5）；开机自动做一次问候轮（FR-9）。
 */
void ai_chat_task(void *arg);

/**
 * @brief  请求播放本地预录 PCM（兜底话术 / MultiNet 本地回复共用通道）
 * @param  name storage 分区下的文件名，如 PROMPT_NET_ERR（"net_err.pcm"）
 * @note   任意任务可调；真正播放发生在 ai_chat 任务里
 *         （I2S 阻塞写绝不能在 afe_fetch 任务里做）。文件缺失退化为短提示音。
 */
void ai_chat_request_prompt(const char *name);

/**
 * @brief  立即中断当前对话（断流 + 停播 + 收尾恢复采集），收尾不播兜底话术
 * @note   供优先级仲裁器调用（A.2：告警 > 对讲 > AI 对话）。本期暂无调用方。
 */
void ai_chat_abort(void);

#endif /* CHAT_H */
