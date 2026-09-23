# OV2640 摄像头排障记录 (2026-09-20)

## 现状: 已经能出图 (2026-09-21)

先看文末的 [收尾](#收尾-20260921-摄像头已经能出图了) 一节 —— 那里是最终结论和改法。
中间几节是排查过程, 其中"D4/D5 没有信号"的说法**后来被推翻了**: 那是用 1ms 固定
窗口量出来的假象 (OV2640 的 JPEG 输出是一阵一阵的), 换成"数到足够多边沿才收工"
之后, 8 根数据线全都有数据。真正的病根是:

* AXP2101 的摄像头供电冷启动只有一路开 (SCCB 写得进但传感器内部没上电);
* XCLK 被降到 10MHz 时, 传感器生成的 JPEG 头里宽高是错的;
* JPEG 帧缓冲按 `w*h/5` 只有 61KB, 细节多时整帧判 `FB-OVF` 丢掉;
* esp32-camera 的 `cam_hal.c` 要求帧头必须在第 0 字节, 而本板每次连拍的头一两帧
  常常从半个帧开始 → 永远 `NO-SOI`, 驱动把每一帧都丢掉。

处理完这四条以后, VGA 640x480 每帧约 78KB, 拍的 `FF D8 ... FF D9` 完整。

## 是怎么量出来的

menuconfig -> `摄像头 (OV2640 DVP)` -> `TMX_CAMERA_PIN_PROBE` = y, 开机日志会打:

```
W tmx_camera: 探针 PCLK  (IO16):    1738 沿/1   ms =  1738000 Hz  不过滤  [被驱动为高]
W tmx_camera: 探针 VSYNC (IO3 ):       2 沿/200 ms =       10 Hz  滤波1us [被驱动为低]
W tmx_camera: 探针 HREF  (IO46):     850 沿/20  ms =    42500 Hz  滤波1us [悬空/没被驱动]
W tmx_camera: 探针 D0    (IO7 ):     129 沿/1   ms =   129000 Hz  不过滤  [悬空/没被驱动]
W tmx_camera: 探针 D1    (IO5 ):      54 沿/1   ms =    54000 Hz  不过滤  [被驱动为低]
W tmx_camera: 探针 D2    (IO4 ):     251 沿/1   ms =   251000 Hz  不过滤  [悬空/没被驱动]
W tmx_camera: 探针 D3    (IO6 ):      37 沿/1   ms =    37000 Hz  不过滤  [悬空/没被驱动]
W tmx_camera: 探针 D4    (IO15):       0 沿/1   ms =        0 Hz  不过滤  [被驱动为低]
W tmx_camera: 探针 D5    (IO17):       0 沿/1   ms =        0 Hz  不过滤  [被驱动为低]
W tmx_camera: 探针 D6    (IO18):     204 沿/1   ms =   204000 Hz  不过滤  [被驱动为低]
W tmx_camera: 探针 D7    (IO9 ):     106 沿/1   ms =   106000 Hz  不过滤  [被驱动为高]
W tmx_camera: === 全引脚扫描: 每根 1ms, 只列有活动的脚 ===
W tmx_camera: === 扫描结束 ===
```

怎么读这几行:

* 边沿数是 **硬件 PCNT 计数器**数的 (软件采样在 MHz 量级会混叠, 数字不可信)。
* 方括号里的是"内部上拉/下拉各测一次"的结果: 两次都读 0 = 这根脚被外部低阻钳死在低电平;
  两次都读 1 = 被钳死在高电平; 跟着电平走 = 悬空/没被驱动 (或者信号在跳)。
* **全引脚扫描**把板上没被外设占用的 GPIO (0/10/11/22/23/24/25) 全量了一遍, 一根都没信号 ——
  所以这两个 bit 不是"被走到别的 IO 上了" (那样改 Kconfig 就能救), 而是这两根线本身的问题:
  断线 / 短到地 / 模组或连接器上那两个脚虚焊。
* 摄像头断电 (AXP2101 两路 LDO 关掉) 再上电时探针照样会跑: 8 根数据线全变"被驱动为低"
  (没供电的 CMOS 输入被内部上拉经 ESD 二极管钳住), 可以用这一步区分问题在模组还是板子。

## 下一步 (要动硬件)

1. 重插 / 换一根摄像头排线, 检查连接器和模组这一侧的 `IO15_DVP_D4` / `IO17_DVP_D5`
   有没有虚焊、短路、连锡 (这两根在探针里是唯一 0 沿的)。
2. 想区分"板子短了"还是"模组/排线拉的": 把摄像头模组拔掉再上电。固件现在在摄像头
   初始化失败时**也会跑探针**, 所以日志里照样有那十几行:

   * IO15/IO17 还是"被驱动为低" -> 问题在板子这一侧;
   * 变成"悬空/没被驱动" -> 是模组/排线那侧把它们拉住的。
3. 有条件就换一块 OV2640 模组复测, 只看这两行即可判定。

## 供电 (AXP2101) —— 已修好

* 本板 OV2640 的 AVDD -> BLDO1、DVDD -> BLDO2, 由 AXP2101 (I2C `0x34`) 供电。
  芯片上电默认只开 BLDO1 (`0x90 = 0x55`), DVDD 那一路是关的。这时 SCCB 还能认出
  `0x30`, 但寄存器写会 NACK / 读回错值, 日志是:

  ```
  E sccb-ng: SCCB_Write Failed addr:0x30, reg:0x41, data:0x24, ret:259
  E camera: Camera probe failed with error 0xffffffff(ESP_FAIL)
  ```

  摄像头完全起不来 (冷启动后必现, 只有先用 PC 脚本把两路打开才行)。
* 固件现在会在初始化摄像头之前把 `0x90` 的 bit4/bit5 都置 1
  (Kconfig `TMX_CAMERA_PMIC_AXP2101`, 默认 y; **只动使能位, 不改电压**), 冷启动直接可用:

  ```
  I tmx_camera: PMIC AXP2101: LDO 使能 0x90 = 0x75 (BLDO1/AVDD=on, BLDO2/DVDD=on)
  I tmx_camera: OV2640 ready: VGA 640x480, JPEG 质量 12, XCLK 10MHz, fb=1, PID=0x26
  ```
* 手动看 / 改: `python tools\axp2101_power.py <板子IP>` (加 `--on` / `--off`,
  或 `--avdd 2800 --dvdd 1200`, 最后跟 `--reset` 让固件重新初始化摄像头)。
* 实测芯片默认 `BLDO1=1.8V / BLDO2=2.8V` 和 `2.8V / 1.2V` 两种组合都能出流 (PCLK 1.7MHz、
  VSYNC 10Hz)。固件不改电压, 具体用哪组请对着原理图确认。

## 常用自检命令

```powershell
python D:\esp\onegpio\tools\stop_s3extend.ps1          # 板子同时只服务一个客户端
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.106 --info   # 看状态
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.106          # 拍 1 帧
python D:\esp\onegpio\tools\axp2101_power.py 192.168.0.106            # 看 AXP2101 状态
python D:\esp\onegpio\tools\ov2640_regs.py 192.168.0.106              # 直接读传感器寄存器
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe tools\serial_tail.py COM14 --seconds 12 --filter "PMIC|OV2640|NO-SOI|探针|扫描"

## 引脚归属排查 (2026-09-20 晚): IO15/IO17 没被 UART / ADC / 32K 晶振占用

怀疑这两个 bit 是"被别的功能占了"时, 固件探针会先打三行只读的寄存器状态
(`PIN ... IOMUX=... / 矩阵 ... / RTC ...`)。ESP32-S3 上这三根脚的先天身份是:

| 脚 | IO MUX 寄存器名 | RTC pad |
| --- | --- | --- |
| IO15 (D4)  | `PERIPHS_IO_MUX_XTAL_32K_P_U` | `RTC_IO_XTAL_32P_PAD_REG` |
| IO16 (PCLK)| `PERIPHS_IO_MUX_XTAL_32K_N_U` | `RTC_IO_XTAL_32N_PAD_REG` |
| IO17 (D5)  | `PERIPHS_IO_MUX_DAC_1_U`      | `RTC_IO_PAD_DAC1_REG` |

实测 (摄像头拔掉、初始化失败时; 三根脚一模一样):

```
PIN D4   IO15 IOMUX=0x00001A02 MCU_SEL=1 IE=1 PU=0 PD=0
PIN D4   IO15 矩阵 out_sel=256(GPIO 输出寄存器) oen_sel=1 oen_inv=0 GPIO_OE=0
PIN D4   IO15 RTC XTAL_32K_P=0x40000000 mux=0 fun=0 ie=0 rue=0 rde=0
PIN PCLK IO16 ... (同上)
PIN D5   IO17 ... (同上)
```

逐条结论:

1. **不是 32.768kHz 晶振 (XTAL_32K_P)**: `RTC_IO_XTAL_32P_PAD` 的 `mux_sel = 0`
   → 这根 pad 处在"数字 GPIO"模式, 没有被 RTC 域接管; `fun_sel = 0`、`fun_ie = 0`、
   `rue = rde = 0` (RTC 侧的上下拉也全关)。固件里 `CONFIG_RTC_CLK_SRC_INT_RC=y`
   (RTC 时钟用内部 RC), 根本没启用外部 32K 晶振。
2. **不是 UART**: 工程里没有 `uart_set_pin`/`uart_driver_install`, 控制台走
   USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_UART_NUM=-1`)。寄存器层面: IO MUX
   `MCU_SEL = 1` 就是"普通 GPIO", 不是 `U0RTS`/`U1TXD` 那种固定功能;
   GPIO 矩阵 `out_sel = 256` 指向自己的 GPIO 输出寄存器 (不是 UART 信号),
   `GPIO_OE = 0` (输出关闭) —— 没有任何外设在驱动这两根脚。
