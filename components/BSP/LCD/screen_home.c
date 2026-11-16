#include "screen_home.h"

#include "screen_chat.h"
#include "ui_nav_icons.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SCR_HOME";

/* ── 控件句柄：只有本文件用，外部一律走 set_xxx() ─────────────────── */
static lv_obj_t *s_scr;         /* 根屏 */
static lv_obj_t *s_ring_guard;  /* 守护徽记：外环（监护中） */
static lv_obj_t *s_ring_link;   /* 守护徽记：内环（对端在线） */
static lv_obj_t *s_ico_check;   /* 环内对勾 */
static lv_obj_t *s_dot_state;   /* 标题左侧状态点 */
static lv_obj_t *s_lbl_title;   /* 监护中 / 联网中 / 聆听中… / 回复中… */
static lv_obj_t *s_lbl_sub;     /* 已静默 N 分钟 / 正在连接网络 / … */
static lv_obj_t *s_lbl_clock;   /* 状态栏时间 */
static lv_obj_t *s_lbl_pct_top; /* 状态栏电量百分比 */
static lv_obj_t *s_batt_top;    /* 状态栏电量图标 */
static lv_obj_t *s_batt_card;   /* 守护卡内电量图标 */
static lv_obj_t *s_lbl_pct_card;/* 守护卡内电量百分比 */

/* ── 业务状态快照（界面侧只读这份，等下一轮 set_xxx 覆盖）───────── */
static lcd_status_t  s_status  = LCD_STATUS_STANDBY;
static lcd_emotion_t s_emotion = LCD_EMO_NORMAL;
static int           s_silence_min = 12;   /* 与原型 home.png 一致，接真实静默计时后覆盖 */
static int           s_battery_pct = 82;   /* 原型占位值，接 PWR 电量后覆盖 */
static char          s_clock[8] = "--:--"; /* 无 RTC/SNTP：不显示假时间，接入后 set_clock() */

/* ── 颜色映射：状态点跟状态，进度环跟表情 ────────────────────────── */
static lv_color_t status_color(lcd_status_t st)
{
    switch (st) {
    case LCD_STATUS_CONNECTING:   /* "进行中"用琥珀，DESIGN.md:64 */
    case LCD_STATUS_REPLYING:
        return COL_AMBER;
    case LCD_STATUS_LISTENING:
    case LCD_STATUS_STANDBY:
    default:
        return COL_ACCENT;
    }
}

/* ── 点击：本期只验证"点得到"（README.md 5.2 / 7.③）───────────────── */
static void on_tap(const char *name)
{
    /* 本期只有两个屏：主屏 ←→ 对话屏。其余可点区域先只打日志，
     * 第 2 步接 ui_nav（DESIGN.md:142-147 移植顺序）时统一换成路由表。 */
    if (strcmp(name, "card-ai-chat") == 0) {
        screen_chat_show();
        return;
    }
    ESP_LOGI(TAG, "tap on '%s' (navigation not wired yet)", name);
}

/* ── 状态栏（原型 #sb，y 0..22）──────────────────────────────────── */
static void create_status_bar(lv_obj_t *scr)
{
    s_lbl_clock = ui_label(scr, 12, 10, &ui_font_cn_12, COL_MUTED, s_clock);

    ui_signal_bars(scr, 158, 11, COL_ACCENT);
    s_batt_top = ui_battery(scr, 176, 12, s_battery_pct);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_battery_pct);
    s_lbl_pct_top = ui_label(scr, 0, 10, &ui_font_cn_12, COL_MUTED, buf);
    lv_obj_align(s_lbl_pct_top, LV_ALIGN_TOP_RIGHT, -UI_SAFE_PAD, 10);
}

