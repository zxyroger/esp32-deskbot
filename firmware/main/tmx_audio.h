/*
 * ES8311 音频输入/输出
 *
 * 硬件 (见 docs/audio-es8311.md):
 *   - I2S 全双工: MCLK/BCLK/WS/DOUT/DIN, ESP32-S3 做主机, codec 做从机
 *   - I2C 控制: 与 Scratch 的 I2C 积木共用一条总线 (默认 SDA=1 / SCL=2)
 *   - PA 功放使能脚, 由 codec 驱动在放音时自动拉高
 *
 * 对外能力:
 *   - 播放音调 (正弦波), 可指定频率/时长/音量
 *   - 麦克风响度 (0~100), 直接读最近一次采集结果
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 初始化 I2S + codec + PA。可重复调用; 失败时不会影响其它功能。 */
esp_err_t tmx_audio_init(void);

bool tmx_audio_is_ready(void);

/* 播放一段正弦音调 (非阻塞, 再调一次会换成新的音调)。
 * freq_hz: 20 ~ 采样率*0.45, duration_ms: 1 ~ 60000, volume_percent: 0 ~ 100 */
esp_err_t tmx_audio_play_tone(int freq_hz, int duration_ms, int volume_percent);

/* 朗读一段 UTF-8 文字 (中文语音合成, 非阻塞; 会打断正在播放的音调) */
esp_err_t tmx_audio_say_text(const char *utf8_text, int speed);

/* 立刻停止放音 / 停止朗读 (喇叭静音) */
void tmx_audio_stop(void);

bool tmx_audio_tts_active(void);

/* 把合成的 16kHz/16bit 单声道 PCM 回传一份给 PC (调试/存 wav 用) */
void tmx_audio_set_mirror(bool enable);
bool tmx_audio_mirror_on(void);
int  tmx_audio_mirror_read(uint8_t *dst, int max_len);
bool tmx_audio_mirror_end_pending(void);
void tmx_audio_mirror_clear_end(void);

/* 打开/关闭麦克风采集 (关闭后响度归 0, 也不再有上报) */
void tmx_audio_set_mic_reporting(bool enable);

bool tmx_audio_mic_reporting(void);

/* 最近一次采集的麦克风响度, 0 ~ 100 */
int tmx_audio_mic_level(void);
