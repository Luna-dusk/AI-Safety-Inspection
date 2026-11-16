#include "ui_theme.h"

/* 16px 正文的可写副本（原始全量字库是 const，metrics 要改，只能复制一份）。
 * 定义在这里而不是 ui_font_cn_common_16.c 里，因为生成物是 lv_font_conv 的一次性输出，
 * 不该手改；归一化属于"主题适配"，归主题层管。 */
lv_font_t ui_font_cn_16;

void ui_theme_init(void)
{
    /* 常用字库的行高/基线归一化（原因见 ui_theme.h 的注释）：
     * 字形是按基线定位的，行高只决定"文本框有多高"，
     * 所以把 (31,9) 改回 (20,5) 不会动任何一个字的像素位置，
     * 只是让 16px 文本的行盒与子集字库版本完全一致。 */
    ui_font_cn_16 = ui_font_cn_common_16;
    ui_font_cn_16.line_height = 20;
    ui_font_cn_16.base_line   = 5;

    /* 默认主题（LV_THEME_DEFAULT）会给控件加形状补间、按下变深等效果，
     * 与 DESIGN.md「无阴影、无渐变、层级靠字号 + 颜色」相冲突，而且
     * 会让手工写死的坐标在过渡帧里抖动。basic 主题只保留最小样式，
     * 后面每个控件都显式设样式，视觉完全可控。 */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != NULL) {
        lv_disp_set_theme(disp, lv_theme_basic_init(disp));
    }
}