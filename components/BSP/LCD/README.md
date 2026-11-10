# LCD/LVGL 模块设计说明（ST7789 SPI · LVGL 8.3 移植层）

> 配套文档：`ui/DESIGN.md`（8 屏视觉规范 + LVGL 一一对应表）、`ui/prototype/index.html`（交互原型）
> 屏幕硬件：ST7789 SPI，240×320，RGB565；触摸芯片型号待定（第 6 节留接口）。
> 本期范围：**移植层 + 主屏第一卡**（= `ui/DESIGN.md:142-147` 移植顺序的第 1 步）。
>
> **实施状态（2026-11-07）**：本文是**写代码之前的设计讲解**，`lcd_ui.c` 仍是占位实现。
> 文中所有 file:line 引用以当前代码为准；esp_lcd 与 LVGL 的关键行为均已对照本机
> IDF 5.4 源码和 LVGL 8.3 源码核实（出处见文末附录），不是凭印象写的。

---

## 0. 先回答问题：要不要写接口？写哪个接口？

**对上的接口已经存在，不用写；对下的接口要写，而且只有三个点。**

这个项目的 UI 是三层结构：

```
┌──────────────────────────────────────────────────────────────┐
│ 业务层：AFE_SR / CHAT / MultiNet                              │
│   只调 lcd_ui_set_status() / lcd_ui_set_emotion()            │
│   一个 #include "lvgl.h" 都没有 —— 业务层不知道 LVGL 存在       │
├──────────────────────────────────────────────────────────────┤
│ 接口层：lcd_ui.h —— 已经写好，签名别动                        │
│   4 个函数 + 2 个枚举（lcd_ui.h:22-36, 39-49）                │
├──────────────────────────────────────────────────────────────┤
│ 实现层：LCD/ 目录内部（本文要写的全部内容）                     │
│   LVGL（界面怎么画）+ 移植层（LVGL 怎么到屏幕）                │
│   + esp_lcd（SPI 时序/命令）→ ST7789 硬件                    │
└──────────────────────────────────────────────────────────────┘
```

"移植层"听起来玄，其实 LVGL 把它收敛成了**你必须提供的三个接口点**：

| # | 接口点 | 方向 | 是什么 |
| --- | --- | --- | --- |
| 1 | `flush_cb` | LVGL → 屏幕 | LVGL 把一块像素算好放进 buffer 后，调**你写的**这个函数："这块请送出去"。整个移植层唯一必须写对的函数 |
| 2 | tick | 心跳 | LVGL 自己不计时，需要你定期告诉它"过了 1ms"（`lv_tick_inc`） |
| 3 | `lv_timer_handler()` 循环 | 泵 | LVGL 是纯库不是任务，必须有人反复调它它才干活——这个"有人"就是 `lcd_ui_task` |

除了这三点，剩下的（主题、卡片、字库、8 个屏）都是普通 LVGL 应用代码，不叫接口。

**为什么这样分层是对的**：AFE_SR 和 chat.c 现在（LCD 还是 stub 的今天）就已经在调
`lcd_ui_set_status()` 了——9 处调用点：afe_sr.c:218-219、255-256、435、483、489，
chat.c:135、341。等你把 stub 换成真实现，**这 9 处一行都不用改**。这就是接口层先行的意义。

---

## 1. 现状盘点（2026-11-07）

| 文件 | 状态 | 说明 |
| --- | --- | --- |
| `lcd_ui.h` | ✅ 接口已定 | **别动**。两个枚举 + 4 个函数；头文件注释里还留着移植清单（lcd_ui.h:12-16） |
| `lcd_ui.c` | ⚠️ 占位实现 | init 只打日志（lcd_ui.c:41-44），task 空转（lcd_ui.c:53-59）；本文 3 节逐函数替换其**内部** |
| `main/main.c` | ❌ 没接线 | app_main（main.c:32-67）没调 `lcd_ui_init()`，没建 `lcd_ui_task`（见 3.6） |
| `main/idf_component.yml` | ❌ 无 lvgl | 17-19 行只有 openai / mdns / esp-sr（见 4.1） |
| `components/BSP/CMakeLists.txt` | ❌ 注释着 | 44-48 行 `esp_lcd` / `lvgl__lvgl` 等解开（见 4.2） |
| `sdkconfig.defaults` | ✅ 有利条件 | 8MB Octal PSRAM 已开（`SPIRAM_USE_MALLOC`），将来大 buffer 有地方放 |

---

## 2. 总览：一次状态切换怎么变成屏幕像素

以 `afe_sr.c:255` 的 `lcd_ui_set_status(LCD_STATUS_LISTENING)` 为例，跨任务的完整时序：

```
afe_fetch 任务(核0 prio20)                 lcd_ui 任务(prio8)
──────────────────────────                ──────────────────────────────────────
lcd_ui_set_status(LISTENING)
  ├ s_status_req = LISTENING               for (;;) {
  ├ s_dirty = true                             ① ui_consume_state()
  └ 返回（不等屏幕！调用方零负担）                  │ dirty? → 把状态画到控件上
                                                 ② lv_timer_handler()
                                                    │ 重算脏区 → 渲染进 draw buffer
                                                    │ → 调你的 flush_cb
                                                    │    → draw_bitmap 排队 DMA
                                                    │    → 立即返回
                                                    └ SPI 发完 → ISR 回调
                                                      → lv_disp_flush_ready()
                                                 ③ vTaskDelay(到下一帧)
                                             }
```

