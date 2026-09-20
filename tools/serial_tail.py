#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从板子串口抓一段日志, 按关键字过滤, 并把重复行折叠成计数

为什么不用 idf.py monitor: 摄像头/驱动出问题时日志会每秒刷上千行同样的告警,
监视器直接被冲爆, 想看的那几行根本翻不到。这个脚本只打印关心的行,
重复的行折叠成 "  xN", 一眼就能看出"是在刷同一条错"。

用法 (用 IDF 自带的 Python, 它带 pyserial):
    C:\\Espressif\\tools\\python\\v5.5.4\\venv\\Scripts\\python.exe tools\\serial_tail.py COM14
    ... serial_tail.py COM14 --seconds 15 --filter "tmx_camera|camera:|OV2640"
    ... serial_tail.py COM14 --seconds 20 --filter "探针|EV-|NO-SOI|OV2640" --no-reset

常用参数:
    --seconds N   抓多少秒 (默认 10)
    --filter RE   只打印匹配这个正则的行 (默认全部)
    --no-reset    打开串口后不复位板子 (默认先复位, 好抓完整启动日志)
    --raw         不做重复行折叠, 原样打印
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - 只在没装 pyserial 时触发
    print("需要 pyserial。用 IDF 自带的 Python 跑:")
    print(r"  C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe tools\serial_tail.py COM14")
    sys.exit(2)


def reset_board(port):
    """USB-Serial-JTAG / UART 的经典复位时序: RTS 拉低再放开"""
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.15)
    port.setRTS(False)
    time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(description="抓串口日志并按关键字过滤")
    parser.add_argument("port", help="串口号, 例如 COM14")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=10.0, help="抓多少秒 (默认 10)")
    parser.add_argument("--filter", default=None, help="只打印匹配这个正则的行")
    parser.add_argument("--no-reset", action="store_true", help="打开串口后不复位板子")
    parser.add_argument("--raw", action="store_true", help="不折叠重复行")
    args = parser.parse_args()

    pattern = re.compile(args.filter) if args.filter else None

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        if not args.no_reset:
            reset_board(port)
            port.reset_input_buffer()

        deadline = time.time() + args.seconds
        buffer = b""
        last_line = None
        repeats = 0
        total = 0

        def emit(line):
            nonlocal last_line, repeats
            if pattern is not None and not pattern.search(line):
                return
            if args.raw or line != last_line:
                if repeats and last_line is not None:
                    print(f"    ...上一行重复了 {repeats} 次")
                print(line)
                repeats = 0
            else:
                repeats += 1
            last_line = line

        while time.time() < deadline:
            chunk = port.read(4096)
            if not chunk:
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if line.strip():
                    total += 1
                    emit(line)

        if repeats and last_line is not None:
            print(f"    ...上一行重复了 {repeats} 次")

    print(f"--- 抓了 {args.seconds}s, 共 {total} 行 (已按需要过滤/折叠) ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
