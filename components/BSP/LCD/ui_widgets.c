#include "ui_widgets.h"

#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "UI_WIDGETS";

/* 统一的"裸对象"初始化：去掉主题带来的内边距/滚动条/阴影/描边，
 * 后续每个积木只设自己需要的属性（DESIGN.md:92 无阴影、无渐变）。 */
static lv_obj_t *ui_obj_new(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    return obj;
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *card = ui_obj_new(parent, x, y, w, h);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_bg_color(card, COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, COL_LINE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    return card;
}

lv_obj_t *ui_label(lv_obj_t *parent, int x, int y,
                   const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    /* 卡片是绝对定位布局，标签一律不换行、不吃点击，避免撑破卡片 */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

lv_obj_t *ui_icon(lv_obj_t *parent, int x, int y,
                  const char *symbol, lv_color_t color, const lv_font_t *font)
{
    return ui_label(parent, x, y, font, color, symbol);
}

lv_obj_t *ui_block(lv_obj_t *parent, int x, int y, int w, int h,
                   int radius, lv_color_t color)
{
    lv_obj_t *blk = ui_obj_new(parent, x, y, w, h);
    lv_obj_set_style_radius(blk, radius, 0);
    lv_obj_set_style_bg_color(blk, color, 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(blk, 0, 0);
    return blk;
}

lv_obj_t *ui_block_outline(lv_obj_t *parent, int x, int y, int w, int h,
                           int radius, lv_color_t color)
{
    lv_obj_t *blk = ui_obj_new(parent, x, y, w, h);
    lv_obj_set_style_radius(blk, radius, 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(blk, color, 0);
    lv_obj_set_style_border_width(blk, 1, 0);
    return blk;
}

/* 完整闭合的圆环。用 border 而不是 lv_arc：
 *   - 闭合环没有"进度"语义，画成 3/4 弧会被读成百分比/倒计时
 *     （评审第三轮已否掉过一次假进度弧，见 REVIEW.md 第三轮第 2 条）；
 *   - border 环的外径 = 对象尺寸、环宽 = border_width，正好等于
 *     SVG <circle r=32 stroke-width=6> 的外径 70 / 环宽 6，坐标可照抄原型。
 * LVGL 的 border 向内生长，所以环带落在 (size/2 - width) .. (size/2) 之间，
 * 中心半径 = (size - width) / 2 —— 与 SVG 描边居中于 r 的算法等价。 */
lv_obj_t *ui_ring(lv_obj_t *parent, int x, int y, int size, int width, lv_color_t color)
{
    lv_obj_t *ring = ui_obj_new(parent, x, y, size, size);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, color, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    return ring;
}

void ui_ring_set_color(lv_obj_t *ring, lv_color_t color)
{
    if (ring != NULL) {
        lv_obj_set_style_border_color(ring, color, 0);
    }
}

lv_obj_t *ui_signal_bars(lv_obj_t *parent, int x, int y, lv_color_t color)
{
    /* 原型 ui/prototype/index.html:159-163：3 根柱子 3px 宽，间距 2px */
    lv_obj_t *box = ui_obj_new(parent, x, y, 13, 11);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    ui_block(box, 0,  7, 3, 4, 1, color);
    ui_block(box, 5,  4, 3, 7, 1, color);
    ui_block(box, 10, 2, 3, 9, 1, color);
    return box;
}

void ui_signal_bars_set_color(lv_obj_t *bars, lv_color_t color)
{
    for (uint32_t i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_get_child(bars, (int32_t)i);
        if (bar != NULL) {
            lv_obj_set_style_bg_color(bar, color, 0);
        }
    }
}

/* 内芯宽度：0..12px，按百分比取整 */
static int battery_core_w(int pct)
{
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return 12 * pct / 100;
}

static lv_color_t battery_core_color(int pct)
{
    return (pct >= 20) ? COL_ACCENT : COL_AMBER;   /* 低电量走琥珀 */
}

lv_obj_t *ui_battery(lv_obj_t *parent, int x, int y, int pct)
{
    /* 原型：外框 18×10 rx3 + 内芯 12×6 + 正极 2×4 */
    lv_obj_t *box = ui_obj_new(parent, x, y, 21, 10);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);

    ui_block_outline(box, 0, 0, 18, 10, 3, COL_MUTED);
    ui_block(box, 19, 3, 2, 4, 1, COL_MUTED);

    /* 内芯固定是第 3 个子对象（index 2）：ui_battery_set_pct() 靠这个顺序取 */
    int w = battery_core_w(pct);
    lv_obj_t *core = ui_block(box, 2, 2, w > 0 ? w : 1, 6, 1, battery_core_color(pct));
    if (w <= 0) {
        lv_obj_add_flag(core, LV_OBJ_FLAG_HIDDEN);
    }
    return box;
}

void ui_battery_set_pct(lv_obj_t *battery, int pct)
{
    lv_obj_t *core = lv_obj_get_child(battery, 2);
    if (core == NULL) {
        return;
    }
    int w = battery_core_w(pct);
    if (w <= 0) {
        lv_obj_add_flag(core, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(core, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(core, w);
    lv_obj_set_style_bg_color(core, battery_core_color(pct), 0);
}

/* ── 点击回调：把 void(*)(const char*) 包成 LVGL 的 lv_event_cb_t ── */
typedef struct {
    void (*cb)(const char *name);
    const char *name;
} click_binding_t;

static void click_trampoline(lv_event_t *e)
{
    click_binding_t *b = (click_binding_t *)lv_event_get_user_data(e);
    if (b == NULL) {
        return;
    }
    ESP_LOGI(TAG, "tap: %s", b->name);
    if (b->cb != NULL) {
        b->cb(b->name);
    }
}

void ui_on_click(lv_obj_t *obj, void (*cb)(const char *name), const char *name)
{
    click_binding_t *b = (click_binding_t *)malloc(sizeof(click_binding_t));
    if (b == NULL) {
        ESP_LOGE(TAG, "no mem for click binding %s", name);
        return;
    }
    b->cb = cb;
    b->name = name;

    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, click_trampoline, LV_EVENT_CLICKED, b);
}

/* ── 字形体检：把屏上所有 label 的字都过一遍字库，缺字直接报出来 ──
 *
 * 字库是 lv_font_conv 的**子集**，漏一个字在屏上就是一个豆腐块 □，
 * 而这个遍历把"缺哪个字"直接打进串口（U+XXXX）。
 */
static int utf8_next_cp(const char *s, uint32_t *idx)
{
    const uint8_t *p = (const uint8_t *)s + *idx;
    if (*p == 0) {
        return -1;
    }
    uint32_t cp;
    int len;
    if (*p < 0x80)             { cp = *p;        len = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { (*idx)++; return 0xFFFD; }

    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *idx += (uint32_t)k;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    *idx += (uint32_t)len;
    return (int)cp;
}

int ui_audit_text(const char *text, const lv_font_t *font)
{
    if (text == NULL || font == NULL) {
        return 0;
    }
    const lv_font_t *f = font;
    int missing = 0;
    uint32_t idx = 0;
    int cp;
    while ((cp = utf8_next_cp(text, &idx)) >= 0) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, (uint32_t)cp, 0)) {
            ESP_LOGE(TAG, "字库缺字 U+%04X（文本 \"%s\"）-> 加进 --symbols 重新生成",
                     (unsigned)cp, text);
            missing++;
        }
    }
    return missing;
}

/** @return 缺字个数 */
int ui_audit_fonts(lv_obj_t *root)
{
    if (root == NULL) {
        return 0;
    }
    int missing = 0;
    uint32_t n = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(root, i);
        if (lv_obj_check_type(ch, &lv_label_class)) {
            missing += ui_audit_text(lv_label_get_text(ch),
                                     lv_obj_get_style_text_font(ch, LV_PART_MAIN));
        }
        missing += ui_audit_fonts(ch);
    }
    return missing;
}
