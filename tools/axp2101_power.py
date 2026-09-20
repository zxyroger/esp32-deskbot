#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本板摄像头供电控制: AXP2101 的 BLDO1 = AVDD(2.8V) / BLDO2 = DVDD(1.2V)

这块板子的 OV2640 不是直接吃 3.3V, 而是由 AXP2101 供电:
    AVDD  -> BLDO1 (模拟供电, 典型 2.8V)
    DVDD  -> BLDO2 (数字核心, 典型 1.2V)
    VDDCAM_3V3 是 I/O 那一路 (已经开着的那个)
AVDD/DVDD 没打开时, 现象非常有迷惑性: SCCB 能读能写 (所以固件日志里
"Detected OV2640 camera at address=0x30" 是正常的), 但传感器不出像素时钟,
于是驱动刷 EV-VSYNC-OVF / NO-SOI, 一帧也拍不到。

用法 (先 tools\\stop_s3extend.ps1 把板子让出来):
    python axp2101_power.py 192.168.0.104            # 看当前状态
    python axp2101_power.py 192.168.0.104 --on       # AVDD 2.8V + DVDD 1.2V 打开
    python axp2101_power.py 192.168.0.104 --on --reset   # 打开后复位板子重新初始化摄像头
    python axp2101_power.py 192.168.0.104 --off      # 关掉 (排障用)
    python axp2101_power.py 192.168.0.104 --avdd 3000 --dvdd 1200

寄存器 (XPowersLib / AXP2101 数据手册):
    0x03 芯片型号 (AXP2101 = 0x4A)
    0x90 LDO 使能: bit0~3 = ALDO1~4, bit4 = BLDO1, bit5 = BLDO2
    0x96 BLDO1 电压 = 0x500mV + N*100mV (N 取低 5 位)
    0x97 BLDO2 电压 = 同上
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
    CMD_RESET,
    RPT_I2C_READ,
    RPT_I2C_TOO_FEW,
    RPT_I2C_TOO_MANY,
    TelemetrixClient,
)

PMIC_ADDR = 0x34
REG_IC_TYPE = 0x03
REG_LDO_ONOFF0 = 0x90
REG_BLDO1_VOL = 0x96
REG_BLDO2_VOL = 0x97
BLDO1_BIT = 4
BLDO2_BIT = 5
VOL_MIN_MV = 500
VOL_STEP_MV = 100

LDO_NAMES = ["ALDO1", "ALDO2", "ALDO3", "ALDO4", "BLDO1(AVDD)", "BLDO2(DVDD)"]
VOL_REGS = {0x92: "ALDO1", 0x93: "ALDO2", 0x94: "ALDO3", 0x95: "ALDO4",
            REG_BLDO1_VOL: "BLDO1(AVDD)", REG_BLDO2_VOL: "BLDO2(DVDD)"}


def read_reg(client, reg, count=1, timeout=1.0):
    """读 AXP2101 的寄存器; 失败返回 None"""
    drain(client)
    # stop_between=0: 用"重复起始位"读, 和 OV2640 的 SCCB 读法一致
    client.send(CMD_I2C_READ, PMIC_ADDR, reg, count, 0)
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = client.read_packet()
        if packet is None:
            continue
        if packet[0] == RPT_I2C_READ:
            # 核对 address / reg, 避免读到"错位一拍"的旧报文
            if packet[2] == PMIC_ADDR and packet[3] == reg:
                return bytes(packet[4:4 + packet[1]])
            continue
        if packet[0] in (RPT_I2C_TOO_FEW, RPT_I2C_TOO_MANY):
            if len(packet) >= 3 and packet[2] == PMIC_ADDR:
                return None
    return None


def drain(client, quiet=0.05):
    """把 socket 里残留的报文读干净 (上一次超时剩下的), 免得读到错位的数据"""
    client.sock.settimeout(quiet)
    try:
        while True:
            if not client.read_packet():
                break
    except Exception:
        pass
    finally:
        client.sock.settimeout(0.5)


def write_reg(client, reg, value, timeout=1.0):
    """写 AXP2101 的寄存器; 用读回来确认 (I2C 写命令本身没有应答报告)"""
    client.send(CMD_I2C_WRITE, 2, PMIC_ADDR, reg, value & 0xFF)
    time.sleep(0.05)
    back = read_reg(client, reg, 1, timeout)
    return back is not None and back[0] == (value & 0xFF)


def mv_to_code(mv):
    return (mv - VOL_MIN_MV) // VOL_STEP_MV


def code_to_mv(code):
    return VOL_MIN_MV + (code & 0x1F) * VOL_STEP_MV


