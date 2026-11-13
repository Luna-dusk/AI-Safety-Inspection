# AI-Safety-Inspection · AI 安全监护对讲终端

基于 **ESP32-S3** 的语音监护设备，面向独居老人/独居人群：离线唤醒 + 云端 AI 语音对话（自建流式 ASR → LLM → TTS 流水线）+ 主动告警（关键词呼救 / 静默看护 / 跌倒检测）+ 双机 ESP-NOW 互联。

> 全链路真机已通：唤醒 → 说话 → WebSocket 流式上传 → 火山流式 ASR → DeepSeek → 火山双向流式 TTS → 扬声器出声，VAD 结束 → 首帧典型 **0.7–0.9 s**（验收线 p50 ≤ 1.2 s）。

## 系统架构

```
ESP32-S3 设备                                云端（自建编排层 server/）
┌───────────────────────────┐   v2: WebSocket 长连接（优先）   ┌──────────────────┐
│ ESP-SR: WakeNet 唤醒 + AFE │   v1: HTTP chunked（自动降级）  │  pipeline.run_turn│
│ 双麦 I2S → VAD → 480KB 环形│ ────────────────────────────► │  火山流式 ASR 2.0 │
│ 缓冲 → 512B/包 流式上传     │                                │       ↓           │
│                            │   音频帧(0x01) + JSON 事件(0x02)│  DeepSeek(流式)   │
│ ai_frames 解析 → I2S0 播放 │ ◄──────────────────────────── │       ↓           │
│ MultiNet 本地命令词兜底     │   （一条消息 = 一帧，两端对称）  │  火山双向流式 TTS  │
└───────────────────────────┘                                └──────────────────┘
```

- **传输层 v2（当前）**：WebSocket 长连接（aiohttp `/ws/v1`），连接跨轮复用消除建连延迟，会话按设备 ID 键入、断线重连恢复 10 轮上下文；打断协议（`abort`）已预留，待 AEC 硬件确认后启用。
- **传输层 v1（保留为降级路径）**：HTTP chunked 轮次制，WS 连续失败 3 次自动回退。
- **帧协议**：`[type(1B) | len(u16 BE) | payload]`，0x01 音频 / 0x02 JSON 事件；固件 `ai_frames.c` 与服务端 `frames.py` 严格对称，**一条 WS 消息 = 一帧**，解析器两端零改动。

## 功能状态

| 功能 | 状态 |
| --- | --- |
| AI 语音对话（v2 WebSocket 全链路） | ✅ 真机通过 |
| WiFi SoftAP 配网（portal + NVS 凭证） | ✅ 真机通过 |
| 离线唤醒（WakeNet「你好小鑫」）+ 流式上传 | ✅ 真机通过 |
| 本地命令词兜底（MultiNet，断网可用） | ✅ 真机通过 |
| 陀螺零偏校准 / 姿态融合（MPU6050+QMC5883L） | ✅ 编译通过，待上板 |
| 跌倒检测状态机（FR-3：冲击+姿态保持判据） | ✅ 编译通过，待上板 |
| ESP-NOW 双机互联 / 对讲 | ⚠️ 链路就绪，待配对 |
| 跌倒告警触达（ESP-NOW / 服务器） | ⬜ TODO |
| LCD 真实现（ST7789 + LVGL 8.3） | ⬜ 设计文档就绪 |
| AEC 回声消除 → 全双工打断 | ⬜ 待硬件确认 |

## 目录结构