3. **不是 ADC**: 模拟输入只用了 ADC1 的 GPIO1~6 (Scratch 模拟引脚); 真被配成
   ADC/模拟功能时 IO MUX 会是 `MCU_SEL = 0` + `FUN_IE = 0`, 实测是 1/1。
4. 三根脚 (IO15/IO16/IO17) 在"摄像头拔掉"的状态下寄存器完全一致 → 板子这一侧
   没有把它们接到任何别的功能上; 插上模组后只有 IO15/IO17 变成"被驱动为低",
   所以低电平是模组/排线那一侧带过来的 (传感器 D4/D5 输出被拉死, 或模组/排线
   在这两个位置接的是 GND)。

> 踩坑记录: 解字段只能用移位 `(v >> XX_S) & XX_V`。IDF 的 `REG_GET_FIELD(reg, F)`
> 是宏里带 `REG_READ(reg)` 的 —— 把"读回来的值"传进去, 它会拿这个值当地址去读内存,
> 直接就 Guru Meditation (LoadProhibited)。
```
## 收尾 (2026-09-21): 摄像头已经能出图了

**硬件没问题** —— 是"供电 + XCLK + 帧缓冲大小 + 驱动过严的帧头检查"几件事叠在一起,
表现成"永远 NO-SOI, 一帧也拿不到"。

修好的地方 (按重要性):

1. **供电**: AXP2101 的 BLDO1/BLDO2 冷启动只有一路开 → 固件现在开机自己开
   (`TMX_CAMERA_PMIC_AXP2101`, 见上一节)。
2. **XCLK 回 20MHz**: 调试时被降到 10MHz。10MHz 下拍出来的 JPEG, 头里的宽高是
   `641x480` / `897x480` 这种错值; 回到 20MHz 后 5 张全是 `640x480`。
3. **JPEG 帧缓冲**: 自动算法 `w*h/5` 只有 61440 字节, 画面细节多时整个帧超过它,
   驱动直接判 `FB-OVF` 丢帧 → 改用本地指定大小 100000 字节
   (`CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM=y` + `CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE=100000`),
   JPEG 质量默认从 12 调成 20 (VGA 帧约 78KB)。
4. **驱动"帧头必须在第 0 字节"的检查**: esp32-camera 的 `cam_hal.c` 只要在第一条 DMA
   数据里找不到 `FF D8 FF` 就丢帧并停止采集, 而本板每次开始连拍的**头一两帧**常常是
   从半个帧开始的 (VSYNC/HREF 与 JPEG 数据流的相位), 于是永远拿不到帧。
   本地补丁 (文件里标了 `local debug patch`): 改成只告警不丢帧, 由上层
   `tmx_camera.c` 的 `jpeg_align_frame()` 在整帧里找 `FF D8 FF` / `FF D9`,
   裁出干净的一段再发给 PC; 找不到就丢掉这一帧 (连续 3 帧对不齐才报错)。
   **注意: `managed_components/` 是组件管理器下载的, 重新拉组件会覆盖这个补丁, 要重打。**
5. PC 侧 `pc_camera_check.py`: 固件发的是 esp32-camera 的 `pixformat_t` (4 = JPEG),
   老代码按 3 = JPEG 判断, 会把好帧误报成"不是 JPEG" → 已改成 3/4 都认。

现在的用法 (板子同时只服务一个客户端, 先停掉网关):

```powershell
python D:\esp\onegpio\tools\stop_s3extend.ps1
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.106 --frames 5   # 存 cam_000..004.jpg
python D:\esp\onegpio\tools\pc_camera_check.py 192.168.0.106 --view       # 拍完直接打开
```

实测: VGA 640x480, ~78KB/帧, 5/5 帧都是完整的 `FF D8 ... FF D9`。

> 建议一次拍多帧 (`--frames 3` 以上): 头一两帧常常对不齐会被丢掉, 后面的就干净了。

## 噪点问题 (2026-09-21): XCLK 24MHz

画面"很多噪点"是 **XCLK 频率不对**造成的, 跟硬件/接线无关:

* esp32-camera 里 OV2640 的寄存器表 (`ov2640_settings_cif/jpeg3/...`) 是按
  **24MHz XCLK** 调的 (表头注释就写着 `// 30fps@24MHz`), 传感器内部的 PLL/ADC 时序
  都按这个来。给 20MHz 甚至 10MHz, 传感器照样出图, 但**像素级噪点巨大**, 而且
  采样偶尔出错 —— JPEG 头里的标记字节会被采成邻居值 (`FF C4`→`FF C5`,
  `FF C0`→`FF C1`, 都是最低位翻转 = D0/IO7 那根线上的采样错), 于是大多数帧
  PC 侧根本解不开 (Pillow 报 `cannot handle 0-bit layers`)。
