# Telemetrix 协议速查

本固件实现的是 MrYsLab **Telemetrix4Esp32 (WiFi)** 的协议，
PC 端代码见 `telemetrix-esp32/telemetrix_aio_esp32` 与
`telemetrix-esp32/telemetrix_esp32_common/private_constants.py`。

## 传输层

* TCP，板子做服务器，默认端口 **31336**
* 同一时刻只服务一个客户端（与 Arduino 版一致）
* 每次连接后客户端会先发 `GET_FIRMWARE_VERSION`，再发 `ENABLE_ALL_REPORTS`

## 数据包格式

```
 字节 0      字节 1        字节 2...
┌────────┬────────────┬──────────────────┐
│ 包长度 │ 命令/报告  │ 数据             │
└────────┴────────────┴──────────────────┘
```

包长度 = **本字节之后的字节数**（= 1 + 数据字节数）。
例：数字输出 GPIO2 为高 → `03 02 02 01`。

## 命令（PC → 板子）

| 码 | 名称 | 数据 | 本工程 |
| --- | --- | --- | --- |
| 0 | LOOPBACK | 1 字节 | ✅ 原样回送 |
| 1 | SET_PIN_MODE | 见下表 | ✅ |
| 2 | DIGITAL_WRITE | pin, value | ✅ |
| 3 | ANALOG_WRITE | pin, duty_hi, duty_lo (PWM 占空比) | ✅ |
| 4 | MODIFY_REPORTING | 子命令, pin | ✅ |
| 5 | GET_FIRMWARE_VERSION | - | ✅ 回 `3.2.0` |
| 6 | SERVO_ATTACH | pin, min_hi, min_lo, max_hi, max_lo | ✅ |
| 7 | SERVO_WRITE | pin, angle | ✅ |
| 8 | SERVO_DETACH | pin | ✅ |
| 9 | I2C_BEGIN | sda, scl（0,0 = 默认引脚） | ✅ |
| 10 | I2C_READ | addr, reg, count, stop_flag | ✅ |
| 11 | I2C_WRITE | count, addr, data... | ✅ |
| 12 | SONAR_NEW | trigger, echo | ✅ |
| 13 | DHT_NEW | pin | ❌ 未实现 |
| 14 | STOP_ALL_REPORTS | - | ✅ |
| 15 | SET_ANALOG_SCANNING_INTERVAL | ms | ✅ |
| 16 | ENABLE_ALL_REPORTS | - | ✅ |
| 17 | ANALOG_OUT_ATTACH | pin, channel | ✅ 空操作（ESP32 Core v3 已不需要） |
| 18 | ANALOG_OUT_DETACH | pin | ✅ |
| 19 / 21 | DAC_WRITE / DAC_DISABLE | - | ❌ S3 无 DAC |
| 20 | RESET | - | ✅ `esp_restart()` |
| 22~26 | SPI | - | ❌ 未实现 |
| 27~35 | OneWire | - | ❌ 未实现 |
| 36~56 | 步进电机 | - | ❌ 未实现 |

### 自定义扩展命令（0x70 起）

官方协议只用到 56，0x70 以后是本工程自己加的（板载屏幕 / 音频 / 朗读 / 摄像头），
PC 端网关需要 `tools\apply_local_patches.py` 的**补丁 6（屏幕）/ 7（音频）/
8（朗读）/ 9（摄像头）** 才会转发；摄像头还额外依赖补丁 10 / 11（见
[camera-ov2640.md](camera-ov2640.md)）。

| 码 | 名称 | 数据 | 说明 |
| --- | --- | --- | --- |
| 0x70 | LCD_BACKLIGHT | 1 字节: 0=关, 1~100=亮度% | 屏幕背光 |
| 0x71 | LCD_COLOR | 3 字节: R G B | 整屏填充 |
| 0x72 | AUDIO_TONE | freq_hi, freq_lo, ms_hi, ms_lo, volume(0~100) | 播放正弦音调（非阻塞）|
| 0x73 | AUDIO_STOP | - | 立刻停止放音 |
| 0x74 | AUDIO_MIC | 1 字节: 0=关, 1=开 | 打开/关闭麦克风响度上报 |
| 0x75 | TTS_TEXT | 1 字节标志(bit0=最后一段) + UTF-8 文字 | 中文语音合成（文字 ≤250 字节/包，板子拼起来再念）|
| 0x76 | TTS_STOP | - | 停止朗读 |
| 0x77 | TTS_MIRROR | 1 字节: 0=关, 1=开 | 把合成的 16kHz PCM 回传 PC（调试/存 wav）|
| 0x78 | CAMERA_CONFIG | 分辨率索引(1), JPEG 质量(1), [像素格式(1)] | 任一为 0xFF = 不改; 索引 0=QVGA 1=VGA 2=SVGA 3=XGA 4=SXGA 5=UXGA; 像素格式可选: 1=JPEG(默认) 2=YUV422(排障用, 帧数据不再是 JPEG) |
| 0x79 | CAMERA_SNAPSHOT | 帧数(1), 间隔 ms(2, 大端) | 帧数 0 = 连续拍 (直到 0x7A); 间隔 0 = 用固件里的默认值 |
| 0x7A | CAMERA_STOP | - | 停止拍照, 丢掉正在发送的帧 |
| 0x7B | CAMERA_INFO | - | 回一条 0x12 (摄像头状态) |

