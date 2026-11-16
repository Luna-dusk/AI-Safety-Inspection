#include "lcd_ui.h"

/**
 * @file  lcd_ui.c
 * @brief ST7789 SPI 240×320 + LVGL 8.3 移植层 + UI 引擎（实机实现）
 *
 * 分层（LCD/README.md 第 0 节）：
 *   业务层  AFE_SR / CHAT / MultiNet → 只调 lcd_ui_set_status/set_emotion
 *   接口层  lcd_ui.h（本文件的非 static 函数，签名别动）
 *   实现层  本文件（移植层）+ ui_theme / ui_widgets / screen_home（界面）
 *
 * 移植层只需要写对三个接口点（README.md 第 0 节表格）：
 *   ① flush_cb               LVGL 画好一块 → 我们排队送屏
 *   ② tick                   esp_timer 每 1ms 喂 lv_tick_inc(1)
 *   ③ lv_timer_handler 循环  lcd_ui_task 里泵起来
 */

#include "lvgl.h"
#include "screen_chat.h"
#include "screen_home.h"
#include "ui_theme.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "LCD_UI";

/* ══════════════════════════════════════════════════════════════════
 * 3.0 引脚（外接屏排针 2.54mm 1×8P，H1.3~H1.8）
 *
 *   H1.1/H1.2 = VCC/GND     （按模块要求 3.3V，勿接 5V）
 *   H1.3 SCL  → GPIO9       时钟
 *   H1.4 SDA  → GPIO10      数据
 *   H1.5 RES  → GPIO11      复位
 *   H1.6 DC   → GPIO12      命令/数据（仅 SPI 有意义）
 *   H1.7 CS   → GPIO13      片选（仅 SPI 有意义）
 *   H1.8 BLK  → GPIO14      背光
 *    RGB565 一帧 150KB，400kHz I2C 要 ~3 秒/帧，做不了这个 UI —— 这块屏走 SPI。
 */
#define LCD_PIN_SCL     GPIO_NUM_9      /* 同时也是 SPI SCLK */
#define LCD_PIN_SDA     GPIO_NUM_10     /* 同时也是 SPI MOSI */
#define LCD_PIN_RST     GPIO_NUM_11
#define LCD_PIN_DC      GPIO_NUM_12
#define LCD_PIN_CS      GPIO_NUM_13
#define LCD_PIN_BLK     GPIO_NUM_14

#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIXEL_CLK   (40 * 1000 * 1000)  /* 起步 40MHz；花屏就降到 20MHz */

/* 面板像素序：**这块屏是 RGB，不是 BGR**（写成 BGR 红卡会整块变蓝）。
 * 判据：只看呼救卡——红卡变蓝 = 通道反，改这里；颜色像负片 = 改 LCD_INVERT_COLOR。
 * 排障经过见 HANDOFF 2.10。 */
#define LCD_RGB_ORDER   LCD_RGB_ELEMENT_ORDER_RGB
/* 部分模块需要打开反色（IPS 屏常见）。颜色像负片就把它设 0。 */
#define LCD_INVERT_COLOR   1
#define LCD_BLK_ACTIVE_HIGH 1

/* draw buffer：默认 240×12 行 ≈ 5.7KB（README.md 3.1 ②）。
 * 不能放 PSRAM：SPI DMA 只认 DMA|INTERNAL。块大小几乎不影响帧速（整屏字节数
 * 一样，只是多切几刀），所以内部 RAM 紧张时优先取小（HANDOFF 坑 31）。 */
#define DRAW_BUF_LINES_MAX  12
#define DRAW_BUF_LINES_MIN  3
#define DRAW_BUF_PX_MAX     (UI_HOR_RES * DRAW_BUF_LINES_MAX)
#define DRAW_BUF_BYTES_MAX  (DRAW_BUF_PX_MAX * sizeof(lv_color_t))

