# ESP32-S3 引脚可用性（配合 Scratch 的 OneGPIO ESP32 扩展）

Scratch 扩展的引脚下拉框是照 **经典 ESP32** 写的，直接搬到 ESP32-S3 会有几处不匹配，
本固件已按 S3 的实际情况处理。

## 数字输出 / PWM / 舵机

扩展里可选的引脚：`2 4 5 12 13 14 16 17 18 19 21 22 23 25 26 27 32 33`

| 引脚 | 在 ESP32-S3 上 | 本固件行为 |
| --- | --- | --- |
| 2 | I2C SCL（板载总线） | 接了音频/摄像头之后别当普通 IO 用 |
| 4, 5, 16, 17, 18 | 被板载 OV2640 摄像头占用 | 拒绝（关 `TMX_CAMERA_ENABLE` 释放）|
| 12, 13, 14 | 被板载 ES8311 音频占用 | 拒绝（关 `TMX_AUDIO_ENABLE` 释放）|
| 21 | 板载 ILI9341 屏幕的 CS | 归屏幕使用, 不会分给积木 |
| 19 | 原生 USB 的 D-，默认被调试串口占用 | 默认拒绝 |
| 22, 23, 25 | **S3 上没有这些 GPIO** | 拒绝并打印告警 |
| 26, 27, 32, 33 | 模组内部 SPI0/1（Flash / PSRAM） | 默认拒绝 |

> 板载外设全开时，Scratch 的数字/PWM/舵机实际上只剩 **GPIO10 / GPIO11**
> （扩展的引脚表由 `tools\apply_local_patches.py` 默认补上这两个脚），
> 以及没进下拉框的 **GPIO43 / GPIO44**（UART0，本工程日志走原生 USB）。

