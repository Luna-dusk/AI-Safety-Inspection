#!/usr/bin/env python3
"""生成 LVGL 16px 常用中文字库（components/BSP/LCD/ui_font_cn_common_16.c）

字集 = GB2312 **一级汉字 3755 字** + **第 1 区标点/符号 94 个**
     + ASCII + Latin-1 + 通用标点 + 全角形式
覆盖现代汉语文本约 99.7%（GB2312 一级字表是按拼音排序的常用字表）。

为什么不用全量（CJK 基本区 20992 字）：4bpp 要 2.7MB，app 分区装不下，
生成物 .c 更是 18MB 源码；常用字集只有 600KB 左右、编译产物约 3.4MB 源码。

用法：
    pip install --user fonttools        # 仅当需要从 .ttc 里抽 .otf
    python3 tools/gen_font_common.py [--font /path/NotoSansSC-Regular.otf]

生成后**必须**检查两件事（脚本已自动校验，失败会报）：
    1) `.bitmap_format = 0`   —— 压缩位图在 LV_USE_FONT_COMPRESSED=0 时会让文字全部消失
    2) `.line_height` / `.base_line` —— 会被个别字形撑大，由 ui_theme_init() 归一到 (20,5)

lv_font_conv 获取：`npm i lv_font_conv`（本项目开发机上装在 /tmp/fontconv）。
"""

import argparse
import pathlib
import shutil
import subprocess
import sys

OUT = pathlib.Path(__file__).resolve().parent.parent / "components/BSP/LCD/ui_font_cn_common_16.c"
FONT_NAME = "ui_font_cn_common_16"


def build_charset() -> str:
    """返回去重后的 UTF-8 字集字符串"""
    chars = []
    # GB2312 一级汉字：0xB0A1 ~ 0xD7F9（3755 字）
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # GB2312 第 1 区：中文标点与常用符号（94 个）
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([0xA1, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
    seen, out = set(), []
    for ch in chars:
        if ch not in seen:
            seen.add(ch)
            out.append(ch)
    return "".join(out)


def find_tool(explicit: str | None) -> list[str]:
    if explicit:
        return explicit.split()
    if shutil.which("lv_font_conv"):
        return ["lv_font_conv"]
    # 开发机上装在 /tmp/fontconv（npm i lv_font_conv）
    local = pathlib.Path("/tmp/fontconv/node_modules/.bin/lv_font_conv")
    if local.exists():
        return [str(local)]
    if shutil.which("npx"):
        return ["npx", "-y", "lv_font_conv"]
    sys.exit("找不到 lv_font_conv：npm i lv_font_conv，或用 --tool 指定路径")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="/tmp/NotoSansSC-Regular.otf",
                    help="源字体（思源黑体/Noto Sans SC，.otf 或 .ttf）")
    ap.add_argument("--tool", default=None, help="lv_font_conv 可执行文件")
    args = ap.parse_args()

    if not pathlib.Path(args.font).exists():
        sys.exit(f"源字体不存在: {args.font}\n"
                 "  从 .ttc 抽取：python -c \"from fontTools.ttLib import TTCollection;"
                 "TTCollection('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')"
                 ".fonts[2].save('/tmp/NotoSansSC-Regular.otf')\"  # index 2 = SC")

    sym = build_charset()
    print(f"字集：{len(sym)} 个字符（一级汉字 3755 + 标点 94）")

    cmd = find_tool(args.tool) + [
        "--bpp", "4", "--size", "16", "--font", args.font,
        "-r", "0x20-0x7F",       # ASCII
        "-r", "0xA0-0xFF",       # Latin-1（°×÷ 等）
        "-r", "0x2000-0x206F",   # 通用标点（— ‘ ’ … ）
        "-r", "0x3000-0x303F",   # CJK 标点
        "-r", "0xFF00-0xFF65",   # 全角形式
        "--symbols", sym,
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--no-compress",         # ⚠ 必须：否则文字全部消失（LV_USE_FONT_COMPRESSED=0）
        "--lv-font-name", FONT_NAME,
        "-o", str(OUT),
    ]
    print("运行:", " ".join(cmd[:4]), "...")
    subprocess.run(cmd, check=True)

    src = OUT.read_text(encoding="utf-8")
    ok = True
    if ".bitmap_format = 0," not in src:
        print("✗ 位图格式不是 PLAIN(0) —— 检查 --no-compress")
        ok = False
    # 行高/基线：ui_theme_init() 会归一到 (20,5)，这里只做提示
    for line in src.splitlines():
        if ".line_height =" in line or ".base_line =" in line:
            print("   ", line.strip())
    print(f"✓ 生成 {OUT.relative_to(OUT.parents[3])} ({OUT.stat().st_size/1048576:.1f} MB 源码)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())