"""设备帧流（附录 A.3.2）—— 与固件端 ai_frames.c 严格对称，任何字段改动两端同步。

    +--------+------------------+---------------------------+
    | type   | length (u16 BE)  | payload                   |
    | 1 字节 | 2 字节           | length 字节               |
    +--------+------------------+---------------------------+
      0x01 = 音频块（PCM16 LE / 单声道 / 16000 Hz）
      0x02 = JSON 事件（asr / reply / action / done / err）
"""
import json

FRAME_AUDIO = 0x01
FRAME_JSON = 0x02


def pack_audio(pcm: bytes) -> bytes:
    return bytes([FRAME_AUDIO]) + len(pcm).to_bytes(2, "big") + pcm


def pack_json(event: dict) -> bytes:
    payload = json.dumps(event, ensure_ascii=False).encode("utf-8")
    return bytes([FRAME_JSON]) + len(payload).to_bytes(2, "big") + payload


class AudioChunker:
    """把 TTS 任意大小的音频块重切成 A.3.2 约定的设备友好块：
    第 1 块 80 ms 尽快出声，之后每块 320 ms 抗抖动（A.9 #6 残帧的对偶：设备拼，
    服务端也尽量给整块，两边都不赌对方实现）。"""

    def __init__(self, rate: int = 16000, first_ms: int = 80, later_ms: int = 320):
        self._first = rate // 1000 * first_ms * 2   # 2560 B
        self._later = rate // 1000 * later_ms * 2   # 10240 B
        self._buf = bytearray()
        self._first_done = False

    def feed(self, pcm: bytes):
        """喂入 TTS 音频，返回本次能切出的整块列表。"""
        self._buf += pcm
        out = []
        while True:
            want = self._later if self._first_done else self._first
            if len(self._buf) < want:
                break
            out.append(bytes(self._buf[:want]))
            del self._buf[:want]
            self._first_done = True
        return out

    def flush(self):
        """流结束，把不足一块的余量交出去（不足 2 字节的采样残尾丢弃）。"""
        out = []
        if len(self._buf) >= 2:
            out.append(bytes(self._buf[: len(self._buf) // 2 * 2]))
        self._buf.clear()
        return out
