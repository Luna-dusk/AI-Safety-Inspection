"""A.5 分句器 —— 按需求文档附录 A 原样实现。

注意：火山 V3 双向流式官方文档（2026-05 版）明确"该接口会处理碎片化文本……
推荐将大模型流式输出直接输入该接口，而不要额外增加切句或攒句的逻辑"，
所以本模块默认关闭（SPLIT_TEXT=0，LLM delta 直喂 TaskRequest）。
保留它有两个用途：
  1) 实测遇到"无标点不合成"（A.4.4 坑 1）时置 SPLIT_TEXT=1 立即启用；
  2) 将来换不抗碎片的 TTS 供应商时兜底。
"""


class Splitter:
    SENT_END = "。！？；\n"
    SOFT_END = "，、："

    def __init__(self):
        self.buf = ""
        self.first = True          # 首句必须短（尽快出声）

    def feed(self, delta: str):
        """喂 LLM 增量，返回本次切出的完整句子列表。"""
        self.buf += delta
        out = []
        while True:
            cut = self._find_cut()
            if cut <= 0:
                break
            out.append(self.buf[:cut])
            self.buf = self.buf[cut:]
            self.first = False
        return out

    def _find_cut(self) -> int:
        for i, ch in enumerate(self.buf):                 # 1) 硬标点
            if ch in self.SENT_END:
                return i + 1
        min_len = 6 if self.first else 14                 # 2) 软化切分
        if len(self.buf) >= min_len:
            for i, ch in enumerate(self.buf):
                if ch in self.SOFT_END and i + 1 >= min_len:
                    return i + 1
        return 30 if len(self.buf) >= 30 else -1          # 3) 强制切分

    def flush(self):
        """LLM 结束后调用，交出最后不足一句的余量。"""
        t, self.buf = self.buf, ""
        return [t] if t.strip() else []