* 实测对比 (VGA, 质量 20, 5 帧):

  | XCLK | 能解码的帧 | 帧大小 | 高频能量 (噪点指标) |
  | --- | --- | --- | --- |
  | 20MHz | 1~2 / 5 | ~78KB | ~53 |
  | 24MHz | **5 / 5** | ~67KB | **~11** |

  高频能量 = 每个像素与四邻平均之差的均值, 越小越干净 (5~11 是有内容的正常画面,
  50 以上就是"满屏噪点")。帧变小也说明噪点少了 (噪点最费码流)。
* 结论: **XCLK 保持 24MHz** (`CONFIG_TMX_CAMERA_XCLK_FREQ_HZ=24000000`)。
  之前"降 XCLK 到 10~16MHz 去噪"的说法是错的 —— 那是把信号问题误当成时钟太快,
  实际反而更糟。

## "一半的开机是满屏噪点" 与开机自检 (2026-09-21 深夜)

压完 XCLK 之后又发现一个**跟开机运气有关**的毛病: 同样的固件, 有的开机拍出来是
正常画面, 有的开机拍出来 "一屏噪点、PC 侧谁也不能解码"。连开 4 次板子的实测:

```
第1次: C4/OK  C4/OK  C4/OK
第2次: C4/OK  C4/OK  C4/OK
第3次: C5/FAIL C5/FAIL C5/FAIL   <- 这次开机就是坏的
第4次: C4/OK  C4/OK  C4/OK
```

