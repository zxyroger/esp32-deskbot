#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
板载 OV2640 摄像头自检: 让板子拍照, 把 JPEG 收下来存成文件

用法:
    python pc_camera_check.py 192.168.0.103                     # 拍 1 帧, 存 cam_000.jpg
    python pc_camera_check.py 192.168.0.103 --frames 5          # 拍 5 帧
    python pc_camera_check.py 192.168.0.103 --size QVGA         # 先切到 QVGA 再拍
    python pc_camera_check.py 192.168.0.103 --size VGA --quality 8
    python pc_camera_check.py 192.168.0.103 --stream 5          # 连续拍 5 秒 (按间隔存帧)
    python pc_camera_check.py 192.168.0.103 --info              # 只看摄像头状态
    python pc_camera_check.py 192.168.0.103 --scan              # 扫 I2C, 看 SCCB 上有没有设备
    python pc_camera_check.py 192.168.0.103 --view              # 拍完用系统看图程序打开

做了什么:
    1. 连接 TCP 31336, 查一次固件版本 (确认链路通)
    2. 可选: 0x78 改分辨率/JPEG 质量; 0x7B 查摄像头状态
    3. 0x79 让板子拍照 (帧数 / 间隔), 板子把每帧拆成 0x0F 分片发回来,
       每帧开头有一条 0x10 (序号/格式/宽/高/长度), 收齐后拼成 .jpg
    4. --stream 秒数: 一直拍, 直到时间到再发 0x7A 停下

排障:
    * 一帧都收不到 / 板子初始化日志报错 -> 先 --scan 看 SCCB 上有没有 0x30 (OV2640)
      和 0x18 (ES8311); 没有 0x30 就是摄像头供电/排线/SIOD-SIOC 的问题
    * 画面花屏/横条纹 -> 把 menuconfig 里 XCLK 频率从 20MHz 降到 10~16MHz
    * 板子报内存不够 -> menuconfig 里把分辨率降到 QVGA, 或帧缓冲数量改成 1

注意: 跑之前先停掉 s3-extend (板子同时只服务一个客户端):
    tools\\stop_s3extend.ps1
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import (  # noqa: E402
    CMD_CAMERA_CONFIG,
    CMD_CAMERA_INFO,
    CMD_CAMERA_SNAPSHOT,
    CMD_CAMERA_STOP,
    CMD_CAMERA_PROBE,
    CMD_GET_FIRMWARE_VERSION,
    CMD_I2C_BEGIN,
    CMD_I2C_READ,
    RPT_CAMERA_FRAME,
    RPT_CAMERA_FRAME_INFO,
    RPT_CAMERA_INFO,
    RPT_CAMERA_STATUS,
    RPT_FIRMWARE,
    RPT_I2C_READ,
    RPT_I2C_TOO_FEW,
    RPT_I2C_TOO_MANY,
    TelemetrixClient,
)

SIZES = ["QVGA", "VGA", "SVGA", "XGA", "SXGA", "UXGA"]
CAM_STATE = {0: "空闲", 1: "正在拍", 2: "出错"}


def query_info(client, timeout=3.0):
    """0x7B -> 0x12: 打印摄像头状态, 返回 (状态, 宽, 高, 质量, 分辨率索引) 或 None"""
    client.send(CMD_CAMERA_INFO)
    packet = client.wait_report(RPT_CAMERA_INFO, timeout=timeout)
    if packet is None:
        return None
    data = packet[1:]
    state = data[0]
    width = (data[1] << 8) | data[2]
    height = (data[3] << 8) | data[4]
    quality = data[5]
    size_index = data[6] if len(data) > 6 else -1
    xclk_mhz = data[7] if len(data) > 7 else 0
    name = SIZES[size_index] if 0 <= size_index < len(SIZES) else f"?{size_index}"
    print(f"摄像头: {CAM_STATE.get(state, state)}  分辨率 {width}x{height} ({name})  "
          f"JPEG 质量 {quality}  XCLK {xclk_mhz}MHz")
    if state == 2:
        print("  (状态是\"出错\": 板子初始化没成功, 看串口日志里 OV2640 那几行)")
    return state, width, height, quality, size_index


