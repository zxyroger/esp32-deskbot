/*
 * Telemetrix4Esp32 (WiFi) 协议常量 —— 与 PC 端
 * telemetrix_aio_esp32 / telemetrix_esp32_common 保持一致。
 *
 * 数据包格式（串口/WiFi 通用）:
 *   [0] 包长度  = 本字节之后的字节数 (= 1 + 数据字节数)
 *   [1] 命令 / 报告类型
 *   [2..] 数据
 *
 * 例: 数字写  =>  03 02 <pin> <value>
 *     数字报告 =>  03 02 <pin> <value>
 */

#pragma once

/* ------------------------------------------------------------------ */
/* 命令 (PC -> 板子)                                                   */
/* ------------------------------------------------------------------ */
#define TMX_CMD_LOOPBACK                     0
#define TMX_CMD_SET_PIN_MODE                 1
#define TMX_CMD_DIGITAL_WRITE                2
#define TMX_CMD_ANALOG_WRITE                 3
#define TMX_CMD_MODIFY_REPORTING             4
#define TMX_CMD_GET_FIRMWARE_VERSION         5
#define TMX_CMD_SERVO_ATTACH                 6
#define TMX_CMD_SERVO_WRITE                  7
#define TMX_CMD_SERVO_DETACH                 8
#define TMX_CMD_I2C_BEGIN                    9
#define TMX_CMD_I2C_READ                     10
#define TMX_CMD_I2C_WRITE                    11
#define TMX_CMD_SONAR_NEW                    12
#define TMX_CMD_DHT_NEW                      13
#define TMX_CMD_STOP_ALL_REPORTS             14
#define TMX_CMD_SET_ANALOG_SCANNING_INTERVAL 15
#define TMX_CMD_ENABLE_ALL_REPORTS           16
#define TMX_CMD_ANALOG_OUT_ATTACH            17
#define TMX_CMD_ANALOG_OUT_DETACH            18
#define TMX_CMD_DAC_WRITE                    19
#define TMX_CMD_RESET                        20
#define TMX_CMD_DAC_DISABLE                  21
/* 22 ~ 26 : SPI           — 本固件未实现 */
/* 27 ~ 35 : OneWire       — 本固件未实现 */
/* 36 ~ 56 : Stepper       — 本固件未实现 */
#define TMX_CMD_MAX_SUPPORTED                56

/* ------------------------------------------------------------------ */
/* 自定义扩展命令 (Telemetrix 官方协议只用到 56, 0x70 起是本工程自己加的)   */
/* ------------------------------------------------------------------ */
#define TMX_CMD_LCD_BACKLIGHT                0x70  /* 1 字节: 0=关, 1~100=亮度% */
#define TMX_CMD_LCD_COLOR                    0x71  /* 3 字节: R G B (0~255), 整屏填充 */
#define TMX_CMD_AUDIO_TONE                   0x72  /* 5 字节: 频率(2) 时长ms(2) 音量%(1) */
#define TMX_CMD_AUDIO_STOP                   0x73  /* 无数据: 立刻停音 */
#define TMX_CMD_AUDIO_MIC                    0x74  /* 1 字节: 0=关, 1=开麦克风响度上报 */
#define TMX_CMD_TTS_TEXT                     0x75  /* 1 字节标志 + UTF-8 文字 (标志 bit0=最后一段) */
#define TMX_CMD_TTS_STOP                     0x76  /* 无数据: 停止朗读 */
#define TMX_CMD_TTS_MIRROR                   0x77  /* 1 字节: 0=关, 1=把合成 PCM 回传 PC */
#define TMX_CMD_CAMERA_CONFIG                0x78  /* 2 字节: 分辨率索引, JPEG 质量 (任一 0xFF = 不改) */
#define TMX_CMD_CAMERA_SNAPSHOT              0x79  /* 3 字节: 帧数, 间隔ms(2); 帧数 0 = 连续拍到 CAMERA_STOP */
#define TMX_CMD_CAMERA_STOP                  0x7A  /* 无数据: 停止拍照 */
#define TMX_CMD_CAMERA_INFO                  0x7B  /* 无数据: 查询摄像头状态 */
#define TMX_CMD_CAMERA_PROBE                 0x7C  /* 无数据: 量一遍 DVP 各信号线 (排障, 结果打到串口) */
#define TMX_CMD_CAMERA_TUNE                  0x7D  /* 3 字节: 字段(1) + 值(2, 大端有符号) 在线调传感器 */
/* 命令号上限 (switch 之前用它拦非法命令) */
#define TMX_CMD_TABLE_SIZE                   (TMX_CMD_CAMERA_TUNE + 1)

