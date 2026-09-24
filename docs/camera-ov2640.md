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
| PSRAM 约 132KB | VGA 帧缓冲 100KB + DMA 缓冲 32KB | 已经开在 **8MB Octal PSRAM** 上，不占内部 RAM；关掉 `CONFIG_SPIRAM` 就会退回内部 RAM |

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
| `TMX_CAMERA_XCLK_FREQ_HZ` | 20000000 | XCLK 频率。OV2640 的寄存器表是按 24MHz 定的，本工程实际默认 24MHz；画面有横条纹先看 `TMX_CAMERA_WARMUP_MS`，不是 XCLK 的问题（见 [camera-debug-notes.md](camera-debug-notes.md)） |
| `TMX_CAMERA_XCLK_LEDC_TIMER` / `_CHANNEL` | 1 / 6 | XCLK 占用的 LEDC 资源 |
| `TMX_CAMERA_FRAMESIZE_*` | VGA | QVGA/VGA/SVGA/XGA/SXGA/UXGA |
| `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM` / `_SIZE` | y / 100000 | JPEG 帧缓冲大小；自动算法 `w*h/5` 只有 61KB，画面细节一多整帧就被判 `FB-OVF` 丢掉（组件菜单，不在本工程 Kconfig 里）|
| `TMX_CAMERA_JPEG_QUALITY`（默认由 12 调成 20） | 20 | 0~63，越小越清晰、帧越大；VGA 用质量 12 时帧可到 100KB，配 100KB 缓冲取 20 更稳 |
| `TMX_CAMERA_XCLK_FREQ_HZ`（默认由 20MHz 调成 24MHz） | 24000000 | **重要**：ESP32 的 OV2640 寄存器表是按 24MHz 调的。20MHz/10MHz 实测画面全是噪点、JPEG 头里的 DHT/SOF 标记字节被采错（`C4→C5`/`C0→C1`），一半的帧无法解码；24MHz 下全部正常 |
| 夜间模式（在线切 XCLK） | 24MHz | 想要夜里横纹少一点，用 `python tools\pc_camera_tune.py <ip> --set xclk 12` 把 XCLK 降到 12MHz：帧率约减半、曝光时间上限翻倍，同样亮度下自动增益更低 → 逐行噪声更少（白天想恢复就 `--set xclk 24`）。重启后回到 Kconfig 默认值 |
| `TMX_CAMERA_GAIN_CEILING` | 3 | 自动增益上限（0=2X … 3=16X … 6=128X）。弱光下压小它噪点更少（画面偏暗），自动曝光会拉长曝光补偿；想要原厂行为填 6。开机还会自动做一次 JPEG 头自检，发现问题就重新初始化传感器 |
| `TMX_CAMERA_IDLE_POWER_OFF` | y | 空闲时给摄像头断电降温：开机自检后立刻停 XCLK+PWDN 掉电，拍照前重新上电初始化（约 +0.3s），拍完再断电 |
| `TMX_CAMERA_WARMUP_MS` | **2000** | 上电后预热时长：初始化完先抓帧丢掉不返回，等 AEC/AGC/AWB 收敛再拍。OV2640 上电时自动算法是从默认值逐帧收敛的，头几帧增益拉满、白平衡没收敛；配合上面的"拍完就断电"，不加预热的话**每一张照片都是最脏的第 0 帧**（实测逐行噪声是收敛后的 4.5~8 倍，就是画面上的"很多噪点 + 彩色横纹"）。0 = 关 |
| `TMX_CAMERA_WARMUP_FRAMES` | 20 | 预热除了等够时间还至少丢掉这么多帧（收敛按帧推进，夜间模式帧率减半时防止时间够了帧不够） |
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
| 画面很多噪点 / 彩色横纹 | **`TMX_CAMERA_WARMUP_MS` 是不是被改成 0 了**（上电预热，见下）；其次用 `tools\pc_camera_stripe.py` 量一下行噪声；最后才是杜邦线缩短、数据线别和电源线绞在一起 |
| 画面花屏 / 整屏噪点 / 颜色错乱 | 先 `--scan` 看 SCCB 有没有 0x30、再确认 VDDCAM_3V3(ALDO2) 有没有开；XCLK 降到 10~16MHz 是以前的老办法，现在先查供电 |
| 画面很暗 / 偏色 | 正常现象, OV2640 默认增益在弱光下偏暗; 可通过 `sensor_t` 调 `set_brightness/set_gain_ctrl` (暂未做成积木) |
| 帧数据不完整 (`不是完整的 JPEG`) | 看脚本打印的"分片错位"计数; TCP 上不该丢, 多了说明板子内存被打爆 |
| 拍照后其它积木变卡 | 连续拍是"边拍边发", 把 `interval` 调大或拍完发 `CAMERA_STOP` |
| 接上摄像头后板子启动异常 / 一直进下载模式 | VSYNC(IO3) 和 HREF(IO46) 是 ESP32-S3 的 Strapping 脚, 摄像头模块上电瞬间的电平可能影响启动采样。先断开这两个信号试上电: 能正常启动就是它, 换到非 Strapping 脚或给这两条线加上拉/下拉即可 |

## Scratch 积木

扩展 `scratch/esp32s3.js` 里有 7 块相机积木，整条链路和 PC 脚本走的是同一套协议。
主线是**流式播放**：打开摄像头后板子连续出图，每一帧原地刷到当前角色的
「摄像头画面」造型上 —— 舞台上看到的就是实时画面。

