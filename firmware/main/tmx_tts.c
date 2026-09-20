/*
 * 中文语音合成 (TTS) 实现
 *
 * 数据流:
 *   PC/Scratch 文字 (UTF-8) ──► 0x75 命令 ──► tmx_tts_begin()
 *        └─ 音频任务: tmx_tts_read() ──► esp_tts_stream_play() ──► 16kHz PCM
 *                       └─ tmx_audio.c 升采样 3 倍 ──► I2S ──► ES8311 ──► 喇叭
 *
 * 线程模型: begin/stop 由协议任务调用, read 只由音频任务调用, 中间用互斥锁保护。
 */

#include "tmx_tts.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if !CONFIG_TMX_TTS_ENABLE

esp_err_t tmx_tts_init(void) { return ESP_OK; }
bool tmx_tts_is_ready(void) { return false; }
esp_err_t tmx_tts_begin(const char *text, int speed)
{
    (void)text; (void)speed;
    return ESP_ERR_NOT_SUPPORTED;
}
void tmx_tts_stop(void) { }
bool tmx_tts_is_active(void) { return false; }
int tmx_tts_read(int16_t *dst, int max_samples) { (void)dst; (void)max_samples; return 0; }

#else

#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tmx_tts";

static esp_tts_handle_t           s_tts;
static esp_tts_voice_t           *s_voice;
static esp_partition_mmap_handle_t s_map_handle;
static SemaphoreHandle_t          s_lock;

static const int16_t *s_chunk;        /* 当前这一块 PCM */
static int            s_chunk_left;   /* 还剩多少采样 */
static int            s_speed;
static volatile bool  s_ready;
static volatile bool  s_active;

/* 待处理的朗读请求 (协议任务写好, 音频任务取走) */
static char          s_req_text[CONFIG_TMX_TTS_TEXT_MAX];
static int           s_req_speed;
static volatile bool s_req_pending;
static volatile bool s_req_stop;

esp_err_t tmx_tts_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, CONFIG_TMX_TTS_PARTITION);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到音色数据分区 \"%s\" —— 烧录时是不是没写 voice_data? "
                      "(idf.py flash 会自动带上, 手工烧录要加 --partition-name voice_data)",
                 CONFIG_TMX_TTS_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }

    const void *map_data = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                       &map_data, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音色数据 mmap 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_voice = esp_tts_voice_set_init(&esp_tts_voice_template, (void *)map_data);
    if (s_voice == NULL) {
        ESP_LOGE(TAG, "音色数据不合法 (voice_set_init 失败)");
        return ESP_FAIL;
    }
    s_tts = esp_tts_create(s_voice);
    if (s_tts == NULL) {
        ESP_LOGE(TAG, "TTS 实例创建失败 (内存不够?)");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    /* 引擎内部会把每个音节都打成 INFO 日志, 串口会很吵, 压到 WARN */
    esp_log_level_set("tts_parser", ESP_LOG_WARN);
    ESP_LOGI(TAG, "esp-tts ready: 音色数据 %u 字节 (分区 %s), 输出 16kHz/16bit 单声道",
             (unsigned)part->size, CONFIG_TMX_TTS_PARTITION);
    return ESP_OK;
}

bool tmx_tts_is_ready(void)
{
    return s_ready;
}

esp_err_t tmx_tts_begin(const char *utf8_text, int speed)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (utf8_text == NULL || utf8_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (speed < 0) {
        speed = 0;
    } else if (speed > 5) {
        speed = 5;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_req_text, utf8_text, sizeof(s_req_text) - 1);
    s_req_text[sizeof(s_req_text) - 1] = '\0';
    s_req_speed = speed;
    s_req_stop = false;
    s_req_pending = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void tmx_tts_stop(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req_pending = false;
    s_req_stop = true;
    xSemaphoreGive(s_lock);
}

bool tmx_tts_is_active(void)
{
    return s_active;
}

static void tts_start(const char *text, int speed)
{
    esp_tts_stream_reset(s_tts);
    s_chunk = NULL;
    s_chunk_left = 0;
    if (!esp_tts_parse_chinese(s_tts, text)) {
        /* 生僻字/纯英文/空串都可能解析不出来 */
        ESP_LOGW(TAG, "解析失败, 这段文字念不出来: %.48s", text);
        s_active = false;
        return;
    }
    s_speed = speed;
    s_active = true;
    ESP_LOGI(TAG, "开始朗读 (语速 %d): %.48s", speed, text);
}

static void tts_end(void)
{
    if (s_active) {
        ESP_LOGI(TAG, "朗读结束");
    }
    s_active = false;
    s_chunk = NULL;
    s_chunk_left = 0;
    if (s_tts) {
        esp_tts_stream_reset(s_tts);
    }
}

int tmx_tts_read(int16_t *dst, int max_samples)
{
    if (dst == NULL || max_samples <= 0) {
        return 0;
    }

    bool want_stop = false;
    bool want_start = false;
    char start_text[CONFIG_TMX_TTS_TEXT_MAX];
    int start_speed = s_speed;

    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        if (s_req_stop) {
            s_req_stop = false;
            want_stop = true;
        }
        if (s_req_pending) {
            s_req_pending = false;
            strncpy(start_text, s_req_text, sizeof(start_text) - 1);
            start_text[sizeof(start_text) - 1] = '\0';
            start_speed = s_req_speed;
            want_start = true;
        }
        xSemaphoreGive(s_lock);
    }

    if (want_stop) {
        tts_end();
    }
    if (want_start) {
        tts_start(start_text, start_speed);
    }
    if (!s_active) {
        return 0;
    }

    int copied = 0;
    while (copied < max_samples) {
        if (s_chunk_left <= 0) {
            int len = 0;
            int16_t *data = (int16_t *)esp_tts_stream_play(s_tts, &len, s_speed);
            if (data == NULL || len <= 0) {
                tts_end();
                break;
            }
            s_chunk = data;
            s_chunk_left = len;
        }

        int n = s_chunk_left;
        if (n > max_samples - copied) {
            n = max_samples - copied;
        }
        memcpy(dst + copied, s_chunk, (size_t)n * sizeof(int16_t));
        s_chunk += n;
        s_chunk_left -= n;
        copied += n;
    }
    return copied;
}

#endif /* CONFIG_TMX_TTS_ENABLE */
