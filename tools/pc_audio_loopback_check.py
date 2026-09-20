#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
音频"自听"回环自检: 放一段音调, 同时读麦克风响度

用法:
    python pc_audio_loopback_check.py 192.168.0.103
    python pc_audio_loopback_check.py 192.168.0.103 --freq 1000 --volume 100

做法:
    1. 打开麦克风上报 (0x74)
    2. 先安静地读 1.5 秒, 记下环境本底响度
    3. 放 1.5 秒 1kHz 音调 (0x72, 音量 100%)
    4. 同时继续读响度, 看喇叭的声音有没有被麦克风拾到

为什么这么测: 如果"麦克风响度明显高过本底", 说明 DAC -> PA -> 喇叭 (输出) 和
麦克风 -> ADC (输入) 两条路都是通的, 不需要人耳/耳机也能判断固件和硬件没问题。

注意: 跑之前先停掉 s3-extend (板子同时只服务一个客户端):
    tools\\stop_s3extend.ps1
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import (  # noqa: E402  (路径要先插进 sys.path)
    CMD_AUDIO_MIC,
    CMD_AUDIO_TONE,
    RPT_AUDIO_LEVEL,
    TelemetrixClient,
)


def collect_levels(client, seconds):
    """读 seconds 秒响度上报, 返回列表"""
    levels = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        packet = client.read_packet()
        if packet is None:
            continue
        if packet[0] == RPT_AUDIO_LEVEL:
            levels.append(packet[1])
    return levels


def main():
    parser = argparse.ArgumentParser(description="音频自听回环自检 (音调 -> 麦克风)")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--freq", type=int, default=1000, help="音调频率 Hz (默认 1000)")
    parser.add_argument("--volume", type=int, default=100, help="音调音量 %% (默认 100)")
    parser.add_argument("--baseline", type=float, default=1.5, help="本底采集秒数 (默认 1.5)")
    parser.add_argument("--tone-ms", type=int, default=1500, help="音调时长 ms (默认 1500)")
    args = parser.parse_args()

    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print("连接失败: %s" % exc)
        print("检查: 板子是否上电联网 / 是否已经 stop_s3extend.ps1 (板子同时只服务一个客户端)")
        return 1

    try:
        freq = max(20, min(20000, args.freq))
        volume = max(0, min(100, args.volume))

        print("打开麦克风上报 ...")
        client.send(CMD_AUDIO_MIC, 1)
        time.sleep(0.2)

        print("采集本底 (%.1f 秒) ..." % args.baseline)
        baseline = collect_levels(client, args.baseline)

        print("播放 %dHz / %dms / 音量 %d%%, 同时读麦克风 ..." % (freq, args.tone_ms, volume))
        client.send(CMD_AUDIO_TONE,
                    (freq >> 8) & 0xff, freq & 0xff,
                    (args.tone_ms >> 8) & 0xff, args.tone_ms & 0xff,
                    volume)
        during = collect_levels(client, args.tone_ms / 1000.0 + 0.8)

        client.send(CMD_AUDIO_MIC, 0)

        base_max = max(baseline) if baseline else 0
        tone_max = max(during) if during else 0
        print()
        print("本底响度: %s (最大 %d)" % (baseline, base_max))
        print("放音响度: %s (最大 %d)" % (during, tone_max))

        if not baseline and not during:
            print("没有收到任何响度上报 —— 固件里音频没起来? 看串口有没有 'ES8311 ready'")
            return 1

        if tone_max - base_max >= 10:
            print("结论: 麦克风明显拾到了喇叭的声音 -> 音频输入/输出两条路都通")
            return 0

        print("结论: 没看到明显耦合 (可能音量小/喇叭没接/麦克风增益低/环境太吵)。")
        print("      放音和采集本身是通的 (有上报), 请人耳再确认一次声音;")
        print("      仍然没声音就看 docs/audio-es8311.md 的排障表。")
        return 0
    except ConnectionError as exc:
        print("连接中断: %s" % exc)
        return 4
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