两个任务**只通过影子变量交接，没有锁、没有队列**——这正是兑现 lcd_ui.h:44
"任意任务调用，下一帧生效"承诺的方式（原理见 3.5）。

一次 flush 的字节账：局部重绘 240×40 行 = 19200 B，SPI @ 40 MHz ≈ **4 ms/块**；
整屏 240×320 = 150 KB ≈ 31 ms。LVGL 只重画脏区，静止界面几乎零流量。

---

## 3. 移植层逐函数设计

### 3.0 引脚宏（唯一要你自己查数的地方）

```c
/* lcd_ui.c 顶部。对着你的屏排线丝印填，丝印名在括号里 */
#define LCD_PIN_MOSI  (-1)  /* SDI / SDA */
#define LCD_PIN_CLK   (-1)  /* SCL / SCK */
#define LCD_PIN_CS    (-1)  /* CS  */
#define LCD_PIN_DC    (-1)  /* DC / RS —— ST7789 必须有，命令/数据切换 */
#define LCD_PIN_RST   (-1)  /* RES；有的模块板载 RC 复位电路，可填 -1 */
#define LCD_PIN_BLK   (-1)  /* BLK / BL 背光 */
#define LCD_SPI_HOST  SPI2_HOST
```

先填对 DC 和 CS，这俩错了屏幕完全没反应；RST/BLK 错了屏幕能收到命令但点不亮。

### 3.1 lcd_ui_init()：六步

```c
void lcd_ui_init(void)
{
    /* ── 第 1 步：SPI 总线（屏幕独占；将来 SPI 触摸芯片也可挂同一总线）── */
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 320 * 2,   /* 见下方"为什么"③ */
    };
    spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

    /* ── 第 2 步：panel IO —— esp_lcd 替你管 DC 线和命令/数据分帧 ── */
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = LCD_PIN_DC, .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,        /* 起步 40MHz，花屏就降 20MHz */
        .spi_mode = 0,
        .trans_queue_depth = 10,            /* 见"为什么"①：必须有，回调才触发 */
        .on_color_trans_done = lcd_ui_color_trans_done,   /* 3.2 */
        .user_ctx = &s_drv,                 /* 直接把 LVGL 驱动句柄递给回调 */
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io, &s_io);

    /* ── 第 3 步：ST7789 面板（IDF 自带驱动，不用自己写初始化命令序列）── */
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   /* 见"为什么"④ */
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_swap_xy(s_panel, false);        /* 240(宽)×320(高) 竖屏 */
    esp_lcd_panel_mirror(s_panel, false, false);  /* 装反/镜像再调这四个 */
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* ── 第 4 步：背光（先普通 GPIO 拉高，要调亮度时换 LEDC/PWM）── */
    gpio_config_t blk = { .pin_bit_mask = 1ULL << LCD_PIN_BLK, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&blk);
    gpio_set_level(LCD_PIN_BLK, 1);

    /* ── 第 5 步：LVGL 引擎接上"屏幕代理" ── */
    lv_init();
    s_draw_mem = heap_caps_malloc(240 * 40 * sizeof(lv_color_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  /* 见"为什么"② */
    lv_disp_draw_buf_init(&s_draw_buf, s_draw_mem, NULL, 240 * 40);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = 240;  s_drv.ver_res = 320;
    s_drv.flush_cb = lcd_ui_flush_cb;         /* 3.2 */
    s_drv.draw_buf = &s_draw_buf;
    s_drv.wait_cb  = lvgl_wait_cb;            /* 见"为什么"⑤ */
    s_drv.antialiasing = false;
    lv_disp_drv_register(&s_drv);

    /* ── 第 6 步：tick + 界面根对象 ── */
    lvgl_tick_start();                        /* 3.3 */
    ui_theme_init();                          /* 5.1 */
    screen_home_create();                     /* 5.2，内部 lv_scr_load */
}
```

**五个"为什么"**

① **`trans_queue_depth = 10` 是回调能触发的前提**。esp_lcd 的彩色数据分片后走
`spi_device_queue_trans` 异步 DMA（已核源码：esp_lcd_panel_io_spi.c:353-395，
`tx_buffer` 直接指向你的 buffer，**不拷贝**），事务池就是按这个数 calloc 的
（esp_lcd_panel_io_spi.c:73）。填 0 连初始化都过不去——底层拿它建 FreeRTOS
事务队列，长度 0 创建失败，`esp_lcd_new_panel_io_spi` 直接返回 ESP_ERR_NO_MEM。
顺带的好处：池子满时 esp_lcd 会阻塞等前一片发完再收下一片（:361-363），
**自带背压**，LVGL 画得再快也冲不垮 DMA。