* 判据是 JPEG 头里的 `FF C4` (DHT) 被采成了 `FF C5` (DAC)(DAC 在 Huffman JPEG 里
  本来就不该出现), 说明**传感器初始化时少写进去几个寄存器 / DVP 采样错位**,
  跟接线无关; `Pillow`、Windows 自带解码器都打不开这种帧。
* 处理: 固件开机后**自己抓一帧自检**(`frame_header_ok()`: 头 700 字节里不能有
  `FF C5`, 且必须出现 `FF C4`), 不对就 `esp_camera_deinit()` + 重新
  `esp_camera_init()` 再来, 最多 3 次。加完以后连开 5 次板子, 10/10 帧全部
  `C4/OK`。

## 弱光噪点: 自动增益上限 (`TMX_CAMERA_GAIN_CEILING`)

晚上/弱光下 OV2640 的自动增益会一直加到上限, 画面就是"能看出来但很多噪点"。
固件现在初始化后会把增益上限压到 Kconfig 里设的值 (默认 3 = 16X, 原来是 128X),
自动曝光会自动用更长曝光补偿, 画面亮一些、颗粒感也会变。

* 想更干净: 调到 2 (8X) 或 1 (4X), 画面会暗一些;
* 想恢复原厂: 填 6 (128X);
* 改完要重新编译烧录 (在线调参的 `pc_camera_tune.py` 目前还没打通, 见下)。

## 真正的病根找到了: VDDCAM_3V3(ALDO2) 一直是关的 (2026-09-21 深夜)

拿万用表量三路电压时发现: AVDD=2.8V ✓、DVDD=1.2V ✓、**VDDCAM_3V3 只有 1.8V** ✗。
按板子确认, VDDCAM_3V3 接的是 **AXP2101 的 ALDO2**, 而它的使能位 (`0x90` bit1)
**从开机起就是 0** —— 量到的 1.8V 是 I/O 漏电把悬空的轨抬起来的假电压。
也就是说摄像头的 **I/O 供电一直是断的**:

