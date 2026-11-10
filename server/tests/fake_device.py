#!/usr/bin/env python3
"""模拟设备联调工具（设备端 README 8.④ 的 PC 对拍器）。

生成 N 秒 PCM（前 0.3s 静音 + 700Hz 音）→ 上行（v1 HTTP chunked 或 v2 WebSocket）→
解析下行帧流（与固件 ai_frames.c 同构）→ 打印事件与音频统计，可选存 wav 试听。

用法（服务器本机自测）：
    python tests/fake_device.py --greeting
    python tests/fake_device.py --seconds 2 --save reply.wav
用法（从 PC 打服务器）：
    python tests/fake_device.py --host <SERVER_IP> --seconds 2
用法（v2 WebSocket，附录 B）：
    python tests/fake_device.py --ws --port 8899 --greeting
    python tests/fake_device.py --ws --seconds 2
    python tests/fake_device.py --ws --seconds 5 --abort   # utterance_end 后发 abort（预留路径）
"""
import argparse
import asyncio
import json
import math
import struct
import time

import aiohttp

SR = 16000


async def pcm_iter(seconds: float):
    """按 512B（16ms）一片 yield，模拟设备边录边发（A.3.1）。"""
    n = int(SR * seconds)
    buf = bytearray()
    for i in range(n):
        v = 0 if i < SR * 3 // 10 else int(8000 * math.sin(2 * math.pi * 700 * i / SR))
        buf += struct.pack("<h", v)
        if len(buf) >= 512:
            yield bytes(buf)
            buf.clear()
            await asyncio.sleep(0.016)          # 实时节奏，给服务端流式 ASR 真实感
    if buf:
        yield bytes(buf)


async def empty_iter():
    """问候轮：空 body（设备端 session_start 后立刻发终止块）。"""
    return
    yield  # noqa: 让函数成为 async generator


def parse_frames(buf: bytes):
    """从缓冲里尽量多地拆帧，返回 ([(type, payload)], 剩余字节)。"""
    out = []
    p = 0
    while p + 3 <= len(buf):
        ftype = buf[p]
        length = int.from_bytes(buf[p + 1:p + 3], "big")
        if p + 3 + length > len(buf):
            break
        out.append((ftype, buf[p + 3:p + 3 + length]))
        p += 3 + length
    return out, buf[p:]


async def run_http(args, audio: bytearray, t_first: list):
    """v1：chunked POST /api/v1/chat。"""
    session = f"fake-{args.device}-{asyncio.get_event_loop().time():.0f}"
    headers = {
        "Content-Type": "audio/L16;rate=16000;channels=1",
        "X-Device-Id": args.device,
        "X-Session-Id": session,
        "X-Turn-Index": "1",
        "X-Greeting": "1" if args.greeting else "0",
    }
    url = f"http://{args.host}:{args.port}/api/v1/chat"

    async with aiohttp.ClientSession() as http:
        data = empty_iter() if args.greeting else pcm_iter(args.seconds)
        async with http.post(url, data=data, headers=headers) as resp:
            print(f"HTTP {resp.status} {resp.headers.get('Content-Type')}")
            rest = b""
            while True:
                chunk = await resp.content.readany()
                if not chunk:
                    break
                rest += chunk
                frames, rest = parse_frames(rest)
                for ftype, payload in frames:
                    if ftype == 0x01:
                        if not t_first:
                            t_first.append(time.monotonic())
                        audio += payload
                    else:
                        print("event:", payload.decode("utf-8", "replace"))


async def run_ws(args, audio: bytearray, t_first: list):
    """v2：WebSocket 长连接（附录 B）。hello → welcome → 轮次 → 帧回传。"""
    url = f"ws://{args.host}:{args.port}/ws/v1"
    events = []

    async with aiohttp.ClientSession() as http:
        async with http.ws_connect(url) as ws:
            await ws.send_json({"t": "hello", "device": args.device})

            async def reader():
                """收帧任务：binary 拆帧（ai_frames 同构），text 不该出现。"""
                rest = b""
                async for msg in ws:
                    if msg.type == aiohttp.WSMsgType.BINARY:
                        rest += msg.data
                        fr, rest = parse_frames(rest)
                        for ftype, payload in fr:
                            if ftype == 0x01:
                                if not t_first:
                                    t_first.append(time.monotonic())
                                audio.extend(payload)
                            else:
                                text = payload.decode("utf-8", "replace")
                                print("event:", text)
                                events.append(text)
                    elif msg.type == aiohttp.WSMsgType.TEXT:
                        print("text(意外):", msg.data)
                    elif msg.type in (aiohttp.WSMsgType.CLOSED,
                                      aiohttp.WSMsgType.ERROR):
                        break

            reader_task = asyncio.create_task(reader())
            if args.greeting:
                await ws.send_json({"t": "greeting"})
            else:
                await ws.send_json({"t": "turn_start", "turn": 1})
                async for chunk in pcm_iter(args.seconds):
                    await ws.send_bytes(chunk)
                await ws.send_json({"t": "utterance_end"})
                if args.abort:
                    await asyncio.sleep(0.2)
                    await ws.send_json({"t": "abort"})

            # 等 done / err / aborted 事件或 60s 超时
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                if any(f'"{k}"' in e for e in events for k in ("done", "err", "aborted")):
                    break
                await asyncio.sleep(0.1)
            await ws.close()
            await reader_task


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--seconds", type=float, default=2.0, help="上行音频时长（问候轮忽略）")
    ap.add_argument("--greeting", action="store_true", help="问候轮（不发音频）")
    ap.add_argument("--device", default="DC1")
    ap.add_argument("--save", default="", help="把下行音频存成 wav 试听")
    ap.add_argument("--ws", action="store_true", help="v2 WebSocket 模式（默认 v1 HTTP）")
    ap.add_argument("--abort", action="store_true", help="WS 模式：utterance_end 后发 abort")
    args = ap.parse_args()

    audio: bytearray = bytearray()
    t_first: list = []                             # 惯用可变引用（闭包里记首帧时刻）
    t0 = time.monotonic()

    if args.ws:
        await run_ws(args, audio, t_first)
    else:
        await run_http(args, audio, t_first)

    if t_first:
        print(f"audio: {len(audio)} bytes ({len(audio) / SR / 2:.2f}s), "
              f"first frame after {(t_first[0] - t0) * 1000:.0f} ms")
    if args.save and audio:
        import wave
        with wave.open(args.save, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(bytes(audio))
        print("saved:", args.save)


if __name__ == "__main__":
    asyncio.run(main())