② **draw buffer 为什么是 240×40 行 ≈ 19 KB、为什么放内部 RAM**。
1 行（480 B）DMA 效率差；整帧（150 KB）浪费；40 行是甜点。
ESP32 经典款 SPI DMA 根本碰不到 PSRAM；ESP32-S3 的 GDMA 虽然能经 cache
访问 PSRAM，但每次传输都要多一轮 cache 同步，吞吐打折——**别拿 PSRAM 做
draw buffer**。本项目内部 RAM 充裕，19 KB 毫无压力；PSRAM（sdkconfig 已开
8MB）留给将来的大东西（截图、字库扩展）。

③ **`max_transfer_sz` 必须设**。SPI 总线默认单次 DMA 上限 4092 字节，不设的话
esp_lcd 把你的 40 行切成 5 小片（能跑，但每片都带一次 CS/命令开销）。设成一帧大小，
一整块 buffer 一片发完。

④ **BGR 不是笔误**。市面上绝大多数 ST7789 模块的面板是 BGR 像素序，配 RGB 会红蓝反。
先按 BGR 写，上电发现红蓝反了再换 `LCD_RGB_ELEMENT_ORDER_RGB`（见 7.② 的现象表）。

⑤ **`wait_cb` 是为了别空转**。LVGL 8.3 在单缓冲模式下、把下一块脏区画进 buffer
之前，会自旋等上一块的 DMA 发完（`while(draw_buf->flushing)`，已核 LVGL 8.3 源码
lv_refr.c `refr_area_part()`）。默认这个自旋是真烧 CPU 的忙等，注册一个
`vTaskDelay(1)` 的 wait_cb 就变成了让出 CPU 的休眠：

```c
static void lvgl_wait_cb(lv_disp_drv_t *drv)   /* LVGL 等 DMA 时调，别让它空转 */
{
    (void)drv;
    vTaskDelay(1);    /* 1 tick ≈ 让出 CPU；DMA 一块才 4ms，睡一拍足够 */
}
```

### 3.2 flush_cb 与 trans_done 回调（移植层唯一"必须写对"的地方）

先说数据通路（全部核过源码，出处见附录）：

```
flush_cb（跑在 lcd_ui 任务）
  esp_lcd_panel_draw_bitmap(x1, y1, x2+1, y2+1, color_map)
      └ 内部：分片 → spi_device_queue_trans 排队 → 立即返回
      └ DMA 在后台从 color_map 读数据发 SPI（buffer 仍被引用着！）

SPI 发完最后一个分片
  └ SPI ISR → esp_lcd post_cb → 你的 lcd_ui_color_trans_done()
      └ lv_disp_flush_ready(drv)   ← 告诉 LVGL：buffer 可以复用了

LVGL 下一块脏区渲染前
  └ while(draw_buf->flushing) wait_cb()   ← 8.3 自带，等 ready
```

于是两个函数的**正确形态**是：

```c
/**
 * flush 回调：LVGL 渲染完一块脏区就调这里。
 * 只负责把这块排进 DMA 队列，立即返回 —— 绝不在这里等发送完成。
 * （等的工作由 LVGL 的 flushing 标志 + 3.1 的 wait_cb 承担）
 */
static void lcd_ui_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    /* LVGL 的 area 是含头含尾 [x1,x2]，esp_lcd 是含头不含尾 [x1,x2)，
     * 所以右/下界 +1。差这一像素，现象是最后一行/列永远是旧内容。 */
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
    /* 注意：这里没有 lv_disp_flush_ready() —— 它在下面的回调里 */
}

/**
 * DMA 发送完成回调（lcd_ui.h:16 预留的那个函数）。
 * 跑在 SPI ISR 上下文里（已核 spi_master.c:796-808），必须 ISR 安全：
 * 只能做这件事 —— lv_disp_flush_ready 在 LVGL 8.3 里就是两次普通赋值。
 */
static bool lcd_ui_color_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *drv = user_ctx;      /* 3.1 第 2 步 .user_ctx = &s_drv 递进来的 */
    lv_disp_flush_ready(drv);
    return false;                       /* false = 不需要唤醒更高优先级任务 */
}
```

**为什么不用信号量**（我设计时对比过两种方案，结论给你留档）：

| 方案 | 做法 | 结论 |
| --- | --- | --- |
| 信号量 | flush_cb 里 `xSemaphoreTake` 等，ISR 里 Give | 能跑，但 Give 必须用 FromISR 变体，而 esp_lcd 的回调上下文在"ISR 或任务"两可（spi_master.c:796 原注释），FromISR 用错上下文是 UB —— 平白引入一个坑 |
| flush_ready 放回调（本文采用） | flush_cb 纯排队即返 | IDF 官方 spi_lcd_touch 示例同款（spi_lcd_touch_example_main.c:77-82, 114-126）；无锁、无 UB 风险，LVGL 的 flushing 标志机制本来就是为这个设计的 |

**单缓冲会不会撕裂**：不会，前提就是本文这套组合——flush_cb 返回 ≠"buffer 解放"，
LVGL 内部 `flushing` 标志挡着，`wait_cb` 让等待变成休眠。撕裂只有一种来源：
你自己手贱在 flush_cb 里加了 `lv_disp_flush_ready()`。