* DVP 数据线的高电平只有 1.8V, 低于 ESP32-S3 的判高门限 (~2.5V) → 数据被读得
  时好时坏, 表现就是帧头找不到 `FF D8`、字节成对重复;
* SCCB 照样能通, 因为 I2C 是开漏 + 上拉, 不吃这个门限 —— 所以之前一直误判成
  "传感器采样错位 / 模组被过压弄坏了"。

修法: 固件开机会把 ALDO2 (=VDDCAM_3V3) 设成 `TMX_CAMERA_PMIC_ALDO2_MV`
(默认 **2800mV**) 并打开; 自检失败做掉电重来时也把它一起关掉再上电。

改完实测 (VGA):

```
I tmx_camera: PMIC: 摄像头供电 AVDD(BLDO1)=2800mV, DVDD(BLDO2)=1200mV, VDDCAM(ALDO2)=2800mV
I tmx_camera: PMIC AXP2101: LDO 使能 0x90 = 0x77 ...
I tmx_camera: 开机自检: 第 1 帧 JPEG 头正常
I tmx_camera: OV2640 ready: VGA 640x480, JPEG 质量 20, XCLK 24MHz, fb=1, PID=0x26
第 1/2/3 帧: 640x480, 能被 Pillow 正常解码
```

同时这一轮也把"空闲断电"修好了: 拍照前那次重新初始化不再被误判成失败
(之前 `tmx_camera_init()` 末尾总会断电, 拍照路径看到 `s_sensor_on=false` 就以为上电失败)。

**更正前几节的推测**: 模组没有坏, "采样错位"是 I/O 供电缺失造成的, 不是过压损伤。
(DVDD 挂 2.8V 确实会让它发烫, 那条已经改回 1.2V。)

## 夜间模式: 降 XCLK 换更长的曝光 (2026-09-22)

夜里那些横纹是**逐行噪声**, 根源在自动增益: 想在同一亮度下把增益压下来, 就得给
自动曝光更长的曝光时间, 而曝光时间上限由**帧时间**决定。于是把帧率降下来即可。

试过的两个寄存器**都没用**:

* `CLKRC (0x11, sensor 档)`: 写 0x01/0x03/0x07/0x0F 后, 帧率(PROBE VSYNC)和 PCLK
  都纹丝不动 —— OV2640 在 JPEG 模式下帧率由编码器决定, 不受这个分频影响;
* `R_DVP_SP (0xD3, DSP 档)`: 同样只影响很小的范围, 帧率不变。

(顺带确认了寄存器写入本身是通的: 写 `0x3C=0x33` 能原样读回。)

**真正管用的是 XCLK**: 24MHz→12MHz 后帧率跟着降一半, 曝光上限翻倍。

实测 (同一场景, 白天, 每档 2~3 帧平均):

| XCLK | 亮度 | 行间跳动(横纹指标) |
| --- | --- | --- |
| 24MHz (默认) | 129.1 | 2.57 |
| **12MHz** | 137.1 | **1.29** |
| 8MHz | 128.5 | 1.97 |

用法 (在线切换, 立刻生效; 重启后回到 Kconfig 默认值):

```powershell
python tools\pc_camera_tune.py 192.168.0.102 --set xclk 12   # 夜间: 帧率减半, 更干净更亮
python tools\pc_camera_tune.py 192.168.0.102 --set xclk 24   # 白天: 恢复 35fps
```

想让它成为开机默认值: 改 `CONFIG_TMX_CAMERA_XCLK_FREQ_HZ` 再编译。

> 更正: 早前"20MHz/10MHz 会让画面更噪"的结论**不成立** —— 那次是在 VDDCAM_3V3
> (ALDO2) 还没供电的状态下测的, 与 XCLK 无关。

## 横纹的结论: 逐行随机噪声, 用多帧平均压掉 (2026-09-23)

24MHz 和 12MHz(夜间模式) 拍出来的横纹**一模一样**, 所以再做几个判别:

* 帧内空间周期: 两种帧率下都是 ~150 行 —— 但那条是画面的平滑渐变(场景本身),
  不是条纹周期; 真正的条纹是 **2~4 行一条**的细纹;
* 逐行起伏幅度 (相邻行均值之差的中位数): 亮区 0.32 / 中区 0.25 / **暗区 2.58**
  —— 暗部最明显, 两种帧率下基本持平;
* 帧与帧之间的相关性: +0.15 / +0.07 / +0.38 (24MHz), +0.09 / -0.25 / -0.28 (12MHz)
  → **每帧都换一个样子 = 随机时间噪声**, 不是固定图案、也不是灯光 100Hz 频闪
  (频闪的周期会随帧率变, 这里没变)。

