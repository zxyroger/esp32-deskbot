#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
电池放电监视: 从 Banyan 订阅板子的电池上报 (固件 0x14 -> 网关 'battery' 消息),
按分钟记录电量/电压, 并估算放电速率 (%/小时)。

用法:
    python tools\\battery_watch.py                    # 一直跑, 每分钟一行
    python tools\\battery_watch.py --minutes 30       # 跑 30 分钟
    python tools\\battery_watch.py --out batt.csv     # 同时写 csv

注意: 速率是按"电量百分比"算的, 而电量计分辨率只有 1%, 所以要看到可信的
数字至少得跑十几分钟; 短于一分钟的变化基本都是噪声。
"""

import argparse
import os
import sys
import time

import zmq

try:
    import msgpack
except ImportError:  # pragma: no cover - 只是给个清楚的提示
    print("需要 msgpack: pip install msgpack", file=sys.stderr)
    raise

TOPIC = b'from_esp32_gateway'
DEFAULT_ENDPOINT = 'tcp://127.0.0.1:43125'


def unpack(raw):
    """Banyan 的消息是 '主题 + 空格 + msgpack'; 前面还有主题时从第一个 map 开始解。"""
    for off in range(0, min(len(raw), 64)):
        head = raw[off]
        if not (0x80 <= head <= 0x8F or head in (0xDE, 0xDF)):
            continue                      # 只从 fixmap / map16 / map32 试着解
        try:
            unpacker = msgpack.Unpacker(raw=False, strict_map_key=False)
            unpacker.feed(raw[off:])
            for msg in unpacker:
                return msg if isinstance(msg, dict) else None
        except Exception:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--endpoint', default=os.environ.get('BANYAN_SUB', DEFAULT_ENDPOINT))
    ap.add_argument('--minutes', type=float, default=0, help='0 = 一直跑')
    ap.add_argument('--interval', type=float, default=60, help='汇总间隔 (秒)')
    ap.add_argument('--out', default='', help='追加写 csv 的路径')
    args = ap.parse_args()

    ctx = zmq.Context()
    sock = ctx.socket(zmq.SUB)
    sock.connect(args.endpoint)
    sock.setsockopt(zmq.SUBSCRIBE, TOPIC)
    poller = zmq.Poller()
    poller.register(sock, zmq.POLLIN)

    csv = open(args.out, 'a', encoding='utf-8') if args.out else None
    if csv and csv.tell() == 0:
        csv.write('time,percent,millivolts,external,charging,vbus\n')

    t0 = time.time()
    deadline = t0 + args.minutes * 60 if args.minutes > 0 else None
    next_summary = t0
    last = None          # 最近一条电池读数
    first = None         # 第一行 (用于算整段平均)
    samples = 0
    csv_last = 0.0

    print('# 端点 %s; 等板子第一次上报 (没插 USB / 没在充电时数字才有意义)' % args.endpoint)
    try:
        while True:
            now = time.time()
            if deadline and now > deadline:
                break
            if poller.poll(500):
                msg = unpack(sock.recv())
                if msg and msg.get('report') == 'battery':
                    last = msg
                    samples += 1
                    # csv 里每次上报都记一行 (按 interval 抽稀), 方便事后算速率
                    if csv and now - csv_last >= args.interval:
                        csv_last = now
                        csv.write('%s,%s,%s,%d,%d,%s\n' % (
                            time.strftime('%H:%M:%S'), last.get('percent', -1),
                            last.get('millivolts', 0), bool(last.get('external_power')),
                            bool(last.get('charging')), last.get('vbus_millivolts', 0)))
                        csv.flush()
            if last is None or now < next_summary:
                continue
            next_summary = now + args.interval

            stamp = time.strftime('%H:%M:%S')
            pct = last.get('percent', -1)
            mv = last.get('millivolts', 0)
            ext = bool(last.get('external_power'))
            chg = bool(last.get('charging'))
            vbus = last.get('vbus_millivolts', 0)
            line = '%s  电量 %3s%%  电压 %.3f V  %s  样本 %d' % (
                stamp, pct, mv / 1000.0,
                ('充电中' if chg else ('外部供电' if ext else '电池供电')), samples)
            if first is None:
                first = last
                first_t = now
            else:
                dt_h = (now - first_t) / 3600.0
                dp = pct - first.get('percent', pct)
                if dt_h > 0.01 and dp <= 0:
                    line += '   放电 %.1f %%/h' % (dp / dt_h)
                elif dt_h > 0.01:
                    line += '   (电量在涨, 应该在充电)'
            print(line)
    except KeyboardInterrupt:
        pass
    finally:
        if csv:
            csv.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
