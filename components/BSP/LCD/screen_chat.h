#ifndef __SCREEN_CHAT_H__
#define __SCREEN_CHAT_H__

/**
 * @file  screen_chat.h
 * @brief 对话屏（类微信气泡列表）—— AI 回复文字上屏
 *
 * 数据来源与流向：
 *
 *   server ──{"t":"asr","text":"..."} ──┐
 *          └─{"t":"reply","text":"..."} ─┤  chat.c: handle_json()
 *                                          ↓ s_event_cb(AI_CHAT_EVT_ASR/REPLY)
 *   main.c: on_chat_event()                ↓
 *   lcd_ui_chat_add(role, text)  ── 任意任务，只入队（不碰 LVGL）
 *                                          ↓ FreeRTOS 队列（lcd_ui.c）
 *   lcd_ui_task: screen_chat_add()  ── 真正的 lv_* 调用只在这里
 *
 * 版面（240×320，DESIGN.md 的色板/圆角/字号）：
 *
 *   ┌──────────────────────────────┐ 0
 *   │ ‹   AI 对话                  │ 头部 48
 *   ├──────────────────────────────┤
 *   │ ╭────────────╮               │ 52
 *   │ │ 今天天气怎么样？      │    │   用户：右对齐，绿底深字
 *   │ ╰────────────╯               │
 *   │      ╭─────────────────╮     │
 *   │      │ 今天晴，22 度，…│     │   AI：左对齐，深底浅字
 *   │      ╰─────────────────╯     │
 *   ├──────────────────────────────┤ 312
 *   │ 长按唤醒"你好小鑫"说话        │ 提示条 8
 *   └──────────────────────────────┘ 320
 *
 * 刻意不做的事：输入框（本机没有键盘，输入通道是语音）、头像、表情、已读回执。
 */

#include "lvgl.h"
#include "lcd_ui.h"

/** 单条消息最多保留的 UTF-8 字节（中文 3 字节 → 约 60 字，够一轮回复） */
#define SCREEN_CHAT_TEXT_MAX  200
/** 最多保留的气泡数（超出删最旧，防止 LVGL 内存无界增长） */
#define SCREEN_CHAT_MAX_BUBBLES  24
/** 气泡最大宽度（240 - 左右安全边距 16 - 气泡内边距） */
#define SCREEN_CHAT_BUBBLE_MAX_W  176

/** 建屏（只建一次；重复调用无副作用） */
void screen_chat_create(void);

/** 切到对话屏（内部 lv_scr_load，必须在 lcd_ui 任务上下文调用） */
void screen_chat_show(void);

/** 当前是否显示对话屏 */
bool screen_chat_is_active(void);

/** 追加一条消息并滚到底（必须在 lcd_ui 任务上下文调用） */
void screen_chat_add(lcd_chat_role_t role, const char *text);

/** 清空全部消息（保留"暂无消息"占位） */
void screen_chat_clear(void);

#endif /* __SCREEN_CHAT_H__ */