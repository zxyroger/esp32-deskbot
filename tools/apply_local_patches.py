#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
给已安装的第三方包打上六个本地补丁 (幂等, pip 升级后可以再跑一次)。

补丁 1: telemetrix_aio_esp32 的"合法引脚"表
    它那份表是照经典 ESP32 抄的, 不含 10/11 等引脚。结果用积木选 10/11 时,
    PC 端在本地就把命令拦下来 (报 Invalid GPIO pin number), 板子毫无反应。
    这里把指定引脚补进 GPIO 输入 / GPIO 输出 / 舵机三张表 (默认 10 11)。

补丁 2: python_banyan 用 psutil 扫进程时的 NoSuchProcess
    banyan_base_aio.py 在自动发现 backplane 时会遍历所有进程:
        for pid in psutil.pids():
            p = psutil.Process(pid)          <-- 进程刚好退出时抛 NoSuchProcess
            try: p_command = p.cmdline()
            except (AccessDenied, ZombieProcess): continue   <-- 漏了 NoSuchProcess
    只要扫描那一瞬间有任何进程退出, 网关就在启动时崩掉 (日志里是
    "psutil.NoSuchProcess: process PID not found"), 表现出来就是反复重启、
    连不上板子。这里把它一并忽略掉。

补丁 3: s3_extend 的 esp32 网关"第一条报文必须是 ip_address"
    esp32_gateway.py 的 main() 把启动后收到的第一条 Banyan 报文当成板子 IP。
    Scratch 里正在跑的积木指令(digital_write 之类)常常先到, 于是
    transport_address 还是 None, telemetrix 抛 RuntimeError
    ("A TCP/IP address must be specified when using WI-FI."), 网关当场退出 ——
    表现出来就是"点了 IP 积木要等几十秒才连上"(守护进程一直在把它拉起来)。
    这里让它跳过所有非 ip_address 的报文, 一直等到 IP 那条。

补丁 4: 网关的接收循环早启动了一个
    esp32_gateway.py 的 main() 里先 begin(start_loop=True) 起了接收循环, 然后又
    begin_receive_loop() 起了第二个, 而这两个循环都在等 ip_address 的那条
    recv_multipart()——同一只 SUB socket 上谁先 await 谁把消息拿走, 于是
    ip_address 有概率被接收循环吞掉, 网关就一直连不上板子 (要点好几次积木)。
    更要命的是接收循环在 self.esp 还是 None 的时候就跑起来了: 这段时间里到的
    任何积木指令都会抛
        AttributeError: 'NoneType' object has no attribute 'servo_write'
    把接收循环打死 (进程还在, 但从此不理任何消息, 只能靠守护进程重启)。
    这里改成 begin(start_loop=False): 等板子连上后再启动唯一的接收循环。

补丁 5: python_banyan 拿"本机当前 IP"当 backplane 地址, 本机 IP 一变就全线断
    backplane 启动时先连一下 8.8.8.8, 拿到本机当前的局域网 IP, 然后只在这个
    地址上监听 43124/43125; wsgw / esp32gw / banyan_publish.py 也各自解析一次
    同一个地址去连它。PC 换网络、DHCP 续租、路由器重启导致本机 IP 变化之后:
      * 老 backplane 还绑在旧地址上 (那个地址甚至可能已经被板子等设备占用);
      * 网关连的是它自己启动时解析到的地址, 而 ZMQ 的 connect 不会报错,
        消息只是石沉大海, 于是"点积木没反应";
      * 日志看起来全都正常, 极难排查, 只能手工重启整套服务。
    本补丁把 backplane 改成监听所有网卡 (0.0.0.0), 其余组件一律用回环地址
    127.0.0.1 连它 (回环地址永远不会变), 本机 IP 再变也不影响 Banyan 内部通信。

补丁 6: s3_extend 的 esp32 网关增加"屏幕积木"命令
    上游网关只认它自己的那套命令 (digital_write 之类)。本工程加了两个积木:
    「屏幕背光 开/关」和「屏幕颜色设为 #rrggbb」, 需要网关把它们转成自定义的
    Telemetrix 命令 (0x70 / 0x71, 见固件 main/tmx_protocol.h) 发给板子。
    这里把它们挂到 additional_banyan_messages() (上游留给硬件网关的扩展点) 上。

