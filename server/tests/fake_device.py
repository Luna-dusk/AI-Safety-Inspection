#!/usr/bin/env python3
"""模拟设备联调工具（设备端 README 8.④ 的 PC 对拍器）。

生成 N 秒 PCM（前 0.3s 静音 + 700Hz 音）→ chunked POST /api/v1/chat →
解析下行帧流（与固件 ai_frames.c 同构）→ 打印事件与音频统计，可选存 wav 试听。

用法（服务器本机自测）：
    python tests/fake_device.py --greeting
    python tests/fake_device.py --seconds 2 --save reply.wav
用法（从 PC 打服务器）：
    python tests/fake_device.py --host 81.71.24.233 --seconds 2
"""
import argparse
import asyncio
import json
import math
import struct

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


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--seconds", type=float, default=2.0, help="上行音频时长（问候轮忽略）")
    ap.add_argument("--greeting", action="store_true", help="发 X-Greeting=1 空 body 问候轮")
    ap.add_argument("--device", default="DC1")
    ap.add_argument("--save", default="", help="把下行音频存成 wav 试听")
    args = ap.parse_args()

    session = f"fake-{args.device}-{asyncio.get_event_loop().time():.0f}"
    headers = {
        "Content-Type": "audio/L16;rate=16000;channels=1",
        "X-Device-Id": args.device,
        "X-Session-Id": session,
        "X-Turn-Index": "1",
        "X-Greeting": "1" if args.greeting else "0",
    }
    url = f"http://{args.host}:{args.port}/api/v1/chat"
    audio = bytearray()
    t_first = None
    import time
    t0 = time.monotonic()

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
                        if t_first is None:
                            t_first = time.monotonic()
                        audio += payload
                    else:
                        print("event:", payload.decode("utf-8", "replace"))

    if t_first:
        print(f"audio: {len(audio)} bytes ({len(audio) / SR / 2:.2f}s), "
              f"first frame after {(t_first - t0) * 1000:.0f} ms")
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
