#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Telemetrix 协议自检脚本（不依赖 Scratch、不依赖第三方库）

用法:
    python pc_tcp_check.py 192.168.1.123
    python pc_tcp_check.py 192.168.1.123 --pin 2 --analog-pin 34
    python pc_tcp_check.py 192.168.4.1 --sonar 4 5
    python pc_tcp_check.py 192.168.1.123 --servo 10 --angles 60,120,90
    python pc_tcp_check.py 192.168.1.123 --servo 10 --min-pulse 1000 --max-pulse 2000
    python pc_tcp_check.py 192.168.1.123 --tone 1000 --tone-ms 800
    python pc_tcp_check.py 192.168.1.123 --mic 5

它会依次做:
    1. 连接 TCP 31336
    2. 查询固件版本并打印
    3. 回环测试
    4. 可选: 音频测试 (--tone 频率 放音调, --mic 秒数 读麦克风响度)
    5. 可选: 舵机测试 (--servo 引脚), 依次转到 --angles 里的角度
    6. 数字输出: 指定引脚闪烁 3 次 (默认 GPIO2)
    7. 模拟输入: 读取 2 秒并打印 (默认引脚 34, 固件会映射到 GPIO3;
       引脚 32/33 对应的 GPIO1/2 已经被音频 codec 的 I2C 占用, 所以不用它们)
    8. 可选: 超声波测距 (--sonar 触发脚 回波脚)

这些都能通过, 说明"固件 + 网络 + 协议"完全正常。

舵机测试的用处: 排除 Scratch/网关之后单独验证"板子给出的舵机信号对不对"。
  * 换个引脚再试 (比如 10 不行就试 11/12/13) -> 区分是引脚问题还是舵机问题
  * 换个脉宽范围再试 (--min-pulse 1000 --max-pulse 2000) -> 有些舵机在 544us
    这种极限脉宽下会顶到机械限位, 表现为"齿轮一直响但不动"
  * 注意: 板子同时只服务一个客户端, 跑这个脚本前先停掉网关 (stop_s3extend.ps1)
