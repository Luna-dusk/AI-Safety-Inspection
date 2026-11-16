#ifndef __LCD_UI_H__
#define __LCD_UI_H__

/**
 * @file  lcd_ui.h
 * @brief 屏幕 UI 接口（业务层唯一可见的头文件）
 *
 * 分层（见 LCD/README.md 第 0 节）：业务层（AFE_SR / CHAT / MultiNet）
 * 只调这两个 set 函数，一个 #include "lvgl.h" 都没有 —— 业务层不需要
 * 知道 LVGL 存在。改界面不动业务代码，改业务不动界面。
 *
 * 实现：lcd_ui.c（esp_lcd + LVGL 8.3 移植层）+ screen_home.c（主屏）
 *
 * 线程安全：任意任务、任意时刻调用，下一帧生效（影子变量 + 脏标志，
 * 调用方零负担、不阻塞）。**绝不要**在业务任务里直接调 lv_xxx()，
 * LVGL 不是线程安全的。
 *
 * 扩展规则（README.md 第 8 节，防止返工）：
 *   ✅ 加新枚举值、加新函数（如 lcd_ui_set_battery）
 *   ❌ 改已有 4 个函数的签名、改已有枚举值的名字 —— 9 个调用点已在
 *      afe_sr.c / chat.c 里编译着了
 */

#include <stdbool.h>

/** 屏幕表情（当前用于主屏守护环/对勾的配色，动画表情是二期） */
typedef enum {
    LCD_EMO_SMILE  = 0,
    LCD_EMO_CRY    = 1,
    LCD_EMO_NORMAL = 2,
    LCD_EMO_ANGRY  = 3,
    LCD_EMO_DROOL  = 4,
} lcd_emotion_t;

/** 屏幕状态栏文案 */
typedef enum {
    LCD_STATUS_STANDBY    = 0,   /* 待机中 → 界面显示"监护中" */
    LCD_STATUS_CONNECTING = 1,   /* 联网中 */
    LCD_STATUS_LISTENING  = 2,   /* 聆听中 */
    LCD_STATUS_REPLYING   = 3,   /* 回复中 */
} lcd_status_t;

/**
 * @brief 初始化 LCD：SPI 总线 → esp_lcd ST7789 → LVGL → 主屏
 * @note  须在 app_main 里、建 lcd_ui_task 之前调用；
 *        失败只打日志并返回（后续 set 调用只写影子变量，不会崩）
 */
void lcd_ui_init(void);

/**
 * @brief UI 刷新任务：消费影子变量 → lv_timer_handler → 让出 CPU
 * @param arg 未使用
 * @note  栈 8192、优先级 8（低于 afe_feed/afe_fetch/ai_chat，
 *        音频掉一帧是爆音，UI 掉一帧没人察觉）
 */
void lcd_ui_task(void *arg);

/* 线程安全：任意任务调用，下一帧生效 */
/** @brief 设置屏幕状态 */
void lcd_ui_set_status(lcd_status_t st);

/** @brief 设置屏幕表情 */
void lcd_ui_set_emotion(lcd_emotion_t emo);

/* ── 对话界面（2026-11-14 新增，按"只加不改"的扩展规则）────────────
 *
 * 数据来源：服务器 {"t":"asr","text":...} / {"t":"reply","text":...}，
 * 由 chat.c 通过 ai_chat_set_event_cb 回调抛出来（见 main.c 的接线）。
 * 这三个函数同样**任意任务可调**：内部只入队/置标志，真正的 lv_* 调用
 * 只发生在 lcd_ui 任务里（LVGL 非线程安全）。 */
/** 对话气泡的发送方 */
typedef enum {
    LCD_CHAT_USER = 0,   /* 用户说的（右对齐绿气泡） */
    LCD_CHAT_AI   = 1,   /* AI 回复（左对齐深色气泡） */
} lcd_chat_role_t;

/** @brief 追加一条对话文本（线程安全；超长按 UTF-8 边界截断） */
void lcd_ui_chat_add(lcd_chat_role_t role, const char *text);

/** @brief 切到对话屏（线程安全） */
void lcd_ui_chat_show(void);

/** @brief 清空对话历史（线程安全） */
void lcd_ui_chat_clear(void);

#endif /* __LCD_UI_H__ */