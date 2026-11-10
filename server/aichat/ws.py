"""v2 WebSocket 端点（附录 B）：一条长连接承载音频与控制消息。

与 v1 HTTP 的关系：两者共用 pipeline.run_turn 编排，行为一致；WS 建连失败时
设备回退 v1 HTTP 轮次制（B.2 降级兜底），所以 HTTP 路径保留不删。

消息约定（设备 ↔ 服务器）：
  上行 text   {"t":"hello","device":"DC1","token":"..."}   连接后第一条，鉴权
              {"t":"turn_start","turn":N}                  开始说话轮（随后 binary 音频）
              {"t":"utterance_end"}                         一句说完（设备 VAD 判定）
              {"t":"greeting"}                              开机问候轮（无音频）
              {"t":"abort"}                                 打断（预留，B.2；设备侧未启用）
  上行 binary  裸 PCM16LE / 16k / 单声道（与 v1 chunked body 同字节流，无帧头）
  下行 binary  一条消息 = 一帧（0x01 音频 / 0x02 JSON 事件，与固件 ai_frames.c 严格
               对称，解析器零改动）；首帧 welcome，其后 asr/reply/done/err 与 v1 相同

会话：按 device_id 键入 SessionStore（"ws:<device>"）。断线重连后重发 hello 即恢复
上下文（B.2），TTL 由 SESSION_TTL_MS 兜底回收。设备 VAD 是轮次结束的权威信号；
服务端另有看门狗（_AudioStream）：轮内长时间无消息按说完收尾，防连接悬挂。
"""
import asyncio
import contextlib
import json
import logging

from aiohttp import WSMsgType, web

from . import frames, pipeline

log = logging.getLogger("aichat.ws")


class _AudioStream:
    """上行音频队列：binary 消息入队、utterance_end 关闭；带空闲看门狗与时长上限。

    看门狗：轮内超过 idle 秒没有任何新消息（设备 VAD 失灵/网络黑洞），按已有音频
    收尾——对应 v1 里"设备不发终止块则请求悬挂"的场景，但这里是主动兜底。
    上限：超过 15s（480KB，A.3.1）即截断，run_turn 用已收内容识别。
    """

    def __init__(self, idle_s: float, max_bytes: int):
        self._q: asyncio.Queue = asyncio.Queue()
        self._idle_s = idle_s
        self._max_bytes = max_bytes
        self._total = 0
        self._closed = False
        self.overflow = False

    def feed(self, pcm: bytes) -> None:
        if self._closed:
            return
        self._total += len(pcm)
        if self._total > self._max_bytes:
            self.overflow = True
            self.close()
            return
        self._q.put_nowait(pcm)

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._q.put_nowait(None)              # 哨兵：迭代器到此为止

    def __aiter__(self):
        return self

    async def __anext__(self) -> bytes:
        if self._closed and self._q.empty():
            raise StopAsyncIteration
        try:
            item = await asyncio.wait_for(self._q.get(), self._idle_s)
        except asyncio.TimeoutError:
            self.close()                          # 看门狗：按说完收尾
            raise StopAsyncIteration
        if item is None:
            raise StopAsyncIteration
        return item


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ctx = request.app["ctx"]
    cfg = ctx.cfg
    ws = web.WebSocketResponse(heartbeat=cfg.ws_heartbeat_s)   # 协议级 ping/pong 保活
    await ws.prepare(request)

    send_lock = asyncio.Lock()                    # welcome 与轮次帧的写出串行化

    async def send_frame(b: bytes):
        async with send_lock:
            await ws.send_bytes(b)

    device = None
    sess = None
    turn_task = None
    stream = None

    async def stop_turn(reason: str):
        """终止当前轮（新 turn_start / abort / 连接关闭）。TTS 连接若停在会话中间，
        整体作废下一轮重建（与 v1 失败路径同策略：正确性优先于复用）。"""
        nonlocal turn_task, stream
        if stream is not None:
            stream.close()
            stream = None
        if turn_task is not None and not turn_task.done():
            turn_task.cancel()
            with contextlib.suppress(BaseException):
                await turn_task
            if sess is not None and sess.tts is not None:
                with contextlib.suppress(Exception):
                    await sess.tts.close()
                sess.tts = None
            log.info("turn stopped (%s): %s", device, reason)
        turn_task = None

    try:
        # ---- 第一条消息必须是 hello（鉴权），超时/格式错即断 ----
        try:
            first = await ws.receive(timeout=cfg.ws_hello_timeout_s)
        except asyncio.TimeoutError:
            await ws.close(code=4400, message=b"hello timeout")
            return ws
        if first.type != WSMsgType.TEXT:
            await ws.close(code=4400, message=b"expected hello")
            return ws
        try:
            ev = json.loads(first.data)
        except ValueError:
            await ws.close(code=4400, message=b"bad json")
            return ws
        device = str(ev.get("device", ""))
        token = str(ev.get("token", ""))
        if device not in cfg.device_ids or (
                cfg.device_tokens.get(device) is not None
                and token != cfg.device_tokens[device]):
            log.warning("ws auth rejected: %r", device)
            await ws.close(code=4401, message=b"auth rejected")
            return ws
        sess = ctx.store.get_or_create("ws:" + device, device)
        await send_frame(frames.pack_json(
            {"t": "welcome", "proto": 2, "session": sess.id}))
        log.info("ws session up: %s (device=%s)", sess.id, device)

        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    ev = json.loads(msg.data)
                except ValueError:
                    continue
                t = ev.get("t")
                if t == "turn_start":
                    await stop_turn("superseded")   # 防御：上一轮没收尾就开新轮
                    try:
                        turn = int(ev.get("turn", 1))
                    except (TypeError, ValueError):
                        turn = 1
                    stream = _AudioStream(cfg.ws_turn_idle_s, cfg.ws_max_audio_bytes)
                    turn_task = asyncio.create_task(pipeline.run_turn(
                        ctx, device=device, sess=sess, turn=turn, greeting=False,
                        audio_iter=stream, send_frame=send_frame))
                elif t == "utterance_end":
                    if stream is not None:
                        stream.close()
                elif t == "greeting":
                    await stop_turn("superseded")
                    turn_task = asyncio.create_task(pipeline.run_turn(
                        ctx, device=device, sess=sess, turn=1, greeting=True,
                        audio_iter=None, send_frame=send_frame))
                elif t == "abort":
                    # 预留（B.2 打断）：设备侧未启用；服务端语义先行——
                    # 取消当轮任务 + 作废 TTS 连接 + 回 aborted 确认
                    await stop_turn("abort")
                    with contextlib.suppress(Exception):
                        await send_frame(frames.pack_json({"t": "aborted"}))
                # 未知 t：忽略（向前兼容）
            elif msg.type == WSMsgType.BINARY:
                if stream is not None:
                    stream.feed(msg.data)
            elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE,
                              WSMsgType.CLOSING, WSMsgType.CLOSED):
                break
    finally:
        await stop_turn("ws closed")
    return ws