def scan_i2c(client, timeout=0.4):
    """用协议里的 I2C 读写扫一遍地址, 看 SCCB 上挂了什么 (0x30=OV2640, 0x18=ES8311)"""
    client.send(CMD_I2C_BEGIN, 0, 0)   # 0,0 = 用固件 Kconfig 里的引脚
    time.sleep(0.2)
    found = []
    for addr in range(0x08, 0x78):
        client.send(CMD_I2C_READ, addr, 0x00, 1, 1)
        deadline = time.time() + timeout
        answered = False
        while time.time() < deadline:
            packet = client.read_packet()
            if packet is None:
                continue
            rpt = packet[0]
            if rpt == RPT_I2C_READ:
                answered = True
                break
            if rpt in (RPT_I2C_TOO_FEW, RPT_I2C_TOO_MANY):
                break
        if answered:
            found.append(addr)
            note = ""
            if addr == 0x30:
                note = "  <- OV2640 (SCCB)"
            elif addr == 0x18:
                note = "  <- ES8311 (音频 codec)"
            print(f"  0x{addr:02X} 有应答{note}")
    if not found:
        print("  一个设备都没应答: 检查 I2C 上拉/接线, 以及固件是否把总线建起来了")
    elif 0x30 not in found:
        print("  注意: 没看到 0x30 (OV2640): 摄像头供电 / SIOD-SIOC / PWDN 先查一遍")
    return found


class FrameAssembler:
    """把 0x10 (帧头) + 0x0F (分片) 拼成一帧 JPEG"""

    def __init__(self, verbose=True):
        self.verbose = verbose
        self.reset()

    def reset(self):
        self.width = 0
        self.height = 0
        self.length = 0
        self.index = -1
        self.data = bytearray()
        self.gaps = 0
        self.started_at = 0.0

    def feed(self, report, data):
        """喂一条报告, 收齐一帧时返回 JPEG bytes, 否则返回 None"""
        if report == RPT_CAMERA_FRAME_INFO:
            self.index = data[0]
            fmt = data[1]
            self.width = (data[2] << 8) | data[3]
            self.height = (data[4] << 8) | data[5]
            self.length = int.from_bytes(bytes(data[6:10]), "little")
            self.data = bytearray()
            self.gaps = 0
            self.started_at = time.time()
            # 固件发的是 esp32-camera 的 pixformat_t: 3 = GRAYSCALE, 4 = JPEG
            # (早期协议文档按 3=JPEG 写的, 这里两个都认, 免得好端端的 JPEG 被当成坏帧)
            if fmt not in (3, 4):
                print(f"  警告: 板子发来的不是 JPEG (format={fmt})")
            return None

        if report != RPT_CAMERA_FRAME:
            return None
        if self.length == 0:
            return None      # 没收到帧头 (比如中途才开始收), 丢掉

        offset = (data[1] << 16) | (data[2] << 8) | data[3]
        payload = bytes(data[4:])
        if offset != len(self.data):
            self.gaps += 1
            if self.verbose and self.gaps <= 3:
                print(f"  警告: 分片错位 (期望 {len(self.data)}, 收到 {offset})")
            return None
        self.data += payload

        if len(self.data) >= self.length:
            return bytes(self.data[:self.length])
        return None


def save_frame(jpeg, path, width, height, index):
    with open(path, "wb") as handle:
        handle.write(jpeg)
    ok = jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9"
    print(f"  第 {index} 帧: {width}x{height}, {len(jpeg)} 字节 -> {path}"
          + ("" if ok else "  (警告: 不是完整的 JPEG: 收包时丢数据了)"))
    return ok