补丁 7: s3_extend 的 esp32 网关增加"音频积木"命令
    板载 ES8311 codec (见 docs/audio-es8311.md)。三块积木要转成自定义命令:
    「播放音调」-> 0x72 (频率/时长/音量), 「停止播放音频」-> 0x73,
    「麦克风检测 开/关」-> 0x74; 板子回的 0x0D (麦克风响度 0~100) 还要接进
    telemetrix 客户端的 report_dispatch, 再以 audio_input 发给 Scratch。
    补丁 7 依赖补丁 6 已经打进网关 (它接在补丁 6 的代码后面)。

补丁 8: s3_extend 的 esp32 网关增加"朗读文字"命令 (板载语音合成)
    板子用 esp-sr 的 esp-tts 把中文念出来 (见 docs/tts-esp-tts.md)。
    「朗读文字 …」积木把文字交给网关, 网关按 UTF-8 字符边界拆成若干 ≤250 字节的
    0x75 包 (最后一段带标志位) 发给板子, 板子拼起来再合成;
    「停止朗读」-> 0x76。补丁 8 依赖补丁 7。

用法:
    python tools\\apply_local_patches.py            # 放行 10, 11
    python tools\\apply_local_patches.py 10 11 4   # 指定额外放行的引脚

打完补丁要重启网关才生效:
    Get-Process esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force
"""

import argparse
import importlib.util
import re
import shutil
import sys
import time
from pathlib import Path

PIN_LISTS = ("valid_gpio_input_pins", "valid_gpio_output_pins", "valid_servo_pins")


def find_module_path(module):
    spec = importlib.util.find_spec(module)
    if not spec or not spec.origin:
        return None
    return Path(spec.origin)


def backup(path):
    dst = path.with_suffix(path.suffix + ".bak-%s" % time.strftime("%Y%m%d%H%M%S"))
    shutil.copy2(path, dst)
    return dst


def patch_telemetrix_pins(path, extra_pins):
    """把 extra_pins 补进三张合法引脚表; 返回被改动的表名 (空 = 已是补丁状态)"""
    text = path.read_text(encoding="utf-8")
    changed = []

    def replace(match):
        head, name, body, tail = match.groups()
        pins = [int(x) for x in re.findall(r"\d+", body)]
        if all(p in pins for p in extra_pins):
            return match.group(0)
        pins = sorted(set(pins) | set(extra_pins))
        changed.append(name)
        return head + ", ".join(str(p) for p in pins) + tail

    pattern = re.compile(r"(self\.(" + "|".join(PIN_LISTS) + r")\s*=\s*\[)([^\]]*)(\])")
    patched = pattern.sub(replace, text)
    if not changed:
        return []
    bak = backup(path)
    path.write_text(patched, encoding="utf-8")
    return changed, bak


def patch_banyan_psutil(path):
    """让 banyan 的进程扫描容忍 NoSuchProcess; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    old = ("            for pid in psutil.pids():\n"
           "                p = psutil.Process(pid)\n"
           "                try:\n"
           "                    p_command = p.cmdline()\n"
           "                # ignore these psutil exceptions\n"
           "                except (psutil.AccessDenied, psutil.ZombieProcess):\n"
           "                    continue\n")
    new = ("            for pid in psutil.pids():\n"
           "                try:\n"
           "                    p = psutil.Process(pid)\n"
           "                    p_command = p.cmdline()\n"
           "                # ignore these psutil exceptions\n"
           "                # (NoSuchProcess: 这个 pid 在扫描的瞬间退出了, 正常现象)\n"
           "                except (psutil.AccessDenied, psutil.ZombieProcess,\n"
           "                        psutil.NoSuchProcess):\n"
           "                    continue\n")

    if new in text:
        return False, None
    if old not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return True, bak


