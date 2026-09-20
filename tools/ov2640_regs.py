#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
直接读 OV2640 的寄存器 (走板子上的 SCCB/I2C, 不经过 esp32-camera 驱动)

用途: 驱动报 "NO-SOI - JPEG start marker missing" 时, 先确认传感器到底在什么模式:
    IMAGE_MODE (0xDA, DSP bank) 应该是 0x12 = JPEG_EN(0x10) | HREF_VSYNC(0x02)
    如果不是 0x12, 说明传感器没切到 JPEG 模式 (寄存器没写进去 / 需要重新上电复位)

用法 (先 tools\\stop_s3extend.ps1):
    python ov2640_regs.py 192.168.0.104
    python ov2640_regs.py 192.168.0.104 --dump          # 多打一些寄存器

注意: 这个脚本会切传感器的 BANK (0xFF). 板子上的驱动自己缓存了 bank,
跑完之后最好复位一次板子 (python tools\\axp2101_power.py <ip> --reset) 让驱动
重新初始化, 免得后面驱动把寄存器写进错误的 bank。
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import (  # noqa: E402
    CMD_I2C_BEGIN,
    CMD_I2C_READ,
    CMD_I2C_WRITE,
    RPT_I2C_READ,
    RPT_I2C_TOO_FEW,
    RPT_I2C_TOO_MANY,
    TelemetrixClient,
)

SENSOR_ADDR = 0x30          # OV2640 SCCB 地址 (7 位)
BANK_SEL = 0xFF
BANK_SENSOR = 0x00
BANK_DSP = 0x01


def read_reg(client, reg, timeout=1.0):
    drain(client)
    # 报告里带着 address / reg, 必须核对上: 迟到的旧报文会造成"错位一拍"的假数据
    for _ in range(3):
        drain(client)
        client.send(CMD_I2C_READ, SENSOR_ADDR, reg, 1, 0)
        deadline = time.time() + timeout
        while time.time() < deadline:
            packet = client.read_packet()
            if packet is None:
                continue
            if packet[0] == RPT_I2C_READ:
                if packet[2] == SENSOR_ADDR and packet[3] == reg:
                    data = bytes(packet[4:4 + packet[1]])
                    return data[0] if data else None
                continue            # 不是这次请求的数据, 丢掉
            if packet[0] in (RPT_I2C_TOO_FEW, RPT_I2C_TOO_MANY):
                if len(packet) >= 3 and packet[2] == SENSOR_ADDR:
                    return None
    return None


def drain(client, quiet=0.05):
    """排空残留报文, 避免读到"错位一拍"的数据"""
    client.sock.settimeout(quiet)
    try:
        while True:
            if not client.read_packet():
                break
    except Exception:
        pass
    finally:
        client.sock.settimeout(0.5)


def write_reg(client, reg, value):
    client.send(CMD_I2C_WRITE, 2, SENSOR_ADDR, reg, value & 0xFF)
    time.sleep(0.02)


def select_bank(client, bank):
    write_reg(client, BANK_SEL, bank)
    time.sleep(0.02)


def main():
    parser = argparse.ArgumentParser(description="读 OV2640 寄存器 (SCCB)")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--dump", action="store_true", help="多打一些寄存器")
    args = parser.parse_args()

    print(f"=== 连接 {args.host}:{args.port} ===")
    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print(f"连接失败: {exc}")
        return 1

    try:
        client.send(CMD_I2C_BEGIN, 0, 0)
        time.sleep(0.2)

        # 总线自检: AXP2101 (0x34) 应该回 0x4A
        client.send(CMD_I2C_READ, 0x34, 0x03, 1, 0)
        deadline = time.time() + 1.0
        pmic = None
        while time.time() < deadline:
            packet = client.read_packet()
            if packet is None:
                continue
            if packet[0] == RPT_I2C_READ:
                pmic = bytes(packet[4:4 + packet[1]])[0]
                break
            if packet[0] in (RPT_I2C_TOO_FEW, RPT_I2C_TOO_MANY):
                break
        print(f"总线自检: AXP2101(0x34) reg0x03 = "
              + ("读失败" if pmic is None else f"0x{pmic:02X}" + (" (OK)" if pmic == 0x4A else " (异常)")))

        # bank 寄存器读写回环: 写 0x00 读回 0x00, 写 0x01 读回 0x01
        for bank in (BANK_SENSOR, BANK_DSP):
            select_bank(client, bank)
            back = read_reg(client, BANK_SEL)
            print(f"bank 自检: 写入 0x{bank:02X} -> 读回 "
                  + ("读失败" if back is None else f"0x{back:02X}"
                     + (" (OK)" if back == bank else " (写不进去!)")))

        select_bank(client, BANK_SENSOR)
        select_bank(client, BANK_SENSOR)
        pid = read_reg(client, 0x0A)
        ver = read_reg(client, 0x0B)
        midh = read_reg(client, 0x1C)
        midl = read_reg(client, 0x1D)
        if pid is None:
            print("SCCB 上 0x30 没有应答 (摄像头没供电 / 不在总线上)")
            return 2
        print(f"PID = 0x{pid:02X}  (OV2640 应是 0x26)"
              + ("  ✓" if pid == 0x26 else "  ✗ 不是 OV2640?")
              .replace("✓", "OK").replace("✗", "X"))
        print(f"VER = 0x{ver:02X}   MIDH = 0x{midh:02X}   MIDL = 0x{midl:02X}")

        select_bank(client, BANK_DSP)
        image_mode = read_reg(client, 0xDA)
        quality = read_reg(client, 0x44)
        reset = read_reg(client, 0xE0)
        ctrl0 = read_reg(client, 0xC0)
        print()
        print("DSP bank (0x01):")
        print(f"  IMAGE_MODE 0xDA = 0x{image_mode:02X}  "
              + ("JPEG 模式已开 (0x12)" if image_mode == 0x12 else
                 f"不是 JPEG 模式 (期望 0x12; JPEG_EN=0x10 HREF_VSYNC=0x02) 当前 0x{image_mode:02X}"))
        print(f"  QS(质量)   0x44 = 0x{quality:02X}  ({quality})")
        print(f"  RESET      0xE0 = 0x{reset:02X}")
        print(f"  CTRL0      0xC0 = 0x{ctrl0:02X}")

        if args.dump:
            print()
            print("更多寄存器 (DSP bank):")
            for reg in (0xD7, 0xE1, 0xE5, 0xD9, 0xDF, 0x33, 0x3C, 0xEB, 0xDD,
                        0xD3, 0x05, 0x04, 0x2C, 0x2D):
                value = read_reg(client, reg)
                print(f"  0x{reg:02X} = " + ("读失败" if value is None else f"0x{value:02X}"))

        select_bank(client, BANK_SENSOR)
        print()
        print("提醒: 跑完这个脚本后建议复位一次板子, 让驱动重新初始化传感器:")
        print(f"  python tools\\axp2101_power.py {args.host} --reset")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