/* ── 守护状态大卡（原型 s-home 第一张卡，x8 y32 w224 h104 rx14）──── */
static void create_guard_card(lv_obj_t *scr)
{
    lv_obj_t *card = ui_card(scr, UI_SAFE_PAD, 32, UI_CONTENT_W, 104, UI_R_BIG);
    ui_on_click(card, on_tap, "guard-card");

    /* 守护徽记：同心双环（原型 index.html 主屏）
     *   外环 r=32 stroke=6 绿 COL_ACCENT = 监护中
     *   内环 r=23 stroke=6 青 COL_CYAN   = 对端在线
     *   中心白色对勾                    = 两者皆正常
     *
     * 规矩：两环**完整闭合**，不含任何进度弧——主屏那个环只表达"一切正常"，
     * 不许让人猜成电量或倒计时。双环而非单环："监护"和"连接"是两个独立子系统。
     *
     * 坐标：环心固定在绝对 (56,84)，卡相对 = (48,52)。ui_ring 的 (x,y) 是外框
     * 左上角，所以
     *   外环 70×70（r32+stroke6）→ (48-35, 52-35) = (13,17)
     *   内环 52×52（r23+stroke6）→ (48-26, 52-26) = (22,26) */
    s_ring_guard = ui_ring(card, 13, 17, 70, 6, COL_ACCENT);
    s_ring_link  = ui_ring(card, 22, 26, 52, 6, COL_CYAN);

    /* 环内对勾：内置符号字形，省一张 16×16 PNG（DESIGN.md:126 走法②）。
     * 白色而非绿色：两环已经各自带了颜色，勾只负责"总结论"。 */
    s_ico_check = ui_icon(card, 0, 0, LV_SYMBOL_OK, COL_TEXT, UI_FONT_ICON_S);
    lv_obj_align_to(s_ico_check, s_ring_guard, LV_ALIGN_CENTER, 0, 0);

    /* 状态点（原型 cx=102 cy=57 r=4.5 → 卡相对 89,20） */
    s_dot_state = ui_block(card, 89, 20, 9, 9, LV_RADIUS_CIRCLE, status_color(s_status));

    /* 主标题 20px：原型基线 64 + 卡偏移 → 卡相对 y=14（base_line 6） */
    s_lbl_title = ui_label(card, 104, 14, &ui_font_cn_20, COL_TEXT, "监护中");

    /* 副行 16px：原型基线 88 → 卡相对 y=41（cn_16: line 20 - base_line 5 = 15） */
    s_lbl_sub = ui_label(card, 92, 41, &ui_font_cn_16, COL_MUTED, "");

    /* 卡内图标行（原型 translate(100,96)）：电量 + 信号 + 文案 */
    s_batt_card = ui_battery(card, 92, 64, s_battery_pct);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_battery_pct);
    s_lbl_pct_card = ui_label(card, 116, 63, &ui_font_cn_12, COL_MUTED, buf);

    ui_signal_bars(card, 146, 64, COL_ACCENT);
    ui_label(card, 164, 63, &ui_font_cn_12, COL_MUTED, "信号良好");

    screen_home_set_status(s_status);    /* 用真实状态填副行 */
}

/* ── 事件 / AI 对话两张小卡（原型 142 行起，108×60 rx12）─────────── */
static void create_two_cards(lv_obj_t *scr)
{
    lv_obj_t *ev = ui_card(scr, UI_SAFE_PAD, 142, 108, 60, UI_R_CARD);
    ui_on_click(ev, on_tap, "card-events");
    ui_icon(ev, 11, 15, LV_SYMBOL_BELL, COL_MUTED, UI_FONT_ICON_S);
    ui_label(ev, 34, 16, &ui_font_cn_16, COL_TEXT, "事件");
    ui_label(ev, 12, 40, &ui_font_cn_12, COL_AMBER, "2 条未读");

    lv_obj_t *ai = ui_card(scr, 124, 142, 108, 60, UI_R_CARD);
    ui_on_click(ai, on_tap, "card-ai-chat");
    ui_icon(ai, 11, 15, LV_SYMBOL_ENVELOPE, COL_MUTED, UI_FONT_ICON_S);
    ui_label(ai, 34, 16, &ui_font_cn_16, COL_TEXT, "AI 对话");
    ui_label(ai, 12, 40, &ui_font_cn_12, COL_MUTED, "说“你好小鑫”");
}