def patch_esp32_gateway_wait_for_ip(path):
    """让 esp32 网关只认 ip_address, 跳过启动期先到的积木指令; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    old = ("        # wait for the first message that should provide the ip address of the WifI connection\n"
           "        self.subscriber = await super(Esp32Gateway, self).get_subscriber()\n"
           "        data = await self.subscriber.recv_multipart()\n"
           "        payload = await self.unpack(data[1])\n"
           "        await self.additional_banyan_messages(None, payload)\n")
    new = ("        # 等带板子 IP 的那条报文: Scratch 里正在跑的积木指令会先到, 老代码把它\n"
           "        # 当成板子地址, transport_address 仍是 None, telemetrix 抛 RuntimeError\n"
           "        # \"A TCP/IP address must be specified when using WI-FI.\" 网关当场退出\n"
           "        # (表现: 点连接要等几十秒, 或要点好几次)。这里跳过所有非 ip_address 报文。\n"
           "        self.subscriber = await super(Esp32Gateway, self).get_subscriber()\n"
           "        while not self.transport_address:\n"
           "            data = await self.subscriber.recv_multipart()\n"
           "            payload = await self.unpack(data[1])\n"
           "            if not isinstance(payload, dict) or payload.get('command') != 'ip_address':\n"
           "                continue\n"
           "            await self.additional_banyan_messages(None, payload)\n")

    if new in text:
        return False, None
    if old not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return True, bak


def patch_esp32_gateway_receive_loop(path):
    """让网关等板子连上后再启动接收循环; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    old = "        await self.begin(start_loop=True)\n"
    new = ("        # 不能在 begin() 里就起接收循环: 那时 self.esp 还是 None, 早到的积木指令\n"
           "        # 会让它抛 AttributeError 直接死掉; 而且这个循环会和下面等 ip_address 的\n"
           "        # recv_multipart() 抢同一条消息(谁先 await 谁拿走), IP 那条被吞掉就连不上了。\n"
           "        # 改成连上板子之后, 由 main() 里的 begin_receive_loop() 启动唯一的接收循环。\n"
           "        await self.begin(start_loop=False)\n")

    if new in text:
        return False, None
    if old not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return True, bak


BANYAN_IP_OLD = (
    "            # determine this computer's IP address\n"
    "            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
    "            # use the google dns\n"
    "            try:\n"
    "                s.connect(('8.8.8.8', 1))\n"
    "                self.back_plane_ip_address = s.getsockname()[0]\n"
    "            except Exception:\n"
    "                self.back_plane_ip_address = '127.0.0.1'\n"
    "            finally:\n"
    "                s.close()\n"
)

BANYAN_IP_NEW = (
    "            # 本地补丁 5: 固定用回环地址找 backplane。\n"
    "            # 上游做法是\"连 8.8.8.8 得到本机当前局域网 IP\"再去连 backplane,\n"
    "            # 本机 IP 一变(换网络 / DHCP 续租 / 路由器重启)各组件解析到的\n"
    "            # 地址就对不上了, 表现是积木点了没反应, 必须手工重启整套服务。\n"
    "            # 回环地址永远不会变, 所以固定用它。\n"
    "            self.back_plane_ip_address = '127.0.0.1'\n"
)


