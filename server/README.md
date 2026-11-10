# AI 安全监护对讲 · 服务端（附录 A 方案 B 的编排层）

> 部署目标：`81.71.24.233`。设备侧固件见 `components/BSP/AI_CHAT/README.md`。
> 职责：只做"编排"——流式 ASR → LLM → TTS，把设备的一句 PCM 换成一段回复语音。
> 换 ASR / LLM / TTS 供应商不需要动设备固件（FR-6 同一哲学）。

```
ESP32-S3 设备                          本服务                        外部服务
┌──────────────┐  chunked PCM 上行   ┌──────────────┐  WSS 大模型流式 ASR  ──► 火山
│  chunked     │ ──────────────────► │  pipeline    │  SSE 流式            ──► deepseek
│  HTTP POST   │ ◄────────────────── │  (asyncio)   │  WSS 双向流式 TTS    ──► 火山
└──────────────┘  帧流下行(0x01/0x02) └──────────────┘
```

## 目录

| 文件 | 职责 |
| --- | --- |
| `aichat/main.py` | aiohttp 入口：POST /api/v1/chat、DELETE /api/v1/chat/{sid}、GET /healthz |
| `aichat/pipeline.py` | 一轮对话的编排（本文档核心）：预热、并行泵、打点、错误兜底 |
| `aichat/frames.py` | 设备帧流封装（0x01/0x02 + u16 BE）+ 80/320ms 重切块 —— 与固件 `ai_frames.c` 对称 |
| `aichat/asr_volc.py` | 火山大模型流式识别 v3（bigmodel_async）WS 客户端 |
| `aichat/llm_deepseek.py` | deepseek 流式（openai 兼容），系统提示词在 `config.py` |
| `aichat/tts_volc.py` | 火山双向流式 TTS V3 客户端（连接跨轮复用） |
| `aichat/splitter.py` | A.5 分句器（默认关，见协议对照表第 4 条） |
| `aichat/session_store.py` | 10 轮上下文、会话锁、TTS 连接空闲回收、文字留痕开关 |
| `tests/fake_device.py` | 模拟设备对拍器：chunked 上行 + 帧流解析，无真机即可联调 |

## 快速开始

```bash
cd server
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env      # 填入火山 appid/token、deepseek key（只放这里，绝不进仓库）
python -m aichat.main     # 默认 0.0.0.0:80
curl http://127.0.0.1/healthz   # 查各阶段 key 是否就位
```

key 未配齐也能起服务：缺哪段，对应轮次回 `err` 事件，设备走本地兜底话术（A.3.4）。

## 与需求附录 A 的协议对照表（以官方文档为准的 4 处差异）

| # | 附录 A 的写法 | 官方 V3 实际（2026-05 版文档） | 本实现 |
| --- | --- | --- | --- |
| 1 | TTS 鉴权 `Authorization: Bearer; <token>` | 双向流式 V3 用请求头 `X-Api-App-Id` + `X-Api-Access-Key`（旧版控制台）或 `X-Api-Key`（新版） | 两种都支持，env 里有什么用什么 |
| 2 | `speech_rate 0.85~0.9`（V1 浮点标度） | V3 的 `speech_rate` ∈ [-50,100]，100=2.0 倍速 | `-15` ≈ 0.85 倍速，env 可调 |
| 3 | `pitch_ratio 0.95` | V3 走 `req_params.post_process.pitch`，半音 [-12,12] | 默认 0（不传），试听后调 |
| 4 | A.5 分句器（LLM 出句再喂 TTS） | V3 官方："该接口会处理碎片化文本……推荐将大模型流式输出直接输入该接口" | **默认直喂 delta**；`SPLIT_TEXT=1` 启用 A.5 分句器（实测遇到"无标点不合成"再开） |

需求 A.3 的**设备侧协议（帧流/头字段/块大小）不变**，本服务严格照做。

## 一轮对话的时序（pipeline.py）

```
设备 POST（chunked PCM）──────────────┐
   │ 边收边喂                          │ TTS 预热任务（与上行并行，A.4.4 坑5）
   ▼                                  ▼
火山 ASR(bigmodel_async)          StartConnection → StartSession
   │ 上行 EOF = 设备终止块                │
   ▼                                  │
ASR final（两阶段精修）──► {"t":"asr"} │
   ▼                                  │
LLM 流式 delta ──直喂──► TaskRequest ──►│（收帧泵同时启动：音频与出字并行）
   ▼                                  ▼
LLM 完 ──► {"t":"reply"}          TTSResponse(16k PCM) ── 重切 80/320ms ──► 0x01 帧
   ▼                                  ▼
发 FinishSession ◄────────────  SessionFinished ──► {"t":"done"}
```