/* ── 紧急呼救红卡（原型 208 行，224×56 rx14，全屏唯一红）─────────── */
static void create_sos_card(lv_obj_t *scr)
{
    lv_obj_t *sos = ui_card(scr, UI_SAFE_PAD, 208, UI_CONTENT_W, 56, UI_R_BIG);
    lv_obj_set_style_bg_color(sos, COL_SOS_BG, 0);
    lv_obj_set_style_border_color(sos, COL_SOS_LINE, 0);
    ui_on_click(sos, on_tap, "sos");

    ui_icon(sos, 13, 7, LV_SYMBOL_WARNING, COL_SOS_ICON, UI_FONT_ICON_S);
    ui_label(sos, 40, 7, &ui_font_cn_20, COL_SOS_TITLE, "紧急呼救");
    ui_label(sos, 40, 34, &ui_font_cn_12, COL_SOS_NOTE, "长按 3 秒，通知家人和对方");
    ui_icon(sos, 203, 19, LV_SYMBOL_RIGHT, COL_SOS_NOTE, UI_FONT_ICON_S);
}

/* ── 底部导航（原型 255 行起：240×48，四项各 60 宽）──────────────── */
static void create_nav_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, 272);
    lv_obj_set_size(bar, UI_HOR_RES, 48);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BAR_LINE, 0);

    static const char *names[4]  = { "nav-guard", "nav-talk", "nav-apps", "nav-settings" };
    /* 图标用本项目自己的 16×16 位图，不用 LV_SYMBOL_*：
     * LVGL 内置符号区里没有盾牌、没有麦克风、没有四宫格，之前拿
     * 房子/喇叭/列表凑，和设计稿完全不是一回事（见 ui_nav_icons.h）。 */
    static const lv_img_dsc_t *icons[4] = { &ui_nav_icon_shield, &ui_nav_icon_mic,
                                            &ui_nav_icon_grid,   &ui_nav_icon_gear };
    static const char *texts[4]  = { "守护", "对讲", "应用", "设置" };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *cell = lv_obj_create(bar);
        lv_obj_set_pos(cell, i * 60, 0);
        lv_obj_set_size(cell, 60, 48);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cell, 0, 0);

        /* 第 1 项 = 当前页：绿色高亮；其余 dim（原型 .teal / .dim） */
        lv_color_t c = (i == 0) ? COL_ACCENT : COL_DIM;

        /* 位图本身是 alpha 蒙版、不带颜色，所以"选中绿 / 未选中灰"
         * 只差一次 recolor，不需要两套图。 */
        lv_obj_t *ico = lv_img_create(cell);
        lv_img_set_src(ico, icons[i]);
        lv_obj_set_style_img_recolor(ico, c, 0);
        lv_obj_set_style_img_recolor_opa(ico, LV_OPA_COVER, 0);
        lv_obj_align(ico, LV_ALIGN_TOP_MID, 0, 11);

        lv_obj_t *txt = ui_label(cell, 0, 0, &ui_font_cn_12, c, texts[i]);
        lv_obj_align(txt, LV_ALIGN_TOP_MID, 0, 30);

        ui_on_click(cell, on_tap, names[i]);
    }
}

/* ── 对外：建屏 ─────────────────────────────────────────────────── */
void screen_home_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    create_status_bar(s_scr);
    create_guard_card(s_scr);
    create_two_cards(s_scr);
    create_sos_card(s_scr);
    create_nav_bar(s_scr);

    /* 建完屏先体检字库：漏字在这里就报出来，不用等肉眼找 □ */
    int missing = ui_audit_fonts(s_scr);
    if (missing > 0) {
        ESP_LOGE(TAG, "字库体检: 共 %d 个字形缺失 —— 按上面的 U+XXXX 重新生成字库", missing);
    }

    lv_scr_load(s_scr);
    ESP_LOGI(TAG, "home screen created (%dx%d)", UI_HOR_RES, UI_VER_RES);
}