/* ── 驱动句柄 ───────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static lv_disp_draw_buf_t        s_draw_buf;
static lv_disp_drv_t             s_drv;
static void                     *s_draw_mem;
static size_t                    s_draw_px;      /* 实际拿到的像素缓冲大小 */
static volatile bool             s_disp_ready;   /* lv_disp_drv_register 之后才允许 flush_ready */
static bool                      s_inited;
/* 已完成的 flush 块数：这是"LVGL 真的在往屏幕送像素"的唯一硬证据。
 * 只看日志里有没有 "status -> xxx" 是没用的——那个函数在面板没初始化时
 * 也照打（只是写影子变量）。0 = 一块都没送出去。 */
static volatile uint32_t         s_flush_count;
static volatile uint32_t         s_loop_count;    /* lcd_ui 任务循环次数（心跳）*/
static TaskHandle_t              s_lcd_task;      /* 供别的任务偷拍状态 */
static bool                      s_warned_no_timer;

/* ── 对话文本的跨任务交接 ─────────────────────────────────────────
 * 文本长度不定，影子变量那套（单变量覆盖写）不适用，改用一个小队列。
 * 队列是 FreeRTOS 对象 → 只能内部 RAM：8 槽 × 201B ≈ 1.6KB，可接受。*/
#define LCD_CHAT_QUEUE_LEN   8
typedef struct {
    uint8_t role;
    char    text[SCREEN_CHAT_TEXT_MAX];
} lcd_chat_msg_t;

static QueueHandle_t  s_chat_q;
static volatile bool  s_chat_show_req;
static volatile bool  s_chat_clear_req;

static void font_selfcheck(void);   /* 定义在下方"对外接口"区之前 */

/** 按 UTF-8 字符边界截断拷贝：截半个字会让 LVGL 渲染出乱码 */
static void utf8_copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    const uint8_t *p = (const uint8_t *)src;
    while (*p != 0) {
        size_t len = (*p < 0x80) ? 1 : ((*p & 0xE0) == 0xC0) ? 2
                   : ((*p & 0xF0) == 0xE0) ? 3 : 4;
        if (o + len + 1 > cap) {
            break;
        }
        for (size_t i = 0; i < len; i++) {
            dst[o++] = (char)p[i];
        }
        p += len;
    }
    dst[o] = 0;
}

/* ── 跨线程影子变量（README.md 3.5：只写不读，谁最后写谁生效）───── */
static volatile lcd_status_t  s_status_req  = LCD_STATUS_STANDBY;
static volatile lcd_emotion_t s_emotion_req = LCD_EMO_NORMAL;
static volatile bool          s_dirty;

/* 供 lcd_ui_task 打印用（不在热路径上） */
static const char *status_name(lcd_status_t st)
{
    switch (st) {
    case LCD_STATUS_STANDBY:    return "STANDBY";
    case LCD_STATUS_CONNECTING: return "CONNECTING";
    case LCD_STATUS_LISTENING:  return "LISTENING";
    case LCD_STATUS_REPLYING:   return "REPLYING";
    default:                    return "?";
    }
}

static const char *emotion_name(lcd_emotion_t emo)
{
    switch (emo) {
    case LCD_EMO_SMILE:  return "SMILE";
    case LCD_EMO_CRY:    return "CRY";
    case LCD_EMO_NORMAL: return "NORMAL";
    case LCD_EMO_ANGRY:  return "ANGRY";
    case LCD_EMO_DROOL:  return "DROOL";
    default:             return "?";
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 3.1 移植层：LVGL → 屏幕
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief DMA 发送完成回调（跑在 SPI ISR 上下文，必须 ISR 安全）
 *
 * 这里只做一件事：告诉 LVGL「buffer 可以复用了」。
 * lv_disp_flush_ready() 在 LVGL 8.3 里就是两次普通赋值，ISR 里安全。
 * @return false = 不需要唤醒更高优先级任务
 */
static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    (void)io;
    (void)edata;
    if (s_disp_ready) {
        lv_disp_flush_ready((lv_disp_drv_t *)user_ctx);
    }
    return false;
}

/**
 * @brief flush 回调：LVGL 渲染完一块脏区就调这里
 *
 * 只负责把这块排进 DMA 队列然后立即返回，**绝不在这里等发送完成**，
 * 也**绝不在这里调 lv_disp_flush_ready**（那是撕裂的头号来源，
 * README.md 3.2 与 9.①-②）。
 */
static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    (void)drv;
    /* LVGL 的 area 含头含尾 [x1,x2]，esp_lcd 含头不含尾 [x1,x2)，右/下界 +1。
     * 差这一像素的现象：最后一行/列永远是旧内容。 */
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel,
                                              area->x1, area->y1,
                                              area->x2 + 1, area->y2 + 1,
                                              px_map);
    s_flush_count++;
    /* 开屏那 27 块逐块打日志：一旦卡死，最后一条就是"送到哪一块"。
     * 之后不再打（正常运行时每帧只有几块，不需要刷屏）。*/
    if (s_flush_count <= 32) {
        ESP_LOGI(TAG, "flush#%u (%d,%d)-(%d,%d) ret=%d",
                 (unsigned)s_flush_count, (int)area->x1, (int)area->y1,
                 (int)area->x2, (int)area->y2, (int)err);
    }
}

