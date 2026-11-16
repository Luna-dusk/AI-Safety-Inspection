# AI-Safety-Inspection · AI 安全监护对讲终端

面向独居老人 / 独居人群的 **ESP32-S3** 语音监护终端：**离线唤醒 + 云端 AI 语音对话 + 主动告警 + 2.4" 触屏 UI + 双机互联**。

全链路真机已通：唤醒词 → 说话 → WebSocket 流式上传 → 火山流式 ASR → DeepSeek → 火山双向流式 TTS → 扬声器出声，VAD 结束到首帧典型 **0.7–0.9 s**（验收线 p50 ≤ 1.2 s）；屏幕已上板，主屏·守护与对话屏两屏可用。

```
用户说话 → 双麦 AFE（降噪/唤醒/VAD）→ 云端编排（ASR→LLM→TTS）→ 扬声器
              ↓ 本地兜底                          ↑
        MultiNet 命令词 / 预录音提示音        屏幕（状态 + 对话气泡）
              ↓
        MPU6050 + QMC5883L → 跌倒状态机 → 告警（TODO：ESP-NOW / 服务器）
```

## 功能状态

| 功能 | 状态 |
| --- | --- |
| AI 语音对话：v2 WebSocket 长连接（跨轮复用、断线重连恢复 10 轮上下文） | ✅ 真机通过 |
| v1 HTTP chunked 轮次制（WS 连续失败 3 次自动降级） | ✅ 真机通过 |
| 离线唤醒 WakeNet「你好小鑫」+ 流式上传 | ✅ 真机通过 |
| 本地命令词兜底 MultiNet（断网可用） | ✅ 真机通过 |
| 双麦 AFE（两颗 INMP441，SE/BSS 空间分离） | ✅ 真机通过 |
| WiFi SoftAP 网页配网（portal + NVS 凭证 + 指数退避重连 + 网络诊断） | ✅ 真机通过 |
| 校时：SNTP + HTTP `Date` 头兜底；状态栏时钟 | ✅ 真机通过 |
| 电源自锁 + 电源键长按关机（PWR） | ✅ 编译通过 |
| LCD 主屏·守护（状态栏 / 同心双环徽记 / 事件卡 / AI 卡 / 呼救卡 / 底部导航） | ✅ 已上板验证 |
| LCD 对话屏（类微信气泡，用户右绿 / AI 左深，UTF-8 安全截断） | ✅ 已上板验证 |
| 跌倒检测状态机（FR-3：2.5g 冲击 + 55° 姿态保持 3s） | ✅ 编译通过，待上板 |
| 陀螺零偏校准 / 互补滤波姿态融合（MPU6050 + QMC5883L） | ✅ 编译通过，待上板 |
| ESP-NOW 双机互联 / 对讲 | ⚠️ 链路就绪，待配对 |
| 跌倒告警触达（ESP-NOW 广播 + 服务器上报） | ⬜ TODO |
| 触摸（型号未定）、其余 6 屏 + 导航返回栈 | ⬜ TODO |
| AEC 回声消除 → 全双工打断（`abort` 协议已预留） | ⬜ 待硬件确认 |

## 技术栈

| 层 | 选型 |
| --- | --- |
| MCU | ESP32-S3（N16R8，16 MB flash + 8 MB Octal PSRAM） |
| 框架 | ESP-IDF **v5.4.0**（不可升降） |
| 语音前端 | esp-sr **2.4.6**（锁死）：AFE + WakeNet + MultiNet |
| UI | LVGL **8.3.11** + esp_lcd（ST7789 SPI，240×320） |
| 传输 | esp_websocket_client（v2）/ esp_http_client（v1） |
| 编排层 | Python aiohttp（`server/`），供应商全在 `.env` |

## 目录结构

```
components/BSP/
  I2S/          双麦采集（INMP441×2, I2S1）与播放（MAX98357, I2S0）+ 喇叭自测
  AFE_SR/       ESP-SR 前端：feed/fetch 双任务、VAD、唤醒/会话状态机
  AI_CHAT/      云链路三模块：AI_FRAMES（帧解析）/ Transport（WS+HTTP）/ CHAT（轮次编排）
  LCD/          ST7789 SPI + LVGL 移植层 + ui_theme / ui_widgets / screen_home / screen_chat
  IMU/          MPU6050 + QMC5883L 驱动（I2C 总线唯一所有者在这里）
  FALL_DET/     跌倒检测：50Hz 采集 + 姿态融合 + 状态机（含 CSV 采集模式）
  PWR/          电源自锁：GPIO40 维持脉冲 + 电源键长按关机（必须在 app_main 最前）
  WIFI/         配网 portal + STA 管理 + 网络诊断
  MultiNet/     离线命令词
  ESP_NOW/      双机互联骨架
  KEY/ LED_Driver/   按键 / 指示灯
components/APP_STATE/   时间与全局状态（SNTP / HTTP 校时）
main/           应用入口：启动序列 + 各模块接线
server/         自建编排层：/api/v1/chat（v1）、/ws/v1（v2）、/healthz
tools/          字库生成（gen_font_common.py）、提示音生成、FALL_DET CSV 采集与分析
ui/             8 屏交互原型（index.html）与视觉规范（DESIGN.md）
```

