#!/usr/bin/env python3
"""生成 AI_CHAT 本地兜底占位 PCM（16 kHz / 16 bit / 单声道裸流，无文件头，A.8）。

真录音就绪前先用不同音高的提示音打通链路；替换时保持文件名与格式即可。

用法：
    python3 tools/gen_prompt_pcms.py [输出目录]     # 默认 tools/prompts

烧录到 storage 分区（spiffs）：
    python3 $IDF_PATH/components/spiffs/spiffsgen.py 0x7F0000 <输出目录> prompt.img \
            --partition-name storage
    parttool.py -p /dev/ttyUSB0 write_partition --partition-name storage --input prompt.img
"""
import math
import os
import struct
import sys

SR = 16000
FADE = 160  # 首尾 10ms 线性淡入淡出，防爆音

# 文件名: (提示音频率 Hz, 时长 ms) —— 与 my_client.h 的 PROMPT_* 一一对应
PROMPTS = {
    "net_err.pcm":  (440.0, 600),   # "网络不太好，我待会儿再说。"
    "greeting.pcm": (660.0, 700),   # "奶奶早，今天感觉怎么样。"
    "cmd_ok.pcm":   (880.0, 300),   # 本地命令确认
    "too_long.pcm": (523.0, 500),   # 15s 上限提示（预留）
}


def tone(freq: float, ms: int) -> bytes:
    n = SR * ms // 1000
    out = bytearray()
    for i in range(n):
        env = min(1.0, i / FADE, (n - 1 - i) / FADE)
        v = int(6000 * env * math.sin(2 * math.pi * freq * i / SR))
        out += struct.pack("<h", v)
    return bytes(out)


def main() -> None:
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "prompts")
    os.makedirs(outdir, exist_ok=True)
    for name, (freq, ms) in PROMPTS.items():
        with open(os.path.join(outdir, name), "wb") as fp:
            fp.write(tone(freq, ms))
        print(f"{name}: {ms}ms @ {freq}Hz -> {os.path.join(outdir, name)}")


if __name__ == "__main__":
    main()
