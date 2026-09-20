"""网关 (esp32gw) 音频积木补丁的自检 (不需要板子)

用法:
    python tools\\test_gateway_audio.py

前提: 已经跑过 tools\\apply_local_patches.py (补丁 7), 也就是网关认识
audio_tone / audio_stop / audio_mic 三条指令和 0x0D 响度上报。

做法: 不启动 Banyan, 直接造一个 Esp32Gateway 实例, 把 self.esp 换成假对象,
然后走一遍 additional_banyan_messages (补丁 6/7 装好的那条路径), 检查真正发给
板子的 Telemetrix 字节 (0x72 / 0x73 / 0x74), 以及 0x0D 上报是否转发成 audio_input。
"""

import asyncio
import importlib
import sys


class FakeEsp:
    def __init__(self):
        self.sent = []
        self.report_dispatch = {}

    async def _send_command(self, command):
        self.sent.append(list(command))


def build_gateway(module):
    gw = module.Esp32Gateway.__new__(module.Esp32Gateway)
    gw.esp = FakeEsp()
    gw.published = []

    async def fake_publish(payload, topic):
        gw.published.append((payload, topic))

    gw.publish_payload = fake_publish
    return gw


async def drive(gw):
    await gw.additional_banyan_messages(
        None, {"command": "audio_tone", "frequency": 1000, "duration": 250, "volume": 70})
    await gw.additional_banyan_messages(None, {"command": "audio_stop"})
    await gw.additional_banyan_messages(None, {"command": "audio_mic", "value": 1})
    await gw.additional_banyan_messages(None, {"command": "audio_mic", "value": 0})
    # 超出范围的值应该被夹到固件能接受的范围
    await gw.additional_banyan_messages(
        None, {"command": "audio_tone", "frequency": 99999, "duration": 0, "volume": 999})
    # 朗读文字: 网关负责 UTF-8 拆包 (每包 ≤250 字节, 最后一段带标志位)
    await gw.additional_banyan_messages(None, {"command": "audio_tts", "text": "你好"})
    await gw.additional_banyan_messages(None, {"command": "audio_tts_stop"})
    # 板子的 0x0D 上报 -> Scratch 的 audio_input
    await gw._audio_level_report([42])


def main():
    try:
        module = importlib.import_module("s3_extend.gateways.esp32_gateway")
    except ImportError:
        print("找不到 s3_extend (先 pip install s3-extend)")
        return 1

    if not hasattr(module, "TMX_CMD_AUDIO_TONE"):
        print("网关还没有音频补丁, 先跑: python tools\\apply_local_patches.py")
        return 1

    gw = build_gateway(module)
    asyncio.run(drive(gw))

    expected = [
        [0x72, 0x03, 0xE8, 0x00, 0xFA, 0x46],   # 1000Hz / 250ms / 70%
        [0x73],                                 # 停止
        [0x74, 1],                              # 麦克风开
        [0x74, 0],                              # 麦克风关
        [0x72, 0x4E, 0x20, 0x00, 0x01, 0x64],   # 20000Hz / 1ms / 100%
        [0x75, 1] + list("你好".encode("utf-8")),  # 朗读 "你好" (一段, 带最后标志)
        [0x76],                                 # 停止朗读
    ]
    expected_report = [({"report": "audio_input", "value": 42}, "from_esp32_gateway")]

    failures = 0
    for i, (got, want) in enumerate(zip(gw.esp.sent, expected)):
        ok = got == want
        failures += 0 if ok else 1
        print("%s 命令 %d: %s%s" % ("PASS" if ok else "FAIL", i + 1, got,
                                    "" if ok else "  期望 %s" % want))
    if len(gw.esp.sent) != len(expected):
        failures += 1
        print("FAIL 命令条数: %d, 期望 %d" % (len(gw.esp.sent), len(expected)))

    ok = gw.published == expected_report
    failures += 0 if ok else 1
    print("%s 响度上报: %s%s" % ("PASS" if ok else "FAIL", gw.published,
                                 "" if ok else "  期望 %s" % expected_report))

    print()
    if failures:
        print("有 %d 项没通过" % failures)
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
