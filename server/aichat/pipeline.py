"""一轮对话的编排（附录 A.2 的服务器段）：鉴权 → 流式 ASR → LLM → TTS → 设备帧流。

时序（A.4.5 打点同名）：
  设备 PCM ──边收边喂──► 火山 ASR(bigmodel_async)
  上行 EOF ──► ASR final（两阶段精修）──► 推送 {"t":"asr"}
  LLM 流式 delta ──直喂──► TTS TaskRequest（V3 官方推荐，不切句；SPLIT_TEXT=1 时走 A.5 分句器）
  TTSResponse(raw 16k PCM) ──重切 80/320ms──► 设备帧 0x01
  LLM 完 ──► {"t":"reply"}；TTS SessionFinished ──► {"t":"done"}

关键设计：
  - TTS 预热与上行并行（A.4.4 坑 5：不能等 LLM 出字才建连）
  - TTS 收帧泵与 LLM 出字并行（否则音频堵在 WS 缓冲里，首帧延迟退化为整段 LLM 时长）
  - 音频绝不落盘（A.10）；文本留痕可关（RECORD_TEXT）

v2（附录 B）：run_turn 与传输解耦——audio_iter 产出上行 PCM、send_frame 写出设备帧；
HTTP（handle_chat）与 WebSocket（ws.py）各自适配，轮次编排全站只此一份。
2026-11-10 修复：问候轮此前只发 reply/done 事件、从未启动收帧泵（设备端无声音，
fake_device --greeting 实测 0 音频帧）——现与对话轮共用 pump+FinishSession+drain 路径。
"""
import asyncio
import contextlib
import json
import logging
import time
from typing import AsyncIterator, Awaitable, Callable, Optional

import aiohttp
from aiohttp import web

from . import frames
from .asr_volc import VolcASR, ASRError
from .frames import AudioChunker
from .llm_deepseek import LLMError
from .splitter import Splitter
from .tts_volc import TTSError

log = logging.getLogger("aichat.pipeline")
metrics = logging.getLogger("aichat.metrics")

# 传输无关的两个口子：上行音频源 / 下行帧汇（HTTP 与 WS 各自提供实现）
AudioIter = AsyncIterator[bytes]
SendFrame = Callable[[bytes], Awaitable[None]]