### 3.3 tick：给 LVGL 一个时钟

```c
static void lv_tick_cb(void *arg) { (void)arg; lv_tick_inc(1); }

static void lvgl_tick_start(void)
{
    const esp_timer_create_args_t args = {
        .callback = &lv_tick_cb, .name = "lv_tick",
    };
    esp_timer_handle_t t;
    esp_timer_create(&args, &t);
    esp_timer_start_periodic(t, 1000);   /* 1ms = 1000μs */
}
```

LVGL 的动画、长按判定、`lv_timer` 全靠这个时基。用 esp_timer 硬件定时器发，
不占任务；1 ms 粒度足够（IDF 官方示例用的 2ms，同理）。
**忘了接 tick 的症状**：界面能画出来，但动画不动、长按 3 秒永远不触发。

### 3.4 lcd_ui_task()：UI 的心脏

```c
void lcd_ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        ui_consume_state();                    /* 3.5：影子变量 → 真正改控件 */
        uint32_t idle_ms = lv_timer_handler(); /* 返回"下次再来"的建议间隔 ms */
        vTaskDelay(pdMS_TO_TICKS(idle_ms < 5 ? 5 : idle_ms));
    }
}
```

- **栈 8192**：LVGL 渲染一帧的局部开销不小，4KB 常常侥幸能跑，加上字体渲染/将来
  图片解码就爆栈（爆栈症状是随机重启，backtrace 深处的函数每次还不一样）。
  和 ai_chat 任务同规格（main.c:62）。
- **优先级 8，不绑核**：低于 afe_feed(21)/afe_fetch(20)/ai_chat(12)。UI 掉一帧没人
  察觉，音频掉一帧就是爆音。优先级才是关键，绑不绑核无所谓。
- **5ms 下限**：界面静止时 `lv_timer_handler` 可能返回 0（只有 tick 这种小活），
  不设下限就变成纯忙等烧 CPU。

### 3.5 set_status / set_emotion：跨线程交接的实现

头文件承诺"任意任务调用，下一帧生效"（lcd_ui.h:44）。实现方案对比：

| 方案 | 做法 | 评价 |
| --- | --- | --- |
| FreeRTOS 队列 | 每次调用 push 一个事件 | 正统但多余：状态是"最新者胜"，天然适合单变量覆盖写 |
| **影子变量 + 脏标志（采用）** | 见下 | 枚举是 4 字节对齐的 32 位写，Xtensa 上天然原子；谁最后写谁生效，语义正好是"下一帧生效" |

```c
/* —— 业务任务只碰这三个变量 —— */
static volatile lcd_status_t  s_status_req  = LCD_STATUS_STANDBY;
static volatile lcd_emotion_t s_emotion_req = LCD_EMO_NORMAL;
static volatile bool          s_dirty       = false;

void lcd_ui_set_status(lcd_status_t st)    /* 任意任务，任意时刻 */
{
    if (st != s_status_req) {
        s_status_req = st;
        s_dirty = true;                    /* 先写值再置脏，顺序别反 */
    }
}

void lcd_ui_set_emotion(lcd_emotion_t emo)
{
    if (emo != s_emotion_req) {
        s_emotion_req = emo;
        s_dirty = true;
    }
}

/* —— lcd_ui_task 每轮开头调（3.4 ①）—— */
static void ui_consume_state(void)
{
    if (!s_dirty) return;
    s_dirty = false;                       /* 先清脏再取值：竞态最坏多刷一帧，无害 */
    screen_home_set_status((lcd_status_t)s_status_req);
    screen_home_set_emotion((lcd_emotion_t)s_emotion_req);
}
```

竞态分析（为什么敢无锁）：`s_dirty` 清零瞬间业务任务又置脏 → 最坏结果
是 UI 任务下一轮多消费一次，画出来的还是最新值，**没有丢状态窗口**——
丢状态只发生在"读值先于置脏"的顺序里，所以写入顺序必须是 值→脏。
不要用关中断去"修"这个竞态，它不需要修。

stub 版 `lcd_ui_set_status` 里的日志可以保留（换成 LOGD），初调有用。

### 3.6 app_main 接线（main.c 现在缺这两行）

```c
#include "lcd_ui.h"          /* main.c 头部 */

/* app_main 里，建议放在第 3 步（ESP-NOW）之后、语音链路（第 4 步）之前：
 * 屏幕早一秒亮，用户早一秒有"开机了"的实感 */
lcd_ui_init();
if (xTaskCreate(lcd_ui_task, "lcd_ui", 8192, NULL, 8, NULL) != pdPASS) {
    ESP_LOGE(TAG, "create lcd_ui task failed");
}
```

顺序**不是硬约束**是这个设计的又一个好处：就算 `lcd_ui_init()` 还没跑，
AFE 先调了 `lcd_ui_set_status()` 也无害——影子变量只写不读，init 完成后
第一帧自动带上最新状态。

---

## 4. 依赖与配置（三处，lcd_ui.h:12-16 注释的展开）

### 4.1 main/idf_component.yml：加 lvgl

在 17-19 行的依赖列表末尾加一行（注意缩进两个空格，和上面三项对齐）：