def patch_banyan_loopback(path):
    """把 banyan 解析 backplane 地址的方式改成固定回环; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    if BANYAN_IP_NEW in text:
        return False, None
    if BANYAN_IP_OLD not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    path.write_text(text.replace(BANYAN_IP_OLD, BANYAN_IP_NEW, 1), encoding="utf-8")
    return True, bak


BACKPLANE_BIND_OLD = (
    "        # get ip address of this machine\n"
    "\n"
    "        # create a socket\n"
    "        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
    "\n"
    "        # use the google dns to figure out the machine's address and use that address for the backplane.\n"
    "        # this precludes the necessity of having a network configuration file.\n"
    "        # noinspection PyPep8\n"
    "        try:\n"
    "            s.connect(('8.8.8.8', 1))\n"
    "            self.bp_ip_address = s.getsockname()[0]\n"
    "        except:\n"
    "            self.bp_ip_address = '127.0.0.1'\n"
    "        finally:\n"
    "            s.close()\n"
)

BACKPLANE_BIND_NEW = (
    "        # 本地补丁 5: 固定监听所有网卡 (0.0.0.0), 不再绑定\"启动那一刻的本机 IP\"。\n"
    "        # 上游做法是连 8.8.8.8 拿到本机当前局域网 IP 再 bind 上去; 一旦 PC 换网 /\n"
    "        # DHCP 换地址, 这个绑定地址就不再属于本机(甚至可能被别的设备占用), 于是\n"
    "        # 所有组件都连不上 backplane, 表现是积木点了没反应, 只能手工重启服务。\n"
    "        # 绑 0.0.0.0 之后回环 127.0.0.1 与任意当前网卡地址都能连上, 与本机 IP 无关。\n"
    "        self.bp_ip_address = '0.0.0.0'\n"
)


def patch_backplane_bind_all(path):
    """让 backplane 监听所有网卡, 不再绑死本机当前 IP; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    if BACKPLANE_BIND_NEW in text:
        return False, None
    if BACKPLANE_BIND_OLD not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    path.write_text(text.replace(BACKPLANE_BIND_OLD, BACKPLANE_BIND_NEW, 1), encoding="utf-8")
    return True, bak


# ---- 补丁 6: esp32 网关支持"屏幕积木" (LCD 背光 / 颜色) ----
ESP32_GW_LCD_OLD = (
    "    async def additional_banyan_messages(self, topic, payload):\n"
    "        if payload['command'] == 'ip_address':\n"
    "            # start up telemetrix-aio\n"
    "            # if not self.connection_socket:\n"
    "            #     self.connection_socket = True\n"
    "            #     self.esp.ip_port = 31335\n"
    "            #     self.esp.ip_address = payload['address']\n"
    "            #     await self.esp.start_aio()\n"
    "                # await asyncio.sleep(1)\n"
    "            self.transport_address = payload['address']\n"
)

ESP32_GW_LCD_NEW = (
    "    async def additional_banyan_messages(self, topic, payload):\n"
    "        # 本地补丁 6: 屏幕积木 —— LCD 背光 (0x70) / 整屏颜色 (0x71)\n"
    "        if not isinstance(payload, dict):\n"
    "            return\n"
    "        command = payload.get('command')\n"
    "        if command == 'ip_address':\n"
    "            self.transport_address = payload['address']\n"
    "            return\n"
    "        if command == 'lcd_backlight':\n"
    "            # 背光: 0=关, 1~100=亮度%\n"
    "            await self.send_lcd_command([TMX_CMD_LCD_BACKLIGHT,\n"
    "                                         clamp_byte(payload.get('value', 100), 100)])\n"
    "        elif command == 'lcd_color':\n"
    "            # 颜色: R/G/B 各 0~255, 整屏填充\n"
    "            await self.send_lcd_command([TMX_CMD_LCD_COLOR,\n"
    "                                         clamp_byte(payload.get('red', 0), 255),\n"
    "                                         clamp_byte(payload.get('green', 0), 255),\n"
    "                                         clamp_byte(payload.get('blue', 0), 255)])\n"
    "\n"
    "    async def send_lcd_command(self, command):\n"
    "        \"\"\"把自定义屏幕命令发给板子 (板子还没连上就忽略)\"\"\"\n"
    "        if self.esp is None:\n"
    "            return\n"
    "        try:\n"
    "            await self.esp._send_command(command)\n"
    "        except Exception as exc:   # 网关接收循环没有异常保护, 这里自己兜住\n"
    "            print('lcd command failed:', exc)\n"
)

ESP32_GW_LCD_IMPORT_OLD = (
    "from telemetrix_aio_esp32 import telemetrix_aio_esp32\n"
)

