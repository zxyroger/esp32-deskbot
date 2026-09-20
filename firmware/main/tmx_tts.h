/*
 * 中文语音合成 (TTS) — Espressif esp-tts (esp-sr 组件里的 esp_tts_chinese)
 *
 * 输出固定是 16kHz / 16bit / 单声道 PCM, 由 tmx_audio.c 升采样到 48kHz 立体声
 * 后喂给 ES8311 播放。
 *
 * 音色数据 (xiaole, 2.9MB) 放在 flash 的 voice_data 分区, 运行时 mmap,
 * 既不占 RAM, 也不用把数据编进 app 镜像。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 初始化: 找 voice_data 分区 -> mmap -> 建 voice set 和 TTS 实例 */
esp_err_t tmx_tts_init(void);

bool tmx_tts_is_ready(void);

/* 请求朗读一段 UTF-8 文字 (非阻塞, 由音频任务真正合成/播放)。
 * speed: 0(最慢) ~ 5(最快)。会打断当前正在朗读的内容。 */
esp_err_t tmx_tts_begin(const char *utf8_text, int speed);

/* 停止朗读, 丢弃剩余内容 */
void tmx_tts_stop(void);

bool tmx_tts_is_active(void);

/* 取最多 max_samples 个 16kHz/16bit/单声道采样 (只由音频任务调用)。
 * 返回实际拿到的采样数, 0 表示这一段已经读完。 */
int tmx_tts_read(int16_t *dst, int max_samples);
