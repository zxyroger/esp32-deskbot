/*
 * ES8311 音频输入/输出实现
 *
 * 数据通路 (全双工, 48kHz / 16bit / 立体声):
 *
 *   Scratch 积木 ──► 0x72 AUDIO_TONE ──► 音频任务 ──正弦波──► I2S TX ──► ES8311 DAC ──► PA ──► 喇叭
 *   Scratch 积木 ◄── 0x0D AUDIO_LEVEL ◄─ 响度 ◄── I2S RX ◄── ES8311 ADC ◄─ 麦克风
 *
 * 说明:
 *  - codec 由 esp_codec_dev 的 ES8311 驱动初始化 (寄存器序列与板级配置一致)。
 *  - 放音结束只是把 DAC 静音 (mute), codec 一直保持打开:
 *    esp_codec_dev 这一版 close() 之后内部 is_open 变 false, 不能再 open。
 *  - 麦克风数据每 10ms 取一块, 既算响度也避免 RX 的 DMA 缓冲积压。
 */

#include "tmx_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tmx_i2c.h"
#include "tmx_tts.h"

#if !CONFIG_TMX_AUDIO_ENABLE

/* 没打开音频时给出空实现, 上层函数不用到处写 #if */
esp_err_t tmx_audio_init(void) { return ESP_OK; }
bool tmx_audio_is_ready(void) { return false; }
esp_err_t tmx_audio_play_tone(int f, int d, int v)
{
    (void)f; (void)d; (void)v;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t tmx_audio_say_text(const char *text, int speed)
{
    (void)text; (void)speed;
    return ESP_ERR_NOT_SUPPORTED;
}
void tmx_audio_stop(void) { }
void tmx_audio_set_mic_reporting(bool enable) { (void)enable; }
bool tmx_audio_mic_reporting(void) { return false; }
int tmx_audio_mic_level(void) { return 0; }
bool tmx_audio_tts_active(void) { return false; }
void tmx_audio_set_mirror(bool enable) { (void)enable; }
bool tmx_audio_mirror_on(void) { return false; }
int tmx_audio_mirror_read(uint8_t *dst, int max_len) { (void)dst; (void)max_len; return 0; }
bool tmx_audio_mirror_end_pending(void) { return false; }
void tmx_audio_mirror_clear_end(void) { }

#else

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "tmx_audio";

#define AUDIO_FRAMES      480                       /* 10ms @48kHz 的帧数 */
#define AUDIO_FRAME_BYTES 4                         /* 16bit × 2 声道 */
#define AUDIO_BLOCK_BYTES (AUDIO_FRAMES * AUDIO_FRAME_BYTES)

#define TTS_FRAMES        (AUDIO_FRAMES / 3)         /* TTS 是 16kHz, 48k/16k = 3 倍 */
#define MIRROR_BUF_BYTES  (CONFIG_TMX_TTS_MIRROR_MAX_KB * 1024)

#define SINE_POINTS       256
#define SINE_PEAK         30000                     /* 留一点余量, 避免削顶 */

enum {
    AUDIO_SRC_NONE = 0,
    AUDIO_SRC_TONE,
    AUDIO_SRC_TTS,
};

static i2s_chan_handle_t      s_tx;
static i2s_chan_handle_t      s_rx;
static esp_codec_dev_handle_t s_codec;
static TaskHandle_t           s_task;
static int16_t               *s_block;              /* 收发共用的 10ms 缓冲 */
static int16_t                s_sine[SINE_POINTS];
static int16_t                s_tts_buf[TTS_FRAMES];/* 从 TTS 取来的 10ms 单声道 */
static int16_t                s_tts_last;           /* 上一块的最后一个采样, 升采样用 */

static volatile bool     s_ready;
static volatile int      s_src;                     /* AUDIO_SRC_xxx */
static volatile bool     s_tone_active;
static volatile int      s_tone_freq;
static volatile int      s_tone_volume;
static volatile uint32_t s_tone_frames_left;
static volatile bool     s_mic_on;
static volatile int      s_mic_level;

/* 「把合成的语音回传一份给 PC」用的环形缓冲 (音频任务写, 协议任务读) */
static uint8_t           s_mirror_buf[MIRROR_BUF_BYTES];
static volatile int      s_mirror_head;
static volatile int      s_mirror_tail;
static volatile bool     s_mirror_on;
static volatile bool     s_mirror_end;
static volatile uint32_t s_mirror_drops;

/* 初始化失败时把 I2S 收回来, 免得 codec 没应答还一直占着 5 个引脚 */
static void audio_i2s_close(void)
{
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

static i2s_mclk_multiple_t mclk_multiple(void)
{
    switch (CONFIG_TMX_AUDIO_MCLK_MULTIPLE) {
        case 128: return I2S_MCLK_MULTIPLE_128;
        case 384: return I2S_MCLK_MULTIPLE_384;
        case 512: return I2S_MCLK_MULTIPLE_512;
        default:  return I2S_MCLK_MULTIPLE_256;
    }
}

/* 把 10ms 麦克风数据换算成 0~100 的响度 (dBFS 线性映射: -60dB → 0, 0dB → 100) */
static void update_mic_level(void)
{
    const int channel = CONFIG_TMX_AUDIO_MIC_CHANNEL;
    int64_t sum = 0;

    for (int i = 0; i < AUDIO_FRAMES; i++) {
        int32_t sample = s_block[i * 2 + channel];
        sum += (int64_t)sample * sample;
    }

    float rms = sqrtf((float)(sum / AUDIO_FRAMES));
    float db = 20.0f * log10f(rms / 32768.0f + 1e-9f);
    int level = (int)((db + 60.0f) * (100.0f / 60.0f) + 0.5f);
    if (level < 0) {
        level = 0;
    } else if (level > 100) {
        level = 100;
    }

    /* 上升快一点、回落慢一点, 读数不至于跳得厉害 */
    int old = s_mic_level;
    s_mic_level = (level > old) ? (level * 2 + old) / 3 : (level + old * 3) / 4;
}

/* ---------- TTS 回传 (给 PC 存 wav / 验证用) ---------- */

static int mirror_space(void)
{
    int head = s_mirror_head;
    int tail = s_mirror_tail;
    return (head >= tail) ? (MIRROR_BUF_BYTES - 1 - head + tail) : (tail - 1 - head);
}

static void mirror_push(const int16_t *samples, int count)
{
    if (!s_mirror_on || count <= 0) {
        return;
    }
    const uint8_t *src = (const uint8_t *)samples;
    int bytes = count * 2;
    int done = 0;

    while (done < bytes) {
        int space = mirror_space();
        if (space <= 0) {
            s_mirror_drops++;      /* PC 端取不过来就丢, 不阻塞放音 */
            return;
        }
        int chunk = bytes - done;
        if (chunk > space) {
            chunk = space;
        }
        int to_end = MIRROR_BUF_BYTES - s_mirror_head;
        if (chunk > to_end) {
            chunk = to_end;
        }
        memcpy(&s_mirror_buf[s_mirror_head], src + done, (size_t)chunk);
        s_mirror_head = (s_mirror_head + chunk) % MIRROR_BUF_BYTES;
        done += chunk;
    }
}

/* 16kHz 单声道 -> 48kHz 立体声 (线性插值 3 倍升采样) */
static int fill_tts_block(int count)
{
    for (int i = 0; i < count; i++) {
        int16_t a = s_tts_buf[i];
        int16_t b = (i + 1 < count) ? s_tts_buf[i + 1] : s_tts_last;
        int16_t s0 = a;
        int16_t s1 = (int16_t)((2 * (int32_t)a + b) / 3);
        int16_t s2 = (int16_t)(((int32_t)a + 2 * (int32_t)b) / 3);
        int o = i * 3;
        s_block[(o + 0) * 2] = s0;
        s_block[(o + 0) * 2 + 1] = s0;
        s_block[(o + 1) * 2] = s1;
        s_block[(o + 1) * 2 + 1] = s1;
        s_block[(o + 2) * 2] = s2;
        s_block[(o + 2) * 2 + 1] = s2;
    }
    if (count > 0) {
        s_tts_last = s_tts_buf[count - 1];
    }
    return count * 3;
}

static void audio_task(void *arg)
{
    (void)arg;
    uint32_t phase = 0;
    bool was_playing = false;

    while (1) {
        if (!s_ready) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (esp_codec_dev_read(s_codec, s_block, AUDIO_BLOCK_BYTES) == 0 && s_mic_on) {
            update_mic_level();
        }

        /* ---- 语音合成 (TTS): 取 10ms 单声道, 升采样成 10ms 立体声写出去 ---- */
        if (s_src == AUDIO_SRC_TTS) {
            if (!was_playing) {
                esp_codec_dev_set_out_vol(s_codec, CONFIG_TMX_TTS_VOLUME);
                esp_codec_dev_set_out_mute(s_codec, false);
                was_playing = true;
                s_tts_last = 0;
            }

            int got = tmx_tts_read(s_tts_buf, TTS_FRAMES);
            if (got <= 0) {
                s_src = AUDIO_SRC_NONE;
                if (was_playing) {
                    esp_codec_dev_set_out_mute(s_codec, true);
                    was_playing = false;
                }
                if (s_mirror_on) {
                    if (s_mirror_drops > 0) {
                        ESP_LOGW(TAG, "TTS 回传丢掉了 %u 字节 (PC 端没跟上)",
                                 (unsigned)s_mirror_drops);
                        s_mirror_drops = 0;
                    }
                    s_mirror_end = true;
                }
                continue;
            }

            mirror_push(s_tts_buf, got);
            int out_frames = fill_tts_block(got);
            esp_codec_dev_write(s_codec, s_block, out_frames * AUDIO_FRAME_BYTES);
            continue;
        }

        if (s_src == AUDIO_SRC_TONE && s_tone_frames_left > 0) {
            if (!was_playing) {
                /* 每次放音前重新应用一遍音量, 再取消静音 */
                esp_codec_dev_set_out_vol(s_codec, CONFIG_TMX_AUDIO_VOLUME);
                esp_codec_dev_set_out_mute(s_codec, false);
                was_playing = true;
                phase = 0;
            }

            uint32_t step = (uint32_t)(((uint64_t)s_tone_freq * SINE_POINTS << 16) /
                                       CONFIG_TMX_AUDIO_SAMPLE_RATE);
            int volume = s_tone_volume;
            for (int i = 0; i < AUDIO_FRAMES; i++) {
                int16_t sample = (int16_t)((int32_t)s_sine[(phase >> 16) & (SINE_POINTS - 1)] *
                                           volume / 100);
                s_block[i * 2] = sample;
                s_block[i * 2 + 1] = sample;
                phase += step;
            }

            uint32_t frames = s_tone_frames_left;
            if (frames > AUDIO_FRAMES) {
                frames = AUDIO_FRAMES;
            }
            esp_codec_dev_write(s_codec, s_block, (int)(frames * AUDIO_FRAME_BYTES));
            s_tone_frames_left -= frames;
            if (s_tone_frames_left == 0) {
                s_tone_active = false;
                s_src = AUDIO_SRC_NONE;
            }
            continue;
        }

        if (was_playing) {
            /* 播完了: 只静音 DAC, codec 保持打开 (close 之后没法再 open) */
            esp_codec_dev_set_out_mute(s_codec, true);
            was_playing = false;
        }
    }
}

esp_err_t tmx_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* ---------- 1. I2S: 一路全双工 (TX 给扬声器, RX 收麦克风) ---------- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* 没数据写时自动补 0, 喇叭静音而不是循环旧数据 */

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 通道创建失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_TMX_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)CONFIG_TMX_AUDIO_I2S_MCLK_PIN,
            .bclk = (gpio_num_t)CONFIG_TMX_AUDIO_I2S_BCLK_PIN,
            .ws   = (gpio_num_t)CONFIG_TMX_AUDIO_I2S_WS_PIN,
            .dout = (gpio_num_t)CONFIG_TMX_AUDIO_I2S_DOUT_PIN,
            .din  = (gpio_num_t)CONFIG_TMX_AUDIO_I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = mclk_multiple();

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败: %s", esp_err_to_name(err));
        goto fail;
    }
    /* 先使能起来: codec 驱动配置采样格式前会先 disable 一次, 没使能过就会打
     * "the channel has not been enabled yet" 的报错日志 (功能不受影响, 但很吵)。 */
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 使能失败: %s", esp_err_to_name(err));
        goto fail;
    }

    /* ---------- 2. I2C 总线 + ES8311 codec ---------- */
    err = tmx_i2c_begin(0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 初始化失败, 无法配置 codec: %s", esp_err_to_name(err));
        goto fail;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,                                  /* 与 tmx_i2c 里的 I2C_NUM_0 对应 */
        .addr = CONFIG_TMX_AUDIO_I2C_ADDR,          /* 8 位写法, 驱动内部会 >>1 */
        .bus_handle = tmx_i2c_get_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "codec I2C 控制接口创建失败 (总线 0x%02X)",
                 (unsigned)CONFIG_TMX_AUDIO_I2C_ADDR);
        err = ESP_FAIL;
        goto fail;
    }

    audio_codec_i2s_cfg_t i2s_if_cfg = {
        .port = 0,                                  /* 与 I2S_NUM_0 对应 */
        .rx_handle = s_rx,
        .tx_handle = s_tx,
        .clk_src = 0,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (data_if == NULL || gpio_if == NULL) {
        ESP_LOGE(TAG, "codec 数据/GPIO 接口创建失败");
        err = ESP_FAIL;
        goto fail;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,  /* DAC + ADC 都要 */
        .pa_pin = CONFIG_TMX_AUDIO_PA_PIN,
        .pa_reverted = (CONFIG_TMX_AUDIO_PA_ACTIVE_LEVEL == 0),
        .master_mode = false,                        /* ESP32-S3 出时钟, codec 做从机 */
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .no_dac_ref = !CONFIG_TMX_AUDIO_ADC_DAC_REF, /* false = ADCL 麦克风 + DACR 回采 */
        .mclk_div = CONFIG_TMX_AUDIO_MCLK_MULTIPLE,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
            .pa_gain = (float)CONFIG_TMX_AUDIO_PA_GAIN_DB,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "ES8311 没有应答 (I2C 地址 0x%02X), 检查接线/上电",
                 (unsigned)CONFIG_TMX_AUDIO_I2C_ADDR);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == NULL) {
        ESP_LOGE(TAG, "codec 设备创建失败");
        err = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,                        /* 左右两路都要 */
        .sample_rate = CONFIG_TMX_AUDIO_SAMPLE_RATE,
        .mclk_multiple = CONFIG_TMX_AUDIO_MCLK_MULTIPLE,
    };
    int rc = esp_codec_dev_open(s_codec, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec 打开失败 (%d)", rc);
        err = ESP_FAIL;
        goto fail;
    }
    esp_codec_dev_set_out_vol(s_codec, CONFIG_TMX_AUDIO_VOLUME);
    esp_codec_dev_set_out_mute(s_codec, true);       /* 开机静音, 有音调再放 */
    esp_codec_dev_set_in_gain(s_codec, (float)CONFIG_TMX_AUDIO_MIC_GAIN_DB);

    /* ---------- 3. 正弦表 + 音频任务 ---------- */
    for (int i = 0; i < SINE_POINTS; i++) {
        s_sine[i] = (int16_t)(SINE_PEAK * sinf(2.0f * (float)M_PI * (float)i / (float)SINE_POINTS));
    }
    s_block = malloc(AUDIO_BLOCK_BYTES);
    if (s_block == NULL) {
        ESP_LOGE(TAG, "音频缓冲申请失败");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_ready = true;
    if (xTaskCreate(audio_task, "tmx_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        s_ready = false;
        ESP_LOGE(TAG, "音频任务创建失败");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* TTS 是可选的: 音色数据分区没烧 / 内存不够都只影响朗读积木 */
    if (tmx_tts_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音合成 (TTS) 不可用: 朗读积木会被忽略");
    }

    ESP_LOGI(TAG, "ES8311 ready: I2S0 %dHz/16bit/2ch (MCLK=%d BCLK=%d WS=%d DIN=%d DOUT=%d), "
                  "PA=%d(active %d), I2C addr=0x%02X",
             CONFIG_TMX_AUDIO_SAMPLE_RATE,
             CONFIG_TMX_AUDIO_I2S_MCLK_PIN, CONFIG_TMX_AUDIO_I2S_BCLK_PIN,
             CONFIG_TMX_AUDIO_I2S_WS_PIN, CONFIG_TMX_AUDIO_I2S_DIN_PIN,
             CONFIG_TMX_AUDIO_I2S_DOUT_PIN,
             CONFIG_TMX_AUDIO_PA_PIN, CONFIG_TMX_AUDIO_PA_ACTIVE_LEVEL,
             (unsigned)CONFIG_TMX_AUDIO_I2C_ADDR);
    return ESP_OK;

fail:
    s_ready = false;
    if (s_codec) {
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    audio_i2s_close();
    return err;
}

bool tmx_audio_is_ready(void)
{
    return s_ready;
}

esp_err_t tmx_audio_play_tone(int freq_hz, int duration_ms, int volume_percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz < 20) {
        freq_hz = 20;
    }
    int max_freq = CONFIG_TMX_AUDIO_SAMPLE_RATE * 45 / 100;   /* 别越过奈奎斯特 */
    if (freq_hz > max_freq) {
        freq_hz = max_freq;
    }
    if (duration_ms < 1) {
        duration_ms = 1;
    } else if (duration_ms > 60000) {
        duration_ms = 60000;
    }
    if (volume_percent < 0) {
        volume_percent = 0;
    } else if (volume_percent > 100) {
        volume_percent = 100;
    }

    uint32_t frames = (uint32_t)((int64_t)CONFIG_TMX_AUDIO_SAMPLE_RATE * duration_ms / 1000);
    if (frames == 0) {
        frames = 1;
    }

    s_tone_freq = freq_hz;
    s_tone_volume = volume_percent;
    s_tone_frames_left = frames;
    s_tone_active = true;
    s_src = AUDIO_SRC_TONE;
    tmx_tts_stop();                       /* 放音调时不再念文字 */
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

esp_err_t tmx_audio_say_text(const char *text, int speed)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!tmx_tts_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = tmx_tts_begin(text, speed);
    if (err == ESP_OK) {
        s_tone_active = false;            /* 朗读优先, 音调让位 */
        s_tone_frames_left = 0;
        s_src = AUDIO_SRC_TTS;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
    return err;
}

void tmx_audio_stop(void)
{
    s_tone_active = false;
    s_tone_frames_left = 0;
    s_src = AUDIO_SRC_NONE;
    tmx_tts_stop();
}

void tmx_audio_set_mic_reporting(bool enable)
{
    s_mic_on = enable;
    if (!enable) {
        s_mic_level = 0;
    }
}

bool tmx_audio_mic_reporting(void)
{
    return s_mic_on;
}

int tmx_audio_mic_level(void)
{
    return s_mic_level;
}

bool tmx_audio_tts_active(void)
{
    return tmx_tts_is_active();
}

/* ---------- TTS 回传 (PC 端存 wav / 验证用) ---------- */

void tmx_audio_set_mirror(bool enable)
{
    s_mirror_on = false;                  /* 先停, 避免读到半清理的状态 */
    s_mirror_head = 0;
    s_mirror_tail = 0;
    s_mirror_end = false;
    s_mirror_drops = 0;
    s_mirror_on = enable;
}

bool tmx_audio_mirror_on(void)
{
    return s_mirror_on;
}

int tmx_audio_mirror_read(uint8_t *dst, int max_len)
{
    if (!s_mirror_on || dst == NULL || max_len <= 0) {
        return 0;
    }
    int n = 0;
    while (n < max_len && s_mirror_tail != s_mirror_head) {
        dst[n++] = s_mirror_buf[s_mirror_tail];
        s_mirror_tail = (s_mirror_tail + 1) % MIRROR_BUF_BYTES;
    }
    return n;
}

bool tmx_audio_mirror_end_pending(void)
{
    return s_mirror_end && (s_mirror_tail == s_mirror_head);
}

void tmx_audio_mirror_clear_end(void)
{
    s_mirror_end = false;
}

#endif /* CONFIG_TMX_AUDIO_ENABLE */