```
点「打开摄像头」   └─ 扩展 ws ─► wsgw ─► Banyan ─► esp32gw ─► TCP 0x79(帧数=0) ─► 板子
                                                                                 │
板子 0x10 帧头 + 0x0F 分片 (每片 ≤240 字节, 一路连续发)  ◄───────────────────────┘
   └─ esp32gw 按偏移拼回整帧 ─► base64 ─► Banyan ─► wsgw ─► 扩展 (每帧一次)
                                                                 │
            第一帧: vm.addCostume 建一个「摄像头画面」造型
            之后每帧: renderer.updateBitmapSkin 原地换贴图 (造型数量不涨)
```

| 积木 | 发出去的命令 | 说明 |
| --- | --- | --- |
| 摄像头尺寸 [QVGA/VGA/.../UXGA] | `camera_config` (0x78) | 只改分辨率，质量那一项发 `0xFF` 表示不改 |
| 拍照质量 [0~63] | `camera_config` (0x78) | 拍照片用的质量（默认 20，清晰但帧大） |
| 视频质量 [0~63] | `camera_config` (0x78) | **流式播放期间**用的质量（默认 35，帧更小、帧率更高）。点「打开摄像头」时临时切过去，关闭时把拍照质量还回来 |
| **打开摄像头（画面显示在当前角色上）** | `camera_snapshot` (0x79)，帧数=0 | **流式播放**。不等：开完立刻往下走，画面过几秒（上电 + 预热）才出现 |
| 关闭摄像头 | `camera_stop` (0x7A) | 停流 + 断电降温；点编辑器停止按钮也会自动调它 |
| 拍一张照片（变成新造型） | `camera_snapshot` (0x79)，帧数=1 | **会等**：造型挂好了才继续。摄像头开着时直接截当前帧（瞬时） |
| 摄像头状态 | `camera_info` (0x7B) + 上报 0x12 | 报告积木；显示"最近发生的一件事"（视频帧率／拍照结果／分辨率质量 XCLK／出错提示） |
| 照片（数据 URL） | —— | 最近一张照片的 `data:image/jpeg;base64,...`，可以自己拿去用 |

### 流式播放的实测帧率

**帧率 ≈ 板子到 PC 的带宽 ÷ 每帧字节数**（实测带宽 45~90 KB/s，随 WiFi 环境波动），
所以想让画面动起来只有一条路：**把每帧变小**。三个旋钮从上到下依次是尺寸、视频质量、
以及我们自己在扩展里加的帧间隔。经完整链路（Scratch 那一套：网关拼帧 + base64 +
WebSocket）实测：

| 设置 | 帧大小 | 帧率 |
| --- | --- | --- |
| QVGA 320x240 + 视频质量 35 | ~3.2 KB | **14 帧/秒** |
| VGA 640x480 + 视频质量 45 | ~8.2 KB | **7 帧/秒** |
| VGA 640x480 + 视频质量 35 | ~10 KB | **5 帧/秒** |
| VGA 640x480 + 拍照质量 20（老默认） | ~14 KB | 3.7 帧/秒 |

所以**想要流畅就用 QVGA**（帧率是 VGA 的 3 倍左右，因为像素只有 1/4）；
VGA 只适合"看得清"而不是"动得顺"。为什么上限是这个数、都试过哪些旋钮，
见 [camera-debug-notes.md](camera-debug-notes.md) 的"流式播放"两节。

### 几个要知道的点和坑

* **「拍照」大概要 3 秒**。固件为了降温默认"拍完就断电"，所以每次拍照都要重新上电
  初始化，再加上 `TMX_CAMERA_WARMUP_MS`(默认 2 秒) 的预热帧。嫌慢就把预热调短
  （见上面"横纹/噪点"那节的取舍）。
* **照片按"双倍分辨率"挂成造型**：640x480 的照片在 480x360 的舞台上占 320x240，
  正好放得下。想让它更大就自己在 Scratch 里改角色大小。
* **"自动变成造型"要用支持非沙箱扩展的编辑器**（TurboWarp）。扩展顶部有
  `Scratch.extensions.unsandboxed = true`，靠它才能拿到 scratch-vm 的
  `runtime.storage` / `vm.addCostume`。拿不到的编辑器里，拍照本身没问题，
  「照片（数据 URL）」也能取到图，只是不会自动生成造型 —— 状态积木会说明原因。
* **照片是 JPEG，造型是 PNG**：扩展会在浏览器里过一遍 canvas 转成 PNG 再挂上去，
  和 Scratch 自己"上传一张 jpg"时的处理一致。
* **摄像头只能一个人用**：板子同一时刻只服务一个客户端。Scratch 连着板子时，
  PC 上的 `pc_camera_check.py` 会自动连不上（先 `tools\stop_s3extend.ps1`）。

### 网关侧的三个补丁（`tools/apply_local_patches.py`）

| 补丁 | 为什么必须有 |
| --- | --- |
| 9 | 网关认识 `camera_config/snapshot/stop/info`，并把 0x0F~0x12 转成 Scratch 的 report |
| 10 | telemetrix 的 WiFi 读函数要**读满** `num_bytes`。一帧 80 多片，跨 TCP 分段时必现短读 |
| 11 | 接收循环不能因为**没注册的上报码**（比如调试用的引脚探针 0x13）或处理器异常而静默死掉 |

补丁 10 / 11 缺一个的现象都很像"板子坏了"：网关进程还在、到板子的 TCP 还是
ESTABLISHED、串口里板子照常"拍照 / 送 20620 字节"，但照片永远收不到，连
`camera_info` 都不再回。排查记录见 [camera-debug-notes.md](camera-debug-notes.md)。