lv_obj_t *screen_home_get(void)
{
    return s_scr;
}

void screen_home_show(void)
{
    if (s_scr != NULL && lv_scr_act() != s_scr) {
        lv_scr_load(s_scr);
    }
}

/* ── 对外：状态 → 控件 ──────────────────────────────────────────── */
void screen_home_set_status(lcd_status_t st)
{
    s_status = st;
    if (s_lbl_title == NULL) {
        return;             /* 屏还没建：只存快照，create 时统一填 */
    }

    const char *title;
    const char *sub;
    char sub_buf[48];

    switch (st) {
    case LCD_STATUS_CONNECTING:
        title = "联网中";
        sub   = "正在连接网络";
        break;
    case LCD_STATUS_LISTENING:
        title = "聆听中…";
        sub   = "请说，我在听";
        break;
    case LCD_STATUS_REPLYING:
        title = "回复中…";
        sub   = "正在回答…";
        break;
    case LCD_STATUS_STANDBY:
    default:
        title = "监护中";               /* 固件"待机"= 界面"监护中"，一字不改原型 */
        snprintf(sub_buf, sizeof(sub_buf), "已静默 %d 分钟", s_silence_min);
        sub = sub_buf;
        break;
    }

    lv_label_set_text(s_lbl_title, title);
    lv_label_set_text(s_lbl_sub, sub);
    lv_obj_set_style_bg_color(s_dot_state, status_color(st), 0);
}

void screen_home_set_emotion(lcd_emotion_t emo)
{
    s_emotion = emo;

    /* ⚠ 表情**不参与**颜色：环与勾恒为 COL_ACCENT 安心绿。
     * DESIGN.md 只有 ACCENT/AMBER/RED 三色语义，红专属呼救与告警；
     * 而 enter_idle() 会把表情置成 DROOL，若映射成 AMBER，主屏"一切正常"
     * 就会变成橙黄，看上去像在报警。表情留给二期那张会动的脸。*/
    ESP_LOGD(TAG, "emotion=%d（仅记录；守护徽记不跟表情变色）", (int)emo);
}

void screen_home_set_peer_online(bool online)
{
    if (s_ring_link == NULL) {
        return;
    }
    ui_ring_set_color(s_ring_link, online ? COL_CYAN : COL_DIM);
    ESP_LOGD(TAG, "peer %s -> 内环%s", online ? "online" : "offline",
             online ? "点亮" : "转灰");
}

void screen_home_set_silence_minutes(int minutes)
{
    s_silence_min = minutes;
    if (s_status == LCD_STATUS_STANDBY) {
        char sub[48];
        snprintf(sub, sizeof(sub), "已静默 %d 分钟", minutes);
        if (s_lbl_sub != NULL) {
            lv_label_set_text(s_lbl_sub, sub);
        }
    }
}

void screen_home_set_battery(int pct)
{
    s_battery_pct = pct;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);

    if (s_lbl_pct_top != NULL) {
        lv_label_set_text(s_lbl_pct_top, buf);
    }
    if (s_lbl_pct_card != NULL) {
        lv_label_set_text(s_lbl_pct_card, buf);
    }
    if (s_batt_top != NULL) {
        ui_battery_set_pct(s_batt_top, pct);
    }
    if (s_batt_card != NULL) {
        ui_battery_set_pct(s_batt_card, pct);
    }
}

const char *screen_home_get_clock(void)
{
    if (s_lbl_clock == NULL) {
        return "--:--";
    }
    return lv_label_get_text(s_lbl_clock);
}

void screen_home_set_clock(const char *hhmm)
{
    if (hhmm == NULL) {
        return;
    }
    snprintf(s_clock, sizeof(s_clock), "%s", hhmm);
    if (s_lbl_clock != NULL) {
        lv_label_set_text(s_lbl_clock, s_clock);
    }
}