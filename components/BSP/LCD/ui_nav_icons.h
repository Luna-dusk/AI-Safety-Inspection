#ifndef __UI_NAV_ICONS_H__
#define __UI_NAV_ICONS_H__

/**
 * @file  ui_nav_icons.h
 * @brief 底部导航的 4 个图标（16×16，LV_IMG_CF_ALPHA_4BIT）
 *
 * 为什么不用 LV_SYMBOL_*：LVGL 内置 Montserrat 符号区里**没有盾牌、没有
 * 四宫格、也没有麦克风**，只能拿房子/喇叭/列表凑，和设计稿不是一回事。
 *
 * 为什么用位图而不是图标字体：
 *   - 原型图标是 stroke 风格（stroke-width 1.7 + 圆角端点），图标字体
 *     的字形是实心填充，做不出这种细线；
 *   - 4 张 16×16 的 ALPHA_4BIT 一共 128 B×4 = 512 B，比再嵌一份图标字体
 *     （几十 KB）省得多，也省掉 lv_font_conv 的一道工序。
 *
 * 用法：lv_img_set_src(img, &ui_nav_icon_shield) 后用
 *      style_img_recolor 染色——位图本身是 alpha 蒙版，不带颜色，
 *      所以"选中绿 / 未选中灰"不需要两套图。
 *
 * ⚠ 图标源是 ui/prototype/index.html 的 #i-shield / #i-mic / #i-grid /
 *   #i-gear。**改图标要同时改原型和这里**，生成脚本见文件末尾注释。
 */

#include "lvgl.h"

extern const lv_img_dsc_t ui_nav_icon_shield;  /* 守护 */
extern const lv_img_dsc_t ui_nav_icon_mic;     /* 对讲 */
extern const lv_img_dsc_t ui_nav_icon_grid;    /* 应用 */
extern const lv_img_dsc_t ui_nav_icon_gear;    /* 设置 */

#endif /* __UI_NAV_ICONS_H__ */