ESP32_GW_LCD_IMPORT_NEW = (
    "from telemetrix_aio_esp32 import telemetrix_aio_esp32\n"
    "\n"
    "# 本地补丁 6: 屏幕 (LCD) 自定义 Telemetrix 命令, 与固件 main/tmx_protocol.h 对应\n"
    "TMX_CMD_LCD_BACKLIGHT = 0x70   # 1 字节: 0=关, 1~100=亮度%\n"
    "TMX_CMD_LCD_COLOR = 0x71       # 3 字节: R G B (0~255), 整屏填充\n"
    "\n"
    "\n"
    "def clamp_byte(value, upper):\n"
    "    \"\"\"把积木传来的值夹到 0~upper, 非法值按 0 处理\"\"\"\n"
    "    try:\n"
    "        return max(0, min(upper, int(value)))\n"
    "    except (TypeError, ValueError):\n"
    "        return 0\n"
)


def patch_esp32_gateway_lcd(path):
    """让 esp32 网关支持屏幕积木; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    if "本地补丁 6" in text and "TMX_CMD_LCD_BACKLIGHT" in text:
        return False, None
    if ESP32_GW_LCD_OLD not in text or ESP32_GW_LCD_IMPORT_OLD not in text:
        return None, None      # 没找到预期代码, 交给上层提示
    bak = backup(path)
    text = text.replace(ESP32_GW_LCD_IMPORT_OLD, ESP32_GW_LCD_IMPORT_NEW, 1)
    text = text.replace(ESP32_GW_LCD_OLD, ESP32_GW_LCD_NEW, 1)
    path.write_text(text, encoding="utf-8")
    return True, bak


# ---- 补丁 7: esp32 网关支持"音频积木" (音调 / 停止 / 麦克风) ----
AUDIO_GW_CONST_OLD = (
    "TMX_CMD_LCD_COLOR = 0x71       # 3 字节: R G B (0~255), 整屏填充\n"
)

AUDIO_GW_CONST_NEW = AUDIO_GW_CONST_OLD + (
    "\n"
    "# 本地补丁 7: 音频 (ES8311) 自定义命令/上报, 与固件 main/tmx_protocol.h 对应\n"
    "TMX_CMD_AUDIO_TONE = 0x72      # 5 字节: 频率(2) 时长ms(2) 音量%(1)\n"
    "TMX_CMD_AUDIO_STOP = 0x73      # 无数据\n"
    "TMX_CMD_AUDIO_MIC = 0x74       # 1 字节: 0=关, 1=开麦克风响度上报\n"
    "TMX_REPORT_AUDIO_LEVEL = 0x0D  # 1 字节: 0~100 麦克风响度\n"
    "\n"
    "\n"
    "def clamp_int(value, low, high, default):\n"
    "    \"\"\"把积木传来的值夹到 low~high, 非法值用 default\"\"\"\n"
    "    try:\n"
    "        return max(low, min(high, int(value)))\n"
    "    except (TypeError, ValueError):\n"
    "        return default\n"
)

AUDIO_GW_CMD_OLD = (
    "        elif command == 'lcd_color':\n"
    "            # 颜色: R/G/B 各 0~255, 整屏填充\n"
    "            await self.send_lcd_command([TMX_CMD_LCD_COLOR,\n"
    "                                         clamp_byte(payload.get('red', 0), 255),\n"
    "                                         clamp_byte(payload.get('green', 0), 255),\n"
    "                                         clamp_byte(payload.get('blue', 0), 255)])\n"
)

AUDIO_GW_CMD_NEW = AUDIO_GW_CMD_OLD + (
    "        elif command == 'audio_tone':\n"
    "            # 放音: 频率 Hz / 时长 ms / 音量 %\n"
    "            freq = clamp_int(payload.get('frequency'), 20, 20000, 440)\n"
    "            ms = clamp_int(payload.get('duration'), 1, 60000, 500)\n"
    "            vol = clamp_int(payload.get('volume'), 0, 100, 60)\n"
    "            await self.send_raw_command([TMX_CMD_AUDIO_TONE,\n"
    "                                         (freq >> 8) & 0xFF, freq & 0xFF,\n"
    "                                         (ms >> 8) & 0xFF, ms & 0xFF,\n"
    "                                         vol])\n"
    "        elif command == 'audio_stop':\n"
    "            await self.send_raw_command([TMX_CMD_AUDIO_STOP])\n"
    "        elif command == 'audio_mic':\n"
    "            await self.send_raw_command([TMX_CMD_AUDIO_MIC,\n"
    "                                         1 if payload.get('value', 1) else 0])\n"
)

AUDIO_GW_METHOD_OLD = (
    "        except Exception as exc:   # 网关接收循环没有异常保护, 这里自己兜住\n"
    "            print('lcd command failed:', exc)\n"
)

AUDIO_GW_METHOD_NEW = AUDIO_GW_METHOD_OLD + (
    "\n"
    "    async def send_raw_command(self, command):\n"
    "        \"\"\"把自定义命令原样发给板子 (板子还没连上就忽略)\"\"\"\n"
    "        if self.esp is None:\n"
    "            return\n"
    "        try:\n"
    "            await self.esp._send_command(command)\n"
    "        except Exception as exc:   # 同上, 别让一条命令把接收循环带崩\n"
    "            print('custom command failed:', exc)\n"
    "\n"
    "    async def _audio_level_report(self, data):\n"
    "        \"\"\"板子 0x0D 上报: 麦克风响度 0~100 -> Scratch\"\"\"\n"
    "        await self.publish_payload({'report': 'audio_input', 'value': data[0]},\n"
    "                                   'from_esp32_gateway')\n"
)

AUDIO_GW_DISPATCH_OLD = (
    "        await self.esp.start_aio()\n"
)

AUDIO_GW_DISPATCH_NEW = (
    "        await self.esp.start_aio()\n"
    "\n"
    "        # 本地补丁 7: 板子的音频上报 (0x0D) 走自定义处理, 再发给 Scratch\n"
    "        self.esp.report_dispatch[TMX_REPORT_AUDIO_LEVEL] = self._audio_level_report\n"
)


def patch_esp32_gateway_audio(path):
    """让 esp32 网关支持音频积木; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    if "本地补丁 7" in text and "TMX_CMD_AUDIO_TONE" in text:
        return False, None
    if (AUDIO_GW_CMD_OLD not in text or AUDIO_GW_CONST_OLD not in text or
            AUDIO_GW_METHOD_OLD not in text or AUDIO_GW_DISPATCH_OLD not in text):
        return None, None      # 没找到预期代码(或补丁 6 还没打), 交给上层提示
    bak = backup(path)
    text = text.replace(AUDIO_GW_CONST_OLD, AUDIO_GW_CONST_NEW, 1)
    text = text.replace(AUDIO_GW_CMD_OLD, AUDIO_GW_CMD_NEW, 1)
    text = text.replace(AUDIO_GW_METHOD_OLD, AUDIO_GW_METHOD_NEW, 1)
    text = text.replace(AUDIO_GW_DISPATCH_OLD, AUDIO_GW_DISPATCH_NEW, 1)
    path.write_text(text, encoding="utf-8")
    return True, bak


