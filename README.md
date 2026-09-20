# ESP32-S3 Scratch 服务器 (ESP-IDF v5.5.4)

用 ESP32-S3 做一台 **Telemetrix 服务器**，配合 PC 上已经安装的
[s3-extend](https://github.com/MrYsLab/s3-extend) 扩展服务器，让
**Scratch 3 离线版**可以直接控制板子的数字口、PWM、舵机、模拟输入和超声波。

```
┌────────────────────┐   WebSocket    ┌──────────────────────┐   TCP 31336   ┌──────────────────┐
│  Scratch 3 离线版  │ 127.0.0.1:9007 │  s3-extend (PC 端)   │  局域网/WiFi  │   ESP32-S3       │
│  (OneGPIO ESP32)   │ ─────────────► │ backplane + wsgw +   │ ────────────► │  本工程固件      │
│  IP 地址积木       │ ◄───────────── │ esp32gw              │ ◄──────────── │  (WiFi + TCP)    │
└────────────────────┘   报告上报      └──────────────────────┘   报告上报     └──────────────────┘
```

s3-extend 里的 `esp32gw` 网关使用 `telemetrix_aio_esp32` 客户端，
它会以 **WiFi / TCP** 方式连接板子，端口默认 **31336**，
协议是 MrYsLab 的 **Telemetrix 二进制协议**（`[长度][命令][数据...]`）。
本工程就是把这个协议用 ESP-IDF v5.5.4 在 ESP32-S3 上重写了一遍。

## 目录结构

```
onegpio/
├─ firmware/                 ESP-IDF v5.5.4 工程 (目标芯片 esp32s3)
│  ├─ CMakeLists.txt
│  ├─ sdkconfig.defaults
│  └─ main/
│     ├─ app_main.c          启动入口
│     ├─ wifi_link.c/.h      WiFi STA (+可选 SoftAP 兜底 + UDP 广播板子 IP)
│     ├─ tmx_server.c/.h     TCP 服务器 (默认 31336)
│     ├─ tmx_core.c/.h       协议引擎: 命令解析 / 引脚状态 / 上报扫描
│     ├─ tmx_io.c/.h         GPIO / ADC / PWM / 舵机 / 超声波
│     ├─ tmx_i2c.c/.h        I2C 主站 (新版 i2c_master 驱动)
│     ├─ tmx_audio.c/.h      ES8311 音频输入/输出 (I2S 全双工 + PA)
│     ├─ tmx_camera.c/.h     OV2640 DVP 摄像头 (esp32-camera, SCCB 共用 I2C)
│     ├─ tmx_protocol.h      协议常量 (命令码 / 报告码 / 引脚模式)
│     └─ Kconfig.projbuild   menuconfig 配置项 (WiFi / 引脚 / 屏幕 / 音频 / 摄像头)
├─ tools/
│  ├─ idf.ps1                一键调用 ESP-IDF (menuconfig / build / flash / monitor)
│  ├─ start_s3extend.ps1     启动 s3-extend (-Check 体检 / -Logs 看日志 / -Background 后台)
│  ├─ stop_s3extend.ps1      停止全部组件
│  ├─ supervise_s3extend.ps1 守护进程: 组件掉线自动拉起
│  ├─ start_launcher.ps1     启动"按需启动器" (-Check / -Stop / -Foreground)
│  ├─ onegpio_launcher.py    微型启动器本体: /start /stop /status + 托管扩展脚本
│  ├─ install_autostart.ps1  安装/卸载登录自启 (-OnDemand = 只常驻启动器)
│  ├─ serve_scratch_extension.ps1  把 scratch/ 挂到 http://127.0.0.1:8000
│  ├─ find_board.py          自动找板子: 先听 UDP 广播, 收不到再扫网段
│  ├─ pc_tcp_check.py        不依赖 Scratch 的协议自检脚本 (含舵机测试)
│  ├─ pc_telemetrix_demo.py  用 s3-extend 同款 Python 库直接驱动板子
│  ├─ apply_local_patches.py 给第三方包打本地补丁 (引脚表/网关/回环地址/屏幕/音频命令)
│  ├─ banyan_publish.py      往 Banyan 总线上发一条消息 (调试/状态回传用)
│  ├─ test_esp32s3_status.js   扩展"连接状态"逻辑的冒烟测试 (假 WebSocket)
│  ├─ test_esp32s3_autostart.js 扩展"点积木自动拉起服务"逻辑的冒烟测试
│  ├─ test_esp32s3_audio.js  扩展"音频积木"的冒烟测试 (检查发出去的报文)
│  ├─ test_gateway_audio.py  网关"音频积木"补丁的自检 (检查翻译出的协议字节)
│  ├─ pc_audio_loopback_check.py 真机音频自检: 放音调同时读麦克风 (自听回环)
│  ├─ pc_tts_check.py        真机 TTS 自检: 发文字让板子念, 并回传 PCM 存 wav
│  ├─ pc_camera_check.py     真机摄像头自检: 拍照并把 JPEG 存下来 / 扫 SCCB
│  └─ test_autostart_live.js  真机联调: 停掉服务再靠点积木把它拉起来
├─ scratch/
│  └─ esp32s3.js             自带 Scratch 3 扩展 (已端到端实测)
└─ docs/
   ├─ protocol.md            协议字节表 (命令 / 报告)
   ├─ audio-es8311.md        板载 ES8311 音频输入输出 (接线 / 配置 / 排障)
   ├─ tts-esp-tts.md         板载中文语音合成 (分区 / 积木 / 自检 / 排障)
   ├─ camera-ov2640.md       板载 OV2640 摄像头 (接线 / 协议 / 拍照自检 / 排障)
   ├─ service-install.md     服务安装说明: 登录自启 + 点积木自动拉起 (按步骤操作)
   ├─ scratch-setup.md       Scratch 离线版 + s3-extend 连接步骤
   └─ pins-esp32s3.md        引脚可用性与限制
```

## 快速开始

### 1. 配置并烧录固件

```powershell
cd D:\esp\onegpio\firmware
..\tools\idf.ps1 menuconfig      # 填写 WiFi 名称/密码 (菜单: ESP32-S3 Scratch 服务器配置)
..\tools\idf.ps1 -Port COM5 flash monitor
```

> **用哪个 USB 口**：本工程把调试串口放在板子的**原生 USB 口**（USB Serial/JTAG），
> 所以一根 USB 线就能同时烧录、看日志、复位。`-Port` 填的就是这个 USB 口对应的 COM 号。
> 如果板子上还有一个 USB 转串口（CH340/CP2102）的口，用它烧录也可以，但**日志只会从 USB 口出来**。

第一次编译也可以直接用 IDF 桌面快捷方式的环境：

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'
cd D:\esp\onegpio\firmware
idf.py -p COM5 flash monitor
```

上电后串口会打印：

```
I (1234) wifi: 板子 IP 地址: 192.168.1.123   端口: 31336
I (1234) wifi: 请把该 IP 填进 Scratch 的 IP 地址积木
```

### 2. 先用 PC 脚本确认板子在线（推荐）

```powershell
python D:\esp\onegpio\tools\pc_tcp_check.py 192.168.1.123
```

它会读取固件版本、做回环测试、点亮/熄灭 GPIO2、读一次模拟输入。
这一步跑通，说明固件和网络都没问题，剩下的问题只可能在 PC 端。

### 3. 启动 s3-extend 并打开 Scratch

```powershell
# 启动 ESP32 扩展服务器 (backplane + websocket gateway 9007 + esp32 网关)
D:\esp\onegpio\tools\start_s3extend.ps1
```

> 本机的 Python 是微软商店版，`s3-extend` 的启动器目录不在 PATH 里，
> 所以不要直接敲 `s32`（会报 `FileNotFoundError`）。这个脚本会先补好 PATH 再启动。
> 想检查环境是否就绪：`D:\esp\onegpio\tools\start_s3extend.ps1 -Check`。

Scratch 3 离线版里：

1. 加载扩展（WebSocket 地址固定为 `ws://127.0.0.1:9007`）：
   推荐用 **TurboWarp Desktop + 本项目自带的 [scratch/esp32s3.js](scratch/esp32s3.js)**
   （完全离线，且不会有下面第 2 条说的主题名问题）；
   原版 Scratch Desktop 3.32.1 没有加载自定义扩展的入口。
2. **IP 地址积木可以留空**：后台守护进程会自动发现板子并把地址发给网关
   （见下一节）。只有手动方式启动（`-Logs`）或想指定另一块板子时，
   才需要把串口打印的 IP 填进 **IP 地址积木**。
3. 用数字输出 / PWM / 舵机 / 数字输入 / 模拟输入 / 超声波积木控制板子。

详细步骤和常见问题见 [docs/scratch-setup.md](docs/scratch-setup.md)。

## 板子 IP 自动发现（不用去串口抄 IP）

板子换 IP（DHCP 续租/换路由器/换网络）以后，以前必须去串口日志里抄 IP 再手填，
现在整条链路都能自动完成：

```
板子 UDP 广播 "ONEGPIO <ip> <port>"  ──►  PC 守护进程/工具发现
       (每 2 秒一次, 也能用 "ONEGPIO?" 主动探测, 毫秒级应答)
                                          │
       网关(esp32gw) ◄──── ip_address ─────┘
              │
              ▼  TCP 31336
            板子  ──► board_status(connected, ip)  ──►  Scratch 扩展自动采用这个 IP
```

* 固件：`CONFIG_TMX_UDP_BEACON_ENABLE=y`（默认开），端口 `31337`，间隔 2 秒，
  广播内容就是 `ONEGPIO 192.168.0.103 31336`；串口日志里会打印
  `UDP beacon started: broadcasting 192.168.0.103:31336`。
* PC 侧：`tools/find_board.py` 先发 `ONEGPIO?` 探测并听广播（毫秒级），
  收不到再退回"扫本机 /24 的 31336 端口 + 固件版本握手"的老办法，
  所以**旧固件（没有广播）也一样能用**。
* 守护进程：启动时、地址连不上时、以及板子一直没连上时（每 15 秒）
  都会自动发现一次——板子后上电、换了 DHCP 地址都能自己连上。
* Scratch 扩展：收到 `board_status(connected)` 时会自动把网关报上来的地址
  当作目标地址，因此**不点 IP 积木也能直接发指令**。

手动用一下：

```powershell
python D:\esp\onegpio\tools\find_board.py              # 找板子, 打印 IP
python D:\esp\onegpio\tools\find_board.py --first      # 只打印 IP (给脚本用)
python D:\esp\onegpio\tools\find_board.py --beacon-only # 只用广播, 不扫网段
python D:\esp\onegpio\tools\find_board.py --no-beacon   # 只用扫网段
```

## 本机（PC）IP 变了也不用重启服务

板子换 IP 有自动发现兜着（上一节），**PC 自己换 IP** 以前是个暗坑：
上游的 Banyan backplane 会把"启动那一刻的本机局域网 IP"绑死（连 8.8.8.8 得到
本机地址，再在这个地址上监听 43124/43125），`wsgw` / `esp32gw` 也各自解析一次
同一个地址去连它。于是 PC 换网络 / DHCP 续租 / 路由器重启导致本机 IP 变化以后：

* 老 backplane 还绑在旧地址上（那地址甚至可能已被板子等设备占用），谁都连不上它；
* ZMQ 的 `connect` 不会报错，消息只是石沉大海，日志里一切正常，
  表现就是"点积木没反应"，只能手工重启整套服务。

`tools\apply_local_patches.py` 的**补丁 5** 把它彻底修掉了：

| 组件 | 改前 | 改后 |
| --- | --- | --- |
| backplane | 只绑"启动那一刻的本机 IP" | 绑 `0.0.0.0`（所有网卡都能连） |
| wsgw / esp32gw | 连本机当前 IP:43124/43125 | 一律连 `127.0.0.1`（回环地址不会变） |
| banyan_publish.py | 同上 | 同上 |

回环地址与"本机 IP 是多少"完全无关，所以**本机 IP 再怎么变都不用重启**。
另外守护进程加了一层保险：万一 backplane 还绑在旧地址上（比如补丁被 pip 升级
冲掉了），它会自己发现"监听的地址已经不是本机地址"并自动重启 backplane 和两个
网关，几秒内恢复，不需要人工介入。

验证方式：

```powershell
# backplane 应该监听 0.0.0.0 (不再是某个具体的内网 IP)
Get-NetTCPConnection -State Listen | Where-Object LocalPort -in 43124,43125,9007 |
    Select-Object LocalAddress,LocalPort
```

## 已实现的积木 / 命令

| Scratch 积木 | 板子上的动作 | 说明 |
| --- | --- | --- |
| IP 地址 | 建立 TCP 连接 | 网关收到后连接 `IP:31336` |
| 数字输出 | `digital_write` | GPIO 推挽输出 |
| PWM 输出 | `set_mode_pwm` + `pwm_write` | 5 kHz / 8 bit，0~100% 对应 0~255 |
| 舵机 | `servo_attach` + `servo_write` | 50 Hz，脉宽 544~2400 us，0~180° |
| 数字输入 | `set_mode_digital_input` | 变化即上报 |
| 模拟输入 | `set_mode_analog_input` | 12 bit 原始值 0~4095 |
| 超声波 | `set_mode_sonar` | HC-SR04，返回厘米 |
| 屏幕背光 | `lcd_backlight` (0x70) | 积木给 开=100 / 关=0；固件本身支持 1~100 调亮度 |
| 屏幕颜色 | `lcd_color` (0x71) | 整屏填充，积木把 `#rrggbb` 转成 R/G/B 三个字节 |
| 播放音调 | `audio_tone` (0x72) | ES8311 DAC → PA → 喇叭；频率 / 时长 / 音量三个参数 |
| 停止播放音频 | `audio_stop` (0x73) | 立刻停音（只静音 DAC，不动 codec 时钟）|
| 麦克风响度 | `audio_mic` (0x74) + 上报 0x0D | 读麦克风 0~100，需要先用「麦克风检测 开」打开 |
| 朗读文字 | `audio_tts` (0x75) | 板子本地做中文语音合成（esp-tts），文字由网关按 UTF-8 拆包 |
| 停止朗读 | `audio_tts_stop` (0x76) | 立刻停止朗读 |

另外固件还实现了协议层的 I2C 读写、输入上报开关、模拟扫描间隔、固件版本查询、
回环测试和复位命令（这些是 s3-extend / telemetrix 客户端会用到或便于调试的命令）。

摄像头目前只开放在**协议层**（还没有 Scratch 积木，见
[docs/camera-ov2640.md](docs/camera-ov2640.md) 最后一节）：

| 协议命令 | 板子上的动作 | 说明 |
| --- | --- | --- |
| `camera_config` (0x78) | 在线改分辨率 / JPEG 质量 | 不用重新编译, 拍之前发一条即可 |
| `camera_snapshot` (0x79) | 拍 N 帧或连续拍 | 每帧拆成 0x0F 分片回传, 帧头是 0x10 |
| `camera_stop` (0x7A) | 停止拍照 | 丢掉正在发的那一帧 |
| `camera_info` (0x7B) | 回一条 0x12 | 状态 / 分辨率 / 质量 / XCLK |

PC 侧一条命令就能拍:

```powershell
D:\esp\onegpio\tools\stop_s3extend.ps1     # 板子同时只服务一个客户端
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --view
```

屏幕、音频和朗读这几块积木走的是自定义命令（0x70 ~ 0x77，官方 Telemetrix 协议没有），
需要 PC 端网关认识它们：网关侧由 `python tools\apply_local_patches.py` 的
**补丁 6（屏幕）/ 补丁 7（音频）/ 补丁 8（朗读）** 写入，链路是
Scratch → wsgw/backplane → esp32gw → 板子。如果升级过 s3-extend，记得重跑一次
补丁脚本并重启网关，否则只有这几块积木没反应，其它积木照常工作。

触摸、DHT、SPI、OneWire、步进电机这些原版 Arduino 固件里有、但 Scratch 的
ESP32 扩展没有开放的命令，本工程暂未实现（收到时会打印一条告警日志，不会崩）。

## 引脚限制（重要）

ESP32-S3 与经典 ESP32 的引脚不同，Scratch 扩展的引脚下拉框是经典 ESP32 的列表，
因此：

* 数字/PWM/舵机实际可用的引脚（板载外设都打开时）：**GPIO10、GPIO11**
  （Scratch 扩展的引脚表由 `tools\apply_local_patches.py` 补上这两个脚，
  默认就会补），以及 **GPIO43/44**（UART0 的 TX/RX；本工程日志走原生 USB，
  所以这两脚空着，只是扩展的下拉框里没有）；
  GPIO2 是 I2C SCL（接了音频/摄像头之后别当普通 IO 用）
* 其它经典引脚各有归属：**21** 屏幕 CS、**39/40/41/42** 屏幕与背光、
  **12/13/14/38/45/47** 音频、**3~9/15~18/46/48** 摄像头（见下面各节）
* **GPIO22 / 23 / 25**：ESP32-S3 上不存在，选了不会有任何反应
* **GPIO26 ~ 37**：模组内部接 Flash / PSRAM，默认拒绝使用（可在 menuconfig 中放开）
* **GPIO19 / 20**：被原生 USB（调试串口）占用，固件默认拒绝当普通 IO 使用
* **GPIO39 / 40 / 41 / 42**：板载 ILI9342C 屏幕（DC / MOSI / SCK / 背光），归屏幕使用
* **GPIO12 / 13 / 14 / 38 / 45 / 47**：板载 ES8311 音频（I2S + PA 使能），
  归音频使用；**GPIO1 / 2** 是音频 codec 的 I2C 总线（SDA / SCL），
  与 Scratch 的 I2C 积木共用
* **GPIO3~9 / 15~18 / 48**：板载 OV2640 摄像头（DVP 数据/同步线 + XCLK + PWDN），
  归摄像头使用；关掉 `TMX_CAMERA_ENABLE` 即释放（见下一节）
* 模拟输入：Scratch 里只能选 32/33/34/35/36/39，固件会自动映射到
  S3 的 ADC1（GPIO1~GPIO6），上报时仍使用原引脚号
  （用了音频/I2C 之后 GPIO1/GPIO2 被占用，模拟引脚 32/33 不可用，请改用 34/35/36/39；
  **开了摄像头之后 GPIO3~GPIO6 也被占，模拟输入就没有可用引脚了**）

详见 [docs/pins-esp32s3.md](docs/pins-esp32s3.md)。

## 板载 ILI9342C SPI 屏幕

固件启动时会初始化板载的 ILI9342C（320x240 横屏）。ILI9342C 的寄存器跟
ILI9341 基本兼容，所以复用 esp_lcd 的 ILI9341 驱动 + 默认初始化表，
只有方向（MADCTL）要按 9342C 来设。默认接线：

| 屏幕引脚 | GPIO | 说明 |
| --- | --- | --- |
| SCK / CLK | 41 | SPI 时钟 |
| MOSI / DIN | 40 | SPI 数据（只写不读，MISO 未接） |
| DC / RS | 39 | 命令/数据选择 |
| CS | 21 | 片选 |
| BLK | 42 | 背光（PWM 调亮度，默认 80%）|
| RESET | — | 未接，用软件复位 |

* 所有引脚、方向、颜色顺序都在 `menuconfig` → `LCD (ILI9342C, SPI 屏幕)` 里，
  不用改代码；不用屏幕就把 `TMX_LCD_ENABLE` 关掉，该模块完全不占资源；
* **ILI9342C 的显存是 320(列) x 240(行)**，320x240 就是它的原生方向，所以
  `TMX_LCD_SWAP_XY` 保持 n；换成 ILI9341（显存 240x320）那种屏时才要打开它，
  两边搞反了画面就是错位的斜条纹；
* 方向已实测确认（本板）：`swap=0`、`mirror_x=1`、`mirror_y=1`，也就是默认值，
  上电后画面左上角红、右上绿、左下蓝、右下黄，方向和颜色都对；
* 换屏或对不上时，把 `TMX_LCD_ORIENT_TEST` 打开，上电后会每 2.5 秒
  换一种 swap/mirror 组合轮播 5 张图，每张左上角画 N 个白方块表示"第 N 张"，
  画面正常时左上角红、右上绿、左下蓝、右下黄。记下哪一张正常，把对应的
  swap/mirror 填进配置，然后把 `TMX_LCD_ORIENT_TEST` 关掉即可；
* 显示相关的典型症状：画面像底片 → 改 `TMX_LCD_INVERT_COLOR`；
  红蓝互换 → 改 `TMX_LCD_BGR`；上下颠倒 → 两个 mirror 一起取反；
  杜邦线长、有雪花点 → 把 `TMX_LCD_SPI_CLOCK_HZ` 从 40MHz 降到 10~20MHz；
* 屏幕占用的引脚会自动从 Scratch 的引脚池里排除，背光 PWM 还会占掉
  LEDC 的 1 个通道 + 1 个定时器（PWM 从 3 组频率变 2 组、通道从 8 路变 7 路），
  关掉 `TMX_LCD_BACKLIGHT_PWM` 可以把这个资源还回来。

## 板载 ES8311 音频输入 / 输出

板子上的 ES8311 codec 走 **I2S 全双工**（48 kHz / 16 bit / 立体声）+ **I2C 配置**，
功放由 GPIO 使能。默认接线（都在 `menuconfig` → `音频 (ES8311 Codec)` 里）：

| 音频引脚 | GPIO | 说明 |
| --- | --- | --- |
| I2S MCLK / BCLK / WS | 38 / 14 / 13 | 时钟（ESP32-S3 做主机，MCLK = 采样率 × 256）|
| I2S DOUT | 45 | 板子 → codec（放音）|
| I2S DIN | 12 | codec → 板子（录音）|
| PA 使能 | 47 | 高电平开，增益 6 dB |
| I2C SDA / SCL | 1 / 2 | codec 控制总线（地址 0x30，与 Scratch 的 I2C 积木共用）|

固件启动时会打一行 `ES8311 ready: I2S0 48000Hz/16bit/2ch ...`；
如果 codec 没应答会打 `ES8311 没有应答 (I2C 地址 0x30)`，此时只有音频积木不可用，
其它功能照常。对应的板级描述（`audio_dac` / `audio_adc` 两个 `audio_codec` 设备）
和每个字段落到哪个 Kconfig 选项，见 [docs/audio-es8311.md](docs/audio-es8311.md)。

> 只做“音调输出 + 麦克风响度”，不做 PCM 音频流：Telemetrix 单包最长 255 字节，
> 中间还要过 Banyan/ZMQ/WebSocket，实时传 48kHz 立体声（约 192 KB/s）不现实。
> PC 上想放整段音乐，用 Scratch 自己的声音积木即可。

## 板载 OV2640 摄像头（DVP 并口，拍照）

摄像头是 **DVP 并口**的 OV2640，驱动用 Espressif 的 esp32-camera 组件。
控制口 (SCCB) **和 ES8311 共用板载 I2C 总线**（SDA=GPIO1 / SCL=GPIO2，地址 0x30 不冲突），
XCLK 由 LEDC 输出 20MHz。默认接线（都在 `menuconfig` → `摄像头 (OV2640 DVP)` 里）：

| 摄像头信号 | GPIO | 说明 |
| --- | --- | --- |
| SIOD / SIOC (SCCB) | 1 / 2 | 控制口，与音频 codec 共用 I2C |
| VSYNC / HREF | 3 / 46 | 同步信号（Strapping 脚当输入用没问题）|
| XCLK | 8 | LEDC 输出 20MHz |
| PCLK | 16 | 像素时钟 |
| D0 ~ D7 | 7 / 5 / 4 / 6 / 15 / 17 / 18 / 9 | 8 位数据 |
| PWDN | 48 | 低电平工作（不用可以填 -1）|
| RESET | — | 未接，配置里填 -1 |

固件不做 HTTP 图传，而是把 JPEG 按 240 字节分片通过协议发给 PC：

```
PC: 0x79 拍照 ─► 板子取帧 ─► 0x10 (帧头) + 0x0F ×N (分片) ─► PC 拼成 .jpg
```

一条命令验货（跑之前先 `tools\stop_s3extend.ps1` 让出板子）：

```powershell
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --view
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --stream 5   # 连拍 5 秒存帧
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.103 --scan       # 扫 I2C: 0x30 是摄像头
```

开摄像头要付出三样代价，换板子时心里有数：

* **13 个 GPIO 归它**（3~9 / 15~18 / 48），Scratch 里选这些脚会被拒绝；
* **模拟输入没了**：Scratch 的模拟引脚 32~39 对应 GPIO1~GPIO6，全被摄像头/I2C 占掉；
* **约 92KB 内部 RAM**（VGA 帧缓冲 61KB + DMA 缓冲 32KB，本板没开 PSRAM），
  内存不够就把分辨率降到 QVGA 或把 `TMX_CAMERA_FB_COUNT` 保持 1。

接线、协议字段、帧率说明和排障见 [docs/camera-ov2640.md](docs/camera-ov2640.md)。

## 板载中文语音合成（朗读文字）

「朗读文字 [ ]」积木把文字发到板子，板子用 Espressif 的 **esp-tts**（esp-sr 组件里的
中文小乐音色）**本地**合成语音，从 ES8311 喇叭念出来 —— 不联网、不上云。

```
Scratch「朗读文字」 ─► Banyan ─► esp32gw ─► 0x75 (UTF-8 拆包) ─► 板子
                                                                  └► esp-tts 16kHz ─► 升采样 ─► I2S ─► 喇叭
```

因为音色数据有 **2.9MB**，flash 从 2MB 换成了 **16MB**，用自定义分区表
[firmware/partitions_16m.csv](firmware/partitions_16m.csv)：

| 分区 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- |
| factory (app) | 0x10000 | 4M | 固件（现在约 978KB）|
| voice_data | 0x410000 | 3M | TTS 音色数据（运行时 mmap，不占 RAM）|

音色数据已经挂到 `idf.py flash` 上（`esptool_py_flash_to_partition`），
所以还是一条命令烧全：

```powershell
D:\esp\onegpio\tools\idf.ps1 -Port COM5 flash monitor
```

开机日志里会打印 `tmx_tts: esp-tts ready: 音色数据 3145728 字节 (分区 voice_data) ...`。
细节、自检工具和排障见 [docs/tts-esp-tts.md](docs/tts-esp-tts.md)；
不经过 Scratch 想听板子念一段：

```powershell
D:\esp\onegpio\tools\stop_s3extend.ps1
python D:\esp\onegpio\tools\pc_tts_check.py 192.168.0.103 "你好，我是小乐" --play
```

不打开 Scratch 也能先验证音频硬件（跑之前先 `tools\stop_s3extend.ps1` 让出板子）：

```powershell
python D:\esp\onegpio\tools\pc_tcp_check.py 192.168.1.123 --tone 1000 --tone-ms 800
python D:\esp\onegpio\tools\pc_tcp_check.py 192.168.1.123 --mic 5
python D:\esp\onegpio\tools\pc_audio_loopback_check.py 192.168.1.123   # 放音调+收麦克风, 自听回环
```

改完扩展/网关的代码想快速确认没写错（不需要板子）：

```powershell
node   D:\esp\onegpio\tools\test_esp32s3_audio.js     # 积木发出去的报文
python D:\esp\onegpio\tools\test_gateway_audio.py     # 网关翻成的协议字节
```

## 调试串口（USB Serial/JTAG）

默认配置：

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y      # 日志走板子原生 USB 口
CONFIG_ESP_CONSOLE_UART_DEFAULT           # (未启用) 日志走 UART0 / GPIO43,44
```

* 插上板子的 USB 口，Windows 10/11 会直接识别成 `USB 串行设备 (COMx)`，无需额外驱动；
* `idf.py -p COMx monitor` 看日志，`Ctrl+]` 退出；
* 该模式下 **GPIO19/20 归 USB 使用**，固件会把这两脚当保留引脚拒绝；
* 想换回 UART0（例如要和板载 USB 转串口芯片配合），只要在 `menuconfig` →
  `Component config` → `Channel for console output` 里选 `UART0`，
  或者把 `sdkconfig.defaults` 里那两行注释互换后重新编译。

## 重启 PC 后，点积木自动拉起服务（按需模式，推荐）

不想让三个 Python 进程整天挂着？把登录自启换成**按需模式**：

```powershell
D:\esp\onegpio\tools\install_autostart.ps1 -OnDemand
```

> 想照着步骤从头装一遍（含前置检查、验证、卸载、排错），看
> [docs/service-install.md](docs/service-install.md)。

登录后只常驻一个十几 MB 的**微型启动器**（`tools\start_launcher.ps1` →
`tools\onegpio_launcher.py`，监听 `127.0.0.1:8000`，只用 Python 标准库，
不占 9007 / 43124 / 43125）。真正重的三件套等你**点积木**时才起来：

```
Scratch 里点积木 ──► 扩展发现 9007 连不上
                        │ 0.7 秒后 POST http://127.0.0.1:8000/start
                        ▼
                    启动器 ──► start_s3extend.ps1 -Background (守护进程 + 三件套)
                        │
                        ▼ 约 15 秒
                    扩展自动重连 ws://127.0.0.1:9007, 排队等着的积木指令自动补发