传感器自带的降噪开关也用不上: `set_denoise` / `set_sharpness` 在 esp32-camera 的
OV2640 驱动里是**空实现**(直接返回 -1), 只有 `set_bpc` / `set_wpc` / `set_lenc`
有实现(坏点/镜头校正, 对行噪声没用)。

### 办法: 多帧平均 (`tools/pc_camera_merge.py`)

既然是每帧独立的随机行噪声, 拍同一静止场景 N 帧对齐平均就能把噪声降到 **1/√N**。
实测 (12 帧, 暗区):

| | 暗区逐行抖动 | 像素级噪点 |
| --- | --- | --- |
| 单帧 | 1.67 ~ 3.05 | 0.90 ~ 1.04 |
| **12 帧平均** | **0.60** | **0.59** |

```powershell
# 一步到位: 拍 12 帧再合并 (输出 PNG, 不二次压缩)
python tools\pc_camera_merge.py --host 192.168.0.102 --frames 12 --out tools\merged.png

# 也可以合并已经拍好的图
python tools\pc_camera_merge.py --files tools\avgframe_*.jpg --out tools\merged.png
```

代价: 只适合静止画面 (动的东西会糊), 而且要连拍 N 帧。4 帧降一半, 16 帧降到 1/4。

## 还没解决的

1. **串口日志开机后一段时间会停**: USB-Serial-JTAG 的日志通道会卡住 (TCP 命令
   照常执行), 复位一次就好。怀疑是某个任务在日志上阻塞把日志锁住了, 需要单独查。
2. ~~**在线调参命令 (0x7D) 没生效**~~ **已确认是通的** (2026-09-23): 用必然可观测的
   `--set xclk 12` 验证过, `--info` 立刻变成 `XCLK 12MHz`, 改回 24 也生效;
   `--quality` 改完帧大小跟着变 (质量 40 约 10KB / 质量 8 约 27KB)。
   之前觉得"没生效"的两种情况是:

   * `set_hmirror` / `set_vflip` 在 **JPEG 模式下本来就是空操作** (驱动只写 REG04 的
     镜像位, JPEG 数据通路绕过它) —— 实测 `--set hmirror 1` 后画面相关系数 0.996;
   * `aec_value` 在自动曝光还开着时会被自动算法覆盖, 看着"没变"。

## 摄像头发烫 (2026-09-21): 供电电压 + 空闲断电

摸上去很烫, 查出来是两件事:

1. **供电电压之前被留在 `2.8V / 2.8V`**: OV2640 的 DVDD 是 **1.2V 内核供电**,
   挂在 2.8V 上属于过压, 会明显发热 (时间长了也可能伤到传感器)。之前调试供电映射时
   为了做对照实验把 BLDO2 抬到 2.8V, 之后一直没改回来。
   固件现在**每次开机都把两路设成确定值**并打开:

   | Kconfig | 默认 | 接到 |
   | --- | --- | --- |
   | `TMX_CAMERA_PMIC_AVDD_MV` | 2800 | AVDD = BLDO1 |
   | `TMX_CAMERA_PMIC_DVDD_MV` | **1200** | DVDD = BLDO2 |

   开机日志会打: `PMIC: 摄像头供电 AVDD(BLDO1)=2800mV, DVDD(BLDO2)=1200mV`。

2. **初始化完就一直出流**: esp32-camera 一旦 init, 传感器就不停出图 (XCLK 一直跑,
   JPEG 编码器一直干活), 摸上去当然是温的。固件现在 `TMX_CAMERA_IDLE_POWER_OFF=y`
   (默认开):

   * 开机只做一次初始化自检, 随后**断电** (停 XCLK + PWDN 拉高);
   * 每次拍照前重新上电初始化 (~0.3s, 含一次 JPEG 头自检), 拍完 (或 CAMERA_STOP) 立刻断电;
   * 空闲时 SCCB 上 `0x30` 不应答, 模块是凉的 (用 `tools/ov2640_regs.py` 可以验证)。

   另外自检失败时, 重试现在会做一次**真正的掉电再上电** (PWDN 拉高 150ms 再拉低),
   因为只调 `esp_camera_init()` 清不掉"采样错位"那个状态。

## 横纹/噪点的真正病根: 上电后自动算法没收敛, 而固件每拍一张就断电 (2026-09-23)

前面几轮一直在往"供电纹波 / DVP 信号完整性"上查, 那些都不成立。真正的原因很朴素,
但被"空闲降温"这个设计放大了:

