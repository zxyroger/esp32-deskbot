#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
往本地 Banyan backplane 发一条消息（给 Scratch 扩展做状态回传用）。

守护进程 supervise_s3extend.ps1 用它把网关/板子连接状态报给 Scratch:
    {
      "report":   "board_status",
      "state":    "connected" | "waiting" | "disconnected" | "error",
      "address":  "192.168.0.107",      # 连上的板子 IP（可选）
      "firmware": "3.2.0",              # 固件版本（可选）
      "message":  "..."                 # 附加说明/错误行（可选）
    }

用法:
    python banyan_publish.py --state connected --address 192.168.0.107 --firmware 3.2.0
    python banyan_publish.py --json "{\"report\":\"board_status\",\"state\":\"waiting\"}"
    python banyan_publish.py -t to_esp32_gateway --command ip_address --address 192.168.0.107

backplane 的 IP 默认用回环地址 127.0.0.1：补丁 5 之后 backplane 监听的是 0.0.0.0，
回环永远连得上，本机 IP 变了也不受影响；也可以用 -b/--back-plane 指定。
"""

import argparse
import json
import socket
import sys
import time

import msgpack
import zmq


def local_ip():
    """和 python_banyan 一样: 连一个外网地址, 拿到本机使用的内网 IP。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 1))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    parser = argparse.ArgumentParser(description="向 Banyan backplane 发布一条消息")
    parser.add_argument("-b", "--back-plane", default=None, help="backplane IP (默认自动发现)")
    parser.add_argument("-t", "--topic", default="from_esp32_gateway", help="发布主题")
    parser.add_argument("-p", "--publish-port", default="43124", help="backplane 发布端口")
    parser.add_argument("--json", default=None, help="完整 payload (JSON 字符串)")
    parser.add_argument("--state", default="", help="board_status.state")
    parser.add_argument("--address", default="", help="板子 IP")
    parser.add_argument("--firmware", default="", help="固件版本")
    parser.add_argument("--message", default="", help="附加说明/错误行")
    parser.add_argument("-c", "--command", default="",
                        help='发一条普通指令, 例如 ip_address (配合 --address 使用)')
    parser.add_argument("--wait", type=float, default=0.6,
                        help="发送前的等待秒数 (等 PUB/SUB 完成订阅匹配, 太短消息会被丢掉)")
    args = parser.parse_args()

    if args.json:
        payload = json.loads(args.json)
    elif args.command:
        payload = {"command": args.command}
        if args.address:
            payload["address"] = args.address
    else:
        payload = {"report": "board_status", "state": args.state}
        if args.address:
            payload["address"] = args.address
        if args.firmware:
            payload["firmware"] = args.firmware
        if args.message:
            payload["message"] = args.message

    ip = args.back_plane or '127.0.0.1'

    ctx = zmq.Context()
    pub = ctx.socket(zmq.PUB)
    pub.connect("tcp://%s:%s" % (ip, args.publish_port))
    time.sleep(args.wait)
    pub.send_multipart([args.topic.encode(), msgpack.packb(payload, use_bin_type=True)])
    time.sleep(0.15)
    pub.close(0)
    ctx.term()
    return 0


if __name__ == "__main__":
    sys.exit(main())
