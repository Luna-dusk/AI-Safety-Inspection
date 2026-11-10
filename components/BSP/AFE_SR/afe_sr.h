#ifndef _AFE_SR_H_
#define _AFE_SR_H_

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_vad.h"
#include "i2s.h"
#include "esp_err.h"
#include "key.h"
#include "LED.h"
#include "lcd_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 连续多少帧非语音才认定一句说完（句尾 hangover, ~12帧≈380ms） */
#define CONTINUE_FRAME_TIME 12
/* 会话空闲超时：5 分钟无用户说话则休眠，等待下次唤醒 */
#define SESSION_IDLE_TIMEOUT_MS  (5 * 60 * 1000)

/**
 * @brief  初始化 AFE：加载模型、创建实例，并顺带初始化 LED 与本地命令
 * @return ESP_OK 成功；ESP_FAIL 模型/实例创建失败
 */
esp_err_t afe_init(void);
/**
 * @brief  启动 feed(核1) / fetch(核0) 双任务链路
 * @param  pcm_queue 上传队列句柄，接收 AUDIO_FRAME_DATA / AUDIO_FRAME_END
 * @return ESP_OK / ESP_ERR_INVALID_STATE(未先 afe_init) / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM
 */
esp_err_t afe_sr_start(QueueHandle_t pcm_queue);
/**
 * @brief  停止 AFE 链路
 * @warning 当前为空实现，不会删除任务、不释放资源，"停止再启动"尚不成立。
 */
void afe_sr_stop(void);

/* TTS/上传期间静音聆听，防止扬声器回灌麦克风形成自激循环 */
void afe_sr_set_capture_enabled(bool enabled);

/* 是否处于会话（聆听）状态；false = 待机等唤醒词。供 main 同步 LCD 显示 */
bool afe_sr_in_session(void);

#endif /* _AFE_SR_H_ */