```yaml
  lvgl/lvgl: "~8.3.0"
```

**为什么锁 8.3 不是 9.x**：LVGL 9.0 把显示驱动 API 整个翻新了
（`lv_disp_drv_t` → `lv_display_create()`，flush_cb 签名都变了）。
本文所有代码、`ui/DESIGN.md:114-128` 对照表里的 API 名，全部是 8.3 的。
头文件注释（lcd_ui.h:14）写的 `~8.3.0` 就是这个意思，`~` 允许 8.3.x 补丁版。

### 4.2 components/BSP/CMakeLists.txt：解开注释

删掉 44-48 行的 TODO 注释块和两个 `#`，让 requires 变成：

```cmake
                mbedtls
                esp-sr
                esp_lcd
                lvgl__lvgl
                )
```

组件名是 **`lvgl__lvgl`** 不是 `lvgl`——component manager 拉下来的组件在构建
系统里的名字带命名空间前缀（头文件注释 lcd_ui.h:15 写对了，照抄即可）。

### 4.3 sdkconfig.defaults：LVGL 字节序

```ini
# LVGL: ST7789 按大端收 RGB565（IDF 示例原注释："because SPI LCD is big-endian,
# we need to swap the RGB bytes order"），LVGL 8.3 用编译期 swap 对齐
CONFIG_LV_COLOR_16_SWAP=y
```

原理一句话：LVGL 内存里 RGB565 默认小端（低字节在前），SPI 按字节流顺序发，
先发出去的会被 ST7789 当高字节——所以把 LVGL 生成的字节序换过来，DMA 可以
原样直发，一个字节都不用 CPU 搬。

改动后 `idf.py reconfigure`（拉取 lvgl 组件）→ `idf.py build`。
LVGL 其余配置走 menuconfig（`CONFIG_LV_*`），本期默认值基本都对，
只需要知道两个：`LV_COLOR_DEPTH` 默认 16（别动）；`LV_MEM_SIZE` 默认 32KB
（LVGL 内部堆，本期一张卡够用，8 屏全上时不够再提到 64KB）。

---

## 5. 主屏第一卡（DESIGN.md 移植顺序第 1 步）

### 5.0 文件结构（本期只建这四个，别一次建满 8 屏）

```
components/BSP/LCD/
  lcd_ui.h / lcd_ui.c        移植层（3 节）+ 接口层（不动）
  ui_theme.h / ui_theme.c    色值宏 + 字体指针
  screen_home.h / screen_home.c  主屏（本期：一张守护状态卡）
  lv_font_cn_16.c            字库生成物（5.3）
```

`ui/DESIGN.md:131-138` 的完整分层（ui_nav / ui_widgets / screen_*×8）是 8 屏
全上时的终态。第一卡阶段 **ui_nav 还不存在**——没有第二个屏，谈不上跳转。

### 5.1 ui_theme：把设计规范变成宏

色值照抄 `ui/DESIGN.md` 第 3 节的表（54-72 行），一比一：

```c
/* ui_theme.h */
#include "lvgl.h"

#define COL_BG      lv_color_hex(0x0B1116)   /* 屏幕底色 */
#define COL_SURFACE lv_color_hex(0x151D24)   /* 卡片 */
#define COL_LINE    lv_color_hex(0x2B3842)   /* 卡片描边 1px */
#define COL_TEXT    lv_color_hex(0xF2F7FA)   /* 主文字 */
#define COL_MUTED   lv_color_hex(0xA8B9C4)   /* 次文字 */
#define COL_ACCENT  lv_color_hex(0x3DDC97)   /* 安心绿：正常/在线/已送达 */
#define COL_AMBER   lv_color_hex(0xFFB020)   /* 需要确认/进行中 */
#define COL_RED     lv_color_hex(0xFF5A5F)   /* 只用于呼救与告警 */

extern lv_font_t ui_font_cn_16;   /* lv_font_cn_16.c 里的生成物 */
```

DESIGN.md 的字体纪律现在只生效一条："层级靠**字号 + 颜色**，不靠字重"
（中文不做粗体——每档粗体都是一份新字库）。本期只生成 16 一档；
"监护中"按设计是 20px，第一卡先用 16 顶着也行，强迫症就再生成一档 20（5.3 同一条命令换个 `--size`）。

### 5.2 screen_home：第一张卡

内容对照原型 `ui/prototype/index.html` 的 `s-home` 组（166 行起）和截图
`ui/shots/home.png`：绿环 + 对勾、**监护中**（主标题）、**已静默 12 分钟**
（副行）、电量 82% + 信号。本期先把环 + 两行文字 + 一个点击区做出来，
电量行等接状态时再加。

