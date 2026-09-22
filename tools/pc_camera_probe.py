#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
让板子在"摄像头正在出流"的状态下跑一次 DVP 引脚探针, 并把结果抓回来。

为什么需要它: 固件空闲时会给摄像头断电降温, 探针在断电状态下量不到东西;
而且探针结果是从串口直连输出的 (ets_printf), 所以要一边开流一边看串口。

用法 (先 tools\\stop_s3extend.ps1 让出板子):
    # 一个窗口开着串口
    C:\\Espressif\\tools\\python\\v5.5.4\\venv\\Scripts\\python.exe tools\\serial_tail.py COM14 --seconds 12 --filter "PROBE" --raw

    # 另一个窗口触发
    python tools\\pc_camera_probe.py 192.168.0.106
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import TelemetrixClient, RPT_CAMERA_PROBE  # noqa: E402

CMD_CAMERA_SNAPSHOT = 0x79
CMD_CAMERA_STOP = 0x7A
CMD_CAMERA_PROBE = 0x7C


def main():
    parser = argparse.ArgumentParser(description="开流 + 触发 DVP 引脚探针")
    parser.add_argument("host", help="板子 IP")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--warmup", type=float, default=0.6,
                        help="先开流多久让传感器稳定 (秒, 默认 0.6)")
    parser.add_argument("--probe", type=float, default=2.5,
                        help="探针跑多久 (秒, 默认 2.5; 探针本身约 0.5~1s)")
    args = parser.parse_args()

    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print("连接失败: %s (板子在线吗? 网关占着吗? tools\\stop_s3extend.ps1)" % exc)
        return 1

    try:
        client.send(CMD_CAMERA_SNAPSHOT, 0, 0, 200)   # 连续拍
        # 必须一边读一边丢帧: 板子发不出去就会自己停止连拍 (然后空闲断电, 探针就量不到了)
        deadline = time.time() + args.warmup
        while time.time() < deadline:
            try:
                client.read_packet()
            except Exception:
                pass
        client.send(CMD_CAMERA_PROBE)                 # 让板子量一遍 DVP 引脚
        deadline = time.time() + args.probe
        results = {}
        done = False
        while time.time() < deadline and not done:
            try:
                packet = client.read_packet()
            except Exception:
                break
            if packet is None:
                continue
            if packet[0] == RPT_CAMERA_PROBE and len(packet) >= 4:
                kind = packet[1]
                value = (packet[2] << 8) | packet[3]
                if kind == 0x7F:
                    done = True
                else:
                    results.setdefault(kind, []).append(value)
        client.send(CMD_CAMERA_STOP)
        time.sleep(0.2)
    finally:
        client.close()

    names = {0: "PCLK  (每 1ms 边沿)", 1: "VSYNC (每 200ms 边沿)", 2: "HREF  (每 20ms 边沿)",
             0x20: "PCLK 连测 5 次 (kHz, 看抖动)"}
    for i in range(8):
        names[3 + i] = "D%d    (数到 1000 沿为止, 0 = 没信号)" % i
    print("--- 探针结果 ---")
    for kind in sorted(results):
        vals = results[kind]
        print("  %-34s %s" % (names.get(kind, "类型 %d" % kind), " ".join(str(v) for v in vals)))
    if not results:
        print("  (没收到探针上报: 板子在出流吗? 网关占着板子吗?)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
