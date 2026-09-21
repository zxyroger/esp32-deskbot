# 板载 OV2640 摄像头 (DVP 并口)

板子上的 OV2640 是 **DVP 并口**摄像头: 8 位数据线 + PCLK/VSYNC/HREF 同步线 +
XCLK 时钟, 控制口是 SCCB (本质是 I2C)。固件用 Espressif 的
[esp32-camera](https://components.espressif.com/components/espressif/esp32-camera)
组件驱动它 (`firmware/main/tmx_camera.c`)。

## 接线 (menuconfig 默认值就是本板的接线)

| 摄像头信号 | GPIO | 说明 |
| --- | --- | --- |
| SIOD / SCCB SDA | 1 | 控制口数据, **和 ES8311 音频 codec 共用板载 I2C 总线** |
| SIOC / SCCB SCL | 2 | 控制口时钟, 同上 |
| VSYNC | 3 | 帧同步 (Strapping 脚, 当输入用没问题) |
| PWDN | 48 | 低电平工作; 不用就填 -1 |
| HREF | 46 | 行有效 (Strapping 脚, 当输入用没问题) |
| XCLK | 8 | 摄像头主时钟, 由 LEDC 输出 20MHz |
| PCLK | 16 | 像素时钟 (摄像头 → 板子) |
| D0 ~ D7 | 7 / 5 / 4 / 6 / 15 / 17 / 18 / 9 | 8 位数据 |
| RESET | — | 未接 (menuconfig 里填 -1) |

SCCB 上两个设备地址不同, 所以可以挂在同一条总线上:
OV2640 是 **0x30**, ES8311 是 **0x18** (8 位写法 0x60 / 0x30)。

## 它占用了什么

| 资源 | 占用 | 怎么还回来 |
| --- | --- | --- |
| GPIO 3/4/5/6/7/8/9/15/16/17/18/46/48 | 摄像头 DVP + XCLK + PWDN | 关掉 `TMX_CAMERA_ENABLE` |
| GPIO 1/2 | SCCB (与 I2C 积木共用, 本来就被音频占着) | 同上 |
| ADC1 的 GPIO3~GPIO9 | DVP 数据/同步脚, 不再能做模拟输入 | 同上 |
| LEDC 定时器 1 + 通道 6 | XCLK (20MHz) | 同上, 或换 `TMX_CAMERA_XCLK_LEDC_*` |
| 内部 RAM 约 92KB | VGA 帧缓冲 61KB + DMA 缓冲 32KB | 降分辨率 / `FB_COUNT=1` / 开 PSRAM |

**对 Scratch 的影响**:

* 摄像头用掉的 13 个 GPIO 会从"数字/模拟引脚"里排除, 选了会打一条告警日志;
* **模拟输入基本全没了**: Scratch 的模拟引脚 32~39 映射到 GPIO1~GPIO6,
  其中 GPIO3~GPIO6 被摄像头占掉, GPIO1/GPIO2 是 I2C 总线 —— 所以开了摄像头
  就别指望模拟输入了 (想两者都要, 只能把摄像头改到别的脚, 见 menuconfig);
* PWM/舵机可用路数不变 (通道池 8 路里让出 1 路), 但普通 PWM 的
  "频率/分辨率组合数" 从 3 组变 1 组 —— Scratch 的 PWM 积木固定用
  5kHz/8bit, 所以积木侧完全无感;
* 屏幕、音频、TTS、超声波都不受影响。

## 配置项 (menuconfig)

菜单: `ESP32-S3 Scratch 服务器配置` → `摄像头 (OV2640 DVP)`

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `TMX_CAMERA_ENABLE` | y | 总开关; 关掉后引脚和内存全部还给 Scratch |
| `TMX_CAMERA_XCLK_PIN` 等 | 见上表 | 各信号线引脚 |
| `TMX_CAMERA_PWDN_PIN` / `_RESET_PIN` | 48 / -1 | 没接就填 -1 |
| `TMX_CAMERA_XCLK_FREQ_HZ` | 20000000 | 画面有横条纹/花屏时降到 10~16MHz |
| `TMX_CAMERA_XCLK_LEDC_TIMER` / `_CHANNEL` | 1 / 6 | XCLK 占用的 LEDC 资源 |
| `TMX_CAMERA_FRAMESIZE_*` | VGA | QVGA/VGA/SVGA/XGA/SXGA/UXGA |
| `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM` / `_SIZE` | y / 100000 | JPEG 帧缓冲大小；自动算法 `w*h/5` 只有 61KB，画面细节一多整帧就被判 `FB-OVF` 丢掉（组件菜单，不在本工程 Kconfig 里）|
| `TMX_CAMERA_JPEG_QUALITY`（默认由 12 调成 20） | 20 | 0~63，越小越清晰、帧越大；VGA 用质量 12 时帧可到 100KB，配 100KB 缓冲取 20 更稳 |
| `TMX_CAMERA_XCLK_FREQ_HZ`（默认由 20MHz 调成 24MHz） | 24000000 | **重要**：ESP32 的 OV2640 寄存器表是按 24MHz 调的。20MHz/10MHz 实测画面全是噪点、JPEG 头里的 DHT/SOF 标记字节被采错（`C4→C5`/`C0→C1`），一半的帧无法解码；24MHz 下全部正常 |
| `TMX_CAMERA_GAIN_CEILING` | 3 | 自动增益上限（0=2X … 3=16X … 6=128X）。弱光下压小它噪点更少（画面偏暗），自动曝光会拉长曝光补偿；想要原厂行为填 6。开机还会自动做一次 JPEG 头自检，发现问题就重新初始化传感器 |
| `TMX_CAMERA_IDLE_POWER_OFF` | y | 空闲时给摄像头断电降温：开机自检后立刻停 XCLK+PWDN 掉电，拍照前重新上电初始化（约 +0.3s），拍完再断电 |
| `TMX_CAMERA_PMIC_AVDD_MV` / `TMX_CAMERA_PMIC_DVDD_MV` | 2800 / **1200** | 每次开机把 AXP2101 的 AVDD(BLDO1) / DVDD(BLDO2) 设成这个值并打开。**DVDD 别填 2.8V**：那是 1.2V 内核供电，过压会让模块明显发烫 |
| `TMX_CAMERA_PMIC_ALDO2_MV` | 2800 | 摄像头 I/O 供电 VDDCAM_3V3 接在 AXP2101 的 **ALDO2** 上，芯片默认是**关的**。不打开时 DVP 高电平只有漏电电压（实测 1.8V），低于 ESP32 判高门限 → 帧数据全乱、拍不出图（SCCB 因为开漏上拉仍能通，很容易误判成模组坏了）。固件开机会把它设成这个电压并打开 |
| `TMX_CAMERA_PMIC_AXP2101` | y | 开机先把 AXP2101 的 BLDO1(AVDD)/BLDO2(DVDD) 打开 (本板摄像头供电, 只动使能位不改电压)；关掉就完全不动 PMIC |
| `TMX_CAMERA_PIN_PROBE` | n | 启动时量一遍 DVP 各根线的活动 (PCNT 硬件计数) 并扫一遍空闲 GPIO；2026-09-20 的排查结论见 [camera-debug-notes.md](camera-debug-notes.md) |
| `TMX_CAMERA_JPEG_QUALITY` | 12 | 0~63, 越小越清晰 (码流越大) |
| `TMX_CAMERA_FB_COUNT` | 1 | 帧缓冲份数; 内存够就改 2 更流畅 |
| `TMX_CAMERA_STREAM_INTERVAL_MS` | 200 | 连续拍的默认间隔 (约 5fps) |

开机日志里会打印摄像头是否就绪, 例如:

```
I (812) tmx_camera: OV2640 ready: VGA 640x480, JPEG 质量 12, XCLK 20MHz, fb=1, PID=0x26
I (812) tmx_camera: 引脚: XCLK=8 PCLK=16 VSYNC=3 HREF=46 D0..D7=7,5,4,6,15,17,18,9 PWDN=48 SCCB=IO1/IO2(共用 I2C)
```

初始化失败时只影响拍照, 其它积木照常:

```
E (900) tmx_camera: OV2640 初始化失败: ESP_ERR_NOT_FOUND
E (900) tmx_camera:   检查: 摄像头供电 / PWDN=IO48 / XCLK=IO8 / SIOD=IO1 / SIOC=IO2
```

## 怎么把画面取出来

固件不做 HTTP 图传, 而是把 JPEG 通过 Telemetrix 协议分片发给 PC
(协议细节见 [protocol.md](protocol.md) 的 0x78~0x7B / 0x0F~0x12),
PC 侧脚本负责拼成一帧 `.jpg`:

```powershell
# 板子同时只服务一个客户端, 先让出板子
D:\esp\onegpio\tools\stop_s3extend.ps1

python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103              # 拍 1 帧
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --frames 5   # 拍 5 帧
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --stream 5   # 连续拍 5 秒
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --size QVGA --quality 8
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --info       # 只看状态
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --scan       # 扫 I2C/SCCB
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --view       # 拍完直接打开看图
```

`--scan` 是排查接线的第一站, 它会把 I2C 上应答的地址都列出来:

```
  0x18 有应答  <- ES8311 (音频 codec)
  0x30 有应答  <- OV2640 (SCCB)
```

只有 0x18 没有 0x30, 就说明板子/固件都好, 问题在摄像头这一侧
(供电、排线、SIOD/SIOC 接反、PWDN 一直是低/高)。

## 传输速度与帧率

每帧 JPEG 按 240 字节一片发 (`0x0F`), 空闲时主循环每轮最多发 16 片,
所以慢链路也不会把服务器任务占死。实测参考 (VGA / 质量 12, WiFi 局域网):

* 单帧大约 15~30KB, 一堆包加起来几毫秒发完;
* 连续拍时帧率由 `interval` 决定 (默认 200ms ≈ 5fps);
  真正能跑多快取决于分辨率、JPEG 质量和 WiFi 带宽。

> 为什么不做实时视频流: Telemetrix 单包最长 255 字节, 中间还要过
> Banyan/ZMQ/WebSocket, 再叠加 Scratch 自己的渲染, 传 VGA 实时视频不现实。
> 现在的定位是"拍照 + 低帧率连拍", 想连续看画面用 `--stream` 存帧再看。

## 排障

| 症状 | 先看这里 |
| --- | --- |
| 日志 `OV2640 初始化失败: ESP_ERR_NOT_FOUND` | 摄像头供电 / 排线 / SIOD-SIOC / PWDN; 用 `--scan` 确认 0x30 |
| 日志 `frame buffer malloc failed` | 内存不够: 分辨率降 QVGA, 或 `TMX_CAMERA_FB_COUNT=1` |
| `--scan` 看不到 0x30 但能看到 0x18 | 问题在摄像头一侧 (板上 I2C 是好的) |
| 收不到任何帧 (超时) | 板子是不是被网关占着 (先 `stop_s3extend.ps1`); 串口里有没有 "拍照: ..." 日志 |
| 画面花屏 / 横条纹 / 颜色错乱 | XCLK 降到 10~16MHz; 杜邦线尽量短, 数据线别和电源线绞在一起 |
| 画面很暗 / 偏色 | 正常现象, OV2640 默认增益在弱光下偏暗; 可通过 `sensor_t` 调 `set_brightness/set_gain_ctrl` (暂未做成积木) |
| 帧数据不完整 (`不是完整的 JPEG`) | 看脚本打印的"分片错位"计数; TCP 上不该丢, 多了说明板子内存被打爆 |
| 拍照后其它积木变卡 | 连续拍是"边拍边发", 把 `interval` 调大或拍完发 `CAMERA_STOP` |
| 接上摄像头后板子启动异常 / 一直进下载模式 | VSYNC(IO3) 和 HREF(IO46) 是 ESP32-S3 的 Strapping 脚, 摄像头模块上电瞬间的电平可能影响启动采样。先断开这两个信号试上电: 能正常启动就是它, 换到非 Strapping 脚或给这两条线加上拉/下拉即可 |

## 和 Scratch 的关系

目前摄像头只在**协议层**开放 (上面那个 PC 脚本), Scratch 扩展里还没有
"拍照" 积木: 把 JPEG 搬进 Scratch 需要先把二进制经 Banyan/WebSocket
转成 base64 再做成造型, 属于另一条链路。要加的话建议这样分工:

```
板子 0x0F 分片 ─► esp32gw 拼帧 ─► Banyan(from_esp32_gateway)
                                   └► Scratch 扩展把 base64 变成造型
```

`tools/apply_local_patches.py` 里补丁 6/7/8 就是给网关加自定义命令的模板,
补丁 9 可以照抄那套写法。
