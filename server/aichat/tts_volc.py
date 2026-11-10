"""火山双向流式 TTS V3（wss://openspeech.bytedance.com/api/v3/tts/bidirection）客户端。

协议依据：官方文档《WebSocket 双向流式-V3》字节表与事件定义（2026-05 版）；
与需求附录 A.4 的差异以官方为准（见 README 协议对照表）：
  - 鉴权用 X-Api-* 请求头（新版 X-Api-Key；旧版 X-Api-App-Id + X-Api-Access-Key），
    不是 V1 的 `Authorization: Bearer; <token>`
  - speech_rate 标度 [-50,100]（100=2.0 倍速），不是 V1 的 0.5~2.0 浮点
  - pitch 走 req_params.post_process.pitch（半音 [-12,12]）

封帧（全部大端）：
  上行 FullClientRequest = [0x11, 0x14, 0x10, 0x00] + int32 event + 按 event 附带：
    StartConnection/FinishConnection : uint32 len(payload) + payload("{}")
    StartSession/FinishSession/CancelSession/TaskRequest:
        uint32 len(sid) + sid + uint32 len(payload) + payload(JSON)
  下行 = [4B header] + int32 event + uint32 len(id) + id + uint32 len(payload) + payload
    (0b1001 JSON / 0b1011 raw 音频；0b1111 错误帧 = int32 code + len + msg)

连接复用（官方最佳实践）：一条 WSS 顺序跑多个 session，收到 SessionFinished 后可再
StartSession，跨轮免重连（省 200-400ms，对应 A.4.4 坑 5）。同一连接不允许并发 session。
"""
import asyncio
import json
import logging
import struct
import uuid
from typing import Awaitable, Callable

import aiohttp

log = logging.getLogger("aichat.tts")

URL = "wss://openspeech.bytedance.com/api/v3/tts/bidirection"

PROTO_VERSION = 0b0001
HEADER_SIZE = 0b0001
FULL_CLIENT_REQUEST = 0b0001
FULL_SERVER_RESPONSE = 0b1001
AUDIO_ONLY_RESPONSE = 0b1011
ERROR_RESPONSE = 0b1111
FLAG_WITH_EVENT = 0b0100
JSON_SER, NO_SER = 0b0001, 0b0000
NO_COMP, GZIP = 0b0000, 0b0001

EV_START_CONNECTION = 1
EV_FINISH_CONNECTION = 2
EV_CONNECTION_STARTED = 50
EV_CONNECTION_FAILED = 51
EV_START_SESSION = 100
EV_CANCEL_SESSION = 101
EV_FINISH_SESSION = 102
EV_SESSION_STARTED = 150
EV_SESSION_CANCELED = 151
EV_SESSION_FINISHED = 152
EV_SESSION_FAILED = 153
EV_TASK_REQUEST = 200
EV_TTS_SENTENCE_START = 350
EV_TTS_SENTENCE_END = 351
EV_TTS_RESPONSE = 352


class TTSError(RuntimeError):
    pass


