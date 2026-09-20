/*
 * 硬件抽象层实现: GPIO / ADC / PWM / 舵机 / 超声波
 *
 * 说明:
 *  - ESP32-S3 只有低速 LEDC (8 通道 / 4 定时器), 因此 PWM 与舵机共用通道池;
 *    定时器 0~2 给普通 PWM (最多 3 种不同的 频率/分辨率 组合),
 *    定时器 3 固定给舵机 (50Hz / 14bit)。
 *  - ADC 使用 ADC1 (GPIO1~GPIO10)。
 *  - 超声波用 GPIO 软件计时 (与 Arduino Ultrasonic 库行为一致)。
 */

#include "tmx_io.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "tmx_protocol.h"

static const char *TAG = "tmx_io";

#define TMX_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define TMX_LEDC_MAX_SLOTS  8
/* PWM 定时器候选: 板载外设 (LCD 背光 / 摄像头 XCLK) 占掉的不参与分配。
 * 舵机固定用 LEDC_TIMER_3, 所以候选里没有它。 */
#define TMX_PWM_TIMER_CANDIDATES 3
#define TMX_SERVO_TIMER     LEDC_TIMER_3
#define TMX_SERVO_RES_BITS  14
#define TMX_SERVO_FREQ_HZ   50
#define TMX_SONAR_TIMEOUT_US 25000  /* 单次等待回波的最长时间 */

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int             pin;         /* -1 表示空闲 */
    ledc_channel_t  channel;
    uint8_t         resolution;  /* 占空比位数 */
    bool            is_servo;
    uint16_t        min_pulse;
    uint16_t        max_pulse;
} ledc_slot_t;

typedef struct {
    bool     used;
    uint32_t freq_hz;
    uint8_t  resolution;
} pwm_timer_t;

static ledc_slot_t  s_slots[TMX_LEDC_MAX_SLOTS];
static pwm_timer_t  s_timers[TMX_PWM_TIMER_CANDIDATES];
/* 可用的 PWM 定时器 (板载外设占用的已在 init 里剔除), 只用到前 s_timer_count 个 */
static ledc_timer_t s_timer_ids[TMX_PWM_TIMER_CANDIDATES];
static int          s_timer_count;
static const ledc_timer_t s_timer_candidates[TMX_PWM_TIMER_CANDIDATES] = {
    LEDC_TIMER_0, LEDC_TIMER_1, LEDC_TIMER_2
};

static adc_oneshot_unit_handle_t s_adc1;
static bool s_adc1_ready;
static bool s_adc_chan_ready[10];

/* ------------------------------------------------------------------ */
/* 引脚检查                                                            */
/* ------------------------------------------------------------------ */

bool tmx_pin_is_valid(int pin)
{
    return (pin >= 0) && (pin < TMX_MAX_PINS) && (pin <= 48);
}