/**
 * @brief LVGL 单缓冲等 DMA 时被调用（README.md 3.1 ⑤）
 *
 * LVGL 8.3 在渲染下一块脏区前会 `while(draw_buf->flushing)` 自旋，
 * 注册这个回调就把忙等换成让出 CPU —— 否则 UI 任务会和音频抢核。
 */
static void lvgl_wait_cb(lv_disp_drv_t *drv)
{
    /* 正常情况下这里只等 1~3ms（5760B @40MHz ≈ 1.2ms）。
     * 但一旦某一块的 DMA 完成回调丢了，`flushing` 就永远是 1，LVGL 会在这里
     * 无限自旋 —— 现象是：屏幕停在最后一帧、界面再也不更新、日志也没了。
     * 所以加一个看门狗：等超过 WC 次就直接替驱动把 ready 置上。
     * 宁可画面撕裂一次，也不能让整块屏死在那儿。*/
    static uint32_t wait_ticks;
    static uint32_t seen_flush;
    enum { WAIT_TIMEOUT_TICKS = 200 };   /* 约 200ms（每轮 vTaskDelay(1)）*/

    if (s_flush_count != seen_flush) {   /* 有新块送出 → 重新计时 */
        seen_flush = s_flush_count;
        wait_ticks = 0;
    }
    if (++wait_ticks >= WAIT_TIMEOUT_TICKS) {
        ESP_LOGW(TAG, "flush 等待超时(%u ticks, flush=%u) -> 强制 flush_ready；"
                      "若反复出现，说明 SPI 完成回调丢了",
                 (unsigned)wait_ticks, (unsigned)s_flush_count);
        wait_ticks = 0;
        lv_disp_flush_ready(drv);
    }
    vTaskDelay(1);
}

/* 3.3 tick：LVGL 自己不计时，靠我们喂 */
static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_tick_start(void)
{
    const esp_timer_create_args_t args = {
        .callback = &lv_tick_cb,
        .name     = "lv_tick",
    }; 
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000));  /* 1ms */
}

/* 3.1 第 4 步：背光（先普通 GPIO 拉高；要亮度可换 LEDC/PWM） */
static esp_err_t backlight_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_BLK,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "backlight gpio cfg failed");
    int level = LCD_BLK_ACTIVE_HIGH ? 1 : 0;
    return gpio_set_level(LCD_PIN_BLK, level);
}