```
components/BSP/
  I2S/          双麦采集（INMP441×2）与 I2S0 播放（MAX98357）
  AFE_SR/       ESP-SR 前端：feed/fetch 双任务、VAD、唤醒事件
  AI_CHAT/      云链路三模块：AI_FRAMES（帧解析）/ Transport（WS+HTTP 双模式）/ CHAT（编排）
  IMU/          MPU6050 + QMC5883L 驱动（I2C 总线唯一所有者在此）
  FALL_DET/     跌倒检测：50Hz 采集 + 姿态融合 + 跌倒状态机（含 CSV 采集模式）
  PWR/          电源自锁：GPIO40 维持脉冲 + 电源键长按关机（必须在 app_main 最前）
  WIFI/         SoftAP 配网（portal.html 内嵌）+ STA 管理 + 网络诊断
  MultiNet/     离线命令词
  ESP_NOW/      双机互联骨架
  KEY/LED_Driver/LCD/   按键 / LED / LCD（LCD 暂为 stub）
main/           应用入口（启动序列 + 各模块接线）
server/         自建编排层（aiohttp）：/api/v1/chat（v1）、/ws/v1（v2）、/healthz
tools/          兜底提示音生成脚本 + fall_log.py/fall_check.py（FALL_DET CSV 采集与分析）
ui/             8 屏交互原型与视觉规范
```

私有文件不入库（`.gitignore`）：服务器地址/凭据常量 `my_client.h`、设计讲解与交接文档、`.env`。

## 服务端

`server/` 是与设备解耦的编排层：**换 ASR/LLM/TTS 供应商不动设备固件**。模块化设计，供应商全在 `.env` 配置（key 只存服务器环境变量，绝不进仓库）：

| 段 | 当前实现 |
| --- | --- |
| ASR | 火山引擎 大模型流式语音识别 2.0（`volc.seedasr.sauc.*`） |
| LLM | DeepSeek 官方 API（流式，思考可关，TTFT 0.59s） |
| TTS | 火山引擎 双向流式 TTS V3（连接跨轮复用，16kHz PCM 零重采样） |

部署与协议细节见 [`server/README.md`](server/README.md)；无 key 也能启动（缺哪段对应轮回 `err` 事件，设备走本地兜底话术）。

## 编译与烧录

```bash
# ESP-IDF v5.4（不可升降）；组件 esp-sr 锁死 2.4.6
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY   # 代理会让组件注册表连不上
. ./esp-idf-env.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

首次烧录或改过分区表后需 `idf.py erase-flash`。语音模型 `srmodels.bin`（3.9MB）由 `idf.py flash` 自动烧到 model 分区。

## 联调（无真机也能测服务端）

```bash
cd server && pip install -r requirements.txt
cp .env.example .env          # 填供应商 key（只放服务器，不进仓库）
python -m aichat.main

# 另开终端（⚠️ 普通 conda 终端，ESP-IDF 终端的 python 无 aiohttp）
python tests/fake_device.py --greeting                # v1 HTTP 问候轮
python tests/fake_device.py --ws --greeting           # v2 WebSocket 问候轮
python tests/fake_device.py --ws --seconds 3 --save reply.wav
```

## 硬件接线

| 外设 | 引脚 |
| --- | --- |
| MAX98357（I2S0 TX） | BCK=GPIO15, WS=GPIO16, DIN=GPIO7 |
| INMP441 ×2（I2S1 RX） | BCK=GPIO5, WS=GPIO4, SD=GPIO6（左麦 L/R→GND，右麦 L/R→3.3V） |
| MPU6050 + QMC5883L（I2C，400kHz） | SDA=GPIO41, SCL=GPIO42 |
| BOOT 键 | GPIO0 |

采样 16kHz；麦克风侧 32bit 时隙（`slot>>16` 取 PCM16），喇叭侧 16bit。

## 安全说明

- 所有供应商 key 只存服务器 `.env`（600 权限），设备固件与仓库零密钥。
- 设备侧鉴权：设备 ID 白名单 + 可选 token；当前为明文 HTTP，TLS+域名升级在计划中（服务器升级后设备仅需改一处常量，该常量文件不入库）。
- 音频只以流形态过内存，任何路径不落盘（可开启文本留痕，默认关）。

## Roadmap

1. 跌倒告警闭环：FALL_DET 回调 → ESP-NOW 广播配对设备 + 服务器上报（≤2s 本机/配对，≤5s 家属手机）
2. AEC 硬件确认 → 启用 `abort` 打断（全双工）
3. Function calling：LLM 调用业务工具（查记录/通知家属）
4. LCD 真实现（设计文档已就绪）
5. 双机对讲（ESP-NOW 音频流）