```

* **为什么必须有这个启动器**：Scratch / TurboWarp 的扩展跑在浏览器沙箱里，
  只能发 WebSocket / HTTP，**起不了本机进程**；所以需要一个本机小进程接住
  "叫醒服务"的请求。这就是它存在的唯一理由，也是它保持极小的原因。
* **触发点**：任意用到服务的积木（数字输出 / PWM / 舵机 / 读引脚 / 超声波 /
  连接板子 IP），外加新加的两块积木「启动本地服务」「本地服务状态」。
* **服务本来就在跑时不打扰**：点完积木 0.7 秒内 WebSocket 连上了，就不发请求；
  连点积木有 8 秒冷却，同时最多只有一次拉起在路上。
* **启动器不在时会说清楚**：状态积木显示
  `本地服务未启动，启动器也没在运行（先跑一次 tools\start_launcher.ps1）`。

配套命令：

| 命令 | 作用 |
| --- | --- |
| `tools\start_launcher.ps1` | 启动启动器（后台隐藏，重复执行会自动跳过） |
| `tools\start_launcher.ps1 -Check` | 看启动器 / 服务 / 登录自启三者的状态 |
| `tools\start_launcher.ps1 -Foreground` | 前台运行，日志直接打在窗口里 |
| `tools\start_launcher.ps1 -Stop` | 停启动器（不影响正在跑的 s3-extend） |
| `tools\start_launcher.ps1 -AllowOrigin https://my.editor` | 额外放行一个编辑器来源（默认已放行 TurboWarp 在线版/桌面版、Scratch 在线版、PenguinMod、Adacraft 和本机来源，其它来源会收到 403 并记进日志） |
| `tools\stop_s3extend.ps1` | 停三件套（启动器留着，点积木还能再拉起来） |
| `http://127.0.0.1:8000/` | 状态页，带「启动服务 / 停止服务」按钮 |
| `http://127.0.0.1:8000/esp32s3.js` | 顺带托管的扩展脚本，可直接当 TurboWarp 的扩展 URL |
| `/start` `/stop` `/restart` `/status` `/logs` | 给扩展和脚本用的接口 |