class VolcTTSConn:
    """一条到火山的 WSS 连接，跨轮复用；生命周期由 session_store 管理。"""

    def __init__(self, cfg, http: aiohttp.ClientSession, device_id: str):
        self._cfg = cfg
        self._http = http
        self._device_id = device_id
        self._ws = None
        self._sid = ""

    # ---------- 帧构造 ----------

    @staticmethod
    def _header(msg_type: int, ser: int) -> bytes:
        return bytes([
            (PROTO_VERSION << 4) | HEADER_SIZE,
            (FULL_CLIENT_REQUEST << 4) | FLAG_WITH_EVENT,
            (ser << 4) | NO_COMP,
            0x00,
        ])

    def _frame_payload_event(self, event: int, payload: bytes = b"{}") -> bytes:
        return self._header(FULL_CLIENT_REQUEST, JSON_SER) + \
            struct.pack(">i", event) + struct.pack(">I", len(payload)) + payload

    def _frame_session_event(self, event: int, payload: bytes = b"{}") -> bytes:
        sid = self._sid.encode()
        return self._header(FULL_CLIENT_REQUEST, JSON_SER) + \
            struct.pack(">i", event) + struct.pack(">I", len(sid)) + sid + \
            struct.pack(">I", len(payload)) + payload

    # ---------- 收帧 ----------

    async def _recv(self, timeout: float = 30.0):
        """收一帧，返回 (event, payload)。文本帧与错误帧直接抛 TTSError。"""
        msg = await self._ws.receive(timeout=timeout)
        if msg.type == aiohttp.WSMsgType.TEXT:            # 官方：文本帧主要用于反馈异常
            raise TTSError(f"tts server text: {str(msg.data)[:200]}")
        if msg.type not in (aiohttp.WSMsgType.BINARY,):
            raise TTSError(f"tts ws closed: {msg.type}")
        b = msg.data
        if b[1] >> 4 == ERROR_RESPONSE:                   # int32 code + len + msg
            code = struct.unpack(">I", b[4:8])[0]
            size = struct.unpack(">I", b[8:12])[0]
            raise TTSError(f"tts error {code}: {b[12:12 + size].decode('utf-8', 'replace')}")
        event = struct.unpack(">i", b[4:8])[0]
        p = 8
        id_len = struct.unpack(">I", b[p:p + 4])[0]
        p += 4 + id_len                                   # connection_id / session_id
        plen = struct.unpack(">I", b[p:p + 4])[0]
        p += 4
        payload = b[p:p + plen]
        if b[2] & 0x0F == GZIP:
            import gzip
            payload = gzip.decompress(payload)
        return event, payload

    async def _recv_expect(self, ok_events: set, fail_events: set, timeout: float = 10.0):
        """等指定事件。句级通知（350/351）随时可能插队到达（实测 seed-tts-2.0 会在
        连接建立后/会话中主动推空句事件），与握手无关，跳过继续等，不能当错误。"""
        while True:
            event, payload = await self._recv(timeout)
            if event in (EV_TTS_SENTENCE_START, EV_TTS_SENTENCE_END):
                continue
            if event in fail_events or event not in ok_events:
                detail = payload.decode("utf-8", "replace")[:200]
                raise TTSError(f"tts unexpected event {event}: {detail}")
            return event, payload

    # ---------- 生命周期 ----------

    async def connect(self):
        headers = {"X-Api-Resource-Id": self._cfg.tts_resource_id,
                   "X-Api-Connect-Id": str(uuid.uuid4())}
        if self._cfg.tts_api_key:                         # 新版控制台
            headers["X-Api-Key"] = self._cfg.tts_api_key
        else:                                             # 旧版控制台
            headers["X-Api-App-Id"] = self._cfg.tts_app_id
            headers["X-Api-Access-Key"] = self._cfg.tts_access_token
        self._ws = await self._http.ws_connect(URL, headers=headers, timeout=10)
        await self._ws.send_bytes(self._frame_payload_event(EV_START_CONNECTION))
        await self._recv_expect({EV_CONNECTION_STARTED}, {EV_CONNECTION_FAILED})
        log.info("tts connected (%s)", self._device_id)

    async def start_session(self):
        """StartSession：TTS 参数只在此刻生效（官方：StartSession 定参，TaskRequest 定文本）。"""
        self._sid = uuid.uuid4().hex
        meta = {
            "user": {"uid": "aichat-" + self._device_id},
            "event": EV_START_SESSION,
            "req_params": {
                "speaker": self._cfg.tts_speaker,
                # A.4.2 强制：pcm + 16000Hz，设备侧零重采样
                "audio_params": {"format": "pcm", "sample_rate": 16000},
                "speech_rate": self._cfg.tts_speech_rate,
            },
        }
        if self._cfg.tts_pitch:                           # A.4.2 pitch_ratio 0.95 ≈ -1 半音
            meta["req_params"]["post_process"] = {"pitch": self._cfg.tts_pitch}
        await self._ws.send_bytes(self._frame_session_event(
            EV_START_SESSION, json.dumps(meta, ensure_ascii=False).encode("utf-8")))
        await self._recv_expect({EV_SESSION_STARTED}, {EV_SESSION_FAILED, EV_SESSION_CANCELED})

    async def send_text(self, text: str):
        """TaskRequest：喂一段文本（V3 官方：可直接喂 LLM delta，服务端自行整理成句）。"""
        payload = json.dumps({"event": EV_TASK_REQUEST,
                              "req_params": {"text": text}},
                             ensure_ascii=False).encode("utf-8")
        await self._ws.send_bytes(self._frame_session_event(EV_TASK_REQUEST, payload))

    async def request_finish(self):
        """发 FinishSession：告诉火山"文本发完了"，触发最后一句的强制合成（A.4.4 坑 1）。"""
        await self._ws.send_bytes(self._frame_session_event(EV_FINISH_SESSION))

    async def pump(self, on_audio: Callable[[bytes], Awaitable[None]]):
        """收帧泵：把合成音频逐块回调，直到 SessionFinished。
        必须在第一个 send_text 之前启动（音频与 LLM 出字并行，否则首帧延迟退化为
        整段 LLM 时长）；FinishSession 由调用方在文本发完后另行发送。"""
        while True:
            event, payload = await self._recv(timeout=60)
            if event == EV_TTS_RESPONSE and payload:      # raw PCM16LE 16k mono
                await on_audio(payload)
            elif event == EV_SESSION_FINISHED:
                return
            elif event == EV_SESSION_FAILED:
                raise TTSError(f"tts session failed: {payload[:200]!r}")
            elif event in (EV_TTS_SENTENCE_START, EV_TTS_SENTENCE_END):
                continue                                   # 句级事件，不需要

    async def cancel_session(self):
        """主动放弃当前 session（打断/异常路径）。官方：收到 SessionStarted 后、
        FinishSession 之前发；客户端收到 SessionCanceled 后如需继续要重新 StartSession。"""
        try:
            await self._ws.send_bytes(self._frame_session_event(EV_CANCEL_SESSION))
        except Exception:                                  # 连接已坏就交给 close()
            pass

    async def close(self):
        if self._ws is None:
            return
        try:
            await self._ws.send_bytes(self._frame_payload_event(EV_FINISH_CONNECTION))
        except Exception:
            pass
        await self._ws.close()
        self._ws = None
        log.info("tts connection closed (%s)", self._device_id)

    @property
    def connected(self) -> bool:
        return self._ws is not None and not self._ws.closed
