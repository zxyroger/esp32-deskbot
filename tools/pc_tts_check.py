#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
板载语音合成 (TTS) 自检: 发一段文字让板子念出来, 并把板子合成的 PCM 回传存成 wav

用法:
    python pc_tts_check.py 192.168.0.103 "你好，我是小乐"
    python pc_tts_check.py 192.168.0.103 --text-file say.txt --out say.wav
    python pc_tts_check.py 192.168.0.103 --hex-utf8 e4bda0e5a5bd   # 命令行编码不可靠时
    python pc_tts_check.py 192.168.0.103 "你好" --play               # 顺带用 PC 喇叭放一遍
    python pc_tts_check.py 192.168.0.103 "你好" --no-mirror          # 只让板子念, 不回传

做了什么:
    1. 打开 TTS 回传 (0x77), 板子会把合成的 16kHz/16bit/单声道 PCM 一起发回来
    2. 把文字按 UTF-8 拆成 ≤250 字节的小包 (0x75, 最后一段带标志位)
    3. 收 PCM 报告 (0x0E) 拼起来, 写成 wav (默认 tts_out.wav)
    4. --play: 用 PC 喇叭放一遍, 方便直接听板子念得对不对

注意: 跑之前先停掉 s3-extend (板子同时只服务一个客户端):
    tools\\stop_s3extend.ps1
"""

import argparse
import os
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pc_tcp_check import (  # noqa: E402
    CMD_TTS_MIRROR,
    CMD_TTS_STOP,
    CMD_TTS_TEXT,
    RPT_TTS_PCM,
    TelemetrixClient,
)

CHUNK_BYTES = 250          # 命令数据 = 1 字节标志 + ≤250 字节文字


def split_utf8(text, limit=CHUNK_BYTES):
    """按 UTF-8 字符边界把文字切成不超过 limit 字节的块"""
    chunks = []
    current = bytearray()
    for ch in text:
        raw = ch.encode('utf-8')
        if len(current) + len(raw) > limit:
            chunks.append(bytes(current))
            current = bytearray()
        current += raw
    if current:
        chunks.append(bytes(current))
    return chunks or [b'']


def send_text(client, text):
    chunks = split_utf8(text)
    for i, chunk in enumerate(chunks):
        last = 1 if i == len(chunks) - 1 else 0
        client.send(CMD_TTS_TEXT, last, *chunk)
    return len(chunks)


def collect_pcm(client, timeout):
    """收 0x0E 报告, 直到收到结束标记 (序号 0xFFFF)"""
    pcm = bytearray()
    seq_seen = set()
    missing = 0
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = client.read_packet()
        if not packet:
            continue
        if packet[0] != RPT_TTS_PCM:
            continue
        seq = (packet[1] << 8) | packet[2]
        if seq == 0xFFFF:
            return pcm, missing
        if seq in seq_seen:
            continue
        seq_seen.add(seq)
        if seq != len(seq_seen) - 1:
            missing += 1
        pcm += bytes(packet[3:])
    return pcm, missing


def main():
    parser = argparse.ArgumentParser(description="板载 TTS (esp-tts) 自检")
    parser.add_argument("host", help="板子 IP 地址")
    parser.add_argument("text", nargs="?", help="要朗读的文字 (UTF-8)")
    parser.add_argument("--text-file", help="从文件读文字 (避免命令行编码问题)")
    parser.add_argument("--hex-utf8", help="文字的 UTF-8 十六进制串, 如 e4bda0e5a5bd")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument("--out", default="tts_out.wav", help="回传 PCM 存成哪个 wav (默认 tts_out.wav)")
    parser.add_argument("--no-mirror", action="store_true", help="不回传 PCM, 只让板子念")
    parser.add_argument("--play", action="store_true", help="存完用 PC 喇叭放一遍")
    parser.add_argument("--timeout", type=float, default=20.0, help="等回传的超时秒数")
    args = parser.parse_args()

    if args.hex_utf8:
        text = bytes.fromhex(args.hex_utf8).decode('utf-8')
    elif args.text_file:
        with open(args.text_file, 'r', encoding='utf-8') as fp:
            text = fp.read().strip()
    elif args.text:
        text = args.text
    else:
        parser.error("要给一段文字: 位置参数 / --text-file / --hex-utf8")

    if not text:
        parser.error("文字是空的")

    try:
        client = TelemetrixClient(args.host, args.port)
    except OSError as exc:
        print("连接失败: %s" % exc)
        print("检查: 板子是否在线 / 是否已经 stop_s3extend.ps1 (板子同时只服务一个客户端)")
        return 1

    try:
        print("朗读文字: %s" % text)
        if not args.no_mirror:
            client.send(CMD_TTS_MIRROR, 1)
            time.sleep(0.1)

        chunks = send_text(client, text)
        print("已发出 %d 个文字包 (每个 ≤%d 字节 UTF-8)" % (chunks, CHUNK_BYTES))

        if args.no_mirror:
            time.sleep(3.0)
            client.send(CMD_TTS_STOP)
            print("板子应该已经念完了 (没回传, 具体声音请听板子喇叭)")
            return 0

        pcm, missing = collect_pcm(client, args.timeout)
        client.send(CMD_TTS_MIRROR, 0)

        if not pcm:
            print("没有收到合成 PCM (板子日志里有没有 'esp-tts ready' / '开始朗读'?)")
            return 1

        with wave.open(args.out, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(bytes(pcm))

        seconds = len(pcm) / 2.0 / 16000.0
        print("收到合成 PCM: %d 字节 = %.2f 秒 (丢包 %d), 已存 %s"
              % (len(pcm), seconds, missing, args.out))

        if args.play:
            try:
                import winsound
                print("用 PC 喇叭播放 ...")
                winsound.PlaySound(args.out, winsound.SND_FILENAME)
            except Exception as exc:
                print("播放失败: %s" % exc)
        return 0
    except ConnectionError as exc:
        print("连接中断: %s" % exc)
        return 4
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