class PipelineError(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(msg)
        self.code = code
        self.msg = msg


class Ctx:
    """应用级单例集合，由 main.py 组装。"""

    def __init__(self, cfg, http, store, llm):
        self.cfg = cfg
        self.http = http
        self.store = store
        self.llm = llm


class _DeviceAudioWriter:
    """TTS 音频回调 → 重切块 → 设备帧写出，并记录首帧时刻。传输无关（v2）。"""

    def __init__(self, send_frame: SendFrame):
        self._send = send_frame
        self._chunker = AudioChunker()
        self.bytes_sent = 0
        self.first_at = None                       # monotonic，服务端 e2e 打点

    async def write(self, pcm: bytes):
        for block in self._chunker.feed(pcm):
            if self.first_at is None:
                self.first_at = time.monotonic()
            self.bytes_sent += len(block)
            await self._send(frames.pack_audio(block))

    async def drain(self):
        for block in self._chunker.flush():
            if self.first_at is None:
                self.first_at = time.monotonic()
            self.bytes_sent += len(block)
            await self._send(frames.pack_audio(block))


def _now_ms(t0: float) -> int:
    return int((time.monotonic() - t0) * 1000)


async def run_turn(ctx: Ctx, *, device: str, sess, turn: int, greeting: bool,
                   audio_iter: Optional[AudioIter], send_frame: SendFrame,
                   on_locked: Optional[Callable[[], Awaitable[None]]] = None,
                   on_finish: Optional[Callable[[], Awaitable[None]]] = None):
    """一轮对话编排（传输无关）：HTTP handle_chat 与 WS ws_handler 共用。

    - audio_iter：产出上行 PCM（边收边喂 ASR）；问候轮传 None。
    - send_frame：写出一条已封好的设备帧（0x01/0x02，与固件 ai_frames.c 对称）。
    - on_locked：拿到会话锁后调用（HTTP 用来 prepare 响应头，头先走）。
    - on_finish：轮次收尾调用（HTTP 用来 write_eof；WS 无需）。
    """
    cfg = ctx.cfg
    t0 = time.monotonic()

    async with sess.lock:                          # 设备半双工；并发请求排队兜底
        if on_locked is not None:
            await on_locked()

        m = {"device": device, "session": sess.id, "turn": turn, "greeting": greeting}
        asr_text, reply_text = "", ""
        warmup = None                              # TTS 预热任务（与上行并行）
        pump = None                                # TTS 收帧泵（与 LLM 出字并行）
        asr = None
        writer = _DeviceAudioWriter(send_frame)

        async def send_json(ev: dict):
            await send_frame(frames.pack_json(ev))

        async def _ensure_tts():
            conn = await ctx.store.ensure_tts(sess)   # 连接级复用；StartConnection 已完成
            await conn.start_session()                # StartSession → SessionStarted
            return conn

        try:
            if greeting:
                # 开机问候（A.8）：固定文案直达 TTS，不走 ASR/LLM，最快出声
                reply_text = cfg.greeting_text
                await send_json({"t": "reply", "text": reply_text})
                warmup = asyncio.create_task(_ensure_tts())
                tts = await asyncio.shield(warmup)
                pump = asyncio.create_task(tts.pump(writer.write))
                await tts.send_text(reply_text)
                await tts.request_finish()          # 单句文案：发完即收
                await pump
                await writer.drain()
            else:
                if not cfg.asr_ready():
                    raise PipelineError(503, "asr_unavailable (VOLC_ASR_* not set)")
                if not cfg.llm_ready():
                    raise PipelineError(503, "llm_unavailable (DEEPSEEK_API_KEY not set)")

                # ---- 上行：边收边喂流式 ASR（音频只过内存，A.10）----
                # 火山偶发 55000000 内部错误（其官方建议"换新会话重试"，
                # 2026-11-08 实测失败率 3/4）：整段音频缓冲在内存（≤15s≈480KB），
                # 失败后用新连接回放重试一次，设备侧无感。
                asr_chunks: list[bytes] = []
                asr = VolcASR(cfg, ctx.http, device)
                await asr.start()
                try:
                    async for chunk in audio_iter:
                        if chunk:
                            m["upload_end_ms"] = _now_ms(t0)   # 记到最后一块
                            asr_chunks.append(chunk)
                            await asr.feed(chunk)
                    asr_text = await asr.finish()
                except ASRError as e:
                    log.warning("asr failed (%s), retrying once with fresh session", e)
                    retry = VolcASR(cfg, ctx.http, device)
                    await retry.start()
                    try:
                        for c in asr_chunks:
                            await retry.feed(c)
                        asr_text = await retry.finish()
                    finally:
                        await retry.close()
                await asr.close()
                await send_json({"t": "asr", "text": asr_text})
                m["asr_ms"] = _now_ms(t0)

                # ---- TTS 预热与上行已并行；LLM 出字前必须就绪 ----
                warmup = asyncio.create_task(_ensure_tts())
                messages = ([{"role": "system", "content": cfg.system_prompt}]
                            + list(sess.context) + [{"role": "user", "content": asr_text}])
                splitter = Splitter() if cfg.split_text else None
                parts = []
                tts = None
                llm_ttft = None

                async def send_piece(piece: str):
                    nonlocal tts
                    if tts is None:
                        tts = await asyncio.shield(warmup)
                    await tts.send_text(piece)

                async for delta in ctx.llm.stream(messages):
                    if llm_ttft is None:
                        llm_ttft = _now_ms(t0)
                    parts.append(delta)
                    for piece in (splitter.feed(delta) if splitter else [delta]):
                        if piece.strip():
                            await send_piece(piece)
                if splitter:
                    for piece in splitter.flush():
                        if piece.strip():
                            await send_piece(piece)
                reply_text = "".join(parts)
                m["llm_ttft_ms"] = llm_ttft

                # ---- 下行音频：收帧泵必须与 LLM 出字并行（见模块 docstring）----
                pump = None
                if tts is not None:
                    pump = asyncio.create_task(tts.pump(writer.write))
                else:
                    log.warning("empty reply, tts skipped (session %s turn %d)",
                                sess.id, turn)

                await send_json({"t": "reply", "text": reply_text})
                if pump is not None:
                    await tts.request_finish()            # 文本发完，触发末句强制合成
                    await pump
                    await writer.drain()
                m["tts_first_ms"] = (int((writer.first_at - t0) * 1000)
                                     if writer.first_at else None)
                m["audio_bytes"] = writer.bytes_sent

            await send_json({"t": "done", "reason": "ok"})
            sess.append_turn(asr_text, reply_text)
            ctx.store.record_text(device, sess.id, turn, asr_text, reply_text)
            m["e2e_first_audio_ms"] = (int((writer.first_at - t0) * 1000)
                                       if m.get("tts_first_ms") else None)
            m["reply_chars"] = len(reply_text)
            m["total_ms"] = _now_ms(t0)
            metrics.info(json.dumps(m, ensure_ascii=False))

        except (ASRError, LLMError, TTSError, PipelineError,
                asyncio.TimeoutError, ConnectionError,
                aiohttp.ClientError, OSError) as e:
            # aiohttp 的 ClientConnectorError 不是内建 ConnectionError 子类，
            # 必须单列（冒烟测试踩过：TTS 建连失败会把 handler 打穿）
            code = getattr(e, "code", 500)
            msg = getattr(e, "msg", None) or f"{type(e).__name__}: {e}"
            log.error("turn failed (%s/%s/%d): %s", device, sess.id, turn, msg)
            m["error"] = msg
            metrics.info(json.dumps(m, ensure_ascii=False))
            with contextlib.suppress(Exception):
                await send_json({"t": "err", "code": code, "msg": msg[:200]})
        finally:
            # 失败路径连接可能停在会话中间，整体作废，下一轮重连（正确性优先于复用）
            if sess.tts is not None and not sess.tts.connected:
                sess.tts = None
            if asr is not None:
                with contextlib.suppress(Exception):
                    await asr.close()
            if pump is not None and not pump.done():
                pump.cancel()
                with contextlib.suppress(Exception):
                    await pump
            if warmup is not None and warmup.done() is False:
                warmup.cancel()
                with contextlib.suppress(Exception):
                    await warmup
            if on_finish is not None:
                with contextlib.suppress(Exception):
                    await on_finish()


async def handle_chat(ctx: Ctx, request: web.Request) -> web.StreamResponse:
    """v1 HTTP 轮次制（A.3，已实现）：chunked 上行 + 帧流下行。v2 降级路径，勿删。"""
    # ---- 鉴权：设备 ID 白名单 + 可选 token（抓包可见明文 HTTP，token 仅防误用）----
    device = request.headers.get("X-Device-Id", "")
    if device not in ctx.cfg.device_ids:
        raise web.HTTPUnauthorized(text="unknown device")
    token = ctx.cfg.device_tokens.get(device)
    if token is not None and request.headers.get("X-Device-Token", "") != token:
        raise web.HTTPUnauthorized(text="bad token")

    session_id = request.headers.get("X-Session-Id", "")
    try:
        turn = int(request.headers.get("X-Turn-Index", "1"))
    except ValueError:
        turn = 1
    greeting = request.headers.get("X-Greeting", "0") == "1"
    if not session_id:
        raise web.HTTPBadRequest(text="missing X-Session-Id")

    sess = ctx.store.get_or_create(session_id, device)
    resp = web.StreamResponse(status=200, headers={
        "Content-Type": "application/x-aichat-frames",
        "Cache-Control": "no-store",
    })

    async def send_frame(b: bytes):
        await resp.write(b)

    async def prepare():
        await resp.prepare(request)                # 响应头先走：设备 fetch_headers 不再干等

    async def finish():
        await resp.write_eof()

    await run_turn(ctx, device=device, sess=sess, turn=turn, greeting=greeting,
                   audio_iter=None if greeting else request.content.iter_any(),
                   send_frame=send_frame, on_locked=prepare, on_finish=finish)
    return resp


async def handle_delete(ctx: Ctx, request: web.Request) -> web.Response:
    session_id = request.match_info["session_id"]
    ctx.store.delete(session_id)                   # A.3.3：设备 5 分钟 idle 发 DELETE
    return web.json_response({"ok": True})


async def handle_health(ctx: Ctx, request: web.Request) -> web.Response:
    cfg = ctx.cfg
    return web.json_response({
        "stages": {"asr": cfg.asr_ready(), "llm": cfg.llm_ready(), "tts": cfg.tts_ready()},
        "split_text": cfg.split_text,
        "record_text": cfg.record_text,
    })