**OV2640 上电时 AEC / AGC / AWB 都是从默认值开始逐帧收敛的。头几帧模拟增益是拉满的、
白平衡也没收敛。而本固件为了降温每次拍照都要断电重上电 —— 于是每一张照片拿到的
恰好都是这个最脏的第 0 帧。**

### 实测 (VGA / 质量 12 / 同一会话连续取帧, 暗部)

| 帧序 | 平均亮度 | 行条纹 | 像素噪声 | 色偏 (R−Y) |
| --- | --- | --- | --- | --- |
| 第 0 帧 | 145.4 | **3.54** | **4.91** | 61.8 |
| 第 1 帧 | 131.3 | 2.91 | 4.96 | 52.3 |
| 第 2 帧 | 125.7 | 2.39 | 4.14 | 48.2 |
| 第 3 帧 | 122.3 | 1.56 | 3.25 | 46.7 |
| 第 5 帧 | 123.4 | 0.97 | 2.27 | 46.3 |
| 第 8 帧 | 129.3 | 0.73 | 2.11 | 50.6 |
| 第 10~19 帧 | ~129.3 | **0.62~0.82** | **1.92~2.16** | ~50.8 |

第 0 帧的逐行噪声是收敛后的 **4.5~8 倍**, 白平衡还差 10 个灰阶。人眼看到的就是
"很多噪点 + 彩色横纹 + 局部色带"。

### 怎么修

初始化之后、自检和拍照之前先抓一批帧丢掉不返回, 等自动算法收敛:

* `TMX_CAMERA_WARMUP_MS` (默认 **2000**, 0 = 关)
* `TMX_CAMERA_WARMUP_FRAMES` (默认 **20**, 兜底帧数: 夜间模式帧率减半时防止时间够了帧不够)

改完实测, **第一张照片**就到位了:

| | 行条纹 | 像素噪声 | 色偏 |
| --- | --- | --- | --- |
| 修复前第 0 帧 | 3.54 | 4.91 | 61.8 |
| 修复后第 0 帧 | **0.44** | **1.69** | **44.0** |

代价: 每次拍照多 2 秒 (只在拍照前, 空闲照样是断电的)。

### 为什么之前那些判断都不成立

在找到"帧序"这条线之前, 已经把常见的都排掉了。留个记录, 免得以后又绕回去:

* **不是 DVP 时序/信号完整性**: 24MHz 和 12MHz XCLK 下条纹指标一样。采样余量不足的话,
  时钟减半会让误码率大幅变化。
* **不是灯光频闪 (50Hz 市电)**: 逐行剖面的垂直频谱里**没有周期分量**, 最强分量就是
  画面自身的低频渐变; 而且帧率减半后条纹完全不变 —— 频闪的条纹位置必然随行读出速率改变。
* **不是电源纹波/串扰**: 逐行剖面的**帧间相关性 ≈ 0** (+0.07 / −0.14)。固定图案的纹波
  会给出接近 1 的相关性。
* **不是带宽/缓冲溢出**: 帧头 `FF D8 FF`、`FF D4`、`FF D9` 都正常, 帧 100% 能解出来。
  JPEG 码流被位错污染会出现大面积色块/解码失败, 而不是 1~4 个灰阶的细纹。
* **不是传感器坏了/模组个体差异**: 同一颗传感器收敛后就很干净, 说明硬件是好的。
* 顺带确认: `hmirror` / `vflip` 在 JPEG 模式下**不生效**(驱动写 REG04 的 HFLIP 位,
  但 JPEG 数据通路绕过了它) —— 实测 `--set hmirror 1` 后画面相关系数 0.996, 等于没变。
  这是驱动限制, 不是我们这边的问题。

### 复现/验证用的工具

`tools/pc_camera_stripe.py` (只依赖 Pillow + numpy, 用 IDF 的 Python 跑) 把
"亮度条 / 色度条 / 明区暗区 / 频谱周期 / 帧间相关性"一次算出来, 用来区分
"行噪声"和"场景内容"、"随机噪声"和"固定图案":

```powershell
& C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe tools\pc_camera_stripe.py "tools\stripe\*.jpg" --region dark
```

关键判据: **同一场景连拍多帧, 行条纹在多帧平均后应该按 1/√N 衰减** —— 衰减了就是随机
行噪声(传感器), 不衰减就是固定图案(纹波/串扰/场景本身)。

## 把摄像头接到 Scratch 积木 (2026-09-23 深夜)

