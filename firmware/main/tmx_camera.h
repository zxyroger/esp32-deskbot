/*
 * OV2640 DVP 摄像头 (esp32-camera 组件)
 *
 * 硬件 (引脚见 menuconfig -> "摄像头 (OV2640 DVP)", 默认值就是本板的接线):
 *   - SCCB 控制总线复用板载 I2C (SDA=1 / SCL=2, 与 ES8311 音频 codec 同一条线)
 *   - DVP 数据/同步线: PCLK / VSYNC / HREF / D0~D7
 *   - XCLK 由 LEDC 输出 (默认 20MHz), 因此会独占一个 LEDC 定时器 + 通道
 *
 * 对外能力:
 *   - 拍一帧或多帧 JPEG, 通过 Telemetrix 协议分片发给 PC (tools/pc_camera_check.py)
 *   - 连续拍 (流模式) 直到 CAMERA_STOP
 *   - 运行中改分辨率 / JPEG 质量
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 发送一包数据给 PC 的回调 (由 tmx_core 提供)。
 * 返回 false 表示连接已断开, 摄像头会停下来。 */
typedef bool (*tmx_camera_send_fn)(const uint8_t *packet, size_t len);

/* 分辨率索引 (协议里用的编码, 见 docs/camera-ov2640.md) */
#define TMX_CAMERA_SIZE_QVGA 0
#define TMX_CAMERA_SIZE_VGA  1
#define TMX_CAMERA_SIZE_SVGA 2
#define TMX_CAMERA_SIZE_XGA  3
#define TMX_CAMERA_SIZE_SXGA 4
#define TMX_CAMERA_SIZE_UXGA 5
#define TMX_CAMERA_SIZE_MAX  TMX_CAMERA_SIZE_UXGA

/* 拍照状态 (CAMERA_STATUS 上报里用) */
#define TMX_CAMERA_STATE_IDLE     0  /* 空闲 (没在拍) */
#define TMX_CAMERA_STATE_STREAM   1  /* 正在拍 (连续流) */
#define TMX_CAMERA_STATE_ERROR    2  /* 出错 (传感器没应答 / 内存不够等) */

/* 上电初始化一次。可重复调用; 失败时只有摄像头不可用, 其它功能照常。 */
esp_err_t tmx_camera_init(void);

bool tmx_camera_is_ready(void);

/* 把协议引擎的发包函数挂上来 (tmx_core_init 里调用) */
void tmx_camera_set_sender(tmx_camera_send_fn send);

/* 拍照:
 *   frames > 0 : 拍这么多帧后自动停
 *   frames <= 0: 一直拍, 直到 CAMERA_STOP
 *   interval_ms: 两帧之间的最小间隔 (0 = 尽快拍)
 * 返回 ESP_ERR_INVALID_STATE 表示摄像头没就绪。 */
esp_err_t tmx_camera_snapshot(int frames, uint32_t interval_ms);

/* 停止拍照; 正在发送的那一帧会被丢掉 */
void tmx_camera_stop(void);

/* 改分辨率 / JPEG 质量 (0~63, 越小越清晰) / 像素格式。
 * 任一为负表示保持不变。
 *   pixformat: TMX_CAMERA_PIX_KEEP(0) 不变, TMX_CAMERA_PIX_JPEG(1), TMX_CAMERA_PIX_YUV422(2)
 * 排障时把格式切成 YUV422 + 小分辨率, 可以拿到"原始像素数据"来看数据线好不好。 */
#define TMX_CAMERA_PIX_KEEP   0
#define TMX_CAMERA_PIX_JPEG   1
#define TMX_CAMERA_PIX_YUV422 2

esp_err_t tmx_camera_set_format(int frame_size, int quality, int pixformat);

/* 当前状态 */
int      tmx_camera_state(void);
uint32_t tmx_camera_frames_sent(void);
int      tmx_camera_quality(void);

/* 把摄像头信息作为 CAMERA_INFO 报告发给 PC (回应 CAMERA_INFO 命令) */
void tmx_camera_send_info(void);

/* 量一遍 DVP 各信号线的边沿 (排障用, 结果打到串口)。
 * 需要 menuconfig 里打开 "启动时量一遍 DVP 各根信号线的活动" 才有实际动作。 */
void tmx_camera_probe_pins(void);

/* 主循环里定期调用: 该拍就拍, 该发就分片发 */
void tmx_camera_poll(void);
/* 在线调传感器 (命令 0x7D: 字段(1) + 值(2))；字段编号见下 */
#define TMX_CAM_FIELD_STATUS      0   /* 只打一遍当前参数到串口 */
#define TMX_CAM_FIELD_BRIGHTNESS  1   /* -2 ~ 2 */
#define TMX_CAM_FIELD_CONTRAST    2   /* -2 ~ 2 */
#define TMX_CAM_FIELD_SATURATION  3   /* -2 ~ 2 */
#define TMX_CAM_FIELD_AE_LEVEL    4   /* -2 ~ 2, 越大越亮 (弱光拍亮一点) */
#define TMX_CAM_FIELD_AGC_GAIN    5   /* 0 ~ 30, 手动增益 (要先关 AGC 才有意义) */
#define TMX_CAM_FIELD_AEC_VALUE   6   /* 0 ~ 1200, 手动曝光 (要先关 AEC) */
#define TMX_CAM_FIELD_GAINCEILING 7   /* 0=2X 1=4X 2=8X 3=16X 4=32X 5=64X 6=128X, 越小噪点越少 */
#define TMX_CAM_FIELD_HMIRROR     8   /* 0/1 左右镜像 */
#define TMX_CAM_FIELD_VFLIP       9   /* 0/1 上下翻转 */
#define TMX_CAM_FIELD_AWB_GAIN    10  /* 0/1 自动白平衡增益 */
#define TMX_CAM_FIELD_AEC2        11  /* 0/1 第二代自动曝光 */
#define TMX_CAM_FIELD_AGC_CTRL    12  /* 0/1 自动增益开关 */
#define TMX_CAM_FIELD_AEC_CTRL    13  /* 0/1 自动曝光开关 */
#define TMX_CAM_FIELD_SET_REG_DSP 14  /* 直接写 DSP 档寄存器: 值 = (寄存器<<8) | 新值 */
#define TMX_CAM_FIELD_SET_REG_SEN 15  /* 直接写 sensor 档寄存器: 值 = (寄存器<<8) | 新值 */
#define TMX_CAM_FIELD_GET_REG     16  /* 读一个寄存器 (值 = (档<<8)|寄存器, 结果打串口) */

esp_err_t tmx_camera_tune(int field, int value);
