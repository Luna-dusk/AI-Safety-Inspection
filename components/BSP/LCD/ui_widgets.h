#ifndef __UI_WIDGETS_H__
#define __UI_WIDGETS_H__

/**
 * @file  ui_widgets.h
 * @brief 屏内复用的积木：卡片 / 文字 / 图标 / 圆点 / 小色块
 *
 * 对应 ui/DESIGN.md:110-128 的「原型 → LVGL」对照表。所有函数都返回
 * 新建的 lv_obj_t*，坐标语义与 SVG 原型完全一致：
 *   - 原型 <rect x y width height rx>  → ui_card(parent, x, y, w, h, rx)
 *   - 原型 <text x y class="f16">      → ui_label(parent, x, y_top, ...)
 *   - 原型 <use href="#i-xxx">         → ui_icon(parent, x, y, LV_SYMBOL_xxx, ...)
 *
 * 注意 SVG 的 y 是**基线**，LVGL 的 y 是**文本框顶部**，两者的换算已经
 * 收在屏文件里（各元素差一个 ascent），这里只管按 LVGL 语义摆放。
 */

#include "lvgl.h"
#include "ui_theme.h"

/**
 * @brief 建一张卡：圆角 + 1px 描边 + 无内边距（子对象绝对定位）+ 不可滚动
 * @param radius 传 UI_R_BIG / UI_R_CARD / 0
 */
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h, int radius);

/**
 * @brief 建一个文字标签（左上角定位）
 * @param y 文本框顶部 y（不是基线）
 */
lv_obj_t *ui_label(lv_obj_t *parent, int x, int y,
                   const lv_font_t *font, lv_color_t color, const char *text);

/**
 * @brief 建一个图标标签（LV_SYMBOL_* 字形，Montserrat 内置符号区）
 */
lv_obj_t *ui_icon(lv_obj_t *parent, int x, int y,
                  const char *symbol, lv_color_t color, const lv_font_t *font);

/**
 * @brief 建一个纯色圆点 / 色块（无描边、无阴影）
 * @param radius 传 LV_RADIUS_CIRCLE 得到正圆
 */
lv_obj_t *ui_block(lv_obj_t *parent, int x, int y, int w, int h,
                   int radius, lv_color_t color);

/**
 * @brief 建一个空心小方块（电量图标的外框那种：1px 描边 + 透明底）
 */
lv_obj_t *ui_block_outline(lv_obj_t *parent, int x, int y, int w, int h,
                           int radius, lv_color_t color);

/**
 * @brief 建一个完整闭合的圆环（不是进度弧）
 *
 * 用 border 画环，而不是 lv_arc：闭合环没有"进度"语义，画出 3/4 弧会
 * 被读成百分比或倒计时（评审第三轮已否掉过一次"假进度弧"，
 * 见 .oil-ui/ui-review/REVIEW.md）。border 环的外径 = size、环宽 = width，
 * 与 SVG 的 <circle r stroke> 一一对应，坐标可直接照抄原型。
 *
 * @param size  外径（px）
 * @param width 环宽（px），建议 5..6 在 240 屏上才看得清
 * @param color 环色
 */
lv_obj_t *ui_ring(lv_obj_t *parent, int x, int y, int size, int width, lv_color_t color);

/**
 * @brief 改环的颜色（不重建对象，免得闪一下）
 */
void ui_ring_set_color(lv_obj_t *ring, lv_color_t color);

/**
 * @brief 建一个 16px 高的信号柱组（3 根，从左到右递增）
 * @param color 传 COL_ACCENT 表示信号良好，COL_DIM 表示弱信号
 * @note  原型见 ui/prototype/index.html:159-163
 */
lv_obj_t *ui_signal_bars(lv_obj_t *parent, int x, int y, lv_color_t color);
void ui_signal_bars_set_color(lv_obj_t *bars, lv_color_t color);

/**
 * @brief 建一个电量图标（外框 + 内芯 + 正极触点），宽 21 高 10
 * @param pct 0..100，内芯宽度按比例；<20 转琥珀
 */
lv_obj_t *ui_battery(lv_obj_t *parent, int x, int y, int pct);

/**
 * @brief 重设电量图标的内芯（不重建对象，免闪烁）
 * @param battery ui_battery() 的返回值
 */
void ui_battery_set_pct(lv_obj_t *battery, int pct);

/**
 * @brief 检查一段文本里有多少字在指定字库里没有字形（打 ERROR 并返回个数）
 * @note  用于任意长度文本：对话消息进来时就查，缺字立刻知道
 */
int ui_audit_text(const char *text, const lv_font_t *font);

/**
 * @brief 字形体检：递归遍历 root 下所有 label，把字库里缺失的字打成 ERROR
 * @return 缺字个数（0 = 屏上文字全部有字形）
 * @note  字库是子集，加新文案后跑一次这里就知道要不要重新生成字库
 */
int ui_audit_fonts(lv_obj_t *root);

/**
 * @brief 给对象挂点击回调（触摸链路通不通，串口日志一眼可见）
 * @param name 日志里打印的控件名，必须是静态字符串
 */
void ui_on_click(lv_obj_t *obj, void (*cb)(const char *name), const char *name);

#endif /* __UI_WIDGETS_H__ */