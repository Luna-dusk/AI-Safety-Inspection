"""火山大模型流式语音识别（v3 bigmodel_async）WS 客户端。

协议依据：官方文档《大模型流式语音识别API》+ 官方 sauc_python demo（2026-05 版）。
若联调对不上（鉴权 4xx / 首包无响应），以官方 demo 的封帧函数为准校准本文件。

帧结构：header(4B) + [4B 序列号，取决于 flags] + 4B payload size + payload
  - byte0 = (协议版本 0b0001 << 4) | 头长 0b0001
  - byte1 = (消息类型 << 4) | flags；flags：0b0001 正序号，0b0011 负序号=最后一包
  - byte2 = (序列化 << 4) | 压缩；JSON=0b0001，gzip=0b0001
  - byte3 = 0x00
时序：full_client_request(配置, seq=1) → audio_only(PCM 分片, seq 递增) →
      最后一包用负序号 → 服务器回完最后一个响应后 close(1000) 收尾。
      实测 2.0（seedasr）不发"负序号响应"，final 判定 = close(1000) 或
      负/无序号响应（enable_nonstream 两阶段：final 为精修结果）。
"""
import asyncio
import gzip
import json
import logging
import struct
import uuid

import aiohttp

log = logging.getLogger("aichat.asr")

URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"

PROTO_VERSION = 0b0001
HEADER_SIZE = 0b0001
FULL_CLIENT_REQUEST = 0b0001
AUDIO_ONLY_REQUEST = 0b0010
FULL_SERVER_RESPONSE = 0b1001
ERROR_RESPONSE = 0b1111
FLAG_POS_SEQ = 0b0001
FLAG_NEG_SEQ_LAST = 0b0011
NO_SER, JSON_SER = 0b0000, 0b0001
NO_COMP, GZIP = 0b0000, 0b0001


class ASRError(RuntimeError):
    pass


class VolcASR:
    """一轮识别一个实例（设备 VAD 结束 = 最后一包）。音频只进内存，绝不落盘（A.10）。"""

    def __init__(self, cfg, http: aiohttp.ClientSession, device_id: str):
        self._cfg = cfg
        self._http = http
        self._device_id = device_id
        self._ws = None
        self._seq = 1
        self._text = ""
        self._error = None
        self._final = asyncio.Event()

    @staticmethod
    def _header(msg_type: int, flags: int, ser: int, comp: int) -> bytes:
        return bytes([
            (PROTO_VERSION << 4) | HEADER_SIZE,
            (msg_type << 4) | flags,
            (ser << 4) | comp,
            0x00,
        ])

    def _frame(self, msg_type: int, flags: int, ser: int, comp: int, payload: bytes) -> bytes:
        seq = struct.pack(">i", self._seq if flags == FLAG_POS_SEQ else -self._seq)
        self._seq += 1
        size = struct.pack(">I", len(payload))
        return self._header(msg_type, flags, ser, comp) + seq + size + payload

    def _auth_headers(self) -> dict:
        h = {
            "X-Api-Resource-Id": self._cfg.asr_resource_id,
            "X-Api-Request-Id": str(uuid.uuid4()),
            "X-Api-Sequence": "-1",
        }
        if self._cfg.asr_api_key:                       # 新版控制台
            h["X-Api-Key"] = self._cfg.asr_api_key
        else:                                           # 旧版控制台（注意：ASR 是 App-Key，TTS 是 App-Id）
            h["X-Api-App-Key"] = self._cfg.asr_app_id
            h["X-Api-Access-Key"] = self._cfg.asr_access_token
        return h

    async def start(self):
        """建连 + 发配置首包，等服务端首响应（把鉴权/参数错误提前暴露）。"""
        self._ws = await self._http.ws_connect(URL, headers=self._auth_headers(), timeout=10)
        config = {
            "user": {"uid": self._device_id},
            "audio": {"format": "pcm", "codec": "raw", "rate": 16000, "bits": 16, "channel": 1},
            "request": {
                "model_name": "bigmodel",
                "enable_punc": True,
                "enable_itn": True,
                # 两阶段：流式出 partial，尾包后给精修 final（A.7：asr_final p50 ≤ 200ms）
                "enable_nonstream": True,
                "show_utterances": True,
                "result_type": "full",
            },
        }
        payload = gzip.compress(json.dumps(config).encode("utf-8"))
        await self._ws.send_bytes(
            self._frame(FULL_CLIENT_REQUEST, FLAG_POS_SEQ, JSON_SER, GZIP, payload))
        await self._recv_response(first=True)

    async def feed(self, pcm: bytes):
        """边录边喂（设备 chunked 上行的每个 chunk 直接转发）。"""
        if not pcm:
            return
        await self._ws.send_bytes(
            self._frame(AUDIO_ONLY_REQUEST, FLAG_POS_SEQ, NO_SER, GZIP, gzip.compress(pcm)))

    async def finish(self) -> str:
        """发负序号尾包，等精修 final。返回最终识别文本。

        实测（2026-11-08，seedasr 2.0 / bigmodel_async）：服务器不发"负序号响应"，
        精修 final 直接随最后一个音频响应到达（响应序号是自己的正计数），然后
        close(1000, "finish last sequence") 正常收尾——所以 final 判定 = 服务端正常
        close 或收到负/无序号响应（兼容旧版行为）。纯静音时 final 可能为空文本。"""
        await self._ws.send_bytes(
            self._frame(AUDIO_ONLY_REQUEST, FLAG_NEG_SEQ_LAST, NO_SER, GZIP, gzip.compress(b"")))
        while not self._final.is_set():
            try:
                await self._recv_response()
            except asyncio.TimeoutError:
                if self._text:
                    log.warning("asr final not received in time, use last partial: %s", self._text)
                    break
                raise
        if self._error:
            raise ASRError(self._error)
        return self._text

    async def _recv_response(self, first: bool = False):
        msg = await self._ws.receive(timeout=5 if first else 10)
        if msg.type == aiohttp.WSMsgType.CLOSE:
            # 1000 + "finish last sequence" = 尾包处理完的正常收尾（见 finish docstring）
            if not first and msg.data == 1000:
                self._final.set()
                return
            raise ASRError(f"asr closed by server: {msg.data} {msg.extra}")
        if msg.type != aiohttp.WSMsgType.BINARY:
            raise ASRError(f"asr unexpected frame: {msg.type} {msg.data!r:.120}")
        b = msg.data
        if b[1] >> 4 == ERROR_RESPONSE:                 # 4B code + 4B size + msg
            code = int.from_bytes(b[4:8], "big")
            size = int.from_bytes(b[8:12], "big")
            raise ASRError(f"asr error {code}: {b[12:12 + size].decode('utf-8', 'replace')}")
        p = 4
        seq = None
        if b[1] & 0x0F & FLAG_POS_SEQ:                  # 响应序号：负值 = 最后一包
            seq = struct.unpack(">i", b[p:p + 4])[0]
            p += 4
        size = int.from_bytes(b[p:p + 4], "big")
        p += 4
        payload = b[p:p + size]
        if b[2] & 0x0F == GZIP:
            payload = gzip.decompress(payload)
        data = json.loads(payload)
        result = data.get("result", {})
        self._text = result.get("text", self._text)
        if not first and (seq is None or seq < 0):      # 负序号（或无序号）= final
            self._final.set()
        log.debug("asr partial (seq=%s): %s", seq, self._text)

    async def close(self):
        if self._ws is not None:
            await self._ws.close()
            self._ws = None
