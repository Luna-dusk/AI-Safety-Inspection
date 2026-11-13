#!/usr/bin/env python3
"""fall_log.py — FALL_DET CSV 采集

配套 components/BSP/FALL_DET/fall_det.c 的 FALL_DET_CSV_MODE=1。
设备端 50Hz 输出，行首为数字或 '#'（表头）或 'MARK,'（按键标记）。

用法:
    python fall_log.py COM3 out/fall_fwd_s01_wai.csv
    按 Ctrl+C 结束

依赖: pip install pyserial
"""
import re
import sys

import serial

port = sys.argv[1]
out = sys.argv[2]

# 只保留表头 / 数据行 / 标记行；其它（残留日志、ANSI 转义）一律丢弃
pat = re.compile(r'^(#|\d|MARK,)')

n = 0
with serial.Serial(port, 115200, timeout=1) as ser, \
        open(out, 'w', encoding='utf-8') as f:
    print(f'抓数据 -> {out}   波特率 115200   Ctrl+C 停止')
    print('请现在复位设备\n')
    try:
        while True:
            line = ser.readline().decode('utf-8', 'ignore').strip()
            if not line or not pat.match(line):
                continue
            f.write(line + '\n')
            n += 1
            if n % 250 == 0:
                print(f'\r已收 {n} 行', end='', flush=True)
    except KeyboardInterrupt:
        print(f'\n共 {n} 行 -> {out}')