验证（会先停掉服务，再靠"点积木"把它拉起来）：

```powershell
node D:\esp\onegpio\tools\test_autostart_live.js --restart
```

```
✓ 启动器在线: PID 3240, 端口 8000
场景: 服务没在跑 -> 点积木应当自动拉起
点「连接板子 IP 192.168.0.104」...
  14.8s: 9007 已监听, 状态: 正在连接 192.168.0.104 …
✓ 服务被积木自动拉起 (14.8s)
扩展「连接状态」: 已连接板子 192.168.0.104
✓ 板子也连上了 (Scratch -> 网关 -> 板子 全链路通)
```

想回到"登录就把三件套全起起来"的老模式：跑 `tools\install_autostart.ps1`
（不带 `-OnDemand`，两种模式互斥，会自动摘掉另一种的登录项）。

## 开机自动后台运行 s3-extend（全量模式）

一键安装（不需要管理员，装到"启动文件夹"）：

```powershell
D:\esp\onegpio\tools\install_autostart.ps1
```

装完之后，每次登录 Windows 都会在后台自动拉起 s3-extend（不弹窗）：

| 项目 | 说明 |
| --- | --- |
| 启动方式 | 计划任务 `s3-extend ESP32 server`（需管理员）；建不了就自动退回启动文件夹里的 `s3extend_autostart.vbs` |
| 实际动作 | `start_s3extend.ps1 -Background` → 拉起守护进程 |
| 日志 | `%LOCALAPPDATA%\s3extend\logs\`（supervisor / backplane / wsgw / esp32gw 各一份） |
| 查看状态 | `D:\esp\onegpio\tools\start_s3extend.ps1 -Check` |
| 停止 | `D:\esp\onegpio\tools\stop_s3extend.ps1` |
| 卸载自启 | `D:\esp\onegpio\tools\install_autostart.ps1 -Remove` |

两个已知坑，脚本都处理了：

1. **必须是"登录时"而不是"开机时"**：s3-extend 装在当前用户的 `%APPDATA%` 下，
   以 SYSTEM 身份在开机阶段运行会找不到 `backplane` / `wsgw` / `esp32gw`。
2. **板子没上电时网关会退出**：`esp32gw` 连不上板子就自己退出，所以脚本带了一个
   守护进程，每 1 秒检查一次，掉线就重新拉起（实测杀掉后 4 秒内自动恢复：
   1 秒发现 + 3 秒等网关订阅上主题 + 发 IP 连板子）。

## 配置项 (menuconfig)
## 配置项 (menuconfig)

菜单：`ESP32-S3 Scratch 服务器配置`

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `TMX_WIFI_SSID` / `TMX_WIFI_PASSWORD` | 占位值 | 要连接的 WiFi |
| `TMX_WIFI_MAX_RETRY` | 8 | 失败重试次数，0 = 一直重试 |
| `TMX_USE_STATIC_IP` | n | 静态 IP（含网关/掩码） |
| `TMX_ENABLE_SOFTAP_FALLBACK` | y | 连不上路由器时自动开热点 `ESP32S3-Scratch` |
| `TMX_TCP_PORT` | 31336 | 必须与 PC 端客户端一致 |
| `TMX_I2C_SDA_PIN` / `TMX_I2C_SCL_PIN` | 1 / 2 | 板载 I2C 总线（ES8311 codec，地址 0x30）|
| `TMX_ADC_LEGACY_PIN_ALIAS` | y | 32~39 → ADC1 的引脚别名 |
| `TMX_ALLOW_RESERVED_PINS` | n | 允许使用 26~37 / 0 / 45 / 46 / 19 / 20 |
| `TMX_LCD_ENABLE` | y | 初始化板载 ILI9342C 屏幕（320x240 横屏）|
| `TMX_LCD_SPI_SCK_PIN` 等 | 41/40/39/21 | 屏幕 SCK / MOSI / DC / CS 引脚 |
| `TMX_LCD_SWAP_XY` / `MIRROR_X` / `MIRROR_Y` | n / y / y | 方向：9342C 显存 320x240，横屏不需要 swap |
| `TMX_LCD_BGR` / `TMX_LCD_INVERT_COLOR` | y / y | 颜色顺序与反相，颜色不对时改这两个 |
| `TMX_LCD_BACKLIGHT_PIN` / `_PERCENT` | 42 / 80 | 背光引脚与开机亮度 |
| `TMX_LCD_BACKLIGHT_PWM` | y | 背光用 PWM 调亮度（占 1 个 LEDC 通道）|
| `TMX_LCD_TEST_PATTERN` | y | 启动时显示方向测试图（4 象限 + 白方块）|
| `TMX_LCD_ORIENT_TEST` | n | 排障用：启动时轮播 5 种 swap/mirror 做对比 |
| `TMX_AUDIO_ENABLE` | y | 初始化板载 ES8311 音频（I2S 全双工 + PA）|
| `TMX_AUDIO_I2S_*_PIN` | 38/14/13/45/12 | MCLK / BCLK / WS / DOUT / DIN 引脚 |
| `TMX_AUDIO_PA_PIN` / `_ACTIVE_LEVEL` / `_GAIN_DB` | 47 / 1 / 6 | 功放使能脚、有效电平、增益 |
| `TMX_AUDIO_SAMPLE_RATE` / `_MCLK_MULTIPLE` | 48000 / 256 | 采样率与 MCLK 倍数（codec 从机）|
| `TMX_AUDIO_VOLUME` / `_MIC_GAIN_DB` | 80 / 30 | 默认音量 % 与麦克风增益 |
| `TMX_AUDIO_ADC_DAC_REF` | y | 录音带一路 DAC 回采（ADCL + DACR）|
| `TMX_AUDIO_MIC_CHANNEL` / `_REPORT_MS` | 0 / 100 | 麦克风在哪一路、响度上报间隔 |
| `TMX_TTS_ENABLE` | y | 启用中文语音合成（esp-tts，占 voice_data 分区 3MB）|
| `TMX_TTS_SPEED` / `_VOLUME` | 4 / 90 | 朗读语速（0~5）与音量 % |
| `TMX_TTS_TEXT_MAX` | 512 | 单次朗读文字上限（字节，UTF-8）|
| `TMX_TTS_PARTITION` | voice_data | 音色数据所在分区名 |
| `TMX_DEBUG_REPORTS` | n | 把协议调试信息作为 DEBUG_PRINT 上报 |
| `TMX_CAMERA_ENABLE` | y | 启用板载 OV2640 摄像头（DVP + SCCB 共用 I2C）|
| `TMX_CAMERA_XCLK_PIN` / `_PCLK_PIN` / `_VSYNC_PIN` / `_HREF_PIN` | 8 / 16 / 3 / 46 | DVP 时钟与同步线 |
| `TMX_CAMERA_D0_PIN` ~ `_D7_PIN` | 7/5/4/6/15/17/18/9 | 8 位数据线 |
| `TMX_CAMERA_PWDN_PIN` / `_RESET_PIN` | 48 / -1 | 电源控制脚，没接填 -1 |
| `TMX_CAMERA_SIOD_PIN` / `_SIOC_PIN` | 1 / 2 | SCCB，直接复用板载 I2C 总线 |
| `TMX_CAMERA_XCLK_FREQ_HZ` | 20000000 | XCLK 频率，花屏就降到 10~16MHz |
| `TMX_CAMERA_XCLK_LEDC_TIMER` / `_CHANNEL` | 1 / 6 | XCLK 占用的 LEDC 资源 |
| `TMX_CAMERA_FRAMESIZE_*` | VGA | 默认分辨率（协议里可在线改）|
| `TMX_CAMERA_JPEG_QUALITY` | 12 | 0~63，越小越清晰 |
| `TMX_CAMERA_FB_COUNT` | 1 | 帧缓冲份数（内存够就改 2）|
| `TMX_CAMERA_STREAM_INTERVAL_MS` | 200 | 连拍默认间隔（约 5fps）|

## 版本与来源

* 框架：ESP-IDF **v5.5.4**（本机路径 `D:\esp\v5.5.4\v5.5.4\esp-idf`）
* 协议来源：`MrYsLab/Telemetrix4Esp32`（Arduino 版 WiFi 服务器）+
  `MrYsLab/telemetrix-esp32`（Python 客户端）
* 组件：`espressif/esp_lcd_ili9341`、`espressif/esp_codec_dev`、`espressif/esp-sr`（esp-tts）、
  `espressif/esp32-camera`（OV2640 DVP，含 `esp_jpeg`）
* 固件版本号上报为 `3.2.0`，与官方 Arduino 固件一致，便于 PC 端兼容
* 本工程为独立实现（未拷贝 Arduino 代码），遵循与原项目相同的 AGPL-3.0 授权

## 编译状态

在本机用 ESP-IDF v5.5.4 (xtensa-esp-elf 14.2.0) 编译通过：

```
esp32s3_scratch_server.bin  1005824 字节 (0xf5900)
app 分区 4MB (0x400000), 剩余 0x30a700 字节 (76%)
voice_data 分区 3MB: esp_tts_voice_data_xiaole.dat (2938039 字节)
```

flash 用 16MB + 自定义分区表 `firmware/partitions_16m.csv`（语音合成的音色数据放在
`voice_data` 分区，`idf.py flash` 会自动一起烧）。

已在实物板卡上烧录验证（COM15 / ESP32-S3，2026-09-18）：

* 开机日志 `ES8311 ready: I2S0 48000Hz/16bit/2ch (MCLK=38 BCLK=14 WS=13 DIN=12 DOUT=45), PA=47(active 1), I2C addr=0x30`
* 音调输出、麦克风采集、麦克风响度上报都实测通过；
  `pc_audio_loopback_check.py` 里本底响度 ~20、放 1kHz 音调时升到 ~97（麦克风拾到了喇叭）
* 网关链路（Scratch→backplane→esp32gw→板子，以及板子→网关→Banyan）用
  `banyan_publish.py` + 订阅 `from_esp32_gateway` 实测通过
* 中文语音合成（TTS）实测通过：`pc_tts_check.py` 让板子念
  “你好，我是小乐，很高兴认识你”→ 回传 113184 字节 / 3.54 秒 PCM（丢包 0）；
  走 Scratch 通道（Banyan → esp32gw → 0x75）也实测能念
