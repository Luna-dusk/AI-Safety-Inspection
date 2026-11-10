#include "lcd_ui.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD_UI";

static lcd_status_t  s_status  = LCD_STATUS_STANDBY;
static lcd_emotion_t s_emotion = LCD_EMO_NORMAL;

static const char *status_name(lcd_status_t st)
{
    switch (st) {
    case LCD_STATUS_STANDBY:    return "STANDBY";
    case LCD_STATUS_CONNECTING: return "CONNECTING";
    case LCD_STATUS_LISTENING:  return "LISTENING";
    case LCD_STATUS_REPLYING:   return "REPLYING";
    default:                    return "?";
    }
}

static const char *emotion_name(lcd_emotion_t emo)
{
    switch (emo) {
    case LCD_EMO_SMILE:  return "SMILE";
    case LCD_EMO_CRY:    return "CRY";
    case LCD_EMO_NORMAL: return "NORMAL";
    case LCD_EMO_ANGRY:  return "ANGRY";
    case LCD_EMO_DROOL:  return "DROOL";
    default:             return "?";
    }
}

/**
 * @brief  初始化 LCD UI
 *
 * 占位实现：不申请总线、不建 panel，只把"没屏幕"这件事明确喊出来，
 * 免得运行时以为屏幕坏了。
 */
void lcd_ui_init(void)
{
    ESP_LOGW(TAG, "LCD UI stub: no panel driver, no LVGL; UI calls are no-op");
}

/**
 * @brief  UI 刷新任务（占位）
 * @param  arg 未使用
 *
 * 只为满足可能的 xTaskCreate 调用点。真实现里这里会跑 LVGL 的
 * lv_timer_handler() 循环。
 */
void lcd_ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief  设置屏幕状态
 * @param  st 目标状态；与当前相同则不重复打印
 */
void lcd_ui_set_status(lcd_status_t st)
{
    if (st != s_status) {
        s_status = st;
        ESP_LOGI(TAG, "status -> %s", status_name(st));
    }
}

/**
 * @brief  设置屏幕表情
 * @param  emo 目标表情；与当前相同则不重复打印
 */
void lcd_ui_set_emotion(lcd_emotion_t emo)
{
    if (emo != s_emotion) {
        s_emotion = emo;
        ESP_LOGI(TAG, "emotion -> %s", emotion_name(emo));
    }
}
