#!/usr/bin/env python3
"""把本地截图交给百炼的视觉模型（qwen3-vl-plus），返回一段文字描述。

用途：当前 pi 会话用的 deepseek-v4.1-flash 是纯文本模型，读不了图。
这个脚本用 models.json 里已有的百炼 key 走 OpenAI 兼容接口，把 PNG 转成
文字反馈，等于给文本模型临时装一只眼睛。

用法：
    python3 ui/tools/see.py ui/shots/home.png "逐行列出你能看清的文字，并指出重叠/越界"
    python3 ui/tools/see.py a.png b.png "对比这两张图的差异"

Key 来源：环境变量 DASHSCOPE_API_KEY，否则读 ~/.pi/agent/models.json 的 百炼.apiKey。
"""

import base64
import json
import os
import sys
import urllib.request
from pathlib import Path

API = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DEFAULT_MODEL = "qwen3-vl-plus"


def api_key() -> str:
    key = os.environ.get("DASHSCOPE_API_KEY")
    if key:
        return key
    cfg = Path.home() / ".pi/agent/models.json"
    data = json.loads(cfg.read_text())
    for name, prov in data.get("providers", {}).items():
        if "dashscope" in str(prov.get("baseUrl", "")) and prov.get("apiKey"):
            return prov["apiKey"]
    raise SystemExit("找不到百炼 API key（DASHSCOPE_API_KEY 或 ~/.pi/agent/models.json）")


def data_url(path: str) -> str:
    raw = Path(path).read_bytes()
    mime = "image/png" if path.lower().endswith(".png") else "image/jpeg"
    return f"data:{mime};base64," + base64.b64encode(raw).decode()


def main() -> None:
    argv = sys.argv[1:]
    if len(argv) < 2:
        raise SystemExit(__doc__)
    *images, prompt = argv

    content = [{"type": "image_url", "image_url": {"url": data_url(p)}} for p in images]
    content.append({"type": "text", "text": prompt})

    body = json.dumps({
        "model": os.environ.get("SEE_MODEL", DEFAULT_MODEL),
        "messages": [{"role": "user", "content": content}],
        "max_tokens": int(os.environ.get("SEE_MAX_TOKENS", "900")),
    }).encode()

    req = urllib.request.Request(
        API, data=body,
        headers={"Authorization": f"Bearer {api_key()}", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            out = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code}: {e.read().decode()[:400]}")

    print(out["choices"][0]["message"]["content"].strip())


if __name__ == "__main__":
    main()