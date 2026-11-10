#ifndef __LCD_UI_H__
#define __LCD_UI_H__

/**
 * @file  lcd_ui.h
 * @brief 屏幕 UI 接口（占位实现，不驱动任何硬件）
 *
 * 本工程的 LCD 模块尚未移植。这里的两个枚举与源工程
 * ai_client/components/BSP/LCD/lcd_ui.h 完全一致，函数是占位实现，
 * 只打印日志，方便上层代码（AFE / MultiNet / main）先编译通过。
 *
 * 移植真实现时：
 *   1) 拷入 LCD/ 全部源码：lcd.c lcd.h lcd_ui.c lcd_ui.h face_data.h lv_font_cn_18.c
 *   2) main/idf_component.yml 加一行 `lvgl/lvgl: ~8.3.0`
 *   3) components/BSP/CMakeLists.txt 的 requires 里解开 esp_lcd / lvgl__lvgl
 *   4) 把 lcd_ui_color_trans_done() 及其 esp_lcd 头文件一起加回来
 */

#include <stdbool.h>

/** 屏幕表情 */
typedef enum {
    LCD_EMO_SMILE  = 0,
    LCD_EMO_CRY    = 1,
    LCD_EMO_NORMAL = 2,
    LCD_EMO_ANGRY  = 3,
    LCD_EMO_DROOL  = 4,
} lcd_emotion_t;

/** 屏幕状态栏文案 */
typedef enum {
    LCD_STATUS_STANDBY    = 0,   /* 待机中 */
    LCD_STATUS_CONNECTING = 1,   /* 联网中 */
    LCD_STATUS_LISTENING  = 2,   /* 聆听中 */
    LCD_STATUS_REPLYING   = 3,   /* 回复中 */
} lcd_status_t;

/** @brief 初始化 LCD（占位：只打日志） */
void lcd_ui_init(void);

/** @brief UI 刷新任务（占位：空转等待） */
void lcd_ui_task(void *arg);

/* 线程安全：任意任务调用，下一帧生效 */
/** @brief 设置屏幕状态 */
void lcd_ui_set_status(lcd_status_t st);

/** @brief 设置屏幕表情 */
void lcd_ui_set_emotion(lcd_emotion_t emo);

#endif /* __LCD_UI_H__ */