固件侧的协议 (0x78~0x7B / 上报 0x0F~0x12) 早就有了, 缺的是网关和 Scratch 扩展这两段。
补上之后踩到两个坑, 现象都很像"板子/摄像头坏了", 记一下。

### 坑 1: 上报码 0x13 (引脚探针) 把网关的接收循环直接打死

telemetrix 的接收循环是这样派发的:

```python
report = packet[0]
await self.report_dispatch[report](packet[1:])     # 直接索引, 没有兜底
```

`report_dispatch` 里没注册的上报码 -> `KeyError` -> **整个接收循环退出**。而且
这个循环是个没人管的 asyncio task, 异常不会打到 stderr, 于是:

* 网关进程还活着;
* 到板子的 TCP 还是 ESTABLISHED;
* 板子串口里照常打 `拍照: 指定帧数` / `帧对齐: 收到 20620 字节` / `拍完`;
* 但 PC 侧**从此收不到板子任何上报**, 连 `0x7B` 摄像头状态都不回, 一行日志都没有。

当时的触发源是 `CONFIG_TMX_CAMERA_PIN_PROBE=y` 已经打开: 每次摄像头初始化都会扫一遍
DVP 各引脚, 顺带用 0x13 把探针结果上报给 PC —— 而网关没注册 0x13, 于是每次拍照前
那一下初始化就把它打死了。

两条一起做的修法:

1. **关掉引脚探针** (`CONFIG_TMX_CAMERA_PIN_PROBE=n`)。它本来就是排障用的, 开着还会
   让每次拍照多扫 40 个 GPIO、刷一堆 `GPIO number error`, 又慢又吵。
   顺带修了一个既存编译错误: 探针关掉时 `probe_report()` 没有定义, 但"在线改 XCLK"
   和"读寄存器"两处还在调它 -> `implicit declaration of function 'probe_report'`。
   现在探针关掉时给一个空实现。
2. **补丁 11**: 接收循环改成
   `handler = self.report_dispatch.get(report)`, 没有处理器就打印一条
   `unknown report id: N` 后 `continue`, 处理器抛异常也只丢掉这一条。
   这样以后再冒出新上报码, 也是"少收一条"而不是"整条链路静默死掉"。

### 坑 2: telemetrix 的 WiFi 读函数按"短读"写, 一帧 80 多片必错位

`socket_aio_transport.py`:

```python
async def read(self, num_bytes=1):
    buffer = await self.reader.read(num_bytes)    # asyncio: 只保证"最多 n 字节"
    return buffer
```

而接收循环是"先读 1 个字节当长度, 再读那么多字节"。数据跨 TCP 分段时
`read(n)` 会短读, 从此全体错位。小包(超声波/I2C/响度)一般一个 TCP 段就过去了,
所以一直没暴露; 摄像头一帧 VGA 要发 80 多片, 必然踩到。

**补丁 10** 把 `read()` 改成读满 `num_bytes` 再返回。

### 怎么快速判断是哪一类问题

不看网关日志光看现象很容易误判, 这几个观察点最省事:

| 观察 | 说明什么 |
| --- | --- |
| 串口里板子在 `拍照`/`帧对齐: 收到 N 字节`/`拍完` | 板子侧完全正常, 问题在 PC 侧 |
| 连 `camera_info` (0x7B) 都不回, 但网关进程和 TCP 都还在 | 接收循环已经死了 -> 坑 1 那一类 |
| `camera_info` 回得来、照片收不到 | 拼帧/分片错位 -> 坑 2 那一类 |
| 网关日志 `report N handler failed` / `unknown report id: N` | 就是坑 1, 看 N 是哪个上报码 |

板子串口本身也不完全可靠(开机一段时间后会卡, 见前面), 但 `ets_printf` 和 TCP 上报
一直是好的, 所以"板子到底有没有收到命令"优先看串口那几行 `tmx_camera` 日志。

### 验证工具

`tools/test_camera_blocks.py` 不打开 Scratch 就能验证整条网关链路 (发
`ip_address`/`camera_config`/`camera_snapshot`, 收 `camera_info`/`camera_frame_start`/
`camera_frame`/`camera_status`, 并把 JPEG 存盘 + 校验 SOI/EOI):

```powershell
# 用系统 Python (python_banyan / s3-extend 装在它下面)
C:\Program Files\Python313\python.exe tools\test_camera_blocks.py --host 192.168.0.103 --frames 2 --dir tools\photos
```

扩展本身的积木逻辑用 `node tools\test_esp32s3_camera.js` 跑 (假 Scratch + 假 WebSocket,
不需要编辑器也不需要板子)。