/* ── 三个 LVGL 接口点的装配（3.1 第 5~6 步）─────────────────────── */
static esp_err_t lvgl_port_init(void)
{
    lv_init();

    /* 内部 RAM + DMA 能力：SPI DMA 只认这块内存（README.md 3.1 ②）。
     * 拿不到就减半重试：碎片化后最大连续块可能只有几 KB。*/
    size_t px = DRAW_BUF_PX_MAX;
    while (s_draw_mem == NULL) {
        s_draw_mem = heap_caps_malloc(px * sizeof(lv_color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_draw_mem != NULL) {
            s_draw_px = px;
            break;
        }
        ESP_LOGW(TAG, "draw buf %u B alloc failed, retry %u lines",
                 (unsigned)(px * sizeof(lv_color_t)), (unsigned)(px / UI_HOR_RES / 2));
        if (px <= (size_t)(UI_HOR_RES * DRAW_BUF_LINES_MIN)) {
            break;
        }
        px /= 2;
    }
    ESP_RETURN_ON_FALSE(s_draw_mem != NULL, ESP_ERR_NO_MEM, TAG,
                        "draw buf alloc failed down to %d lines", DRAW_BUF_LINES_MIN);
    ESP_LOGI(TAG, "draw buf %u B (%dx%u), internal free=%u dma largest=%u",
             (unsigned)(s_draw_px * sizeof(lv_color_t)), UI_HOR_RES,
             (unsigned)(s_draw_px / UI_HOR_RES),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    lv_disp_draw_buf_init(&s_draw_buf, s_draw_mem, NULL, s_draw_px);

    lv_disp_drv_init(&s_drv);
    s_drv.hor_res      = UI_HOR_RES;
    s_drv.ver_res      = UI_VER_RES;
    s_drv.flush_cb     = lcd_flush_cb;
    s_drv.draw_buf     = &s_draw_buf;
    s_drv.wait_cb      = lvgl_wait_cb;
    /* 主屏这张 270° 环全靠抗锯齿才不显锯齿（README.md 示例里写 false，
     * 那是没画圆弧时的取值；S3 上这点开销可以忽略，换观感更值）*/
    s_drv.antialiasing = true;
    lv_disp_drv_register(&s_drv);

    /* 注册完成后 trans_done 才会被触发，之前置位会把 flushing 标志写乱 */
    s_disp_ready = true;

    font_selfcheck();       /* 字库格式体检：压缩位图会让文字静默消失 */
    /* 时钟时区：app_state 的 time_sync() 会设一次，这里再设一遍，
     * 保证"联网校时之前"（甚至没网）也不会把 UTC 当本地时间显示。
     * CST-8 = 中国标准时间 UTC+8（无夏令时）。*/
    setenv("TZ", "CST-8", 1);
    tzset();

    ui_theme_init();        /* 换 basic 主题：无补间、无阴影 */
    lvgl_tick_start();      /* tick 必须在建屏前跑起来，否则动画/长按失效 */
    screen_home_create();   /* 主屏 · 守护（lv_scr_load 在内部） */
    screen_chat_create();   /* 对话屏：提前建好，切屏就是 lv_scr_load */

    /* 内部 RAM 余量：这一行是以后调 LV_MEM_* 配置的依据，别删。
     * （LV_MEM_CUSTOM 以后 lv_mem_monitor 变成空函数，所以这里数的是
     *   系统堆的实际余量，比 LVGL 自己那个池更可信）*/
    ESP_LOGI(TAG, "after screen: internal free=%u largest=%u | psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}


/**
 * @brief 面板 + 背光上电时序（IDF 自带 ST7789 初始化命令序列，不用手写）
 *
 * 每一步都查返回码：面板不应答时（排线/供电/引脚错）这里就会报出来，
 * 否则会变成"日志说 up、屏幕全黑"，只能盲猜。
 * 拆成独立函数是为了能用 ESP_RETURN_ON_ERROR（lcd_ui_init 是 void）。
 */
static esp_err_t panel_bringup(void)
{
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev_cfg, &s_panel),
                        TAG, "new_panel_st7789 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, LCD_INVERT_COLOR != 0),
                        TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "panel swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "panel gap failed");
    /* 忘了这步：背光亮但全黑 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel disp_on failed");

    /* 背光最后开：先让面板进好状态，避免上电闪一屏花点 */
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    ESP_LOGI(TAG, "panel ready: SPI2 @%dMHz, BGR=%d, invert=%d, BLK(GPIO%d)=%d(active-%s)",
             LCD_PIXEL_CLK / 1000000, LCD_RGB_ORDER == LCD_RGB_ELEMENT_ORDER_BGR,
             LCD_INVERT_COLOR, LCD_PIN_BLK, LCD_BLK_ACTIVE_HIGH ? 1 : 0,
             LCD_BLK_ACTIVE_HIGH ? "high" : "low");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 对外接口
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 字库自检：压缩位图 + 未开 LV_USE_FONT_COMPRESSED = 文字全部消失（无日志！）
 * 生成字库加 `--no-compress`（本项目采用，省 CPU）；
 */
static void font_selfcheck(void)
{
#if !LV_USE_FONT_COMPRESSED
    const lv_font_t *fonts[] = { &ui_font_cn_12, &ui_font_cn_16, &ui_font_cn_20 };
    static const char *names[] = { "ui_font_cn_12", "ui_font_cn_16", "ui_font_cn_20" };
    for (int i = 0; i < 3; i++) {
        const lv_font_fmt_txt_dsc_t *fd = (const lv_font_fmt_txt_dsc_t *)fonts[i]->dsc;
        if (fd != NULL && fd->bitmap_format != LV_FONT_FMT_TXT_PLAIN) {
            ESP_LOGE(TAG, "%s 是压缩位图(format=%d)，但 LV_USE_FONT_COMPRESSED 没开"
                          " -> 文字会全部不显示。用 --no-compress 重新生成字库",
                     names[i], (int)fd->bitmap_format);
        }
    }
#endif
}

void lcd_ui_init(void)
{
    if (s_inited) {
        return;
    }

    /* ── 1. SPI 总线（屏幕独占；将来 SPI 触摸也可挂同一总线）── */
    spi_bus_config_t bus = {
        .mosi_io_num     = LCD_PIN_SDA,
        .miso_io_num     = -1,              /* ST7789 只写不读 */
        .sclk_io_num     = LCD_PIN_SCL,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* 取值 = draw buffer 字节数上限，不是整帧（我们没有整帧 buffer）。
         * 实际拿到的 buffer 可能更小，esp_lcd 会自动少切几刀。 */
        .max_transfer_sz = DRAW_BUF_BYTES_MAX,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;
    }

    /* ── 2. panel IO：DC 线与命令/数据分帧交给 esp_lcd 管 ── */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_PIN_CS,
        .dc_gpio_num         = LCD_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = LCD_PIXEL_CLK,
        /* 事务池深度：填 0 连初始化都过不去（底层拿它建 FreeRTOS 队列），
         * 回调也永远不会触发 → 界面卡死在第一帧（README.md 9.③）*/
        .trans_queue_depth   = 10,
        .on_color_trans_done = lcd_color_trans_done,
        .user_ctx            = &s_drv,      /* 直接把 LVGL 驱动句柄递给 ISR 回调 */
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_io_spi failed: %s", esp_err_to_name(err));
        return;
    }

    /* ── 3~4. ST7789 面板 + 背光 ── */
    err = panel_bringup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel bringup failed: %s -> 屏幕不会亮，查排线/供电/引脚",
                 esp_err_to_name(err));
        return;
    }

    /* ── 4.5 对话文本队列（必须在任何 lcd_ui_chat_* 调用之前存在）── */
    if (s_chat_q == NULL) {
        s_chat_q = xQueueCreate(LCD_CHAT_QUEUE_LEN, sizeof(lcd_chat_msg_t));
        if (s_chat_q == NULL) {
            ESP_LOGE(TAG, "chat queue create failed (%d B) -> 对话文字不上屏",
                     (int)(LCD_CHAT_QUEUE_LEN * sizeof(lcd_chat_msg_t)));
        }
    }

    /* ── 5~6. LVGL 引擎 + 主屏 ── */
    err = lvgl_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed: %s", esp_err_to_name(err));
        return;
    }

    s_inited = true;
    s_dirty  = true;   /* 让第一轮任务把影子变量里的最新状态刷上屏 */
    ESP_LOGI(TAG, "ST7789 %dx%d up: SCLK=%d MOSI=%d RST=%d DC=%d CS=%d BLK=%d @%dMHz",
             UI_HOR_RES, UI_VER_RES,
             LCD_PIN_SCL, LCD_PIN_SDA, LCD_PIN_RST,
             LCD_PIN_DC, LCD_PIN_CS, LCD_PIN_BLK,
             LCD_PIXEL_CLK / 1000000);

    /* 屏幕量的顺序不是硬约束：init 之前业务层调 set_status 也无害，
     * 影子变量只写不读，init 完第一帧自动带上最新状态（README.md 3.6）*/
}