bool tmx_pin_is_reserved(int pin)
{
#if CONFIG_TMX_LCD_ENABLE
    /* ILI9341 屏幕占用的引脚属于板载外设, 不再给 Scratch 使用 */
    if (pin == CONFIG_TMX_LCD_SPI_SCK_PIN || pin == CONFIG_TMX_LCD_SPI_MOSI_PIN ||
        pin == CONFIG_TMX_LCD_DC_PIN || pin == CONFIG_TMX_LCD_CS_PIN ||
        pin == CONFIG_TMX_LCD_BACKLIGHT_PIN) {
        return true;
    }
#endif
#if CONFIG_TMX_AUDIO_ENABLE
    /* ES8311 音频占用的 I2S / PA 引脚同理 (I2C 是共用总线, 不在这里排除) */
    if (pin == CONFIG_TMX_AUDIO_I2S_MCLK_PIN || pin == CONFIG_TMX_AUDIO_I2S_BCLK_PIN ||
        pin == CONFIG_TMX_AUDIO_I2S_WS_PIN || pin == CONFIG_TMX_AUDIO_I2S_DOUT_PIN ||
        pin == CONFIG_TMX_AUDIO_I2S_DIN_PIN || pin == CONFIG_TMX_AUDIO_PA_PIN) {
        return true;
    }
#endif
#if CONFIG_TMX_CAMERA_ENABLE
    /* OV2640 的 DVP 引脚 (XCLK 也算: 它是摄像头驱动的输出, 不能给 Scratch 用)。
     * SCCB 的 IO1/IO2 是共用 I2C 总线, 和音频一样不在这里排除。 */
    if (pin == CONFIG_TMX_CAMERA_XCLK_PIN || pin == CONFIG_TMX_CAMERA_PCLK_PIN ||
        pin == CONFIG_TMX_CAMERA_VSYNC_PIN || pin == CONFIG_TMX_CAMERA_HREF_PIN ||
        pin == CONFIG_TMX_CAMERA_D0_PIN || pin == CONFIG_TMX_CAMERA_D1_PIN ||
        pin == CONFIG_TMX_CAMERA_D2_PIN || pin == CONFIG_TMX_CAMERA_D3_PIN ||
        pin == CONFIG_TMX_CAMERA_D4_PIN || pin == CONFIG_TMX_CAMERA_D5_PIN ||
        pin == CONFIG_TMX_CAMERA_D6_PIN || pin == CONFIG_TMX_CAMERA_D7_PIN ||
        (CONFIG_TMX_CAMERA_PWDN_PIN >= 0 && pin == CONFIG_TMX_CAMERA_PWDN_PIN) ||
        (CONFIG_TMX_CAMERA_RESET_PIN >= 0 && pin == CONFIG_TMX_CAMERA_RESET_PIN)) {
        return true;
    }
#endif
#if CONFIG_TMX_ALLOW_RESERVED_PINS
    (void)pin;
    return false;
#else
    /* 0        : Strapping (BOOT)
     * 26 ~ 37  : 模组内 SPI Flash / PSRAM
     * 45 / 46  : Strapping
     */
    if (pin == 0) {
        return true;
    }
    if (pin >= 26 && pin <= 37) {
        return true;
    }
    if (pin == 45 || pin == 46) {
        return true;
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_CDC
    /* 原生 USB 口被用作调试串口, D-/D+ 不能当普通 IO */
    if (pin == 19 || pin == 20) {
        return true;
    }
#endif
    return false;
#endif
}

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

/* 板载外设占用的 LEDC 定时器: LCD 背光 (TIMER_2) / 摄像头 XCLK (menuconfig 选的) */
static bool ledc_timer_is_reserved(int timer)
{
#if CONFIG_TMX_LCD_ENABLE && CONFIG_TMX_LCD_BACKLIGHT_PWM
    if (timer == 2) {
        return true;
    }
#endif
#if CONFIG_TMX_CAMERA_ENABLE
    if (timer == CONFIG_TMX_CAMERA_XCLK_LEDC_TIMER) {
        return true;
    }
#endif
    return false;
}

/* 板载外设占用的 LEDC 通道: 同理, 不允许分给 Scratch 的 PWM/舵机 */
static bool ledc_channel_is_reserved(int channel)
{
#if CONFIG_TMX_LCD_ENABLE && CONFIG_TMX_LCD_BACKLIGHT_PWM
    if (channel == CONFIG_TMX_LCD_BACKLIGHT_LEDC_CHANNEL) {
        return true;
    }
#endif
#if CONFIG_TMX_CAMERA_ENABLE
    if (channel == CONFIG_TMX_CAMERA_XCLK_LEDC_CHANNEL) {
        return true;
    }
#endif
    return false;
}

esp_err_t tmx_io_init(void)
{
    for (int i = 0; i < TMX_LEDC_MAX_SLOTS; i++) {
        s_slots[i].pin = -1;
        s_slots[i].channel = (ledc_channel_t)i;
        s_slots[i].is_servo = false;
    }
    memset(s_timers, 0, sizeof(s_timers));

    /* 把板载外设占掉的定时器从 PWM 池里剔除, 剩下的给 Scratch */
    s_timer_count = 0;
    for (int i = 0; i < TMX_PWM_TIMER_CANDIDATES; i++) {
        if (!ledc_timer_is_reserved((int)s_timer_candidates[i])) {
            s_timer_ids[s_timer_count++] = s_timer_candidates[i];
        }
    }
    if (s_timer_count == 0) {
        ESP_LOGE(TAG, "没有可用的 PWM 定时器 (板载外设把 0~2 都占了)");
    } else if (s_timer_count < TMX_PWM_TIMER_CANDIDATES) {
        ESP_LOGI(TAG, "PWM 定时器池: %d 组 (板载外设占掉 %d 个)", s_timer_count,
                 TMX_PWM_TIMER_CANDIDATES - s_timer_count);
    }

    s_adc1_ready = false;
    memset(s_adc_chan_ready, 0, sizeof(s_adc_chan_ready));
    return ESP_OK;
}

static esp_err_t check_pin(int pin, const char *what)
{
    if (!tmx_pin_is_valid(pin)) {
        ESP_LOGW(TAG, "%s: invalid pin %d", what, pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (tmx_pin_is_reserved(pin)) {
        ESP_LOGW(TAG, "%s: pin %d is reserved (flash/PSRAM/strapping/onboard LCD), ignored",
                 what, pin);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t tmx_gpio_input(int pin, bool pullup, bool pulldown)
{
    esp_err_t err = check_pin(pin, "digital input");
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << pin,
        .pull_down_en = pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t tmx_gpio_output(int pin)
{
    esp_err_t err = check_pin(pin, "digital output");
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = 1ULL << pin,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t tmx_gpio_write(int pin, int level)
{
    esp_err_t err = check_pin(pin, "digital write");
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

int tmx_gpio_read(int pin)
{
    if (!tmx_pin_is_valid(pin)) {
        return 0;
    }
    return gpio_get_level((gpio_num_t)pin) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* ADC (ADC1: GPIO1 ~ GPIO10)                                          */
/* ------------------------------------------------------------------ */

int tmx_adc_gpio_for_pin(int pin)
{
    /* ESP32-S3 的 ADC1_CH0 ~ ADC1_CH9 对应 GPIO1 ~ GPIO10 */
    if (pin >= 1 && pin <= 10) {
        return pin;
    }

#if CONFIG_TMX_ADC_LEGACY_PIN_ALIAS
    /* Scratch (s3-extend / onegpioEsp32) 的模拟输入下拉框只有经典 ESP32 的
     * ADC 引脚 32/33/34/35/36/39, 这里按顺序映射到 GPIO1 ~ GPIO6。
     * 上报时仍然使用请求的引脚号, 因此积木侧无感。 */
    switch (pin) {
        case 32: return 1;
        case 33: return 2;
        case 34: return 3;
        case 35: return 4;
        case 36: return 5;
        case 39: return 6;
        default: break;
    }
#endif

    return -1;
}

int tmx_adc_channel_for_pin(int pin)
{
    int gpio = tmx_adc_gpio_for_pin(pin);
    if (gpio < 0) {
        return -1;
    }
    /* 板载外设 (屏幕 / 音频 / 摄像头) 占用的引脚不再做模拟输入 */
    if (tmx_pin_is_reserved(gpio)) {
        return -1;
    }
    return gpio - 1;
}

esp_err_t tmx_adc_configure(int pin)
{
    int channel = tmx_adc_channel_for_pin(pin);
    if (channel < 0) {
        ESP_LOGW(TAG, "analog input: pin %d 没有 ADC 或被板载外设占用 "
                      "(可用 32..39 别名 / GPIO1..10, 见 docs/pins-esp32s3.md)", pin);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_adc1_ready) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC1 init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_adc1_ready = true;
    }

    if (!s_adc_chan_ready[channel]) {
        /* 与 Arduino ESP32 Core 默认一致: 12bit, 12dB 衰减 (约 0~3.1V) */
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        esp_err_t err = adc_oneshot_config_channel(s_adc1, (adc_channel_t)channel, &chan_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC channel %d config failed: %s", channel, esp_err_to_name(err));
            return err;
        }
        s_adc_chan_ready[channel] = true;
    }

    return ESP_OK;
}

int tmx_adc_read(int pin)
{
    int channel = tmx_adc_channel_for_pin(pin);
    int raw = 0;

    if (channel < 0 || !s_adc1_ready || !s_adc_chan_ready[channel]) {
        return 0;
    }
    if (adc_oneshot_read(s_adc1, (adc_channel_t)channel, &raw) != ESP_OK) {
        return 0;
    }
    return raw;
}

/* ------------------------------------------------------------------ */
/* LEDC: PWM 与舵机                                                    */
/* ------------------------------------------------------------------ */

static ledc_slot_t *slot_for_pin(int pin)
{
    for (int i = 0; i < TMX_LEDC_MAX_SLOTS; i++) {
        if (s_slots[i].pin == pin) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static ledc_slot_t *slot_alloc(int pin)
{
    ledc_slot_t *slot = slot_for_pin(pin);
    if (slot) {
        return slot;
    }
    for (int i = 0; i < TMX_LEDC_MAX_SLOTS; i++) {
        if (s_slots[i].pin == -1 && !ledc_channel_is_reserved((int)s_slots[i].channel)) {
            s_slots[i].pin = pin;
            return &s_slots[i];
        }
    }
    return NULL;
}

static void slot_release(ledc_slot_t *slot)
{
    if (!slot) {
        return;
    }
    ledc_stop(TMX_LEDC_MODE, slot->channel, 0);
    gpio_reset_pin((gpio_num_t)slot->pin);
    slot->pin = -1;
    slot->is_servo = false;
}

static ledc_timer_t timer_for_settings(uint32_t freq_hz, uint8_t resolution)
{
    /* 复用已有相同配置的定时器 */
    for (int i = 0; i < s_timer_count; i++) {
        if (s_timers[i].used && s_timers[i].freq_hz == freq_hz &&
            s_timers[i].resolution == resolution) {
            return s_timer_ids[i];
        }
    }
    /* 使用空闲定时器 */
    for (int i = 0; i < s_timer_count; i++) {
        if (!s_timers[i].used) {
            s_timers[i].used = true;
            s_timers[i].freq_hz = freq_hz;
            s_timers[i].resolution = resolution;
            return s_timer_ids[i];
        }
    }
    /* 定时器用完了: 复用第一个, 并记录警告 */
    ESP_LOGW(TAG, "no free PWM timer, reusing the first one (max %d different freq/res pairs)",
             s_timer_count);
    if (s_timer_count == 0) {
        /* 理论上不会发生 (0~2 最多被占两个), 兜个底免得数组越界 */
        return LEDC_TIMER_0;
    }
    s_timers[0].used = true;
    s_timers[0].freq_hz = freq_hz;
    s_timers[0].resolution = resolution;
    return s_timer_ids[0];
}

esp_err_t tmx_pwm_configure(int pin, uint8_t resolution, double frequency)
{
    if (check_pin(pin, "pwm") != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (resolution < 1 || resolution > 16) {
        ESP_LOGW(TAG, "pwm: resolution %u out of range (1..16)", resolution);
        return ESP_ERR_INVALID_ARG;
    }
    if (!(frequency > 0.0) || frequency > 40000000.0) {
        ESP_LOGW(TAG, "pwm: invalid frequency %.3f", frequency);
        return ESP_ERR_INVALID_ARG;
    }

    /* 重新配置前先释放旧通道 */
    ledc_slot_t *old = slot_for_pin(pin);
    if (old) {
        slot_release(old);
    }

    ledc_slot_t *slot = slot_alloc(pin);
    if (!slot) {
        ESP_LOGE(TAG, "pwm: no free LEDC channel (max %d pwm/servo outputs)",
                 TMX_LEDC_MAX_SLOTS);
        return ESP_ERR_NO_MEM;
    }

    ledc_timer_t timer = timer_for_settings((uint32_t)(frequency + 0.5), resolution);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = TMX_LEDC_MODE,
        .duty_resolution = (ledc_timer_bit_t)resolution,
        .timer_num = timer,
        .freq_hz = (uint32_t)(frequency + 0.5),
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pwm: timer config failed: %s", esp_err_to_name(err));
        slot_release(slot);
        return err;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .speed_mode = TMX_LEDC_MODE,
        .channel = slot->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pwm: channel config failed: %s", esp_err_to_name(err));
        slot_release(slot);
        return err;
    }

    slot->resolution = resolution;
    slot->is_servo = false;
    return ESP_OK;
}

esp_err_t tmx_pwm_write(int pin, uint32_t duty)
{
    ledc_slot_t *slot = slot_for_pin(pin);
    if (!slot || slot->is_servo) {
        ESP_LOGW(TAG, "pwm_write: pin %d is not configured as PWM", pin);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t max_duty = slot->resolution >= 16 ? 0xFFFFu : ((1u << slot->resolution) - 1u);
    if (duty > max_duty) {
        duty = max_duty;
    }

    ledc_set_duty(TMX_LEDC_MODE, slot->channel, duty);
    return ledc_update_duty(TMX_LEDC_MODE, slot->channel);
}

esp_err_t tmx_pwm_detach(int pin)
{
    ledc_slot_t *slot = slot_for_pin(pin);
    if (!slot) {
        return ESP_OK;
    }
    slot_release(slot);
    return ESP_OK;
}

esp_err_t tmx_servo_attach(int pin, uint16_t min_pulse_us, uint16_t max_pulse_us)
{
    if (check_pin(pin, "servo") != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    ledc_slot_t *old = slot_for_pin(pin);
    if (old) {
        slot_release(old);
    }

    ledc_slot_t *slot = slot_alloc(pin);
    if (!slot) {
        ESP_LOGE(TAG, "servo: no free LEDC channel (max %d pwm/servo outputs)",
                 TMX_LEDC_MAX_SLOTS);
        return ESP_ERR_NO_MEM;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = TMX_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = TMX_SERVO_TIMER,
        .freq_hz = TMX_SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo: timer config failed: %s", esp_err_to_name(err));
        slot_release(slot);
        return err;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .speed_mode = TMX_LEDC_MODE,
        .channel = slot->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = TMX_SERVO_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo: channel config failed: %s", esp_err_to_name(err));
        slot_release(slot);
        return err;
    }

    slot->is_servo = true;
    slot->resolution = TMX_SERVO_RES_BITS;
    slot->min_pulse = min_pulse_us ? min_pulse_us : 544;
    slot->max_pulse = max_pulse_us ? max_pulse_us : 2400;
    ESP_LOGI(TAG, "servo: pin %d attached (pulse %u-%u)",
             pin, (unsigned)slot->min_pulse, (unsigned)slot->max_pulse);
    tmx_servo_write(pin, 0);
    return ESP_OK;
}

esp_err_t tmx_servo_write(int pin, int angle)
{
    ledc_slot_t *slot = slot_for_pin(pin);
    if (!slot || !slot->is_servo) {
        ESP_LOGW(TAG, "servo_write: no servo attached on pin %d", pin);
        return ESP_ERR_INVALID_STATE;
    }

    if (angle < 0) {
        angle = 0;
    } else if (angle > 180) {
        angle = 180;
    }

    uint32_t period_us = 1000000u / TMX_SERVO_FREQ_HZ;
    uint32_t pulse_us = slot->min_pulse +
                        ((uint32_t)(slot->max_pulse - slot->min_pulse) * (uint32_t)angle) / 180u;
    uint32_t max_duty = (1u << TMX_SERVO_RES_BITS) - 1u;
    uint32_t duty = (uint32_t)(((uint64_t)pulse_us * max_duty) / period_us);

    ledc_set_duty(TMX_LEDC_MODE, slot->channel, duty);
    return ledc_update_duty(TMX_LEDC_MODE, slot->channel);
}

esp_err_t tmx_servo_detach(int pin)
{
    ledc_slot_t *slot = slot_for_pin(pin);
    if (!slot || !slot->is_servo) {
        return ESP_OK;
    }
    slot_release(slot);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* HC-SR04 超声波                                                      */
/* ------------------------------------------------------------------ */

esp_err_t tmx_sonar_register(int trigger_pin, int echo_pin)
{
    esp_err_t err = check_pin(trigger_pin, "sonar trigger");
    if (err != ESP_OK) {
        return err;
    }
    err = check_pin(echo_pin, "sonar echo");
    if (err != ESP_OK) {
        return err;
    }
    if (trigger_pin == echo_pin) {
        ESP_LOGW(TAG, "sonar: trigger and echo pins must differ");
        return ESP_ERR_INVALID_ARG;
    }

    err = tmx_gpio_output(trigger_pin);
    if (err != ESP_OK) {
        return err;
    }
    err = tmx_gpio_input(echo_pin, false, false);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level((gpio_num_t)trigger_pin, 0);
    return ESP_OK;
}

uint16_t tmx_sonar_read_cm(int trigger_pin, int echo_pin)
{
    if (!tmx_pin_is_valid(trigger_pin) || !tmx_pin_is_valid(echo_pin)) {
        return 0;
    }

    /* 10us 触发脉冲 */
    gpio_set_level((gpio_num_t)trigger_pin, 0);
    esp_rom_delay_us(4);
    gpio_set_level((gpio_num_t)trigger_pin, 1);
    esp_rom_delay_us(10);
    gpio_set_level((gpio_num_t)trigger_pin, 0);

    /* 等待回波上升沿 */
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)echo_pin) == 0) {
        if ((esp_timer_get_time() - start) > TMX_SONAR_TIMEOUT_US) {
            return 0;
        }
    }

    /* 测量高电平宽度 */
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)echo_pin) == 1) {
        if ((esp_timer_get_time() - rise) > TMX_SONAR_TIMEOUT_US) {
            return 0;
        }
    }
    int64_t fall = esp_timer_get_time();

    /* 声速 343m/s: 距离(cm) = 时间(us) / 58 */
    uint32_t width = (uint32_t)(fall - rise);
    uint32_t cm = width / 58u;
    if (cm > 65535u) {
        cm = 65535u;
    }
    return (uint16_t)cm;
}
