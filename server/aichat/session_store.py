"""会话表：上下文（最近 10 轮）、每会话一把锁、TTS 连接复用与空闲回收。

- 上下文只存文本（A.10：默认不保存录音，音频只以 gzip 分片形态流过内存，绝不落盘）。
- TTS 连接跨轮复用（官方最佳实践），空闲超过 TTS_IDLE_CLOSE_MS 由 sweep 任务回收
  （A.4.4 坑 4：按会话计费，空闲连接要主动断）。
- 设备侧 5 分钟 idle 会发 DELETE /api/v1/chat/<session>（A.3.3），这是正常回收路径；
  SESSION_TTL_MS 是设备没发 DELETE 时的兜底过期。
"""
import asyncio
import json
import logging
import time
from collections import deque
from typing import Dict, Optional

from .tts_volc import VolcTTSConn

log = logging.getLogger("aichat.session")

MAX_TURNS = 10                                          # A.3.3：服务器保留最近 10 轮


class Session:
    def __init__(self, session_id: str, device_id: str):
        self.id = session_id
        self.device_id = device_id
        self.lock = asyncio.Lock()                      # 设备半双工，一轮一条；防并发兜底
        self.context = deque(maxlen=MAX_TURNS * 2)      # [{"role","content"}, ...]
        self.tts: Optional[VolcTTSConn] = None
        self.last_used = time.monotonic()

    def touch(self):
        self.last_used = time.monotonic()

    def append_turn(self, user_text: str, assistant_text: str):
        self.context.append({"role": "user", "content": user_text})
        self.context.append({"role": "assistant", "content": assistant_text})


class SessionStore:
    def __init__(self, cfg, http):
        self._cfg = cfg
        self._http = http
        self._sessions: Dict[str, Session] = {}
        self._sweeper = None

    def get_or_create(self, session_id: str, device_id: str) -> Session:
        s = self._sessions.get(session_id)
        if s is None:
            s = Session(session_id, device_id)
            self._sessions[session_id] = s
            log.info("session created %s (device=%s)", session_id, device_id)
        s.touch()
        return s

    def delete(self, session_id: str) -> bool:
        s = self._sessions.pop(session_id, None)
        if s is None:
            return False
        if s.tts is not None:
            asyncio.ensure_future(s.tts.close())
        log.info("session deleted %s", session_id)
        return True

    async def ensure_tts(self, session: Session) -> VolcTTSConn:
        """取可用的 TTS 连接：无则建连；有则复用（连接级复用，session 每轮新开）。"""
        if session.tts is not None and session.tts.connected:
            return session.tts
        if session.tts is not None:                     # 半死连接先收尸
            await session.tts.close()
        conn = VolcTTSConn(self._cfg, self._http, session.device_id)
        await conn.connect()                            # StartConnection → ConnectionStarted
        session.tts = conn
        return conn

    async def start(self):
        self._sweeper = asyncio.create_task(self._sweep_loop())

    async def stop(self):
        if self._sweeper:
            self._sweeper.cancel()
        for sid in list(self._sessions):
            self.delete(sid)

    async def _sweep_loop(self):
        while True:
            await asyncio.sleep(30)
            now = time.monotonic()
            for sid, s in list(self._sessions.items()):
                idle_ms = (now - s.last_used) * 1000
                if s.tts is not None and idle_ms > self._cfg.tts_idle_close_ms:
                    log.info("tts idle recycle %s (%.0fs)", sid, idle_ms / 1000)
                    await s.tts.close()
                    s.tts = None
                if idle_ms > self._cfg.session_ttl_ms:
                    log.info("session ttl expired %s", sid)
                    self.delete(sid)

    def record_text(self, device: str, session: str, turn: int, asr: str, reply: str):
        """A.10 文字留痕：RECORD_TEXT=1 时才落 JSONL（只文本，无音频）。"""
        if not self._cfg.record_text or not self._cfg.text_log_path:
            return
        try:
            with open(self._cfg.text_log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps({
                    "ts": int(time.time()), "device": device, "session": session,
                    "turn": turn, "asr": asr, "reply": reply,
                }, ensure_ascii=False) + "\n")
        except OSError as e:
            log.warning("text log failed: %s", e)
