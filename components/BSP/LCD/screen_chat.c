#include "screen_chat.h"

#include "screen_home.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SCR_CHAT";

static lv_obj_t *s_scr;         /* 根屏 */
static lv_obj_t *s_list;        /* 气泡容器（可滚动） */
static lv_obj_t *s_empty;       /* "还没有消息"占位 */
static int       s_count;       /* 当前气泡数 */
/* 下一个气泡的内容坐标 y。自己维护游标而不是用 lv_obj_align_to 链：
 * align_to 会把"基准对象"指针存进子对象，删掉最旧气泡后那个指针就悬空了，
 * 下一次布局更新会踩野指针；游标方案没有这个耦合。 */
static lv_coord_t s_cursor_y;

static void on_back(const char *name)
{
    (void)name;
    screen_home_show();         /* 主屏是返回栈的唯一落点（本期只有两个屏） */
}

static void on_clear(const char *name)
{
    (void)name;
    screen_chat_clear();
}

/* ── 头部：返回 + 标题 + 清空 ────────────────────────────────────── */
static void create_header(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_HOR_RES, 48);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BAR_LINE, 0);

    /* 返回热区 48×48（触摸最小尺寸 44，DESIGN.md:90） */
    lv_obj_t *back = lv_obj_create(bar);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, 48, 48);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    ui_on_click(back, on_back, "chat-back");
    lv_obj_t *chev = ui_icon(back, 0, 0, LV_SYMBOL_LEFT, COL_MUTED, UI_FONT_ICON_S);
    lv_obj_align(chev, LV_ALIGN_CENTER, 0, 0);

    ui_label(bar, 52, 12, &ui_font_cn_16, COL_TEXT, "AI 对话");

    /* 清空：小按钮，避免对话历史无限占内存 */
    lv_obj_t *clr = lv_obj_create(bar);
    lv_obj_set_size(clr, 48, 48);
    lv_obj_align(clr, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(clr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(clr, 0, 0);
    lv_obj_set_style_bg_opa(clr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clr, 0, 0);
    ui_on_click(clr, on_clear, "chat-clear");
    lv_obj_t *trash = ui_icon(clr, 0, 0, LV_SYMBOL_TRASH, COL_DIM, UI_FONT_ICON_S);
    lv_obj_align(trash, LV_ALIGN_CENTER, 0, 0);
}

static void create_footer(lv_obj_t *scr)
{
    lv_obj_t *lbl = ui_label(scr, 0, 306, &ui_font_cn_12, COL_DIM,
                             "说“你好小鑫”或长按 BOOT 键说话");
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -3);
}

/* ── 对外：建屏 ─────────────────────────────────────────────────── */
void screen_chat_create(void)
{
    if (s_scr != NULL) {
        return;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    create_header(s_scr);

    /* 气泡列表：y 52..304，可竖滚；不加 flex（每个气泡要独立左右对齐） */
    s_list = lv_obj_create(s_scr);
    lv_obj_set_pos(s_list, UI_SAFE_PAD, 52);
    lv_obj_set_size(s_list, UI_CONTENT_W, 252);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_empty = ui_label(s_list, 0, 90, &ui_font_cn_12, COL_DIM, "还没有消息，说句话试试");
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, 90);

    create_footer(s_scr);

    int missing = ui_audit_fonts(s_scr);
    if (missing > 0) {
        ESP_LOGE(TAG, "字库体检: 缺 %d 个字形", missing);
    }
    ESP_LOGI(TAG, "chat screen created");
}

void screen_chat_show(void)
{
    if (s_scr == NULL) {
        ESP_LOGW(TAG, "chat screen not created yet");
        return;
    }
    if (lv_scr_act() != s_scr) {
        lv_scr_load(s_scr);
        /* 切进来时滚到最新一条 */
        lv_obj_update_layout(s_list);
        lv_obj_scroll_by(s_list, 0, -lv_obj_get_scroll_bottom(s_list), LV_ANIM_OFF);
    }
}

bool screen_chat_is_active(void)
{
    return (s_scr != NULL) && (lv_scr_act() == s_scr);
}

static void delete_oldest(void)
{
    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(s_list, i);
        if (ch == s_empty) {
            continue;               /* 占位标签不是气泡，跳过（否则 s_count 不减 -> 死循环）*/
        }
        lv_obj_del(ch);
        if (s_count > 0) {
            s_count--;
        }
        return;
    }
}

void screen_chat_clear(void)
{
    if (s_list == NULL) {
        return;
    }
    while (s_count > 0 && lv_obj_get_child_cnt(s_list) > 1) {
        delete_oldest();
    }
    s_cursor_y = 0;
    if (s_empty != NULL) {
        lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 对外：加一条消息 ───────────────────────────────────────────── */
void screen_chat_add(lcd_chat_role_t role, const char *text)
{
    if (s_list == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    if (s_empty != NULL) {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    while (s_count >= SCREEN_CHAT_MAX_BUBBLES) {
        delete_oldest();
    }

    bool mine = (role == LCD_CHAT_USER);

    /* 常用字库覆盖 GB2312 一级字（约 99.7%），生僻字/emoji 仍可能缺字形。
     * 进来就查一遍，缺的直接报 U+XXXX，别让用户对着 □ 猜。 */
    int miss = ui_audit_text(text, &ui_font_cn_16);
    if (miss > 0) {
        ESP_LOGW(TAG, "本条消息 %d 个字不在字库（显示为 □）", miss);
    }

    /* 一次测量同时拿到"换行后的宽和高"：
     * lv_txt_get_size(max_width=SCREEN_CHAT_BUBBLE_MAX_W) 内部会按最大宽度折行，
     * 返回的是折行后的包围盒 —— 正好是气泡内容区该有的尺寸。
     * 用它显式设尺寸，就不依赖 LVGL 的布局时序（新建对象的高度在
     * 第一次 layout 之前还是 0，靠它算游标会重叠）。*/
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &ui_font_cn_16, 0, 0,
                    SCREEN_CHAT_BUBBLE_MAX_W, LV_TEXT_FLAG_NONE);
    lv_coord_t w = (sz.x < 1) ? 1 : sz.x;
    lv_coord_t h = (sz.y < 1) ? ui_font_cn_16.line_height : sz.y;

    const lv_coord_t pad = 8;
    lv_coord_t bw = w + pad * 2;
    lv_coord_t bh = h + pad * 2;

    lv_obj_t *b = lv_obj_create(s_list);
    lv_obj_set_size(b, bw, bh);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, UI_R_CARD, 0);
    lv_obj_set_style_bg_color(b, mine ? COL_ACCENT : COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, mine ? 0 : 1, 0);
    lv_obj_set_style_border_color(b, COL_LINE, 0);

    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, pad, pad);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, &ui_font_cn_16, 0);
    lv_obj_set_style_text_color(lbl, mine ? COL_BG : COL_TEXT, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    /* 自己说话靠右，AI 靠左 */
    lv_coord_t x = mine ? (UI_CONTENT_W - bw) : 0;
    if (x < 0) {
        x = 0;
    }
    lv_obj_set_pos(b, x, s_cursor_y);
    s_cursor_y += bh + 6;
    s_count++;

    /* 滚到最新一条 */
    lv_obj_update_layout(s_list);
    lv_obj_scroll_by(s_list, 0, -lv_obj_get_scroll_bottom(s_list), LV_ANIM_OFF);

    ESP_LOGI(TAG, "bubble #%d [%s] %dx%d, %u bytes", s_count,
             mine ? "user" : "ai", (int)bw, (int)bh, (unsigned)strlen(text));
}