### SET_PIN_MODE 数据格式

| 模式 | 码 | 数据 |
| --- | --- | --- |
| 输入 | 0 | pin, 0, reporting |
| 输出 | 1 | pin, 1 |
| 输入+上拉 | 2 | pin, 2, reporting |
| 模拟输入 | 3 | pin, 3, diff_hi, diff_lo, reporting |
| 舵机 | 4 | （用 SERVO_ATTACH） |
| 超声波 | 5 | （用 SONAR_NEW） |
| 触摸 | 7 | ❌ 未实现 |
| PWM 输出 | 8 | pin, 8, channel, resolution, frequency(8 字节 double, 小端) |
| 输入+下拉 | 9 | pin, 9, reporting |

## 报告（板子 → PC）

| 码 | 名称 | 数据 | 触发时机 |
| --- | --- | --- | --- |
| 0 | LOOPBACK | 1 字节 | 收到回环命令 |
| 2 | DIGITAL_REPORT | pin, value | 输入电平变化 |
| 3 | ANALOG_REPORT | pin, value_hi, value_lo | 变化量 ≥ differential |
| 5 | FIRMWARE_REPORT | major, minor, build | 收到版本查询 |
| 6 | SERVO_UNAVAILABLE | pin | 舵机数量超限 |
| 7 | I2C_TOO_FEW_BYTES | 1, addr | I2C 读取失败 |
| 9 | I2C_READ_REPORT | count, addr, reg, data... | I2C 读取成功 |
| 10 | SONAR_DISTANCE | trigger_pin, cm_hi, cm_lo | 距离变化 |
| 13 | AUDIO_LEVEL | value (0~100) | 麦克风响度（需先发 0x74 打开）|
| 14 | TTS_PCM | seq_hi, seq_lo, pcm... (16kHz/16bit 单声道) | TTS 回传；序号 0xFFFF 表示这段结束 |
| 15 | CAMERA_FRAME | 序号(1), 偏移(3, 大端), JPEG 数据(≤240 字节) | 拍照时每帧拆成多片发；偏移是帧内字节位置（3 字节，XGA 以上的帧超过 64KB）|
| 16 | CAMERA_FRAME_INFO | 序号(1), 格式(1), 宽(2), 高(2), 长度(4, 小端) | 每帧发数据前先发一条, 告诉 PC 这帧多大 |
| 17 | CAMERA_STATUS | 状态(1), 数值(1) | 0=空闲(拍完), 1=开始拍(数值=要拍的帧数), 2=出错(数值=连续失败次数) |
| 18 | CAMERA_INFO | 状态(1), 宽(2), 高(2), 质量(1), 分辨率索引(1), XCLK MHz(1) | 回应 0x7B |
| 99 | DEBUG_PRINT | id, value_hi, value_lo | `TMX_DEBUG_REPORTS=y` 时 |

### 拍照一条龙 (PC 侧要做什么)

```
0x79 (帧数=1, 间隔)  ─►  板子: 0x11 状态=1
                          0x10 (序号/格式/宽/高/长度) ─► 每帧的头
                          0x0F (序号/偏移/数据)  ×N     ─► 按偏移拼起来
0x7A (停止)          ─►  板子: 0x11 状态=0 (拍完了)
```

格式字节 = `pixformat_t` (3 = JPEG)。固件目前只会发 JPEG。
PC 侧拼帧的参考实现见 `tools/pc_camera_check.py`。

## 与官方 Arduino 版的行为差异

1. **输入初值**：官方固件把输入引脚的上次值初始化为 0，因此一开始就是低电平的
   引脚不会上报。本固件在设置输入模式时立刻上报一次真实电平，Scratch 里的
   "数字读" 积木第一次就能读到正确值。
2. **PWM 通道**：官方固件按客户端给的 channel 参数挂载 LEDC 通道（Scratch 总是传 0，
   多路 PWM 会互相覆盖）。本固件按引脚自动分配通道，最多 8 路。
3. **I2C**：官方固件区分 "读到的字节太少/太多"，本固件用新版 i2c_master 驱动，
   只能判定成功/失败，失败时统一上报 `7`（字节数不足）。
4. **保留引脚**：官方固件不检查，本固件默认拒绝 26~37 / 0 / 45 / 46，避免打到
   Flash/PSRAM 或 Strapping 引脚导致重启。