```c
/* screen_home.c */
#include "screen_home.h"
#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "SCR_HOME";

static lv_obj_t *s_scr_home;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_silence;
static lv_obj_t *s_arc_guard;

void screen_home_create(void)
{
    /* 根屏：底色 BG（原型 s-home 的 <rect width=240 height=320>） */
    s_scr_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_home, COL_BG, 0);

    /* 守护状态卡：x/y/w/h 对着原型里这张卡的 rect 属性抄（x=8, y=8, w=224 起步），
     * rx=14 → radius 14，fill=SURFACE，stroke=LINE 1px（DESIGN.md:115） */
    lv_obj_t *card = lv_obj_create(s_scr_home);
    lv_obj_set_pos(card, 8, 8);
    lv_obj_set_size(card, 224, 150);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, COL_SURFACE, 0);
    lv_obj_set_style_border_color(card, COL_LINE, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);      /* DESIGN.md:115：子对象绝对定位 */

    /* 绿色进度环（原型里那张 141px 的环）：lv_arc。
     * DESIGN.md:118 的完整配方：bg_angles(0,270)、range(0,100)，
     * INDICATOR 部分 arc_width 5 + arc_rounded，圆头 */
    s_arc_guard = lv_arc_create(card);
    lv_obj_set_size(s_arc_guard, 64, 64);      /* 第一卡先缩小画，位置后期对齐原型 */
    lv_obj_align(s_arc_guard, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_arc_set_bg_angles(s_arc_guard, 0, 270);
    lv_arc_set_range(s_arc_guard, 0, 100);
    lv_arc_set_value(s_arc_guard, 100);        /* 监护中 = 满环 */
    lv_obj_set_style_arc_color(s_arc_guard, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_guard, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_guard, true, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc_guard, NULL, LV_PART_KNOB);        /* 去掉旋钮 */
    lv_obj_clear_flag(s_arc_guard, LV_OBJ_FLAG_CLICKABLE);       /* 环不吃点击 */
    lv_arc_set_mode(s_arc_guard, LV_ARC_MODE_NORMAL);

    /* 主标题 20px（暂用 16 字库）+ 副行 16px */
    s_lbl_title = lv_label_create(card);
    lv_label_set_text(s_lbl_title, "监护中");
    lv_obj_set_style_text_color(s_lbl_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_title, &ui_font_cn_16, 0);
    lv_obj_align_to(s_lbl_title, s_arc_guard, LV_ALIGN_OUT_RIGHT_TOP, 12, 6);

    s_lbl_silence = lv_label_create(card);
    lv_label_set_text(s_lbl_silence, "已静默 12 分钟");   /* ASCII 在 5.3 一并收进字库 */
    lv_obj_set_style_text_color(s_lbl_silence, COL_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_silence, &ui_font_cn_16, 0);
    lv_obj_align_to(s_lbl_silence, s_arc_guard, LV_ALIGN_OUT_RIGHT_TOP, 12, 30);

    /* 触摸验证钩子：本期只验证"点得到"，不做跳转 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_card_click, LV_EVENT_CLICKED, NULL);

    lv_scr_load(s_scr_home);                   /* 第一屏直接加载 */
}

static void on_card_click(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "home card clicked");        /* 串口看到这行 = 触摸链路通了 */
}

/* 状态 → 控件（3.5 的 ui_consume_state 调用）。
 * 本期先做最简单的文字变化；表情走图标/动画是二期的活 */
void screen_home_set_status(lcd_status_t st)
{
    if (!s_lbl_silence) return;
    switch (st) {
    case LCD_STATUS_LISTENING:
        lv_label_set_text(s_lbl_silence, "聆听中...");
        break;
    case LCD_STATUS_REPLYING:
        lv_label_set_text(s_lbl_silence, "回复中...");
        break;
    default:
        lv_label_set_text(s_lbl_silence, "已静默 12 分钟");
        break;
    }
}

void screen_home_set_emotion(lcd_emotion_t emo)
{
    (void)emo;   /* 本期表情先不画，接口留好 */
}
```

坐标纪律（DESIGN.md:110 的承诺）：原型里每个元素的 `x/y/width/height` 直接就是
`lv_obj_set_pos/size` 的参数，不用换算。我代码里的数字是起步示意，
正式实现对着 `index.html` 的属性抄。

### 5.3 中文字库：lv_font_conv 子集（一次性操作）

**绝不能用全字库**：Noto Sans SC 全量 16px ≈ 1MB+，factory 分区装不下也浪费。
只收这个 UI 用到的字（DESIGN.md:154 的做法）：

```bash
# 输入字体：NotoSansSC-Regular.otf（思源黑体同源，开源可商用）
# ASCII 区间一起收：数字/百分号/省略号就不用再嵌 Montserrat
npx lv_font_conv --bpp 4 --size 16 --font NotoSansSC-Regular.otf \
  -r 0x20-0x7F \
  --symbols "监护中已静默分钟聆听回复在线离线对讲紧急呼救设置事件应用商店" \
  --format lvgl --lv-font-name ui_font_cn_16 -o lv_font_cn_16.c
```

- `--symbols` 只收列出的字 → 生成物几十 KB。**加新界面文字要回来重新生成**，
  忘了的现象是屏上出豆腐块 □□。
- 生成的 `lv_font_cn_16.c` 拷进 LCD/ 目录即可——BSP 的 `SRC_DIRS` 会自动收编
  （CMakeLists.txt:8 已注册 LCD 目录），不需要再改构建脚本。
- `lv_font_conv` 环境：有 npm 就 `npx` 直接跑；不想装 node 用网页版
  https://lvgl.io/tools/fontconverter 同参数。