## 关键设计

**1. 传输层双模式，对上层完全透明**
`Transport` 是设备唯一的网络 IO 层，四个入口（`session_start` / `tx_write` / `tx_read_loop` / `tx_end`）签名与语义在 v1/v2 下一致，内部按本轮实际模式分派。v2 用 WebSocket 长连接（会话按 device ID 键入服务器，断线重连恢复上下文），连续失败 3 次自动退回 v1 HTTP chunked。

**2. 帧协议两端严格对称**
`[type(1B) | len(u16 BE) | payload]`：`0x01` 音频、`0x02` JSON 事件。**一条 WS 消息 = 一帧**；固件 `ai_frames.c` 与 `server/aichat/frames.py` 一一对应。上行 chunk 封帧与终止块在 v1 下必须手工做（`esp_http_client_write()` 是裸写）。

**3. 内部 RAM 是这块板子最贵的资源**
FreeRTOS 的队列/任务栈只能落内部 8-bit RAM，而音频/屏幕/WiFi 同时要：所以三块大内存全部搬 PSRAM —— PCM 队列 33 KB（`xQueueCreateStatic`）、WS 下行管道 32 KB（`xStreamBufferCreateStatic`）、LVGL 内存池 32 KB（`LV_MEM_CUSTOM` → `lv_mem_psram.h`）。留在内部 RAM 的只有 `MALLOC_CAP_DMA` 的 draw buffer（240×12 = 5.7 KB，拿不到就减半）和任务栈。启动顺序因此被钉死：**PCM 队列 → 屏幕 → 其余**（屏幕要趁内部堆还完整时抢）。

**4. LCD 渲染：跨任务无锁 + 不等 DMA**
业务任务调 `lcd_ui_set_status/chat_*` 只写影子变量/入队并立刻返回；`lv_*` 只允许出现在 lcd_ui 任务里。`flush_cb` 只把分片塞进 DMA 队列就返回，完成后由 ISR 调 `lv_disp_flush_ready()`（另带 200 ms 看门狗：回调真丢了强制放行，宁可撕裂不能卡死）。`lv_timer_handler()` 的返回值要上下限夹在 5~30 ms（它可能返回 `LV_NO_TIMER_READY`）。

**5. 语音前端：feed/fetch 双任务 + 双麦 BSS**
`afe_feed` 固定核 1 优先级 21、`afe_fetch` 固定核 0 优先级 20，用信号量让 fetch 跟着 feed 的节奏走，避免持锁阻塞把 feed 饿死。当前输入格式 `"MM"`（两颗 INMP441 通道交错）：双麦流水线是 `SE(BSS) → VAD → WakeNet`，**不含 NS**（双通道时 BSS 顶掉 NS），买到的是空间分离而不是稳态噪声抑制。没有 AEC，靠 `capture_enabled` 软静音 + 30 帧冷却期隔离播放/采集。

**6. 跌倒检测**
`WATCH → IMPACT → FALLEN`：‖a‖ > 2.5 g 进判定窗，5 s 内倾角相对基线 > 55° 连续保持 3 s 才确认（`fall_det_cancel()` 可撤销，自行站起 10 s 自动重新武装）。用"相对基线"而不是绝对角，与佩戴方式解耦。传感器故障走 L1 总线复位 → L2 重初始化 → L3 降级重试 + `SENSOR_FAULT` 回调的阶梯。

**7. 电源自锁**
`pwr_init()` 必须是 `app_main` 第一行业务代码：电源键松开后栅极只靠电容撑 2.2 s，晚于这个窗口就是"上电→掉电→重启"死循环。副作用是本设计不支持 deep/light sleep。

## 编译与烧录