/**
 * @brief 状态栏时钟：每秒对一次表，但只有"分钟"变化时才动控件
 *
 * 时间来源是 libc 的 time()（APP_STATE 的 SNTP 校时会把系统时间设准）。
 * 校时之前 time() 是 1970，此时显示 "--:--" —— 安全设备上显示假时间比不显示更糟。
 * 只在 lcd_ui 任务里调用（lv_label_set_text 不是线程安全的）。
 */
static char s_clock_shown[8] = "";   /* 已显示的文本，用来判断要不要重绘 */

static void clock_tick(void)
{
    char hhmm[8];

    time_t now = time(NULL);
    struct tm tmv;
    /* 2024-01-01 之后才算校时过（与 app_state.c 的 time_is_valid() 同口径）*/
    if (now > 1704067200 && localtime_r(&now, &tmv) != NULL) {
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(hhmm, sizeof(hhmm), "--:--");
    }

    if (strcmp(hhmm, s_clock_shown) != 0) {
        bool was_placeholder = (s_clock_shown[0] == '-' || s_clock_shown[0] == 0);
        snprintf(s_clock_shown, sizeof(s_clock_shown), "%s", hhmm);
        screen_home_set_clock(s_clock_shown);   /* set_text 自己会 invalidate 标签 */
        if (was_placeholder && hhmm[0] != '-') {
            ESP_LOGI(TAG, "status bar clock -> %s", hhmm);
        }
    }
}