# ---- 补丁 8: esp32 网关支持"朗读文字" (板载 TTS) ----
TTS_GW_CONST_OLD = (
    "TMX_CMD_AUDIO_MIC = 0x74       # 1 字节: 0=关, 1=开麦克风响度上报\n"
)

TTS_GW_CONST_NEW = TTS_GW_CONST_OLD + (
    "\n"
    "# 本地补丁 8: 板载语音合成 (esp-tts), 与固件 main/tmx_protocol.h 对应\n"
    "TMX_CMD_TTS_TEXT = 0x75        # 1 字节标志(bit0=最后一段) + UTF-8 文字\n"
    "TMX_CMD_TTS_STOP = 0x76        # 无数据: 停止朗读\n"
)

TTS_GW_CMD_OLD = (
    "        elif command == 'audio_mic':\n"
    "            await self.send_raw_command([TMX_CMD_AUDIO_MIC,\n"
    "                                         1 if payload.get('value', 1) else 0])\n"
)

TTS_GW_CMD_NEW = TTS_GW_CMD_OLD + (
    "        elif command == 'audio_tts':\n"
    "            # 朗读文字: 按 UTF-8 拆包发给板子, 板子拼起来再合成\n"
    "            text = payload.get('text')\n"
    "            if text:\n"
    "                await self.send_tts_text(str(text))\n"
    "        elif command == 'audio_tts_stop':\n"
    "            await self.send_raw_command([TMX_CMD_TTS_STOP])\n"
)