> GPIO19/20 是原生 USB 的 D-/D+。本工程默认把**调试串口放在原生 USB 口**
> （`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），因此这两脚归 USB PHY 使用，
> 固件把它们当作保留引脚拒绝。想拿它们当普通 IO，就把控制台改回
> `UART0`（`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`，日志从 GPIO43/44 输出），
> 这时 19/20 会重新变成可用引脚。

## 模拟输入

扩展里可选的引脚：`32 33 34 35 36 39`（经典 ESP32 的 ADC1 引脚）

ESP32-S3 的 ADC1 是 **GPIO1 ~ GPIO10**，经典引脚号在 S3 上没有 ADC 能力。
固件内置了别名映射（可在 menuconfig 里关闭 `TMX_ADC_LEGACY_PIN_ALIAS`）：

| Scratch 选的引脚 | 实际读取的 GPIO | ADC1 通道 |
| --- | --- | --- |
| 32 | GPIO1 | CH0 |
| 33 | GPIO2 | CH1 |
| 34 | GPIO3 | CH2 |
| 35 | GPIO4 | CH3 |
| 36 | GPIO5 | CH4 |
| 39 | GPIO6 | CH5 |

上报数据里的引脚号仍是 32~39，所以 Scratch 积木完全无感。
如果你愿意改 Scratch 扩展的引脚下拉框，也可以直接填 1~10 使用原生引脚。

⚠️ 固件会拒绝"被板载外设占用"的模拟引脚（日志里会打印一条告警）：
GPIO1/GPIO2 是 I2C 总线，GPIO3~GPIO6 开了摄像头之后归 DVP 用 ——
所以**摄像头打开时 32~39 六个模拟引脚全部不可用**，要用模拟输入就先
`TMX_CAMERA_ENABLE=n`。

读数与 Arduino ESP32 Core 一致：12 bit 原始值 **0 ~ 4095**，
衰减 12 dB（约 0 ~ 3.1 V 量程）。

## 超声波 HC-SR04

* 触发脚 / 回波脚从下拉框里选（建议 4/5、12/13、16/17 等）
* 回波宽度换算：`距离(cm) = 高电平时间(us) / 58`
* 单次等待回波最多 25 ms，超时返回 0
* 多个超声波会以 33 ms 间隔轮流巡检（与官方固件一致）

## I2C

* 默认使用 GPIO1 (SDA) / GPIO2 (SCL)：本板的 I2C 总线上挂着 ES8311 音频 codec
  （地址 0x30），所以默认值跟着板子的实际接线走
* 可在 menuconfig 中修改，或在协议里通过 `I2C_BEGIN` 带上引脚号
* 需要外接上拉电阻（固件开了内部弱上拉，但高速/长线建议 4.7k 外部上拉）
* ⚠️ GPIO1/GPIO2 同时是 ADC1 的 CH0/CH1，也就是 Scratch 的模拟引脚 32/33。
  用 I2C / 音频之后这两个模拟引脚就不能再读，需要模拟输入请用 34/35/36/39

## 板载 ES8311 音频输入/输出

| 音频引脚 | GPIO | 说明 |
| --- | --- | --- |
| I2S MCLK | 38 | 采样率 × 256 = 12.288 MHz |
| I2S BCLK | 14 | 位时钟 |
| I2S WS | 13 | 声道选择 |
| I2S DOUT | 45 | 板子 → codec（放音）|
| I2S DIN | 12 | codec → 板子（录音）|
| PA 使能 | 47 | 高电平开，增益 6 dB |

这 6 个引脚同样会被当作板载外设占用（Scratch 里选它们会被拒绝）；
关掉 `TMX_AUDIO_ENABLE` 就把它们让回给 Scratch。
细节（音量、麦克风增益、声道选择、排障）见 [audio-es8311.md](audio-es8311.md)。

## 板载 ILI9342C SPI 屏幕

| 屏幕引脚 | GPIO | 说明 |
| --- | --- | --- |
| SCK / CLK | 41 | SPI 时钟 (SPI2_HOST, 40MHz) |
| MOSI / DIN | 40 | SPI 数据, 只写不读 |
| DC / RS | 39 | 命令/数据选择 |
| CS | 21 | 片选 |
| BLK | 42 | 背光, LEDC PWM 调亮度 |
| RESET | — | 未接, 用软件复位 |

固件启动时初始化屏幕并显示 4 个象限：**左上红、右上绿、左下蓝、右下黄**。
本板实测方向为 `swap=0 / mirror_x=1 / mirror_y=1`（默认值），就是正确的。

换屏或方向对不上时，打开 `TMX_LCD_ORIENT_TEST`：上电后会轮播 5 种
swap/mirror 组合，每种 2.5 秒，每张图左上角画 N 个白方块（= 第 N 张）：

* 红块在左上角 = 第 N 张就是正确方向，把它的 swap/mirror 填进配置；
* ILI9342C 显存是 320x240，正确方向下 `TMX_LCD_SWAP_XY` 应该是关的；
  如果只有第 5 张（`swap=1`）正常，说明这块屏其实是 ILI9341 类 240x320 的屏；
* 画面像照片底片 → `TMX_LCD_INVERT_COLOR` 取反；
* 红蓝互换 → `TMX_LCD_BGR` 取反；
* 上下颠倒 → `TMX_LCD_MIRROR_X` / `TMX_LCD_MIRROR_Y` 一起取反；
* 5 张全是雪花/噪点 → 不是方向问题，把 `TMX_LCD_SPI_CLOCK_HZ` 降到 10~20MHz
  （40MHz 在杜邦线上经常跑不稳）。

上面这些都在 `menuconfig` → `LCD (ILI9341, SPI 屏幕)` 里，不用改代码。
这 5 个引脚会被当作板载外设占用，Scratch 里选它们会被拒绝并打告警日志；
把 `TMX_LCD_ENABLE` 关掉（默认是打开的）就会把这些引脚让回给 Scratch。

> GPIO39~42 是 ESP32-S3 的 JTAG 引脚（MTCK/MTDO/MTDI/MTMS）。
> 本工程日志走原生 USB 的 USB-Serial-JTAG，所以这几脚可以当普通 IO 用；
> 如果要用外部 JTAG 调试器，就得给屏幕换引脚。

## 板载 OV2640 摄像头 (DVP)

| 摄像头信号 | GPIO | 说明 |
| --- | --- | --- |
| SIOD / SIOC (SCCB) | 1 / 2 | 控制口, 与 ES8311 共用板载 I2C 总线 |
| VSYNC / HREF | 3 / 46 | 同步信号 (两个都是 Strapping 脚, 当输入用没问题) |
| XCLK | 8 | LEDC 输出 20MHz |
| PCLK | 16 | 像素时钟 |
| D0 ~ D7 | 7 / 5 / 4 / 6 / 15 / 17 / 18 / 9 | 8 位数据 |
| PWDN | 48 | 低电平工作 |

这 13 个 GPIO (3~9 / 15~18 / 46 / 48) 会被当作板载外设占用, Scratch 里选它们
会被拒绝并打告警日志; 关掉 `TMX_CAMERA_ENABLE` 就还给 Scratch。
细节见 [camera-ov2640.md](camera-ov2640.md)。

⚠️ **模拟输入**: Scratch 的模拟引脚 32~39 映射到 GPIO1~GPIO6, 摄像头占掉了
GPIO3~GPIO6 (还有 GPIO1/GPIO2 是 I2C 总线), 所以开了摄像头之后
**模拟输入没有可用的引脚了**; 要用模拟输入就先关掉摄像头。

⚠️ **LEDC**: XCLK 占掉 1 个定时器 + 1 个通道 (默认定时器 1 / 通道 6),
于是普通 PWM 的"频率/分辨率组合"只剩 1 组 (Scratch 的 PWM 积木固定
5kHz/8bit, 所以积木侧无感), PWM+舵机总路数从 7 路变 6 路。

## 保留引脚

默认拒绝使用：`0`（BOOT 按键/Strapping）、`19`/`20`（USB 调试串口占用）、
`26~37`（Flash/PSRAM）、`45`、`46`（Strapping），以及屏幕占用的
`21`/`39`/`40`/`41`/`42`（用 `TMX_LCD_ENABLE` 关闭屏幕即释放）、
音频占用的 `12`/`13`/`14`/`38`/`45`/`47`（用 `TMX_AUDIO_ENABLE` 关闭音频即释放）、
摄像头占用的 `3~9`/`15~18`/`48`（用 `TMX_CAMERA_ENABLE` 关闭摄像头即释放）。
如果确定模组没有使用这些脚（例如已经把控制台换回 UART0、或模组没接 PSRAM），
可在 menuconfig 打开 `TMX_ALLOW_RESERVED_PINS`。

## PWM 与舵机资源

ESP32-S3 的 LEDC 只有低速模式：**8 个通道 + 4 个定时器**。

* 定时器 0~2：普通 PWM，可同时存在 3 组不同的 (频率, 分辨率)；相同配置会共用定时器
* 定时器 3：舵机固定 50 Hz / 14 bit
* 通道池共用，所以 **PWM + 舵机最多 8 路**

屏幕背光打开 `TMX_LCD_BACKLIGHT_PWM`（默认）时会独占
`LEDC_TIMER_2` + 1 个通道（默认通道 7），于是普通 PWM 只剩 2 组频率、
PWM + 舵机最多 7 路。关掉它就只能开/关背光，但资源全部还给 Scratch。