/* 3.5：影子变量 → 真正改控件（只在 lcd_ui 任务里执行） */
static void ui_consume_state(void)
{
    if (!s_dirty) {
        return;
    }
    s_dirty = false;   /* 先清脏再取值：竞态最坏多刷一帧，无害 */

    lcd_status_t  st  = (lcd_status_t)s_status_req;
    lcd_emotion_t emo = (lcd_emotion_t)s_emotion_req;

    ESP_LOGD(TAG, "apply status=%s emotion=%s", status_name(st), emotion_name(emo));
    screen_home_set_status(st);
    screen_home_set_emotion(emo);
}

void lcd_ui_task(void *arg)
{
    (void)arg;

    s_lcd_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "lcd_ui task started (panel %s)", s_inited ? "OK" : "NOT initialized");
    if (!s_inited) {
        ESP_LOGE(TAG, "lcd_ui_task: panel not initialized, idling");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    TickType_t last_report = xTaskGetTickCount();
    uint32_t   last_flush  = 0;

    for (;;) {
        s_loop_count++;
        ui_consume_state();                     /* ① 影子变量 → 控件 */
        clock_tick();                           /* ①a 状态栏时钟 */

        /* ①b 对话文本 / 切屏 / 清空（都来自别的任务） */
        if (s_chat_clear_req) {
            s_chat_clear_req = false;
            screen_chat_clear();
        }
        if (s_chat_show_req) {
            s_chat_show_req = false;
            screen_chat_show();
        }
        if (s_chat_q != NULL) {
            lcd_chat_msg_t m;
            while (xQueueReceive(s_chat_q, &m, 0) == pdTRUE) {
                screen_chat_add((lcd_chat_role_t)m.role, m.text);
            }
        }
        uint32_t idle_ms = lv_timer_handler();  /* ② 渲染 + 自动 flush */

        /* 每 5 秒一行体检日志。**为什么需要**：面板初始化发生在开机
         * 200~400ms，用 `idf.py flash monitor` 时串口往往在芯片已经跑过
         * 那一段之后才连上，早期日志会整段丢失，于是"没有 LCD 日志"
         * 很容易被误读成"LCD 没初始化"。这一行把结论重复到这里：
         *   panel=OK + flushes 在涨   -> 面板通了、LVGL 在画
         *   panel=NOT init             -> lcd_ui_init() 失败或没跑
         *   panel=OK + flushes 一直是 0 -> 面板初始化了但一块像素都没送出
         *                                （LVGL 没触发重绘 / flush 应答卡住）*/
        TickType_t now = xTaskGetTickCount();
        if (now - last_report >= pdMS_TO_TICKS(5000)) {
            uint32_t fc = s_flush_count;
            /* clock=computed/label：computed 是 time() 算出来的，
             * label 是 LVGL 里真实保存的文本。两者不一致 -> 控件没更新；
             * 这行整个不出现 -> lcd_ui 任务卡死（大概率卡在等 DMA）。*/
            ESP_LOGI(TAG, "alive: panel=%s loop=%u flushes=%u (+%u/5s) clock=%s/%s internal=%u",
                     s_inited ? "OK" : "NOT init", (unsigned)s_loop_count, (unsigned)fc,
                     (unsigned)(fc - last_flush),
                     s_clock_shown, screen_home_get_clock(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            last_flush  = fc;
            last_report = now;
        }
        /* ⚠ 上下限都要卡死：LVGL 没有定时器就绪时返回 LV_NO_TIMER_READY
         * （0xFFFFFFFF），直接喂 vTaskDelay 会睡 71 分钟，屏幕永远停在第一帧
         * （HANDOFF 坑 33）。上限 30ms = LV_DISP_DEF_REFR_PERIOD；下限 5ms 防忙等。*/
        enum { UI_IDLE_MIN_MS = 5, UI_IDLE_MAX_MS = 30 };
        if (idle_ms == 0xFFFFFFFFu) {              /* LV_NO_TIMER_READY */
            if (!s_warned_no_timer) {
                s_warned_no_timer = true;
                ESP_LOGW(TAG, "lv_timer_handler 返回 LV_NO_TIMER_READY，按 %d ms 轮询",
                         UI_IDLE_MAX_MS);
            }
            idle_ms = UI_IDLE_MAX_MS;
        }
        if (idle_ms < UI_IDLE_MIN_MS) {
            idle_ms = UI_IDLE_MIN_MS;
        } else if (idle_ms > UI_IDLE_MAX_MS) {
            idle_ms = UI_IDLE_MAX_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(idle_ms));
    }
}

void lcd_ui_set_status(lcd_status_t st)
{
    if (st != s_status_req) {
        s_status_req = st;      /* 先写值再置脏，顺序反了会丢状态（README.md 3.5）*/
        s_dirty = true;
        /* 这个函数跑在别的任务里，顺手把 lcd_ui 任务的状态偷拍一张：
         *   loop 不动 / flushing=1  -> 卡在等 DMA 完成
         *   loop 不动 / flushing=0  -> 卡在别处（比如 SPI 队列满）
         *   state=3(Blocked) 且 loop 在涨 -> 正常睡眠 */
        lv_disp_t *d = lv_disp_get_default();
        ESP_LOGI(TAG, "status -> %s [lcd_ui loop=%u flush=%u flushing=%d state=%d]",
                 status_name(st),
                 (unsigned)s_loop_count, (unsigned)s_flush_count,
                 (d != NULL) ? (int)lv_disp_get_draw_buf(d)->flushing : -1,
                 (int)eTaskGetState(s_lcd_task));
    }
}

void lcd_ui_set_emotion(lcd_emotion_t emo)
{
    if (emo != s_emotion_req) {
        s_emotion_req = emo;
        s_dirty = true;
        ESP_LOGI(TAG, "emotion -> %s", emotion_name(emo));
    }
}

void lcd_ui_chat_add(lcd_chat_role_t role, const char *text)
{
    if (s_chat_q == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    lcd_chat_msg_t m;
    m.role = (uint8_t)role;
    utf8_copy_trunc(m.text, sizeof(m.text), text);
    if (xQueueSend(s_chat_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "chat queue full, drop %u bytes", (unsigned)strlen(text));
    }
}

void lcd_ui_chat_show(void)
{
    s_chat_show_req = true;
}

void lcd_ui_chat_clear(void)
{
    s_chat_clear_req = true;
}