def show_status(client):
    ic = read_reg(client, REG_IC_TYPE)
    if ic is None:
        print("AXP2101 (0x34) 没有应答: 检查 I2C 总线 / PMIC 是否上电")
        return False
    print(f"芯片型号寄存器 0x03 = 0x{ic[0]:02X}"
          + ("  (AXP2101 确认)" if ic[0] == 0x4A else "  (不是 0x4A, 可能不是 AXP2101)"))

    onoff = read_reg(client, REG_LDO_ONOFF0)
    if onoff is None:
        print("0x90 读取失败")
        return False
    value = onoff[0]
    print(f"LDO 使能寄存器 0x90 = 0x{value:02X}")
    for bit, name in enumerate(LDO_NAMES):
        state = "开" if value & (1 << bit) else "关"
        print(f"    {name:<12} {state}")

    print("LDO 电压:")
    for reg, name in VOL_REGS.items():
        raw = read_reg(client, reg)
        if raw is None:
            print(f"    0x{reg:02X} {name:<12} 读取失败")
            continue
        print(f"    0x{reg:02X} {name:<12} {code_to_mv(raw[0]):>4} mV  (寄存器 0x{raw[0]:02X})")
    return True


def set_bldo(client, name, vol_reg, bit, millivolt):
    if millivolt % VOL_STEP_MV or not (VOL_MIN_MV <= millivolt <= 3500):
        print(f"{name}: 电压 {millivolt}mV 非法 (500~3500mV, 100mV 一档)")
        return False

    raw = read_reg(client, vol_reg)
    if raw is None:
        print(f"{name}: 读电压寄存器 0x{vol_reg:02X} 失败")
        return False
    target = (raw[0] & 0xE0) | (mv_to_code(millivolt) & 0x1F)
    if not write_reg(client, vol_reg, target):
        print(f"{name}: 写电压 0x{vol_reg:02X} 失败")
        return False
    print(f"{name}: 电压设为 {millivolt}mV (0x{vol_reg:02X} = 0x{target:02X})")
    return True


def enable_ldo(client, name, bit, enable):
    raw = read_reg(client, REG_LDO_ONOFF0)
    if raw is None:
        print(f"{name}: 读 0x90 失败")
        return False
    value = raw[0]
    new_value = (value | (1 << bit)) if enable else (value & ~(1 << bit))
    if new_value == value:
        print(f"{name}: 已经是{'开' if enable else '关'}的")
        return True
    if not write_reg(client, REG_LDO_ONOFF0, new_value):
        print(f"{name}: 写 0x90 失败")
        return False
    print(f"{name}: {'打开' if enable else '关闭'} (0x90: 0x{value:02X} -> 0x{new_value:02X})")
    return True


def main():
    parser = argparse.ArgumentParser(description="AXP2101 摄像头供电 (BLDO1=AVDD / BLDO2=DVDD)")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--on", action="store_true", help="打开 AVDD/DVDD")
    parser.add_argument("--off", action="store_true", help="关闭 AVDD/DVDD")
    parser.add_argument("--avdd", type=int, default=2800, help="AVDD 电压 mV (默认 2800)")
    parser.add_argument("--dvdd", type=int, default=1200, help="DVDD 电压 mV (默认 1200)")
    parser.add_argument("--reset", action="store_true",
                        help="做完之后复位板子 (让固件重新初始化摄像头)")
    parser.add_argument("--delay", type=float, default=0.3,
                        help="上电后等多久再继续 (秒, 默认 0.3)")
    args = parser.parse_args()

    print(f"=== 连接 {args.host}:{args.port} ===")
    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print(f"连接失败: {exc}")
        print("检查: 板子是否在线 / 网关是不是还占着板子 (tools\\stop_s3extend.ps1)")
        return 1

    try:
        client.send(CMD_I2C_BEGIN, 0, 0)      # 0,0 = 用固件 Kconfig 里的引脚
        time.sleep(0.2)
        print("--- 当前状态 ---")
        if not show_status(client):
            return 2

        if args.on or args.off:
            print("--- 修改 ---")
            ok = True
            if args.on:
                ok &= set_bldo(client, "BLDO1(AVDD)", REG_BLDO1_VOL, BLDO1_BIT, args.avdd)
                ok &= set_bldo(client, "BLDO2(DVDD)", REG_BLDO2_VOL, BLDO2_BIT, args.dvdd)
                ok &= enable_ldo(client, "BLDO1(AVDD)", BLDO1_BIT, True)
                ok &= enable_ldo(client, "BLDO2(DVDD)", BLDO2_BIT, True)
            else:
                ok &= enable_ldo(client, "BLDO1(AVDD)", BLDO1_BIT, False)
                ok &= enable_ldo(client, "BLDO2(DVDD)", BLDO2_BIT, False)
            time.sleep(args.delay)
            print("--- 修改后 ---")
            ok &= show_status(client)
            if not ok:
                print("有步骤失败, 看上面的输出")
                return 3

        if args.reset:
            print("--- 复位板子 (摄像头会用新供电重新初始化) ---")
            try:
                client.send(CMD_RESET)
            except OSError:
                pass          # 复位瞬间连接断掉是正常的
            time.sleep(0.5)
            print("已发送复位命令, 等 3 秒后可以看串口日志 / 拍照")
            time.sleep(2.5)
    finally:
        client.close()

    print("下一步:")
    print("  python tools\\serial_tail.py COM14 --seconds 12 --filter \"tmx_camera|camera:|OV2640|NO-SOI|EV-\"")
    print("  python tools\\pc_camera_check.py <板子IP> --view")
    return 0


if __name__ == "__main__":
    sys.exit(main())