"""

import argparse
import socket
import sys
import time

# 命令
CMD_LOOPBACK = 0
CMD_SET_PIN_MODE = 1
CMD_DIGITAL_WRITE = 2
CMD_GET_FIRMWARE_VERSION = 5
CMD_SERVO_ATTACH = 6
CMD_SERVO_WRITE = 7
CMD_I2C_BEGIN = 9
CMD_I2C_READ = 10
CMD_I2C_WRITE = 11
CMD_SONAR_NEW = 12
CMD_RESET = 20
CMD_ENABLE_ALL_REPORTS = 16
CMD_AUDIO_TONE = 0x72
CMD_AUDIO_STOP = 0x73
CMD_AUDIO_MIC = 0x74
CMD_TTS_TEXT = 0x75
CMD_TTS_STOP = 0x76
CMD_TTS_MIRROR = 0x77
CMD_CAMERA_CONFIG = 0x78
CMD_CAMERA_SNAPSHOT = 0x79
CMD_CAMERA_STOP = 0x7A
CMD_CAMERA_INFO = 0x7B
CMD_CAMERA_PROBE = 0x7C

# 报告
RPT_LOOPBACK = 0
RPT_DIGITAL = 2
RPT_ANALOG = 3
RPT_FIRMWARE = 5
RPT_SERVO_UNAVAILABLE = 6
RPT_I2C_TOO_FEW = 7
RPT_I2C_TOO_MANY = 8
RPT_I2C_READ = 9
RPT_SONAR = 10
RPT_AUDIO_LEVEL = 13
RPT_TTS_PCM = 14
RPT_CAMERA_FRAME = 15
RPT_CAMERA_FRAME_INFO = 16
RPT_CAMERA_STATUS = 17
RPT_CAMERA_INFO = 18
RPT_CAMERA_PROBE = 19
RPT_DEBUG = 99

# 引脚模式
MODE_OUTPUT = 1
MODE_ANALOG = 3


class TelemetrixClient:
    def __init__(self, host, port=31336, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.5)
        self.pending = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, *payload):
        packet = bytes([len(payload)]) + bytes(payload)
        self.sock.sendall(packet)

    def read_packet(self):
        """读一个完整数据包, 超时返回 None"""
        while True:
            if not self.pending:
                try:
                    chunk = self.sock.recv(256)
                except socket.timeout:
                    return None
                if not chunk:
                    raise ConnectionError("连接已被板子关闭")
                self.pending += chunk

            length = self.pending[0]
            if len(self.pending) < length + 1:
                try:
                    chunk = self.sock.recv(256)
                except socket.timeout:
                    continue
                if not chunk:
                    raise ConnectionError("连接已被板子关闭")
                self.pending += chunk
                continue

            packet = self.pending[1:length + 1]
            self.pending = self.pending[length + 1:]
            return list(packet)

    def wait_report(self, report_type, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            packet = self.read_packet()
            if packet is None:
                continue
            if packet[0] == report_type:
                return packet
        return None


def describe(packet):
    if not packet:
        return "空包"
    rpt = packet[0]
    data = packet[1:]
    if rpt == RPT_DIGITAL:
        return f"数字输入 引脚={data[0]} 值={data[1]}"
    if rpt == RPT_ANALOG:
        return f"模拟输入 引脚={data[0]} 值={(data[1] << 8) | data[2]}"
    if rpt == RPT_SONAR:
        return f"超声波 触发脚={data[0]} 距离={(data[1] << 8) | data[2]} cm"
    if rpt == RPT_AUDIO_LEVEL:
        return f"麦克风响度 {data[0]} / 100"
    if rpt == RPT_DEBUG:
        return f"调试信息 id={data[0]} 值={(data[1] << 8) | data[2]}"
    if rpt == RPT_I2C_READ:
        return f"I2C 读 设备=0x{data[1]:02X} 寄存器=0x{data[2]:02X} 数据={list(data[3:])}"
    if rpt == RPT_I2C_TOO_FEW or rpt == RPT_I2C_TOO_MANY:
        return f"I2C 读取失败 设备=0x{data[1]:02X}"
    if rpt == RPT_SERVO_UNAVAILABLE:
        return f"舵机不可用 引脚={data[0]}"
    return f"报告 {rpt} 数据={data}"


def main():
    parser = argparse.ArgumentParser(description="Telemetrix / ESP32-S3 协议自检")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--pin", type=int, default=2, help="用于闪烁的数字引脚 (默认 2)")
    parser.add_argument("--analog-pin", type=int, default=34,
                        help="用于读取的模拟引脚 (默认 34 -> GPIO3; 32/33 已被 I2C 占用)")
    parser.add_argument("--sonar", nargs=2, type=int, metavar=("TRIG", "ECHO"),
                        help="可选: 超声波触发脚与回波脚")
    parser.add_argument("--tone", type=int, metavar="HZ",
                        help="可选: 播放一段音调 (频率 Hz, 时长/音量见 --tone-ms / --tone-vol)")
    parser.add_argument("--tone-ms", type=int, default=800,
                        help="音调时长 ms (默认 800)")
    parser.add_argument("--tone-vol", type=int, default=80,
                        help="音调音量 %% (默认 80)")
    parser.add_argument("--mic", type=float, default=0.0, metavar="SECONDS",
                        help="可选: 采集这么多秒的麦克风响度并打印")
    parser.add_argument("--servo", type=int, metavar="PIN",
                        help="可选: 给这个引脚上的舵机做动作测试")
    parser.add_argument("--min-pulse", type=int, default=544,
                        help="舵机最小脉宽 us (默认 544, 与 Scratch 扩展一致)")
    parser.add_argument("--max-pulse", type=int, default=2400,
                        help="舵机最大脉宽 us (默认 2400, 与 Scratch 扩展一致)")
    parser.add_argument("--angles", default="0,90,180,90",
                        help="舵机依次转到的角度, 逗号分隔 (默认 0,90,180,90)")
    parser.add_argument("--hold", type=float, default=1.0,
                        help="每个角度停留的秒数 (默认 1.0)")
    parser.add_argument("--skip-digital", action="store_true", help="跳过数字输出测试")
    args = parser.parse_args()

    print(f"=== 连接 {args.host}:{args.port} ===")
    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print(f"连接失败: {exc}")
        print("检查: 板子是否上电联网 / IP 是否正确 / 与 PC 是否同网段 / 端口是否 31336")
        return 1

    try:
        client.send(CMD_ENABLE_ALL_REPORTS)

        # 1. 固件版本
        client.send(CMD_GET_FIRMWARE_VERSION)
        packet = client.wait_report(RPT_FIRMWARE)
        if packet is None:
            print("固件版本查询超时 (没有收到报告)")
            return 2
        print(f"固件版本: {packet[1]}.{packet[2]}.{packet[3]}")

        # 2. 回环
        client.send(CMD_LOOPBACK, 0x41)
        packet = client.wait_report(RPT_LOOPBACK)
        if packet is None or packet[1] != 0x41:
            print(f"回环测试失败: {packet}")
            return 3
        print("回环测试: 通过")

        # 3. 音频 (可选): 板载 ES8311, 细节见 docs/audio-es8311.md
        if args.tone is not None:
            freq = max(20, min(20000, args.tone))
            ms = max(1, min(60000, args.tone_ms))
            vol = max(0, min(100, args.tone_vol))
            print(f"音频测试: 播放 {freq}Hz / {ms}ms / 音量 {vol}%")
            client.send(CMD_AUDIO_TONE,
                        (freq >> 8) & 0xff, freq & 0xff,
                        (ms >> 8) & 0xff, ms & 0xff,
                        vol)
            time.sleep(ms / 1000.0 + 0.3)
            print("音频测试: 结束 (没听到声音 -> 查 docs/audio-es8311.md 的排障表)")

        if args.mic > 0:
            seconds = min(60.0, args.mic)
            print(f"麦克风测试: 采集 {seconds:.1f} 秒 (对着麦克风说话/拍手)")
            client.send(CMD_AUDIO_MIC, 1)
            deadline = time.time() + seconds
            levels = []
            while time.time() < deadline:
                packet = client.read_packet()
                if packet is None:
                    continue
                if packet[0] == RPT_AUDIO_LEVEL:
                    levels.append(packet[1])
                    print("   " + describe(packet))
            client.send(CMD_AUDIO_MIC, 0)
            if levels:
                print(f"麦克风测试: 收到 {len(levels)} 条上报, "
                      f"最大 {max(levels)}, 平均 {sum(levels) // len(levels)}")
            else:
                print("   没有收到响度上报 (固件是否开了 TMX_AUDIO_ENABLE? "
                      "启动日志里有没有 ES8311 ready? 见 docs/audio-es8311.md)")

        # 3. 舵机 (可选)
        if args.servo is not None:
            pin = args.servo
            min_pulse = max(1, min(65535, args.min_pulse))
            max_pulse = max(min_pulse + 1, min(65535, args.max_pulse))
            angles = [int(x) for x in str(args.angles).replace(" ", "").split(",") if x != ""]
            print(f"舵机测试: GPIO{pin}  脉宽 {min_pulse}-{max_pulse} us  "
                  f"角度 {angles}")
            client.send(CMD_SERVO_ATTACH, pin,
                        (min_pulse >> 8) & 0xff, min_pulse & 0xff,
                        (max_pulse >> 8) & 0xff, max_pulse & 0xff)
            time.sleep(0.3)
            for angle in angles:
                angle = max(0, min(180, angle))
                print(f"  转到 {angle} 度")
                client.send(CMD_SERVO_WRITE, pin, angle)
                time.sleep(args.hold)
            print("舵机测试: 结束")
            print("  舵机不动、只有齿轮响 -> 换个引脚再试; 还是这样就是供电或舵机本身的问题")
            print("  (舵机建议用独立 5V 供电、和板子共地, 不要接 3.3V)")

        # 4. 数字输出
        if not args.skip_digital:
            print(f"数字输出测试: GPIO{args.pin} 闪烁 3 次")
            client.send(CMD_SET_PIN_MODE, args.pin, MODE_OUTPUT)
            for _ in range(3):
                client.send(CMD_DIGITAL_WRITE, args.pin, 1)
                time.sleep(0.3)
                client.send(CMD_DIGITAL_WRITE, args.pin, 0)
                time.sleep(0.3)
            print("数字输出测试: 通过 (接了 LED 应该看到闪烁)")

        # 4. 模拟输入
        print(f"模拟输入测试: 引脚 {args.analog_pin} 读取 2 秒")
        client.send(CMD_SET_PIN_MODE, args.analog_pin, MODE_ANALOG, 0, 0, 1)
        deadline = time.time() + 2.0
        count = 0
        while time.time() < deadline:
            packet = client.read_packet()
            if packet is None:
                continue
            print("   " + describe(packet))
            count += 1
        if count == 0:
            print("   没有收到模拟输入报告 (该引脚是否有 ADC 能力? 见 docs/pins-esp32s3.md)")
        else:
            print(f"模拟输入测试: 收到 {count} 条报告")

        # 5. 超声波
        if args.sonar:
            trig, echo = args.sonar
            print(f"超声波测试: 触发脚 {trig}, 回波脚 {echo}")
            client.send(CMD_SONAR_NEW, trig, echo)
            deadline = time.time() + 3.0
            got = 0
            while time.time() < deadline:
                packet = client.read_packet()
                if packet is None:
                    continue
                print("   " + describe(packet))
                if packet[0] == RPT_SONAR:
                    got += 1
            if got == 0:
                print("   没有收到超声波数据 (检查传感器接线与供电)")
            else:
                print(f"超声波测试: 收到 {got} 条数据")

        print("\n=== 全部测试结束 ===")
        return 0
    except ConnectionError as exc:
        print(f"连接中断: {exc}")
        return 4
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
