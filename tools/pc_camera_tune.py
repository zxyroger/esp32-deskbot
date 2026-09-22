#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
在线调 OV2640 的曝光/增益/亮度/对比度等 (固件命令 0x7D)。

固件每改一次会把新的参数打到串口, 所以配合 serial_tail 看效果最方便:

    python tools\\pc_camera_tune.py 192.168.0.106 --status
    python tools\\pc_camera_tune.py 192.168.0.106 --set gainceiling 2      # 增益上限 8X (弱光降噪)
    python tools\\pc_camera_tune.py 192.168.0.106 --set ae_level 1         # 自动曝光目标调亮一档
    python tools\\pc_camera_tune.py 192.168.0.106 --set brightness 1 --set contrast 1

字段 (0x7D 的 field):
    status=0 (只看当前值)   brightness=1  contrast=2  saturation=3  ae_level=4
    agc_gain=5 (0~30)       aec_value=6 (0~1200)
    gainceiling=7 (0=2X 1=4X 2=8X 3=16X 4=32X 5=64X 6=128X, 越小噪点越少)
    hmirror=8  vflip=9  awb_gain=10  aec2=11  agc=12(自动增益开关)  aec=13(自动曝光开关)

晚上/弱光: 噪点主要是自动增益拉满造成的。想让画面干净一点就先压增益上限
(gainceiling 2~3), 再用 ae_level 调亮; 想手动固定就先 --set agc 0 --set aec 0,
然后自己给 agc_gain / aec_value。
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import TelemetrixClient  # noqa: E402

CMD_CAMERA_TUNE = 0x7D

FIELDS = {
    "status": 0,
    "brightness": 1,
    "contrast": 2,
    "saturation": 3,
    "ae_level": 4,
    "agc_gain": 5,
    "aec_value": 6,
    "gainceiling": 7,
    "hmirror": 8,
    "vflip": 9,
    "awb_gain": 10,
    "aec2": 11,
    "agc": 12,
    "aec": 13,
    "set_reg_dsp": 14,   # 值 = (寄存器<<8) | 新值, 直接写 DSP 档
    "set_reg_sen": 15,   # 值 = (寄存器<<8) | 新值, 直接写 sensor 档
    "get_reg": 16,       # 值 = (档<<8)|寄存器, 读一个寄存器 (看串口)
    "clr_extra": 17,     # 清空"额外寄存器"列表
}


def main():
    parser = argparse.ArgumentParser(description="在线调 OV2640 参数 (0x7D)")
    parser.add_argument("host", help="板子 IP")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--status", action="store_true", help="只打一遍当前参数 (看串口)")
    parser.add_argument("--set", nargs=2, action="append", metavar=("字段", "值"),
                        help="例如 --set gainceiling 2 (可重复)")
    args = parser.parse_args()

    todo = []
    if args.status or not args.set:
        todo.append((0, 0))
    for name, value in (args.set or []):
        if name not in FIELDS:
            print("不认识的字段: %s (可用: %s)" % (name, ", ".join(FIELDS)))
            return 2
        todo.append((FIELDS[name], int(value, 0)))

    print("=== 连接 %s:%d ===" % (args.host, args.port))
    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print("连接失败: %s" % exc)
        print("检查: 板子是否在线 / 网关是不是还占着板子 (tools\\stop_s3extend.ps1)")
        return 1

    try:
        for field, value in todo:
            payload = [field & 0xFF, (value >> 8) & 0xFF, value & 0xFF]
            client.send(CMD_CAMERA_TUNE, *payload)
            time.sleep(0.3)
            print("已发送: field=%d value=%d" % (field, value))
    finally:
        client.close()

    print("新参数已经打到板子串口, 看日志:")
    print(r'  C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe tools\serial_tail.py COM14 --seconds 6 --filter "传感器|字段"')
    return 0


if __name__ == "__main__":
    sys.exit(main())