TTS_GW_METHOD_OLD = (
    "    async def _audio_level_report(self, data):\n"
    "        \"\"\"板子 0x0D 上报: 麦克风响度 0~100 -> Scratch\"\"\"\n"
    "        await self.publish_payload({'report': 'audio_input', 'value': data[0]},\n"
    "                                   'from_esp32_gateway')\n"
)

TTS_GW_METHOD_NEW = TTS_GW_METHOD_OLD + (
    "\n"
    "    async def send_tts_text(self, text):\n"
    "        \"\"\"按 UTF-8 字符边界拆成 ≤250 字节的 0x75 包 (别把汉字切一半)\"\"\"\n"
    "        chunks = []\n"
    "        current = bytearray()\n"
    "        for ch in text:\n"
    "            raw = ch.encode('utf-8')\n"
    "            if len(current) + len(raw) > 250:\n"
    "                chunks.append(bytes(current))\n"
    "                current = bytearray()\n"
    "            current += raw\n"
    "        if current:\n"
    "            chunks.append(bytes(current))\n"
    "        if not chunks:\n"
    "            return\n"
    "        for index, chunk in enumerate(chunks):\n"
    "            last = 1 if index == len(chunks) - 1 else 0\n"
    "            await self.send_raw_command([TMX_CMD_TTS_TEXT, last] + list(chunk))\n"
)


def patch_esp32_gateway_tts(path):
    """让 esp32 网关支持"朗读文字"积木; 返回 (是否改动, 备份路径)"""
    text = path.read_text(encoding="utf-8")
    if "本地补丁 8" in text and "TMX_CMD_TTS_TEXT" in text:
        return False, None
    if (TTS_GW_CMD_OLD not in text or TTS_GW_CONST_OLD not in text or
            TTS_GW_METHOD_OLD not in text):
        return None, None      # 补丁 6/7 还没打, 交给上层提示
    bak = backup(path)
    text = text.replace(TTS_GW_CONST_OLD, TTS_GW_CONST_NEW, 1)
    text = text.replace(TTS_GW_CMD_OLD, TTS_GW_CMD_NEW, 1)
    text = text.replace(TTS_GW_METHOD_OLD, TTS_GW_METHOD_NEW, 1)
    path.write_text(text, encoding="utf-8")
    return True, bak


