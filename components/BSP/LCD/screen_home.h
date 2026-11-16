#ifndef __SCREEN_HOME_H__
#define __SCREEN_HOME_H__

/**
 * @file  screen_home.h
 * @brief 主屏 · 守护（ui/DESIGN.md:105 的第 1 屏，FR-2 / FR-10）
 *
 * 版面一比一对应 ui/prototype/index.html 的 <g id="s-home">（:166 起）
 * 与截图 ui/shots/home.png：
 *
 *   ┌──────────────────────────────┐ 0
 *   │ 08:24            ▂▄▆  82%    │ 状态栏（22px）
 *   ├──────────────────────────────┤
 *   │ ╭──────────────────────────╮ │ 32
 *   │ │  ◎✓   监护中              │ │ 守护状态大卡（双环：外绿=监护、内青=对端在线）
 *   │ │       已静默 12 分钟       │ │ 224×104
 *   │ │       ▭82% ▂▄▆ 信号良好   │ │
 *   │ ╰──────────────────────────╯ │
 *   │ ╭────────╮ ╭────────╮        │ 142
 *   │ │ 🔔 事件 │ │ 💬 AI对话│      │ 108×60 ×2
 *   │ ╰────────╯ ╰────────╯        │
 *   │ ╭──────────────────────────╮ │ 208
 *   │ │ ⚠ 紧急呼救          ›     │ │ 红卡 224×56（全屏唯一红）
 *   │ ╰──────────────────────────╯ │
 *   ├──────────────────────────────┤ 272
 *   │  守护    对讲    应用   设置  │ 底部导航（48px）
 *   └──────────────────────────────┘ 320
 *
 * 本文件只做「画」和「改」。状态从哪来、点击跳到哪去，都不在这里决定
 * （DESIGN.md:124「界面只读状态，不写业务」）。
 */

#include "lvgl.h"
#include "lcd_ui.h"

/**
 * @brief 建主屏并加载（内部 lv_scr_load，调用即上屏）
 * @note  必须在 lcd_ui_init() 之后、lcd_ui 任务上下文里调用
 */
void screen_home_create(void);

/** @brief 取根屏对象（导航层将来做返回栈时用） */
lv_obj_t *screen_home_get(void);

/**
 * @brief 读回状态栏时钟"控件里真实保存的文本"（诊断用）
 * @return 标签文本；屏还没建时返回 "--:--"
 * @note  和 screen_home_set_clock() 传入的值对比，可区分
 *        "set 没生效" vs "时间源没就绪"
 */
const char *screen_home_get_clock(void);

/** @brief 切回主屏（对话屏的返回键用；本期只有两个屏，不建返回栈） */
void screen_home_show(void);

/**
 * @brief 状态 → 控件（lcd_ui 任务每轮消费影子变量后调）
 * @param st 待机 / 联网 / 聆听 / 回复
 */
void screen_home_set_status(lcd_status_t st);

/**
 * @brief 只记录表情，不改任何颜色（LVGL 侧不做表情动画，DESIGN.md 第 2 期）
 *
 * 守护徽记的两环分别绑"监护状态"和"对端在线"，与表情无关。
 */
void screen_home_set_emotion(lcd_emotion_t emo);

/**
 * @brief 对端是否在线 → 守护徽记内环（青 / 灰）
 * @param online true 内环点亮 COL_CYAN，false 转 COL_DIM 灰
 * @note  这是双环里"内环"的唯一语义来源。当前固件还没有对端在线状态的
 *        产出方（ESP-NOW 链路状态未上报到 UI 层），所以暂时无人调用，
 *        内环按创建时的青色显示。等 BSP/Middlewares 侧有了链路状态，
 *        在 lcd_ui 任务里加一行 screen_home_set_peer_online(...) 即可，
 *        不需要动布局。**在此之前它不代表真实链路状态**，别在评审里
 *        当成"对端确实在线"的证据。
 */
void screen_home_set_peer_online(bool online);

/**
 * @brief 更新"已静默 N 分钟"（仅待机状态下显示，其它状态被活动文案覆盖）
 */
void screen_home_set_silence_minutes(int minutes);

/**
 * @brief 更新电量（状态栏与守护卡内两处图标行同时刷新）
 * @param pct 0..100；<20 内芯转琥珀
 */
void screen_home_set_battery(int pct);

/**
 * @brief 更新状态栏时间文本，格式 "HH:MM"
 * @note  当前无 RTC/SNTP，默认显示 "--:--"，接入时间源后调本函数
 */
void screen_home_set_clock(const char *hhmm);

#endif /* __SCREEN_HOME_H__ */