打点日志（`aichat.metrics`，A.4.5 同名指标）：`asr_ms / llm_ttft_ms / tts_first_ms / e2e_first_audio_ms`，
每次一轮一条 JSON，`e2e_first_audio_ms` 对应验收"VAD 结束 → 设备收到第一帧 p50 ≤ 1.2s"。

## 关键设计决定（为什么这么写）

1. **TTS 连接跨轮复用**（session_store + tts_volc）：官方最佳实践——一条 WSS 顺序跑多个
   session（收到 SessionFinished 才能开下一个）；每轮免掉 200-400ms 建连。空闲 60s 由
   清扫任务回收（A.4.4 坑 4：按会话计费，空闲连接主动断）。
2. **收帧泵与 LLM 并行**（pipeline 里 `pump` 任务）：TTS 的音频在文本喂入期间就开始返回，
   若等 LLM 写完再收，首帧延迟退化为整段 LLM 时长——这是流式编排最常见的错误。
3. **失败路径连接作废**：任何异常后 TTS 连接停在会话中间，直接关闭，下一轮重连。
   正确性优先于复用。
4. **会话锁**：设备半双工不会并发，但锁是免费的兜底。
5. **隐私（A.10）**：音频只在内存以 gzip 分片形态流过，任何路径不写盘；文本留痕
   `RECORD_TEXT=1` 才开（JSONL，只 asr/reply 文本）。
6. **设备鉴权**：`X-Device-Id` 白名单 + 可选 token。当前是明文 HTTP，token 只防误用不防
   抓包——升级 HTTPS + 域名前这层仅作占位（A.12 #1，与固件 my_client.h 的 TODO 同步）。

## 部署（81.71.24.233）

```bash
# systemd：/etc/systemd/system/aichat.service
[Unit]
Description=AI-Safety-Inspection chat orchestrator
After=network.target

[Service]
WorkingDirectory=/opt/aichat/server
EnvironmentFile=/opt/aichat/server/.env
ExecStart=/opt/aichat/server/.venv/bin/python -m aichat.main
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

端口 80 需要 root 或 `setcap 'cap_net_bind_service=+ep' .venv/bin/python`；
或 `PORT=8080` + nginx 反代（顺便挂证书，为 HTTPS 升级铺路）。

## 联调步骤（对应设备端 README 第 8 节）

1. `python tests/fake_device.py --greeting` → 应打印 `event: {"t":"reply"...}` 和音频统计；
   `--save reply.wav` 听音色（缺 key 时得到 `err` 事件，属预期）。
2. `python tests/fake_device.py --seconds 2` → 走完整 ASR→LLM→TTS 链路。
3. 设备指向服务器（`SERVER_BASE` 已是 81.71.24.233），验证 A.7 验收表 1/2/4：
   说"你好，讲个故事"看 `e2e_first_audio_ms`；连续 5 轮；服务器停服看设备 300ms 兜底。

## 待实测校准点（代码里已标注）

- ~~`asr_volc.py`：尾包负序号的具体取值~~ **已实测（2026-11-08，seedasr 2.0）**：服务器不发
  "负序号响应"，final = 处理完尾包后 `close(1000, "finish last sequence")`，代码按此判定
  （兼容负/无序号响应的旧版行为）。注意 **2.0 的 resource_id 是 `volc.seedasr.sauc.*`**
  （1.0 才是 `volc.bigasr.sauc.*`，用错报 403 "resource not granted"）。
- ~~`tts_volc.py`：TaskRequest 的 payload 是否需带 `"event": 200` 字段~~ **已实测**：当前写法
  （带 event:200）工作正常。另实测 2.0 会主动推**句级事件 350/351**（含空句），可能在
  `SessionStarted` 之前插队到达——`_recv_expect` 已按"跳过句级事件"处理。
- 音色 speaker（A.12 #3）：**seed-tts-2.0 只认 `_uranus_bigtts` 后缀音色**，1.0 的
  `_mars_bigtts` 会报 55000000 "resource mismatch"。候选温柔声线（都已验证能出声）：
  `zh_female_vv_uranus_bigtts`（当前默认，温柔女声）/ `zh_female_sophie_uranus_bigtts` /
  `zh_male_liufei_uranus_bigtts`，换 `VOLC_TTS_SPEAKER` 重启即可。
- `SPLIT_TEXT`：先 0；若 V3 实测有"无标点不合成"再置 1。
- **LLM 思考（2026-11-08 实测）**：`deepseek-flash` **默认开思考**，正文 TTFT 2.11s（先流
  ~200 字 reasoning_content）；`thinking: disabled`（`LLM_THINKING=off`）→ 0.59s，
  `reasoning_effort=none` → 1.03s。陪聊场景默认 off。关思考后整链 VAD 结束→首帧
  典型 **0.7-0.9s**（已进 1.2s 验收线），偶发抖动 1.6-2.4s（deepseek/TTS 上游波动，
  真机压测再观察 p50）。