- 将来"监护中"上 20px：同一条命令 `--size 20` 再来一遍，生成 `ui_font_cn_20`。

---

## 6. 触摸（本期留接口，芯片型号未定）

你的屏有没有触摸、是什么芯片（I2C 的 FT6336/CST816，还是 SPI 的 XPT2046）还没确认。
第一卡阶段如果没触摸，**验证"LVGL 活着"用 BOOT 键**（GPIO0，key.c 已在扫描）：
临时在 `ui_consume_state()` 里轮询 `key_consume_wake_request()`，点了就 `lv_arc_set_value()` 动一下环。

将来接触摸，就是 LVGL 的**第二个移植接口 indev**（和 3.2 的 disp 对称）：

```c
/* lv_indev_drv 三件套：init → read_cb → register */
static lv_indev_drv_t s_indev;
lv_indev_drv_init(&s_indev);
s_indev.type = LV_INDEV_TYPE_POINTER;
s_indev.read_cb = touch_read_cb;   /* LVGL 每 30ms 调一次问"现在按着哪" */
lv_indev_drv_register(&s_indev);

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_get_xy(&x, &y)) {              /* 读触摸芯片：esp_lcd_touch 或手写 */
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
```

坐标填对之后，**点中哪个控件是 LVGL 自动分发的**——5.2 里那行
`add_event_cb` 会自己收到 CLICKED，不需要你做命中检测。
推荐路线：用 espressif 的 `esp_lcd_touch` + 对应型号组件（如 `esp_lcd_touch_ft6x36`），
它替你处理寄存器和坐标校准；I2C 触摸芯片走 `esp_lcd_new_panel_io_i2c` 造一个
IO 再传给 touch 组件。**注意**：本项目目前没有 I2C 总线（已 grep 证实），
I2C 触摸意味着新开一路 I2C 初始化，别和将来的音频设备撞总线。

---

## 7. 分级验证（每一步都不依赖下一步）

### ① 编译通过

```bash
source esp-idf-env.sh
idf.py reconfigure     # 拉取 lvgl/lvgl ~8.3.0 进 managed_components
idf.py build
```

| 失败现象 | 原因 |
| --- | --- |
| `fatal error: lvgl.h: No such file` | 4.2 requires 没解开，或 4.1 没加依赖 |
| CMake 报找不到组件 | 名字写成了 `lvgl`，应为 `lvgl__lvgl` |
| `esp_lcd_new_panel_st7789` 未定义 | `esp_lcd` 没解开 |

### ② 屏幕现象对照表（上电第一件事对着这张表排错）

| 现象 | 原因 | 修法 |
| --- | --- | --- |
| 全黑，背光也不亮 | BLK 没拉高 / 接错 | 3.1 第 4 步 |
| 背光亮但全黑 | `disp_on_off(true)` 没调 / RST 没释放 | 3.1 第 3 步；看 esp_lcd 日志有没有 init 报错 |
| 红蓝反 | 面板是 RGB 序不是 BGR | `.rgb_ele_order` 换 `LCD_RGB_ELEMENT_ORDER_RGB` |
| 颜色像负片/怪异灰阶 | 字节序没 swap | 4.3 `CONFIG_LV_COLOR_16_SWAP=y` |
| 画面左右/上下镜像 | 模块安装方向 | `swap_xy` / `mirror` 四个参数试 |
| 最后一行/列永远不更新 | 坐标忘了 +1 | 3.2 |
| 花屏 / 斜条纹 | buffer 不是 DMA 内存 / max_transfer_sz 没设 | 3.1 ②③ |
| 界面冻结但日志还在跑 | `lv_timer_handler` 没被调 / 栈溢出 | 3.4；`xTaskGetStackHighWaterMark` 查 |

### ③ 字库 / 颜色 / 触摸三点核对（DESIGN.md:144 的验收原话）

- 字库：卡上中文是文字不是 □□；
- 颜色：环是 `#3DDC97` 的绿，卡片描边是 `#2B3842`；
- 触摸：点卡片串口出 `home card clicked`（无触摸则走 BOOT 键路径）。

### ④ 接业务状态（临时注一行）

`app_main` 末尾临时加 `lcd_ui_set_status(LCD_STATUS_LISTENING);`，烧录后
副行应变成"聆听中..."。验证完删掉，让 AFE 自然驱动（afe_sr.c:255 唤醒时会来调）。

---

## 8. 与 DESIGN.md 的衔接（本期之后做什么）

本期完成 = `ui/DESIGN.md:142-147` 四步移植顺序的**第 1 步**。之后：

| 步骤 | 内容 | 涉及新文件 |
| --- | --- | --- |
| 2 | 导航 + 8 屏空壳：返回栈、底部导航（DESIGN.md:114, 126） | `ui_nav.c/h`，`screen_*.c` ×7 |
| 3 | 按屏补内容：对讲 → 呼救 → 收到告警 → 事件（P0 四屏）→ 设置/商店/AI | 各 screen 文件 |
| 4 | 接真实状态：静默计时、对端在线、电量、告警中 | 扩展 lcd_ui.h |

