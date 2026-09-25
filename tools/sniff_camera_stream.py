#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""量一量摄像头视频的"周期性卡顿"到底周期是多少、卡在哪一段。

链路 (和 Scratch 里点「打开摄像头」是同一条):

    本脚本 ──Banyan to_esp32_gateway──► esp32gw ──TCP──► 板子
    板子 ──0x10/0x0F──► esp32gw 拼帧 + base64 ──Banyan from_esp32_gateway──► 本脚本

默认**只订阅, 不发指令** (被动嗅探正在跑的流, 不打扰任何客户端)。
加 --start 才会自己发 camera_config/camera_snapshot (结束时会 camera_stop)。

用法 (用系统 Python, 它才有 python_banyan / zmq / msgpack):
    python tools\\sniff_camera_stream.py --seconds 15                 # 被动
    python tools\\sniff_camera_stream.py --start --seconds 15         # 自己开流
    python tools\\sniff_camera_stream.py --start --size QVGA --quality 35 --interval 30

输出里最有用的是两行:
  * 「长停顿」: 帧间隔超过中位数 2 倍的次数和发生时刻 —— 这就是肉眼看到的卡顿;
  * 「自相关主周期」: 停顿是不是按固定节奏重复 (周期性), 还是随机抖动。
"""

import argparse
import base64
import collections
import io
import json
import statistics
import sys
import time

import msgpack
import zmq


class Bus:
    """最简单 Banyan 客户端: 会发 (PUB) 也会收 (SUB)。"""

    def __init__(self, back_plane="127.0.0.1", publish_port="43124",
                 subscribe_port="43125", topic="from_esp32_gateway"):
        self.ctx = zmq.Context()
        self.pub = self.ctx.socket(zmq.PUB)
        self.pub.connect("tcp://%s:%s" % (back_plane, publish_port))
        self.sub = self.ctx.socket(zmq.SUB)
        self.sub.connect("tcp://%s:%s" % (back_plane, subscribe_port))
        self.sub.setsockopt(zmq.SUBSCRIBE, topic.encode())

    def send(self, payload, topic="to_esp32_gateway"):
        self.pub.send_multipart([topic.encode(),
                                 msgpack.packb(payload, use_bin_type=True)])

    def recv(self, timeout=0.2):
        if self.sub.poll(int(timeout * 1000)):
            return msgpack.unpackb(self.sub.recv_multipart()[1], raw=False)
        return None

    def close(self):
        self.pub.close(0)
        self.sub.close(0)
        self.ctx.term()


class WsBus:
    """走 wsgw:9007 的 WebSocket 客户端 —— 和 Scratch 扩展看到的是同一条流。

    连上之后先发 {"id": "to_esp32_gateway"}, wsgw 就会把 from_esp32_gateway
    上的上报转给我们 (也允许我们往下发积木指令)。
    """

    def __init__(self, url="ws://127.0.0.1:9007"):
        from websockets.sync.client import connect
        self.ws = connect(url, open_timeout=5)
        self.ws.send(json.dumps({"id": "to_esp32_gateway"}))
        self.url = url

    def send(self, payload, topic=None):
        self.ws.send(json.dumps(payload))

    def recv(self, timeout=0.2):
        try:
            return json.loads(self.ws.recv(timeout=timeout))
        except TimeoutError:
            return None

    def close(self):
        self.ws.close()


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def autocorrelation(values):
    """返回 {lag: 相关系数}, 用来找"间隔序列自己跟自己的相似度"。

    周期性的卡顿会让 lag = 卡顿周期(以帧为单位) 处出现明显的正相关。
    """
    n = len(values)
    if n < 8:
        return {}
    mean = statistics.fmean(values)
    var = sum((v - mean) ** 2 for v in values)
    if var <= 0:
        return {}
    result = {}
    for lag in range(1, max(2, min(80, n // 3))):
        acc = 0.0
        for i in range(n - lag):
            acc += (values[i] - mean) * (values[i + lag] - mean)
        result[lag] = acc / var
    return result


def pearson(xs, ys):
    """两个序列的相关系数 (没有 scipy 也能算)。"""
    n = min(len(xs), len(ys))
    if n < 3:
        return 0.0
    xs, ys = xs[:n], ys[:n]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sx = sum((x - mx) ** 2 for x in xs) ** 0.5
    sy = sum((y - my) ** 2 for y in ys) ** 0.5
    if sx == 0 or sy == 0:
        return 0.0
    return sum((xs[i] - mx) * (ys[i] - my) for i in range(n)) / (sx * sy)


def verify_frame(payload, index):
    """这一帧的 JPEG 完好么? 坏帧是"视频突然卡住不动"的常见原因 ——
    扩展那边 <img>.onerror 一响就把整条流停掉 (videoOn = false)。"""
    b64 = payload.get('data') or ''
    try:
        raw = base64.b64decode(b64)
    except Exception as exc:
        return {'index': index, 'ok': False, 'why': 'base64 坏了: %s' % exc}
    if len(raw) < 4:
        return {'index': index, 'ok': False, 'why': '只有 %d 字节' % len(raw)}
    if raw[:2] != b'\xff\xd8':
        return {'index': index, 'ok': False, 'why': '没有 SOI 帧头'}
    if raw[-2:] != b'\xff\xd9':
        return {'index': index, 'ok': False,
                'why': '没有 EOI 结尾 (末两字节 %s), 共 %d 字节'
                       % (raw[-2:].hex(), len(raw))}
    declared = payload.get('length')
    if declared and declared != len(raw):
        return {'index': index, 'ok': False,
                'why': '声明的 %d 字节, 实收 %d 字节' % (declared, len(raw))}
    try:
        from PIL import Image
        with Image.open(io.BytesIO(raw)) as image:
            image.load()
    except Exception as exc:
        return {'index': index, 'ok': False, 'why': '解码失败: %s' % exc}
    return {'index': index, 'ok': True, 'why': ''}


def run_diag(args):
    """只监听扩展发的 video_diag 上报 (topic to_esp32_gateway)。

    这是"板子在出图、画面却不动"最直接的一把尺子: 它把扩展内部状态
    (有没有在放、忙不忙、画了多少帧、画画成功离现在多久、正在刷的造型是不是
    当前造型) 摆出来, 不用盯着 Scratch 界面。
    """
    bus = Bus(topic="to_esp32_gateway")
    print("监听扩展的调试上报 (video_diag), 最多 %.0f 秒 —— 扩展里 VIDEO_DIAG_ENABLED "
          "必须是 true, 且重载过扩展" % args.seconds)
    deadline = time.perf_counter() + args.seconds
    seen = 0
    while time.perf_counter() < deadline:
        payload = bus.recv(0.5)
        if not payload or payload.get("command") != "video_diag":
            continue
        seen += 1
        print("\n[%s] 第 %d 条调试上报" % (time.strftime("%H:%M:%S"), seen))
        print("  流式播放: 开着=%s 忙=%s 已画=%s 帧 丢掉=%s 帧 连续失败=%s 自动重画=%s 次 帧率=%s"
              % (payload.get("on"), payload.get("busy"), payload.get("frames"),
                 payload.get("dropped"), payload.get("errors"), payload.get("recoveries"),
                 payload.get("fps")))
        print("  距上一帧到 = %s ms, 距上次画成功 = %s ms%s"
              % (payload.get("since_arrival_ms"), payload.get("since_draw_ms"),
                 ("  ← 有问题: 帧在来但很久没画成功" if _stalled(payload) else "")))
        print("  当前编辑目标 = %s, 造型 = %s, 变形=%s 大小=%s"
              % (payload.get("sprite"), payload.get("current_costume"),
                 payload.get("visible"), payload.get("size")))
        print("  「%s」造型: 存在=%s skinId=%s"
              % ("摄像头画面", payload.get("costume"), payload.get("costume_skin")))
        print("  该角色的全部造型: %s" % (payload.get("costumes"),))
        print("  收到上报 %s 条, 最近一条 = %s (%s ms 前), 拍照计数 = %s"
              % (payload.get("reports"), payload.get("last_report"),
                 payload.get("last_report_ms"), payload.get("photo_count")))
        if payload.get("last_error"):
            print("  最近一次画失败: %s" % payload.get("last_error"))
        print("  连接=%s 状态=%s" % (payload.get("connected"), payload.get("status")))
    if not seen:
        print("\n没收到 video_diag —— 扩展可能还没重载 (旧版没有这个上报)")
    bus.close()
    return 0


def _stalled(payload):
    try:
        arrival = int(payload.get("since_arrival_ms", -1))
        drawn = int(payload.get("since_draw_ms", -1))
    except (TypeError, ValueError):
        return False
    return arrival >= 0 and arrival < 3000 and (drawn < 0 or drawn > 3000)


def main():
    parser = argparse.ArgumentParser(description="量摄像头流式播放的帧间隔/卡顿周期")
    parser.add_argument("--seconds", type=float, default=15.0, help="采集多少秒")
    parser.add_argument("--start", action="store_true",
                        help="自己发 camera_snapshot 开流 (否则只被动嗅探)")
    parser.add_argument("--size", default="QVGA", help="开流时先切的分辨率")
    parser.add_argument("--quality", type=int, default=35, help="开流时的 JPEG 质量")
    parser.add_argument("--interval", type=int, default=30, help="帧间隔 ms")
    parser.add_argument("--restore", default=None,
                        help="跑完不停止, 而是恢复成 尺寸:质量:间隔 例如 SXGA:10:30")
    parser.add_argument("--quiet", action="store_true", help="不逐帧打印")
    parser.add_argument("--gaps", action="store_true", help="把每帧间隔按顺序打印成 CSV")
    parser.add_argument("--info", action="store_true",
                        help="先问一次 camera_info (只读查询, 看板子当前分辨率/质量)")
    parser.add_argument("--dump", default=None,
                        help="把逐帧原始数据写成 CSV: <NAME>.frames.csv / <NAME>.starts.csv")
    parser.add_argument("--via", choices=("banyan", "ws"), default="banyan",
                        help="在哪一层量: banyan=网关发出来的总线, ws=Scratch 收到的 WebSocket")
    parser.add_argument("--ws-url", default="ws://127.0.0.1:9007")
    parser.add_argument("--diag", action="store_true",
                        help="只看扩展自己发的 video_diag 调试上报 (需要扩展里 VIDEO_DIAG_ENABLED = true)")
    parser.add_argument("--verify", action="store_true",
                        help="把每帧 JPEG 都解一遍, 报出坏帧 (解码失败的帧会让扩展停掉视频)")
    args = parser.parse_args()

    if args.diag:
        return run_diag(args)

    sizes = ["QVGA", "VGA", "SVGA", "XGA", "SXGA", "UXGA"]
    size_index = None
    if args.size is not None:
        size_index = int(args.size) if args.size.isdigit() else sizes.index(args.size.upper())

    bus = WsBus(args.ws_url) if args.via == "ws" else Bus()
    print("测量点: %s" % ("WebSocket (和 Scratch 同一条)" if args.via == "ws"
                          else "Banyan 总线 (网关出口)"))
    # ZMQ 的 SUB 连接是异步的, 稍微等一下, 免得漏掉开头的帧
    time.sleep(0.5)

    if args.info:
        bus.send({"command": "camera_info"})

    if args.start:
        bus.send({"command": "camera_config", "size": size_index,
                  "quality": args.quality})
        time.sleep(0.3)
        bus.send({"command": "camera_info"})      # 确认设置真的生效了
        time.sleep(0.3)
        bus.send({"command": "camera_snapshot", "frames": 0, "interval": args.interval})
        print("已发 camera_snapshot (连续), 尺寸 %s 质量 %d 间隔 %dms"
              % (sizes[size_index], args.quality, args.interval))
    else:
        print("被动嗅探 (不发任何指令) —— 需要 Scratch 那边正在「打开摄像头」才有帧")

    deadline = time.perf_counter() + args.seconds
    frames = []                      # (相对时刻, 帧序号, 帧字节数)
    verify_results = []
    start_times = []                 # (相对时刻, 帧序号, 声明长度) —— 帧头到达时刻
    starts = 0
    statuses = []
    first = None
    per_second = collections.Counter()

    while time.perf_counter() < deadline:
        payload = bus.recv(0.2)
        if not payload:
            continue
        report = payload.get("report")
        now = time.perf_counter()
        if report == "camera_frame_start":
            starts += 1
            if first is None:
                first = now
            start_times.append((now, payload.get("index"), payload.get("length")))
            continue
        if report == "camera_status":
            statuses.append((round((now - first) if first else 0, 2),
                             payload.get("state_name"), payload.get("value")))
            continue
        if report == "camera_info":
            print("板子当前状态: %sx%s 质量 %d 尺寸 %s XCLK %dMHz"
                  % (payload.get("width"), payload.get("height"), payload.get("quality"),
                     payload.get("size_name"), payload.get("xclk_mhz") or 0))
            continue
        if report != "camera_frame":
            continue
        if first is None:
            first = now
        if args.verify:
            record = verify_frame(payload, index=len(frames))
            verify_results.append(record)
            if not record['ok'] and len(verify_results) <= 200:
                print("  坏帧 #%d: %s" % (record['index'], record['why']))
        length = len(payload.get("data") or "")
        frames.append((now, payload.get("index"), payload.get("length"), length))
        per_second[int(now - first)] += 1
        if not args.quiet:
            print("  帧 #%s  t=%6.3fs  %5d 字节 (base64 %d)"
                  % (payload.get("index"), now - first, payload.get("length") or 0, length))

    if args.start:
        if args.restore:
            restore_size, restore_quality, restore_interval = args.restore.split(":")
            bus.send({"command": "camera_config",
                      "size": int(restore_size) if restore_size.isdigit()
                              else sizes.index(restore_size.upper()),
                      "quality": int(restore_quality)})
            time.sleep(0.3)
            bus.send({"command": "camera_snapshot", "frames": 0,
                      "interval": int(restore_interval)})
            print("已恢复成 %s 质量 %s 间隔 %sms 的连续流"
                  % (restore_size, restore_quality, restore_interval))
        else:
            bus.send({"command": "camera_stop"})
            print("已发 camera_stop")

    if not frames:
        print("\n没收到任何帧 (没人开摄像头? 或者链路断了)")
        bus.close()
        return 1

    times = [f[0] - frames[0][0] for f in frames]
    gaps = [round((times[i] - times[i - 1]) * 1000, 1) for i in range(1, len(frames))]
    sizes_bytes = [f[2] or 0 for f in frames]
    # 同一帧: 帧头(0x10) -> 整帧收齐 的耗时。按顺序配对 (帧序号 0~255 会回绕, 不能用它当键)
    transfer_times = []
    for i, (when, _index, _length, _b64) in enumerate(frames):
        began = start_times[i][0] if i < len(start_times) else None
        transfer_times.append(round((when - began) * 1000, 1) if began else 0.0)

    if args.dump:
        base = args.dump
        with open(base + ".frames.csv", "w", encoding="utf-8") as handle:
            handle.write("index,t_ms,size,transfer_ms,gap_after_ms\n")
            for i, (when, index, size, _b64) in enumerate(frames):
                gap = gaps[i] if i < len(gaps) else ""
                handle.write("%s,%.1f,%d,%.1f,%s\n"
                             % (index, (when - frames[0][0]) * 1000, size or 0,
                                transfer_times[i], gap))
        with open(base + ".starts.csv", "w", encoding="utf-8") as handle:
            handle.write("index,t_ms,length\n")
            for when, index, length in start_times:
                handle.write("%s,%.1f,%s\n" % (index, (when - frames[0][0]) * 1000, length))
        print("原始数据已写入 %s.frames.csv / %s.starts.csv" % (base, base))

    span = times[-1]
    print("\n===== 汇总 =====")
    print("收到 %d 帧 / %.2f 秒 = %.1f 帧/秒 (帧头 %d 个)"
          % (len(frames), span, (len(frames) - 1) / span if span else 0, starts))
    print("帧大小: 中位 %d 字节, 最大 %d 字节; 合计 %.1f KB/s"
          % (statistics.median(sizes_bytes), max(sizes_bytes),
             sum(sizes_bytes) / 1024 / span if span else 0))
    if gaps:
        print("帧间隔(ms): min %.0f  中位 %.0f  p90 %.0f  max %.0f  平均 %.0f"
              % (min(gaps), statistics.median(gaps), percentile(gaps, 0.9),
                 max(gaps), statistics.fmean(gaps)))
        threshold = 2 * statistics.median(gaps)
        stalls = [(i, g) for i, g in enumerate(gaps) if g > threshold]
        print("长停顿 (>%.0fms): %d 次" % (threshold, len(stalls)))
        for index, gap in stalls[:40]:
            print("   第 %d 帧之后停了 %.0f ms (t=%.2fs)"
                  % (index + 2, gap, times[index + 1]))
        ac = autocorrelation(gaps)
        top = sorted(ac.items(), key=lambda kv: -kv[1])[:5]
        if top:
            print("自相关主周期 (帧间隔序列):")
            for lag, value in top:
                if value <= 0.15:
                    continue
                print("   lag %2d 帧 -> 周期约 %5.0f ms, 相关度 %.2f"
                      % (lag, lag * statistics.fmean(gaps), value))
        outliers = [g for g in gaps if g > 3 * statistics.median(gaps)]
        print("慢帧占比: %.1f%% (>3x 中位), 剔除慢帧后中位 %.0f ms"
              % (100.0 * len(outliers) / len(gaps),
                 statistics.median([g for g in gaps if g <= 3 * statistics.median(gaps)])
                 if len(outliers) < len(gaps) else 0))
        # 抖动是不是"这一帧太大/太慢"造成的? 用相关系数看
        if len(gaps) > 8:
            pair_size = list(zip(sizes_bytes[:-1], gaps))
            pair_move = list(zip(transfer_times[:-1], gaps)) if transfer_times else []
            print("相关性: 帧字节数 vs 下一帧间隔 r=%.2f"
                  % pearson([p[0] for p in pair_size], [p[1] for p in pair_size]))
            if pair_move:
                print("         单帧传输耗时 vs 下一帧间隔 r=%.2f"
                      % pearson([p[0] for p in pair_move], [p[1] for p in pair_move]))
            worst = sorted(zip(gaps, sizes_bytes[1:], transfer_times[1:] if transfer_times else [0] * len(gaps)),
                           reverse=True)[:8]
            print("最长的 8 个间隔 (间隔ms / 帧字节 / 传输ms):")
            for gap, size, move in worst:
                print("   %6.0f ms  %6d 字节  %6.0f ms" % (gap, size, move))
        if args.gaps:
            print("\n逐帧间隔 (ms, 按到达顺序):")
            print(",".join("%.0f" % g for g in gaps))

    if len(start_times) > 1:
        # 帧头 (0x10) 是网关一收到"板子开始发这一帧"就转发的, 所以帧头节奏
        # ≈ 板子出帧的节奏; 整帧到达节奏 = 板子 + PC 管道。
        # 两者一对比, 就知道卡顿出在板子还是 PC 侧。
        start_gaps = [round((start_times[i][0] - start_times[i - 1][0]) * 1000, 1)
                      for i in range(1, len(start_times))]
        print("\n板子出帧节奏 (帧头 0x10 之间的间隔, ms): "
              "min %.0f 中位 %.0f p90 %.0f max %.0f"
              % (min(start_gaps), statistics.median(start_gaps),
                 percentile(start_gaps, 0.9), max(start_gaps)))
        transfer = transfer_times
        if transfer:
            print("单帧传输耗时 (帧头->收齐, ms): 中位 %.0f max %.0f"
                  % (statistics.median(transfer), max(transfer)))
        stalls = [(i, g) for i, g in enumerate(start_gaps) if g > 2 * statistics.median(start_gaps)]
        print("板子侧长停顿 (>%.0fms): %d 次"
              % (2 * statistics.median(start_gaps), len(stalls)))
        for index, gap in stalls[:20]:
            print("   第 %d 个帧头之后停了 %.0f ms" % (index + 1, gap))
        if args.gaps:
            print("帧头间隔 CSV:")
            print(",".join("%.0f" % g for g in start_gaps))

    print("\n每秒帧数:")
    for second in sorted(per_second):
        print("   t=%3ds  %s (%d)" % (second, "#" * per_second[second],
                                       per_second[second]))
    if statuses:
        print("\n板子上报的状态: %s" % statuses)
    if args.verify and verify_results:
        bad = [r for r in verify_results if not r['ok']]
        print("\nJPEG 校验: %d 帧里 %d 帧有问题" % (len(verify_results), len(bad)))
        reasons = collections.Counter(r['why'].split(':')[0] for r in bad)
        for why, count in reasons.most_common(6):
            print("   %-24s x%d" % (why, count))
        if bad:
            print("   坏帧都出现在第 %s 帧附近" % ([r['index'] for r in bad[:8]]))
    bus.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
