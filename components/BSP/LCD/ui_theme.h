#ifndef __UI_THEME_H__
#define __UI_THEME_H__

/**
 * @file  ui_theme.h
 * @brief 视觉规范层：色值 / 字号 / 尺寸常量（ui/DESIGN.md 第 3 节的代码化）
 *
 * 设计纪律（DESIGN.md:54-72, 74-84）：
 *   - 色值只从这里取，屏里不写字面量；
 *   - 字号只有 12 / 16 / 20 三档（28 档等告警屏再生成）；
 *   - 中文不做粗体，层级靠字号 + 颜色。
 *
 * 字库为 Noto Sans SC 的 lv_font_conv 子集（见 LCD/README.md 5.3），
 * 只收了本 UI 实际用到的字 + ASCII。**加新文案要回来重新生成字库**，
 * 忘了的现象是屏上出现豆腐块 □。
 */

#include "lvgl.h"

/* ── 色彩（对照 ui/DESIGN.md:54-72）─────────────────────────────────
 *
 * 语义色 = Apple Dark 系统色。所有屏都从这组宏取色，屏文件里不得出现字面量，
 * 所以换肤只动这里。色值对照表见 .oil-ui/ui-review/REVIEW.md。
 *
 * 纪律：色值只从这里取；字号只有 12/16/20（28 档等告警屏）；
 * 中文不做粗体，层级靠字号 + 颜色；无渐变、无阴影、无滤镜。 */
#define COL_BG          lv_color_hex(0x000000)  /* 屏底：OLED 纯黑（watchOS 的底就是真黑） */
#define COL_BAR         lv_color_hex(0x000000)  /* 状态栏 / 底部导航：与屏底同色，靠分割线分界 */
#define COL_BAR_LINE    lv_color_hex(0x38383A)  /* 导航分割线（iOS separator） */
#define COL_SURFACE     lv_color_hex(0x1C1C1E)  /* 卡片（secondarySystemGroupedBackground） */
#define COL_SURFACE2    lv_color_hex(0x2C2C2E)  /* 次级卡片（tertiarySystemGroupedBackground） */
#define COL_LINE        lv_color_hex(0x2C2C2E)  /* 卡片描边：贴近卡面色，watchOS 的卡是无边框色块 */
#define COL_TEXT        lv_color_hex(0xFFFFFF)  /* 主文字（label） */
#define COL_MUTED       lv_color_hex(0x98989F)  /* 次文字（secondaryLabel，户外已提亮一档） */
#define COL_DIM         lv_color_hex(0x636366)  /* 三级文字 / 未激活图标（tertiaryLabel） */
#define COL_ACCENT      lv_color_hex(0x30D158)  /* 安心绿：正常 / 在线 / 已送达（systemGreen Dark） */
#define COL_AMBER       lv_color_hex(0xFF9F0A)  /* 需要确认 / 降级 / 进行中（systemOrange Dark） */
#define COL_RED         lv_color_hex(0xFF453A)  /* 只用于呼救与告警（systemRed Dark） */

/* ── 主屏专用衍生色（原型里这些值是写死在单个元素上的）───────────── */
#define COL_RING_BG     lv_color_hex(0x0E3B1B)  /* 外环轨道（绿压 20% 透明度到黑） */
#define COL_CYAN        lv_color_hex(0x64D2FF)  /* 内环：对端在线（systemCyan Dark） */
#define COL_CYAN_BG     lv_color_hex(0x0A2A33)  /* 内环轨道（青压 20% 透明度到黑） */
#define COL_SOS_BG      lv_color_hex(0x2A0E0C)  /* 紧急呼救卡底 */
#define COL_SOS_LINE    lv_color_hex(0x7A2B25)  /* 紧急呼救卡描边 */
#define COL_SOS_ICON    lv_color_hex(0xFF6961)  /* 呼救卡图标 */
#define COL_SOS_TITLE   lv_color_hex(0xFF6961)  /* 呼救卡标题 */
#define COL_SOS_NOTE    lv_color_hex(0xC99A96)  /* 呼救卡说明文字 */

/* ── 尺寸（ui/DESIGN.md:86-92）───────────────────────────────────── */
#define UI_HOR_RES      240
#define UI_VER_RES      320
#define UI_SAFE_PAD     8       /* 安全边距，内容宽 = 224 */
#define UI_CONTENT_W    224

/* 圆角：watchOS 那种圆润的胶囊感（原型 rx=20）。 */
#define UI_R_BIG        20      /* 大卡圆角 */
#define UI_R_CARD       16      /* 普通卡圆角 */

/* ── 字库 ──────────────────────────────────────────────────────────
 * 12/20 是**UI 子集**（只收本屏固定文案用到的字，几十 KB）；
 * 16 是**常用字集**：GB2312 一级汉字 3755 字 + 中文标点/符号 + ASCII +
 * 全角，覆盖现代汉语约 99.7%。对话界面要显示服务器返回的任意文字，
 * 子集那套（漏字就是 □）不再适用；全量（20992 字/2.7MB）又太重。
 *
 * ⚠ 这个字库里有个别字形把 line_height 撑到 31px、base_line 撑到 9，
 *   会让 16px 的版面整体下移 7px。字形位置是**基线相对**的，所以
 *   ui_theme_init() 里复制一份并把行高/基线归一到 (20, 5)：
 *     - 想用原始数据用 ui_font_cn_common_16；
 *     - 屏幕代码一律用 ui_font_cn_16（已归一化）。 */
extern const lv_font_t ui_font_cn_12;        /* 说明 / 状态 / 时间（子集） */
extern const lv_font_t ui_font_cn_20;        /* 屏内主标题（子集） */
extern const lv_font_t ui_font_cn_common_16; /* 常用字集，原始生成物，别直接上屏 */
extern lv_font_t       ui_font_cn_16;        /* 16px 正文（= common_16 的归一化副本） */

/* 图标用 LVGL 内置 Montserrat 的符号字形（LV_SYMBOL_*），
 * 不额外占 flash；字号从 CONFIG_LV_FONT_MONTSERRAT_16/20 两档取。 */
#define UI_FONT_ICON_S  (&lv_font_montserrat_16)
#define UI_FONT_ICON_M  (&lv_font_montserrat_20)

/**
 * @brief 初始化主题：把默认主题换成 basic，避免 default 主题的
 *        形状补间动画和阴影干扰手工布局（DESIGN.md:92「无阴影、无渐变」）
 * @note  必须在 lv_disp_drv_register() 之后、建屏之前调用
 */
void ui_theme_init(void);

#endif /* __UI_THEME_H__ */