def main():
    parser = argparse.ArgumentParser(description="板载 OV2640 摄像头自检")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--frames", type=int, default=1,
                        help="拍几帧 (默认 1); 0 表示一直拍, 靠 --timeout 停")
    parser.add_argument("--interval", type=int, default=200,
                        help="两帧之间的间隔 ms (默认 200, 0 = 用固件里的默认值)")
    parser.add_argument("--size", choices=SIZES, help="先把分辨率切成这个 (不改就省略)")
    parser.add_argument("--quality", type=int, default=-1,
                        help="JPEG 质量 0~63 (越小越清晰, 默认不改)")
    parser.add_argument("--pixels", choices=["jpeg", "yuv422"], default=None,
                        help="切换像素格式 (排障用: yuv422 时收到的是原始像素, 不是图片; "
                             "建议配 --size QQVGA)")
    parser.add_argument("--out", default="cam", help="输出文件名前缀 (默认 cam)")
    parser.add_argument("--dir", default=".", help="输出目录 (默认当前目录)")
    parser.add_argument("--stream", type=float, metavar="SECONDS",
                        help="连续拍这么多秒 (等价于 --frames 0 加超时)")
    parser.add_argument("--info", action="store_true", help="只查摄像头状态, 不拍照")
    parser.add_argument("--scan", action="store_true", help="扫一遍 I2C 总线")
    parser.add_argument("--view", action="store_true", help="拍完用系统看图程序打开第一帧")
    parser.add_argument("--probe", action="store_true",
                        help="让板子量一遍 DVP 各信号线 (结果打到串口, 用 serial_tail.py 看)")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="等帧的总超时秒数 (默认 20)")
    args = parser.parse_args()

    if args.frames < 0:
        print("--frames 不能是负数")
        return 1

    print(f"=== 连接 {args.host}:{args.port} ===")
    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print(f"连接失败: {exc}")
        print("检查: 板子是否上电联网 / IP 是否正确 / 端口是否 31336 / 网关是不是还占着板子")
        return 1

    saved = []
    try:
        client.send(CMD_GET_FIRMWARE_VERSION)
        packet = client.wait_report(RPT_FIRMWARE, timeout=3.0)
        if packet is None:
            print("固件版本查询超时: 链路不通")
            return 2
        print(f"固件版本: {packet[1]}.{packet[2]}.{packet[3]}")

        if args.scan:
            print("=== I2C / SCCB 扫描 ===")
            scan_i2c(client)

        info = query_info(client)
        if info is None:
            print("摄像头状态查询超时: 固件里没有摄像头功能? (0x7B)")
            return 3
        if info[0] == 2 and not args.scan:
            print("提示: 摄像头是出错状态, 可以加 --scan 看看 SCCB 上有没有 0x30")

        if args.info:
            return 0

        if args.probe:
            client.send(CMD_CAMERA_PROBE)
            print("已让板子开始量信号线, 结果在串口:")
            print(r"  C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe "
                  r"tools\serial_tail.py COM14 --seconds 8 --filter \"探针\"")
            time.sleep(0.3)
            return 0

        if args.size or args.quality >= 0 or args.pixels:
            size_index = SIZES.index(args.size) if args.size else 0xFF
            quality = args.quality if args.quality >= 0 else 0xFF
            pixels = {"jpeg": 1, "yuv422": 2}.get(args.pixels, 0)
            client.send(CMD_CAMERA_CONFIG, size_index, quality, pixels)
            time.sleep(0.5)      # 改完参数等传感器稳定一下
            query_info(client)

        frames_wanted = args.frames if args.stream is None else 0
        if args.stream is not None:
            timeout = args.stream
        else:
            timeout = args.timeout
        client.send(CMD_CAMERA_SNAPSHOT, frames_wanted,
                    (args.interval >> 8) & 0xFF, args.interval & 0xFF)
        print(f"=== 拍照: {'连续 ' + str(args.stream) + ' 秒' if frames_wanted == 0 else str(frames_wanted) + ' 帧'}, "
              f"间隔 {args.interval}ms ===")

        os.makedirs(args.dir, exist_ok=True)
        asm = FrameAssembler()
        deadline = time.time() + timeout
        last_status = None
        while time.time() < deadline:
            if frames_wanted and len(saved) >= frames_wanted:
                break
            packet = client.read_packet()
            if packet is None:
                continue
            report, data = packet[0], packet[1:]

            if report == RPT_CAMERA_STATUS:
                state, value = data[0], data[1]
                text = CAM_STATE.get(state, state)
                if (state, value) != last_status:
                    last_status = (state, value)
                    print(f"  板子: {text} (数值 {value})")
                    if state == 2:
                        print("  拍照出错, 先看串口日志 (取帧失败/内存不够)")
                        break
                continue

            jpeg = asm.feed(report, data)
            if jpeg is None:
                continue

            # 数据位活动统计: 哪一位恒为 0/1, 就是那根数据线没通 (排障最关键的一眼)
            ones = [0] * 8
            for byte in jpeg[:4096]:
                for bit in range(8):
                    ones[bit] += (byte >> bit) & 1
            sample = min(len(jpeg), 4096)
            activity = " ".join(f"b{b}={100 * ones[b] // max(sample, 1)}%" for b in range(8))
            print(f"            {activity}")
            for bit in range(8):
                if sample and ones[bit] == 0:
                    print(f"            !! 数据位 b{bit} 一个 1 都没有 -> 对应数据线可能没通")

            ext = "bin" if args.pixels == "yuv422" else "jpg"
            name = f"{args.out}_{len(saved):03d}.{ext}"
            path = os.path.join(args.dir, name)
            ok = save_frame(jpeg, path, asm.width, asm.height, len(saved) + 1)
            elapsed = max(time.time() - asm.started_at, 1e-3)
            print(f"            {len(jpeg) / elapsed / 1000:.1f} KB/s, 收包 {asm.gaps} 处分片错位")
            saved.append(path)
            asm.reset()

        if len(saved) < max(frames_wanted, 1) and frames_wanted:
            print(f"只收到 {len(saved)}/{frames_wanted} 帧 (超时 {timeout}s)")
        client.send(CMD_CAMERA_STOP)
        time.sleep(0.2)
    finally:
        client.close()

    if not saved:
        print("没收到任何完整帧")
        return 4

    print(f"=== 共 {len(saved)} 帧 ===")
    if args.view:
        first = os.path.abspath(saved[0])
        try:
            if sys.platform.startswith("win"):
                os.startfile(first)      # noqa: S606 (Windows 专用)
            elif sys.platform == "darwin":
                subprocess.Popen(["open", first])
            else:
                subprocess.Popen(["xdg-open", first])
        except OSError as exc:
            print(f"打开看图程序失败: {exc} (文件在 {first})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
