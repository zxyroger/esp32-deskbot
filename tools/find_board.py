#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
在本地网段里找运行 Telemetrix 的板子 (板子换了 DHCP 地址时用)。

两种做法, 默认先用第一种:

  1. UDP 广播发现 (快, 秒级): 固件 (CONFIG_TMX_UDP_BEACON_ENABLE) 会周期性
     广播自己的地址, 也可以在这里主动发 "ONEGPIO?" 去问, 板子立刻回一条:
         ONEGPIO 192.168.0.103 31336
  2. TCP 扫描 (兜底, 用于固件是旧版 / 广播被路由器挡住的情况): 并发扫本机
     所在 /24 的 31336 端口, 对连上的主机发一条 "查询固件版本" 指令
     (Telemetrix 命令 5), 只有回 [4,5,major,minor,build] 的才算板子,
     避免把别的服务当成板子。

用法:
    python tools\\find_board.py                 # 先听 UDP 广播, 再扫网段
    python tools\\find_board.py --first         # 只打印第一个板子的 IP (给脚本用)
    python tools\\find_board.py --subnet 192.168.0.0/24
    python tools\\find_board.py --beacon-only   # 只用 UDP 广播发现
    python tools\\find_board.py --no-beacon     # 只扫网段
    python tools\\find_board.py --listen 10     # 只听 10 秒广播 (不发探测包)

退出码: 0 = 找到, 1 = 没找到
"""

import argparse
import concurrent.futures
import ipaddress
import socket
import sys
import time

PORT = 31336
CMD_GET_FIRMWARE_VERSION = 5

BEACON_PORT = 31337
BEACON_MAGIC = b"ONEGPIO"
BEACON_PROBE = b"ONEGPIO?"


def local_ipv4():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 1))
        return s.getsockname()[0]
    finally:
        s.close()


def probe(ip, timeout):
    try:
        sock = socket.create_connection((str(ip), PORT), timeout=timeout)
    except OSError:
        return None
    try:
        sock.settimeout(timeout)
        sock.sendall(bytes([1, CMD_GET_FIRMWARE_VERSION]))
        data = sock.recv(16)
    except OSError:
        data = b""
    finally:
        sock.close()
    # 固件应答: [字节数=4][报告类型=5][major][minor][build]
    if len(data) >= 5 and data[0] == 4 and data[1] == 5:
        return (str(ip), "%d.%d.%d" % (data[2], data[3], data[4]))
    return None


def broadcast_targets(subnet_hint=None):
    """要发探测包的广播地址: 本机所在子网的广播地址 + 255.255.255.255"""
    targets = ["255.255.255.255"]
    try:
        if subnet_hint:
            net = ipaddress.ip_network(subnet_hint, strict=False)
        else:
            net = ipaddress.ip_network(local_ipv4() + "/24", strict=False)
        targets.insert(0, str(net.broadcast_address))
    except Exception:
        pass
    return targets


def beacon_discover(timeout, subnet_hint=None, active_probe=True, quiet=False):
    """
    听板子的 UDP 广播 (必要时先主动探测), 返回 [(ip, port), ...]
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", BEACON_PORT))
    except OSError as exc:
        if not quiet:
            print("UDP 端口 %d 被占用 (%s), 跳过广播发现" % (BEACON_PORT, exc))
        sock.close()
        return []

    if active_probe:
        for addr in broadcast_targets(subnet_hint):
            try:
                sock.sendto(BEACON_PROBE, (addr, BEACON_PORT))
            except OSError:
                pass

    found = []
    seen = set()
    deadline = time.time() + timeout
    sock.settimeout(max(0.2, timeout))
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(256)
        except socket.timeout:
            break
        except OSError:
            break
        text = data.decode("utf-8", "replace").strip()
        # 只认 "ONEGPIO <ip> [port]" 这种两段以上的格式。
        # 注意别用 startswith("ONEGPIO"): 自己发的探测包 "ONEGPIO?" 会从本机
        # 广播回环回来, 那样会被误判成一块板子。
        parts = text.split()
        if len(parts) < 2 or parts[0] != BEACON_MAGIC.decode():
            continue
        ip = parts[1]
        try:
            ipaddress.ip_address(ip)
        except ValueError:
            continue
        port = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else PORT
        if ip in seen:
            continue
        seen.add(ip)
        found.append((ip, port))
    sock.close()
    return found


def main():
    parser = argparse.ArgumentParser(description="扫描本地网段找 Telemetrix 板子")
    parser.add_argument("--subnet", default=None, help="要扫描的网段 (默认本机所在 /24)")
    parser.add_argument("--timeout", type=float, default=0.6, help="单个主机超时秒数")
    parser.add_argument("--workers", type=int, default=64, help="并发线程数")
    parser.add_argument("--first", action="store_true", help="只打印第一个板子 IP")
    parser.add_argument("--beacon-timeout", type=float, default=2.5,
                        help="听 UDP 广播的秒数 (默认 2.5)")
    parser.add_argument("--no-beacon", action="store_true", help="跳过 UDP 广播发现")
    parser.add_argument("--beacon-only", action="store_true", help="只用 UDP 广播发现")
    parser.add_argument("--listen", type=float, metavar="SEC",
                        help="只听广播不发探测包, 持续 SEC 秒")
    args = parser.parse_args()

    found_beacon = []
    if args.listen is not None:
        if not args.first:
            print("监听 UDP %d 端口 %.1f 秒 (不发探测包) ..." % (BEACON_PORT, args.listen))
        found_beacon = beacon_discover(args.listen, args.subnet,
                                      active_probe=False, quiet=args.first)
    elif not args.no_beacon:
        if not args.first:
            print("先用 UDP 广播找板子 (端口 %d, 最多 %.1f 秒) ..." %
                  (BEACON_PORT, args.beacon_timeout))
        found_beacon = beacon_discover(args.beacon_timeout, args.subnet,
                                      active_probe=True, quiet=args.first)

    if found_beacon:
        if args.first:
            print(found_beacon[0][0])
            return 0
        print("广播发现 %d 块板子:" % len(found_beacon))
        for ip, port in found_beacon:
            print("  %s  端口 %d" % (ip, port))
        return 0

    if args.beacon_only or args.listen is not None:
        if not args.first:
            print("没有收到板子的 UDP 广播 (板子没上电 / 固件是旧版 / 广播被路由器挡住?)")
        return 1

    if args.subnet:
        net = ipaddress.ip_network(args.subnet, strict=False)
    else:
        ip = local_ipv4()
        net = ipaddress.ip_network(ip + "/24", strict=False)

    if not args.first:
        print("没有收到广播, 改为扫描 %s 的 %d 端口 ..." % (net, PORT))

    targets = [str(h) for h in net.hosts()]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for result in pool.map(lambda ip: probe(ip, args.timeout), targets):
            if result:
                found.append(result)

    if args.first:
        if found:
            print(found[0][0])
            return 0
        return 1

    if not found:
        print("在 %s 里没找到板子 (没有主机回应 Telemetrix 固件查询)" % net)
        return 1

    print("在 %s 里找到 %d 块板子:" % (net, len(found)))
    for ip, version in found:
        print("  %s  固件 %s" % (ip, version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