**lcd_ui.h 的扩展规则**（提前立好，防止返工）：加新的枚举值、加新函数**可以**；
改已有 4 个函数的签名、改已有枚举值的名字**不行**——9 个调用点已经在编译单元里了。
第 4 步大概率要加"电量百分比""对端在线"这类接口，到时按
`lcd_ui_set_battery(int pct)` 这种新函数往上加。

---

## 9. 坑点清单（写代码前过一遍）

| # | 坑 | 现象 | 对策 |
| --- | --- | --- | --- |
| 1 | draw_bitmap 坐标忘了 +1 | 末行/末列永远旧内容 | 3.2 |
| 2 | 在 flush_cb 里调 `lv_disp_flush_ready` | 花屏/撕裂（DMA 还在读 buffer） | 3.2，ready 只能放 trans_done 回调 |
| 3 | trans_queue_depth 填 0 | 回调永远不触发，界面卡死在第一帧 | 3.1 ①，填 10 |
| 4 | draw buffer 放 PSRAM | 慢、cache 噪声 | 内部 RAM，19KB 够（3.1 ②） |
| 5 | `CONFIG_LV_COLOR_16_SWAP` 没开 | 颜色像负片 | 4.3 |
| 6 | BGR/RGB 序没试 | 红蓝反 | 3.1 ④ |
| 7 | 忘接 tick | 动画不动、长按判定失效 | 3.3 |
| 8 | `lv_timer_handler` 间隔 0 忙等 | CPU 占满拖累音频 | 3.4 5ms 下限 |
| 9 | UI 任务栈 4096 | 随机重启，backtrace 位置漂移 | 8192 |
| 10 | UI 任务优先级 ≥ ai_chat | 播放卡顿爆音 | prio 8 |
| 11 | LVGL 全字库 | 1MB+ flash，分区装不下 | 5.3 子集 --symbols |
| 12 | 加了新文字没重新生成字库 | 屏上豆腐块 □□ | 5.3 |
| 13 | 业务任务里直接调 `lv_xxx()` 改控件 | 随机崩溃（LVGL 非线程安全） | 跨线程只写影子变量，lv_ 调用只出现在 lcd_ui 任务（3.5） |
| 14 | 用锁去"修" s_dirty 竞态 | 白费功夫还引入死锁风险 | 3.5 竞态分析，顺序对了就无锁 |
| 15 | 改 lcd_ui.h 已有签名 | 9 个调用点编译炸 | 8 节扩展规则 |

---

## 附：本文技术事实的源码出处

写设计文档最怕凭印象写 API。以下结论都核对过原始代码，写实现时可以直接翻：

| 结论 | 出处 |
| --- | --- |
| 色块分片异步 DMA、`tx_buffer` 指向用户 buffer 不拷贝、末片才置回调标志（:376）、池满自带背压（:361-363） | `$IDF_PATH/components/esp_lcd/spi/esp_lcd_panel_io_spi.c:353-395` |
| 事务池按 `trans_queue_depth` calloc；`.queue_size` = 同值 | 同文件 :73, :84, :112 |
| `on_color_trans_done` / `user_ctx` 是 io 配置字段（5.4 仍支持） | `$IDF_PATH/components/esp_lcd/include/esp_lcd_io_spi.h:28-29` |
| post_cb（→ 你的回调）在事务完成时被调，上下文为 ISR | `$IDF_PATH/components/esp_driver_spi/src/gpspi/spi_master.c:796-808` |
| LVGL 8.3 单缓冲：渲染下一块前 `while(draw_buf->flushing)` 自旋等，`wait_cb` 可让出 | lvgl release/v8.3 `src/core/lv_refr.c` `refr_area_part()` |
| 官方 flush 模式：flush_cb 只发不等；flush_ready 放 trans_done 回调；坐标 +1 | `$IDF_PATH/examples/peripherals/lcd/spi_lcd_touch/main/spi_lcd_touch_example_main.c:77-82, 114-126`（注意该示例是 LVGL 9 API，**模式**可抄，**函数名**不可照抄） |
| ST7789 大端字节序要 swap | 同示例 :122-123 原注释（LVGL 9 用运行时 swap；8.3 用 `CONFIG_LV_COLOR_16_SWAP`） |
| tick 用 esp_timer 周期事件 | 同示例 :257-263 |

## 本期不做（写进代码注释，防止将来疑惑）

- **8 屏全量 / 导航 / 返回栈**：第 2 步的事（DESIGN.md:142-147）。
- **表情动画（LCD_EMO_*）**：接口留好（3.5），第一卡不画。
- **背光 PWM 调亮度 / 休眠省电**：FR 里没有硬性要求，先 GPIO 拉高。
- **触摸**：型号未定，indev 接口和验证路径已备好（第 6 节）。
- **`esp_lvgl_port` 组件**：Espressif 有现成的 LVGL 移植组件能替你写掉 3.1-3.4
  的大部分代码，但那会跳过"移植层到底长什么样"这个学习目标，且偏离
  lcd_ui.h 注释里既定的 ai_client 路线——本期手写，将来如果嫌维护烦再换不迟。
