#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""不开 Scratch, 直接验证"摄像头积木"这条路走不走得通。

链路 (和 Scratch 里点积木走的是同一条):

    本脚本 ──Banyan to_esp32_gateway──► esp32gw ──TCP 31336──► 板子
    板子 ──0x10 帧头 / 0x0F 分片──► esp32gw 拼成整帧并 base64 ──Banyan──► 本脚本

做的事情:
    1. 往 to_esp32_gateway 发 ip_address (网关收到才会去连板子);
    2. 发 camera_config / camera_info / camera_snapshot;
    3. 订阅 from_esp32_gateway, 把 camera_info / camera_frame_start /
       camera_frame / camera_status 都打出来, 并把收到的 JPEG 存成文件;
    4. 校验存下来的字节是不是完整的 JPEG (FF D8 开头 / FF D9 结尾)。

用法 (用系统 Python, 它才有 python_banyan / s3-extend):
    C:\\Program Files\\Python313\\python.exe tools\\test_camera_blocks.py --host 192.168.0.103
    ... --host 192.168.0.103 --size VGA --quality 20 --frames 2

跑之前先把 s3-extend 起起来 (tools\\start_s3extend.ps1), 并且别让板子被
别的客户端占着 (tools\\stop_s3extend.ps1 会停掉整条服务)。
"""

import argparse
import base64
import os
import socket
import sys
import time

import msgpack
import zmq


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 1))
        return s.getsockname()[0]
    finally:
        s.close()


class Bus:
    """一个最简单 Banyan 客户端: 会发 (PUB) 也会收 (SUB)。"""

    def __init__(self, back_plane="127.0.0.1", publish_port="43124",
                 subscribe_port="43125", topic="from_esp32_gateway"):
        self.ctx = zmq.Context()
        self.pub = self.ctx.socket(zmq.PUB)
        self.pub.connect("tcp://%s:%s" % (back_plane, publish_port))
        self.sub = self.ctx.socket(zmq.SUB)
        self.sub.connect("tcp://%s:%s" % (back_plane, subscribe_port))
        self.sub.setsockopt(zmq.SUBSCRIBE, topic.encode())

    def send(self, payload, topic="to_esp32_gateway"):
        self.pub.send_multipart([topic.encode(), msgpack.packb(payload, use_bin_type=True)])

    def recv(self, timeout=1.0):
        if self.sub.poll(int(timeout * 1000)):
            return msgpack.unpackb(self.sub.recv_multipart()[1], raw=False)
        return None

    def close(self):
        self.pub.close(0)
        self.sub.close(0)
        self.ctx.term()


def main():
    parser = argparse.ArgumentParser(description="验证摄像头积木的 Banyan 链路")
    parser.add_argument("--host", required=True, help="板子 IP")
    parser.add_argument("--size", default=None,
                        help="先切分辨率: QVGA/VGA/SVGA/XGA/SXGA/UXGA 或 0~5")
    parser.add_argument("--quality", type=int, default=None, help="JPEG 质量 0~63")
    parser.add_argument("--frames", type=int, default=1, help="拍几帧 (默认 1)")
    parser.add_argument("--interval", type=int, default=0, help="帧间隔 ms (0=固件默认)")
    parser.add_argument("--out", default="scratch_photo", help="保存文件前缀")
    parser.add_argument("--dir", default=".", help="保存目录")
    parser.add_argument("--timeout", type=float, default=25.0,
                        help="等第一帧最多多少秒 (预热要 ~2s, 默认 25)")
    args = parser.parse_args()

    sizes = ["QVGA", "VGA", "SVGA", "XGA", "SXGA", "UXGA"]
    size = None
    if args.size is not None:
        size = sizes.index(args.size.upper()) if args.size.upper() in sizes else int(args.size)

    os.makedirs(args.dir, exist_ok=True)
    bus = Bus()
    # PUB/SUB 要先完成订阅匹配, 否则最先发的消息会被丢掉
    time.sleep(0.8)

    print("=== 连板子 %s ===" % args.host)
    bus.send({"command": "ip_address", "address": args.host})
    time.sleep(2.0)          # 等网关把 TCP 连上

    if size is not None or args.quality is not None:
        cfg = {"command": "camera_config"}
        if size is not None:
            cfg["size"] = size
        if args.quality is not None:
            cfg["quality"] = args.quality
        print("发 camera_config: %s" % cfg)
        bus.send(cfg)
        time.sleep(0.6)

    bus.send({"command": "camera_info"})
    bus.send({"command": "camera_snapshot", "frames": args.frames, "interval": args.interval})

    got = []
    deadline = time.time() + args.timeout
    info = None
    while time.time() < deadline and len(got) < args.frames:
        msg = bus.recv(0.5)
        if msg is None:
            continue
        report = msg.get("report")
        if report == "camera_info":
            info = msg
            print("camera_info: %s %dx%d 质量 %s XCLK %sMHz 状态 %s"
                  % (msg.get("size_name"), msg.get("width"), msg.get("height"),
                     msg.get("quality"), msg.get("xclk_mhz"), msg.get("state")))
        elif report == "camera_frame_start":
            print("camera_frame_start: 第 %s 帧 %sx%s %s 字节"
                  % (msg.get("index"), msg.get("width"), msg.get("height"), msg.get("length")))
        elif report == "camera_status":
            print("camera_status: %s (值 %s)" % (msg.get("state_name"), msg.get("value")))
        elif report == "camera_frame":
            raw = base64.b64decode(msg["data"])
            path = os.path.join(args.dir, "%s_%03d.jpg" % (args.out, msg.get("index", len(got))))
            with open(path, "wb") as handle:
                handle.write(raw)
            ok = raw[:2] == b"\xff\xd8" and raw[-2:] == b"\xff\xd9"
            print("camera_frame: 第 %s 帧 %sx%s %d 字节 -> %s%s"
                  % (msg.get("index"), msg.get("width"), msg.get("height"), len(raw), path,
                     "" if ok else "  (警告: 不是完整 JPEG!)"))
            got.append(path)
        else:
            print("其它报告: %s" % report)

    bus.send({"command": "camera_stop"})
    bus.close()

    print()
    if len(got) < args.frames:
        print("结果: 只收到 %d/%d 帧 —— 链路没通。检查:" % (len(got), args.frames))
        print("  * s3-extend 起来了吗 (tools\\start_s3extend.ps1, 看 9007/43124/43125 在不在)"
              " / 板子是不是被别的客户端占着")
        if info is None:
            print("  * 连 camera_info 都没回来: 网关可能没连上板子 (IP 对不对? 板子在线?)")
        return 1
    print("结果: 收到 %d 帧, 全部保存在 %s" % (len(got), os.path.abspath(args.dir)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