/* ------------------------------------------------------------------ */
/* 报告 (板子 -> PC)                                                   */
/* ------------------------------------------------------------------ */
#define TMX_REPORT_DIGITAL                   2
#define TMX_REPORT_ANALOG                    3
#define TMX_REPORT_FIRMWARE                  5
#define TMX_REPORT_SERVO_UNAVAILABLE         6
#define TMX_REPORT_I2C_TOO_FEW_BYTES         7
#define TMX_REPORT_I2C_TOO_MANY_BYTES        8
#define TMX_REPORT_I2C_READ                  9
#define TMX_REPORT_SONAR_DISTANCE            10
#define TMX_REPORT_DHT                       11
#define TMX_REPORT_TOUCH                     12
#define TMX_REPORT_AUDIO_LEVEL               13  /* 1 字节: 0~100 麦克风响度 */
#define TMX_REPORT_TTS_PCM                   14  /* 序号(2) + 16kHz/16bit 单声道 PCM */
#define TMX_REPORT_CAMERA_FRAME              15  /* 序号(1) + 偏移(2) + JPEG 数据 (分片) */
#define TMX_REPORT_CAMERA_FRAME_INFO         16  /* 序号(1) 格式(1) 宽(2) 高(2) 长度(4) */
#define TMX_REPORT_CAMERA_STATUS             17  /* 状态(1) + 数值(1) */
#define TMX_REPORT_CAMERA_INFO               18  /* 状态(1) 宽(2) 高(2) 质量(1) 分辨率索引(1) XCLK MHz(1) */
#define TMX_REPORT_DEBUG_PRINT               99

/* 固件版本 —— 与官方 Telemetrix4Esp32 (3.2.0) 对齐，PC 端只做打印 */
#define TMX_FIRMWARE_MAJOR 3
#define TMX_FIRMWARE_MINOR 2
#define TMX_FIRMWARE_BUILD 0

/* ------------------------------------------------------------------ */
/* 引脚模式 (SET_PIN_MODE 的第二个字节)                                */
/* ------------------------------------------------------------------ */
#define TMX_MODE_INPUT              0
#define TMX_MODE_OUTPUT             1
#define TMX_MODE_INPUT_PULLUP       2
#define TMX_MODE_ANALOG             3
#define TMX_MODE_SERVO              4
#define TMX_MODE_SONAR              5
#define TMX_MODE_DHT                6
#define TMX_MODE_TOUCH              7
#define TMX_MODE_PWM_OUT            8
#define TMX_MODE_INPUT_PULLDOWN     9
#define TMX_MODE_NOT_SET            255

/* ------------------------------------------------------------------ */
/* 上报开关 (MODIFY_REPORTING 的第一个字节)                            */
/* ------------------------------------------------------------------ */
#define TMX_REPORTING_DISABLE_ALL      0
#define TMX_REPORTING_ANALOG_ENABLE    1
#define TMX_REPORTING_DIGITAL_ENABLE   2
#define TMX_REPORTING_ANALOG_DISABLE   3
#define TMX_REPORTING_DIGITAL_DISABLE  4

/* 表容量 */
#define TMX_MAX_PINS        64  /* ESP32-S3 引脚号 0~48 */
#define TMX_MAX_SERVOS      8
#define TMX_MAX_SONARS      6
#define TMX_MAX_COMMAND_LEN 256     /* TTS 文字分包发过来, 一段最多 255 字节 */