def main():
    parser = argparse.ArgumentParser(description="给第三方包打本地补丁")
    parser.add_argument("pins", nargs="*", type=int, default=None,
                        help="额外放行的 GPIO 号 (默认 10 11)")
    args = parser.parse_args()
    extra_pins = args.pins if args.pins else [10, 11]
    problems = []

    # ---- 补丁 1: telemetrix 引脚表 ----
    telemetrix = find_module_path("telemetrix_aio_esp32.telemetrix_aio_esp32")
    if not telemetrix or not telemetrix.exists():
        problems.append("找不到 telemetrix_aio_esp32 (先 pip install s3-extend)")
    else:
        print("目标: %s" % telemetrix)
        result = patch_telemetrix_pins(telemetrix, extra_pins)
        if not result:
            print("  [1/8] 引脚表: 已放行 %s, 无需改动" %
                  ", ".join(str(p) for p in extra_pins))
        else:
            changed, bak = result
            print("  [1/8] 引脚表: 已放行 %s (%s)" %
                  (", ".join(str(p) for p in extra_pins), ", ".join(changed)))
            print("        备份: %s" % bak)

    # ---- 补丁 2: banyan 的 psutil 扫描 ----
    banyan = find_module_path("python_banyan.banyan_base_aio.banyan_base_aio")
    if not banyan or not banyan.exists():
        problems.append("找不到 python_banyan (先 pip install s3-extend)")
    else:
        print("目标: %s" % banyan)
        changed, bak = patch_banyan_psutil(banyan)
        if changed:
            print("  [2/8] psutil 扫描: 已忽略 NoSuchProcess")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [2/8] psutil 扫描: 已是补丁状态, 无需改动")
        else:
            problems.append("python_banyan 的代码与预期不一致, 请手动检查 psutil 那一段")

    # ---- 补丁 3: esp32 网关只认 ip_address ----
    esp32_gateway = find_module_path("s3_extend.gateways.esp32_gateway")
    if not esp32_gateway or not esp32_gateway.exists():
        problems.append("找不到 s3_extend.gateways.esp32_gateway (先 pip install s3-extend)")
    else:
        print("目标: %s" % esp32_gateway)
        changed, bak = patch_esp32_gateway_wait_for_ip(esp32_gateway)
        if changed:
            print("  [3/8] ip_address 报文: 已忽略非 ip_address 的报文")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [3/8] ip_address 报文: 已是补丁状态, 无需改动")
        else:
            problems.append("esp32_gateway.py 的代码与预期不一致, 请手动检查 main() 那一段")

        # ---- 补丁 4: 接收循环不能在 self.esp 之前启动 ----
        changed, bak = patch_esp32_gateway_receive_loop(esp32_gateway)
        if changed:
            print("  [4/8] 接收循环: 改为连上板子后再启动")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [4/8] 接收循环: 已是补丁状态, 无需改动")
        else:
            problems.append("esp32_gateway.py 里找不到 begin(start_loop=True), 请手动检查")

        # ---- 补丁 6: 屏幕积木 (背光 / 颜色) ----
        changed, bak = patch_esp32_gateway_lcd(esp32_gateway)
        if changed:
            print("  [6/8] 屏幕积木: 已加入 lcd_backlight / lcd_color 命令")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [6/8] 屏幕积木: 已是补丁状态, 无需改动")
        else:
            problems.append("esp32_gateway.py 里找不到 additional_banyan_messages, 请手动检查")

        # ---- 补丁 7: 音频积木 (音调 / 停止 / 麦克风响度) ----
        changed, bak = patch_esp32_gateway_audio(esp32_gateway)
        if changed:
            print("  [7/8] 音频积木: 已加入 audio_tone / audio_stop / audio_mic 与响度上报")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [7/8] 音频积木: 已是补丁状态, 无需改动")
        else:
            problems.append("esp32_gateway.py 里找不到屏幕积木补丁的代码, 请先检查补丁 6")

        # ---- 补丁 8: 朗读文字 (板载 TTS) ----
        changed, bak = patch_esp32_gateway_tts(esp32_gateway)
        if changed:
            print("  [8/8] 朗读文字: 已加入 audio_tts / audio_tts_stop (UTF-8 拆包)")
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [8/8] 朗读文字: 已是补丁状态, 无需改动")
        else:
            problems.append("esp32_gateway.py 里找不到音频积木补丁的代码, 请先检查补丁 7")

    # ---- 补丁 5: Banyan 固定用回环地址 (本机 IP 变化不再影响连接) ----
    loopback_targets = (
        ("banyan_base_aio", "python_banyan.banyan_base_aio.banyan_base_aio",
         patch_banyan_loopback),
        ("banyan_base", "python_banyan.banyan_base.banyan_base",
         patch_banyan_loopback),
        ("backplane", "python_banyan.backplane.backplane", patch_backplane_bind_all),
    )
    for label, module, patch_func in loopback_targets:
        target = find_module_path(module)
        if not target or not target.exists():
            problems.append("找不到 %s, 无法把 backplane 地址固定成回环" % module)
            continue
        changed, bak = patch_func(target)
        if changed:
            print("  [5/8] %s: 已改为回环/全网卡" % label)
            print("        备份: %s" % bak)
        elif changed is False:
            print("  [5/8] %s: 已是补丁状态, 无需改动" % label)
        else:
            problems.append("%s 的代码与预期不一致, 请手动检查 IP 解析那一段" % module)

    print()
    if problems:
        for p in problems:
            print("注意: %s" % p)
        return 1
    print("重启网关让补丁生效:")
    print("    Get-Process esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force")
    return 0


if __name__ == "__main__":
    sys.exit(main())
