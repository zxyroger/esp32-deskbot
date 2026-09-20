#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 s3-extend 同款 Python 库 (telemetrix-aio-esp32) 直接驱动板子。

用途: 在打开 Scratch 之前, 先确认 "PC 端库 + 板子固件" 这条链路可用。

用法:
    python pc_telemetrix_demo.py 192.168.1.123
    python pc_telemetrix_demo.py 192.168.1.123 --pin 2 --analog-pin 32

依赖 (安装 s3-extend 时已经装好):
    pip install telemetrix-esp32
"""

import argparse
import asyncio
import sys
import time

try:
    from telemetrix_aio_esp32 import telemetrix_aio_esp32
except ImportError:
    print("缺少 telemetrix-aio-esp32: 请先执行 pip install telemetrix-esp32")
    sys.exit(1)


async def analog_callback(data):
    """data = [report_type, pin, value, timestamp]"""
    stamp = time.strftime("%H:%M:%S", time.localtime(data[3]))
    print(f"  模拟输入: 引脚={data[1]} 值={data[2]} ({stamp})")


async def run(ip_address, pin, analog_pin, seconds):
    board = telemetrix_aio_esp32.TelemetrixAioEsp32(
        transport_is_wifi=True,
        transport_address=ip_address,
        ip_port=31336,
        autostart=False,
        loop=asyncio.get_running_loop(),
    )
    await board.start_aio()
    print("已连接板子")

    try:
        print(f"数字输出: GPIO{pin} 闪烁 3 次")
        await board.set_pin_mode_digital_output(pin)
        for _ in range(3):
            await board.digital_write(pin, 1)
            await asyncio.sleep(0.4)
            await board.digital_write(pin, 0)
            await asyncio.sleep(0.4)

        print(f"模拟输入: 引脚 {analog_pin} 监听 {seconds} 秒 (Ctrl-C 结束)")
        await board.set_pin_mode_analog_input(analog_pin, 1, analog_callback)
        await asyncio.sleep(seconds)
        print("测试完成")
    finally:
        await board.shutdown()


def main():
    parser = argparse.ArgumentParser(description="telemetrix 直连板子测试")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--pin", type=int, default=2, help="数字输出引脚 (默认 2)")
    parser.add_argument("--analog-pin", type=int, default=32, help="模拟输入引脚 (默认 32)")
    parser.add_argument("--seconds", type=float, default=3.0, help="模拟输入监听时长")
    args = parser.parse_args()

    asyncio.run(run(args.host, args.pin, args.analog_pin, args.seconds))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