```bash
# ESP-IDF v5.4；组件版本见 main/idf_component.yml（esp-sr 锁 2.4.6）
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY   # 代理会让组件注册表连不上
. ./esp-idf-env.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

首次烧录或改过分区表后需 `idf.py erase-flash`。语音模型 `srmodels.bin`（3.9 MB）由 `idf.py flash` 自动写入 `model` 分区。

界面文字改动后要重新生成 16 px 常用字库（GB2312 一级 3755 字，覆盖现代汉语约 99.7%）：

```bash
python3 tools/gen_font_common.py      # 必须 --no-compress，且 sdkconfig 开 LV_FONT_FMT_TXT_LARGE
```

## 联调（无真机也能测服务端）

```bash
cd server && pip install -r requirements.txt
cp .env.example .env          # 填供应商 key（只放服务器，不进仓库）
python -m aichat.main

# 另开终端（⚠️ 普通终端，ESP-IDF 终端的 python 无 aiohttp）
python tests/fake_device.py --greeting                # v1 HTTP 问候轮
python tests/fake_device.py --ws --greeting           # v2 WebSocket 问候轮
python tests/fake_device.py --ws --seconds 3 --save reply.wav
```

## 服务端

`server/` 是与设备解耦的编排层：**换 ASR/LLM/TTS 供应商不动设备固件**。key 只存服务器环境变量，绝不进仓库。

| 段 | 当前实现 |
| --- | --- |
| ASR | 火山引擎 大模型流式语音识别 2.0（`volc.seedasr.sauc.*`） |
| LLM | DeepSeek 官方 API（流式，思考可关，TTFT 0.59 s） |
| TTS | 火山引擎 双向流式 TTS V3（连接跨轮复用，16 kHz PCM 零重采样） |

端点：`POST /api/v1/chat`（v1）、`DELETE /api/v1/chat/{id}`、`GET /ws/v1`（v2）、`GET /healthz`。部署与协议细节见 [`server/README.md`](server/README.md)；无 key 也能启动，缺哪段对应轮回 `err` 事件，设备走本地兜底话术。

## 硬件接线

| 外设 | 引脚 |
| --- | --- |
| MAX98357（I2S0 TX，喇叭） | BCK=GPIO15, WS=GPIO16, DIN=GPIO7 |
| INMP441 ×2（I2S1 RX，双麦） | BCK=GPIO5, WS=GPIO4, SD=GPIO6（左麦 L/R→GND，右麦 L/R→3.3V） |
| ST7789（SPI2，240×320） | SCLK=GPIO9, MOSI=GPIO10, RES=GPIO11, DC=GPIO12, CS=GPIO13, BLK=GPIO14 |
| MPU6050 + QMC5883L（I2C 400 kHz） | SDA=GPIO41, SCL=GPIO42 |
| 电源自锁 / 电源键 | CTRL=GPIO40, KEY=GPIO18（按下为低） |
| BOOT 键 / 指示灯 | GPIO0 / GPIO1 |

采样 16 kHz；麦克风侧 32 bit 时隙（`slot >> 16` 取 PCM16），喇叭侧 16 bit。功放 VIN 与 SD 必须同电位且用 ≥1 A 的 5 V 电源（电脑 USB 口带不动，会静音）。

## 文档

| 文档 | 内容 |
| --- | --- |
| [`server/README.md`](server/README.md) | 服务端部署、协议、供应商接入 |
| [`components/BSP/LCD/README.md`](components/BSP/LCD/README.md) | LVGL 移植层逐函数设计讲解 + 现象对照表 |
| [`ui/DESIGN.md`](ui/DESIGN.md) | 8 屏视觉规范 + LVGL 对照表 |
| [`ui/prototype/index.html`](ui/prototype/index.html) | 可交互原型（真机配色即由此而来） |
| [`components/BSP/WIFI/README.md`](components/BSP/WIFI/README.md) | 配网流程 |

## 安全说明

- 供应商 key 只存服务器 `.env`（600 权限），设备固件与仓库零密钥；设备 ID 白名单 + 可选 token 鉴权。
- 当前设备侧为明文 HTTP/WS，TLS + 域名升级在计划中（服务器升级后设备仅需改一处不入库的常量）。
- 音频只以流形态过内存，任何路径不落盘（可开启文本留痕，默认关）。
- 配网页、设计原型均为静态资源，无外部依赖。

## Roadmap

1. 跌倒告警闭环：FALL_DET 回调 → ESP-NOW 广播配对设备 + 服务器上报（≤2 s 本机 / ≤5 s 家属手机）
2. 触摸（型号确认）→ 其余 6 屏 + `ui_nav` 导航/返回栈
3. AEC 硬件确认 → 启用 `abort` 全双工打断
4. Function calling：LLM 调用业务工具（查记录 / 通知家属）
5. 双机对讲（ESP-NOW 音频流）