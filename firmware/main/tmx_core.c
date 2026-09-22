/*
 * Telemetrix4Esp32 (WiFi) 协议引擎
 *
 * 实现 PC 端 telemetrix_aio_esp32 / s3-extend 所需的命令与上报,
 * 行为与官方 Arduino 版 Telemetrix4Esp32WIFI.ino 对齐:
 *
 *   - 包格式 [长度][命令/上报类型][数据...]
 *   - 数字/模拟输入采用"变化才上报"
 *   - 模拟扫描间隔默认 19ms, 数字扫描每轮都做
 *   - 超声波轮流巡检, 间隔 33ms
 */

#include "tmx_core.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tmx_i2c.h"
#include "tmx_io.h"
#include "tmx_protocol.h"
#include "display_ili9341.h"
#include "tmx_audio.h"
#include "tmx_camera.h"

static const char *TAG = "tmx_core";

#define TMX_CMD_TIMEOUT_MS      2000 /* 一个包内部字节之间的最大间隔 */
#define TMX_SONAR_SCAN_MS       33
#define TMX_ANALOG_DEFAULT_MS   19

typedef struct {
    uint8_t mode;
    bool    reporting;
    int     last_value;
    int     differential;
} pin_state_t;

typedef struct {
    int  pin;
    bool active;
} servo_state_t;

typedef struct {
    int      trigger_pin;
    int      echo_pin;
    uint16_t last_value;
} sonar_state_t;

static int           s_client = -1;
static uint8_t       s_cmd_buffer[TMX_MAX_COMMAND_LEN];
static bool          s_stop_reports;
static uint32_t      s_analog_interval_ms = TMX_ANALOG_DEFAULT_MS;
static uint64_t      s_analog_last_ms;
static uint64_t      s_sonar_last_ms;

static pin_state_t   s_digital_pins[TMX_MAX_PINS];
static pin_state_t   s_analog_pins[TMX_MAX_PINS];
static servo_state_t s_servos[TMX_MAX_SERVOS];
static sonar_state_t s_sonars[TMX_MAX_SONARS];
static int           s_sonar_count;
static int           s_sonar_next;
static uint64_t      s_warned_commands;

/* 朗读文字 (会被网关拆成多个包发过来, 这里拼起来再一次合成) */
#if CONFIG_TMX_TTS_ENABLE
static char   s_tts_text[CONFIG_TMX_TTS_TEXT_MAX];
static size_t s_tts_len;
#endif

/* ------------------------------------------------------------------ */
/* 指令日志 (排查"点了积木到底有没有传到板子")                            */
/* ------------------------------------------------------------------ */
/* 由 menuconfig 的 TMX_LOG_COMMANDS 控制, 默认打开。
 * 高频循环里日志会很多, 所以做了限流: 每秒最多打 TMX_LOG_MAX_PER_SEC 行,
 * 被压掉的行会合并成一条汇总, 避免把串口刷爆拖慢主循环。 */
#if CONFIG_TMX_LOG_COMMANDS
#define TMX_LOG_MAX_PER_SEC 20

static int64_t s_log_window_start_ms;
static int     s_log_lines_in_window;
static int     s_log_suppressed;

static const char *tmx_mode_name(uint8_t mode)
{
    switch (mode) {
        case TMX_MODE_INPUT:          return "input";
        case TMX_MODE_OUTPUT:         return "output";
        case TMX_MODE_INPUT_PULLUP:   return "input_pullup";
        case TMX_MODE_INPUT_PULLDOWN: return "input_pulldown";
        case TMX_MODE_ANALOG:         return "analog_in";
        case TMX_MODE_SERVO:          return "servo";
        case TMX_MODE_SONAR:          return "sonar";
        case TMX_MODE_PWM_OUT:        return "pwm";
        default:                      return "other";
    }
}

static void tmx_log_command(const char *fmt, ...)
{
    int64_t now = esp_timer_get_time() / 1000;

    if ((now - s_log_window_start_ms) >= 1000) {
        if (s_log_suppressed > 0) {
            ESP_LOGI(TAG, "... 上一秒还有 %d 条指令没打印 (高频循环, 属正常)",
                     s_log_suppressed);
        }
        s_log_window_start_ms = now;
        s_log_lines_in_window = 0;
        s_log_suppressed = 0;
    }
    if (s_log_lines_in_window >= TMX_LOG_MAX_PER_SEC) {
        s_log_suppressed++;
        return;
    }
    s_log_lines_in_window++;

    char line[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", line);
}
#else
#define tmx_log_command(...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* 基础收发                                                            */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

bool tmx_core_send(const uint8_t *packet, size_t len)
{
    if (s_client < 0 || len == 0) {
        return false;
    }

    size_t sent = 0;
    while (sent < len) {
        int n = send(s_client, packet + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            vTaskDelay(1);
            continue;
        }
        return false;
    }
    return true;
}

#if CONFIG_TMX_DEBUG_REPORTS
static void send_debug(uint8_t id, int value)
{
    uint8_t packet[5] = { 4, TMX_REPORT_DEBUG_PRINT, id,
                          (uint8_t)((value >> 8) & 0xff), (uint8_t)(value & 0xff) };
    tmx_core_send(packet, sizeof(packet));
}
#endif

/* 0 = 暂时没有数据, 1 = 读到数据, -1 = 连接断开 */
static int recv_nonblock(uint8_t *buf, size_t len)
{
    int n = recv(s_client, buf, len, MSG_DONTWAIT);
    if (n > 0) {
        return 1;
    }
    if (n == 0) {
        return -1; /* 对端正常关闭 */
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    return -1;
}

/* 读取 len 个字节, 最长等待 timeout_ms; 失败返回 false */
static bool recv_with_timeout(uint8_t *buf, size_t len, int timeout_ms)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    size_t got = 0;

    while (got < len) {
        int n = recv(s_client, buf + got, len - got, MSG_DONTWAIT);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return false;
        }
        if (now_ms() >= deadline) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 状态表                                                              */
/* ------------------------------------------------------------------ */

esp_err_t tmx_core_init(void)
{
    tmx_io_init();
    /* 摄像头把 JPEG 分片直接交给协议层发出去 (见 docs/protocol.md 0x0F~0x12) */
    tmx_camera_set_sender(tmx_core_send);

    for (int i = 0; i < TMX_MAX_PINS; i++) {
        s_digital_pins[i].mode = TMX_MODE_NOT_SET;
        s_digital_pins[i].reporting = false;
        s_digital_pins[i].last_value = 0;
        s_digital_pins[i].differential = 0;

        s_analog_pins[i].mode = TMX_MODE_NOT_SET;
        s_analog_pins[i].reporting = false;
        s_analog_pins[i].last_value = 0;
        s_analog_pins[i].differential = 0;
    }
    for (int i = 0; i < TMX_MAX_SERVOS; i++) {
        s_servos[i].pin = -1;
        s_servos[i].active = false;
    }
    memset(s_sonars, 0, sizeof(s_sonars));
    s_sonar_count = 0;
    s_sonar_next = 0;
    s_stop_reports = false;
    s_analog_interval_ms = TMX_ANALOG_DEFAULT_MS;
    s_warned_commands = 0;
    return ESP_OK;
}

void tmx_core_set_client(int sock)
{
    s_client = sock;
    s_analog_last_ms = 0;
    s_sonar_last_ms = 0;
}

void tmx_core_clear_client(void)
{
    s_client = -1;
    /* 客户端断开了就别再往一个死 socket 上发图片了 */
    tmx_camera_stop();
}

static bool pin_ok(int pin, const char *what)
{
    if (!tmx_pin_is_valid(pin)) {
        ESP_LOGW(TAG, "%s: invalid pin %d", what, pin);
        return false;
    }
    if (tmx_pin_is_reserved(pin)) {
        ESP_LOGW(TAG, "%s: pin %d is reserved (flash/PSRAM/strapping/onboard LCD), ignored",
                 what, pin);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 命令处理                                                            */
/* ------------------------------------------------------------------ */

static void cmd_loopback(void)
{
    uint8_t packet[3] = { 2, TMX_CMD_LOOPBACK, s_cmd_buffer[0] };
    tmx_core_send(packet, sizeof(packet));
}

static void cmd_set_pin_mode(void)
{
    int pin = s_cmd_buffer[0];
    uint8_t mode = s_cmd_buffer[1];

    tmx_log_command("set_mode: pin %d -> %s", pin, tmx_mode_name(mode));

    switch (mode) {
        case TMX_MODE_INPUT:
        case TMX_MODE_INPUT_PULLUP:
        case TMX_MODE_INPUT_PULLDOWN: {
            if (!pin_ok(pin, "set_pin_mode")) {
                return;
            }
            tmx_gpio_input(pin,
                           mode == TMX_MODE_INPUT_PULLUP,
                           mode == TMX_MODE_INPUT_PULLDOWN);
            s_digital_pins[pin].mode = mode;
            s_digital_pins[pin].reporting = s_cmd_buffer[2] ? true : false;
            /* 记一个"相反"的初值, 让第一轮扫描立刻把真实电平上报给 PC,
             * 否则 PC 端在电平变化前拿不到初始值。 */
            s_digital_pins[pin].last_value = tmx_gpio_read(pin) ? 0 : 1;
            break;
        }

        case TMX_MODE_OUTPUT: {
            if (!pin_ok(pin, "set_pin_mode")) {
                return;
            }
            s_digital_pins[pin].mode = mode;
            s_digital_pins[pin].reporting = false;
            tmx_gpio_output(pin);
            break;
        }

        case TMX_MODE_ANALOG: {
            if (!tmx_pin_is_valid(pin)) {
                return;
            }
            int channel = tmx_adc_channel_for_pin(pin);
            if (channel < 0) {
                ESP_LOGW(TAG, "set_pin_mode: pin %d 没有 ADC 或被板载外设占用 "
                              "(见 docs/pins-esp32s3.md)", pin);
                return;
            }
            s_analog_pins[pin].mode = mode;
            s_analog_pins[pin].differential =
                ((int)s_cmd_buffer[2] << 8) | (int)s_cmd_buffer[3];
            s_analog_pins[pin].reporting = s_cmd_buffer[4] ? true : false;
            s_analog_pins[pin].last_value = tmx_adc_read(pin);
            tmx_adc_configure(pin);
            break;
        }

        case TMX_MODE_PWM_OUT: {
            if (!pin_ok(pin, "set_pin_mode")) {
                return;
            }
            uint8_t resolution = s_cmd_buffer[3];
            double frequency = 0.0;
            memcpy(&frequency, &s_cmd_buffer[4], sizeof(double));
            tmx_pwm_configure(pin, resolution, frequency);
            break;
        }

        case TMX_MODE_SERVO:
            /* 舵机通过 SERVO_ATTACH 命令配置, 这里与官方固件一样不处理 */
            break;

        case TMX_MODE_TOUCH:
            ESP_LOGW(TAG, "set_pin_mode: touch is not implemented (pin %d)", pin);
            break;

        default:
            ESP_LOGW(TAG, "set_pin_mode: unknown mode %u", mode);
            break;
    }
}

static void cmd_digital_write(void)
{
    int pin = s_cmd_buffer[0];
    if (!pin_ok(pin, "digital_write")) {
        return;
    }
    tmx_log_command("digital_write: pin %d -> %d", pin, s_cmd_buffer[1] ? 1 : 0);
    tmx_gpio_write(pin, s_cmd_buffer[1] ? 1 : 0);
}

static void cmd_analog_write(void)
{
    int pin = s_cmd_buffer[0];
    uint32_t value = ((uint32_t)s_cmd_buffer[1] << 8) | (uint32_t)s_cmd_buffer[2];
    tmx_log_command("analog_write(pwm): pin %d -> %u", pin, (unsigned)value);
    tmx_pwm_write(pin, value);
}

static void cmd_modify_reporting(void)
{
    int pin = s_cmd_buffer[1];

    switch (s_cmd_buffer[0]) {
        case TMX_REPORTING_DISABLE_ALL:
            for (int i = 0; i < TMX_MAX_PINS; i++) {
                s_digital_pins[i].reporting = false;
                s_analog_pins[i].reporting = false;
            }
            break;
        case TMX_REPORTING_ANALOG_ENABLE:
            if (s_analog_pins[pin].mode != TMX_MODE_NOT_SET) {
                s_analog_pins[pin].reporting = true;
            }
            break;
        case TMX_REPORTING_ANALOG_DISABLE:
            if (s_analog_pins[pin].mode != TMX_MODE_NOT_SET) {
                s_analog_pins[pin].reporting = false;
            }
            break;
        case TMX_REPORTING_DIGITAL_ENABLE:
            if (s_digital_pins[pin].mode != TMX_MODE_NOT_SET) {
                s_digital_pins[pin].reporting = true;
            }
            break;
        case TMX_REPORTING_DIGITAL_DISABLE:
            if (s_digital_pins[pin].mode != TMX_MODE_NOT_SET) {
                s_digital_pins[pin].reporting = false;
            }
            break;
        default:
            break;
    }
}

static void cmd_get_firmware_version(void)
{
    uint8_t packet[5] = { 4, TMX_REPORT_FIRMWARE,
                          TMX_FIRMWARE_MAJOR, TMX_FIRMWARE_MINOR, TMX_FIRMWARE_BUILD };
    tmx_log_command("client hello: firmware %d.%d.%d",
                    TMX_FIRMWARE_MAJOR, TMX_FIRMWARE_MINOR, TMX_FIRMWARE_BUILD);
    tmx_core_send(packet, sizeof(packet));
}

static int servo_slot_for_pin(int pin)
{
    for (int i = 0; i < TMX_MAX_SERVOS; i++) {
        if (s_servos[i].active && s_servos[i].pin == pin) {
            return i;
        }
    }
    return -1;
}

static esp_err_t servo_attach_pin(int pin, uint16_t min_pulse, uint16_t max_pulse)
{
    int slot = servo_slot_for_pin(pin);
    if (slot < 0) {
        for (int i = 0; i < TMX_MAX_SERVOS; i++) {
            if (!s_servos[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    if (tmx_servo_attach(pin, min_pulse, max_pulse) != ESP_OK) {
        return ESP_FAIL;
    }
    s_servos[slot].pin = pin;
    s_servos[slot].active = true;
    return ESP_OK;
}

static void cmd_servo_attach(void)
{
    int pin = s_cmd_buffer[0];
    if (!pin_ok(pin, "servo_attach")) {
        return;
    }
    uint16_t min_pulse = ((uint16_t)s_cmd_buffer[1] << 8) | s_cmd_buffer[2];
    uint16_t max_pulse = ((uint16_t)s_cmd_buffer[3] << 8) | s_cmd_buffer[4];

    tmx_log_command("servo_attach: pin %d (pulse %u-%u)",
                    pin, (unsigned)min_pulse, (unsigned)max_pulse);

    esp_err_t err = servo_attach_pin(pin, min_pulse, max_pulse);
    if (err == ESP_ERR_NO_MEM) {
        /* 与官方固件一样回一个 SERVO_UNAVAILABLE 报告给客户端 */
        uint8_t packet[3] = { 2, TMX_REPORT_SERVO_UNAVAILABLE, (uint8_t)pin };
        ESP_LOGW(TAG, "servo_attach: max servos reached (%d)", TMX_MAX_SERVOS);
        tmx_core_send(packet, sizeof(packet));
    }
}

static void cmd_servo_write(void)
{
    int pin = s_cmd_buffer[0];

    tmx_log_command("servo_write: pin %d -> %d", pin, (int)s_cmd_buffer[1]);

    if (!pin_ok(pin, "servo_write")) {
        return;
    }

    /*
     * Scratch keeps a local pin-mode cache. If the board or PC gateway has
     * restarted, the client may send SERVO_WRITE without a new SERVO_ATTACH,
     * so recover by attaching with the default 544-2400us pulse range.
     */
    if (servo_slot_for_pin(pin) < 0) {
        ESP_LOGW(TAG, "servo_write: pin %d not attached, auto-attaching", pin);
        if (servo_attach_pin(pin, 0, 0) != ESP_OK) {
            ESP_LOGW(TAG, "servo_write: auto-attach failed on pin %d", pin);
            return;
        }
    }

    tmx_servo_write(pin, (int)s_cmd_buffer[1]);
}

static void cmd_servo_detach(void)
{
    int pin = s_cmd_buffer[0];
    tmx_log_command("servo_detach: pin %d", pin);
    for (int i = 0; i < TMX_MAX_SERVOS; i++) {
        if (s_servos[i].active && s_servos[i].pin == pin) {
            s_servos[i].active = false;
            s_servos[i].pin = -1;
            tmx_servo_detach(pin);
        }
    }
}

static void cmd_i2c_begin(void)
{
    tmx_log_command("i2c_begin: sda %d scl %d", s_cmd_buffer[0], s_cmd_buffer[1]);
    tmx_i2c_begin((int)s_cmd_buffer[0], (int)s_cmd_buffer[1]);
}

static void cmd_i2c_read(void)
{
    uint8_t address = s_cmd_buffer[0];
    uint8_t reg = s_cmd_buffer[1];
    uint8_t count = s_cmd_buffer[2];
    bool stop_between = s_cmd_buffer[3] ? true : false;
    uint8_t data[64];

    tmx_log_command("i2c_read: addr 0x%02X reg 0x%02X n %d",
                    address, reg, count);
    size_t received = 0;

    if (count > sizeof(data)) {
        /* 官方固件会截断到缓冲区大小 */
        count = sizeof(data);
    }

    esp_err_t err = tmx_i2c_read(address, reg, count, stop_between,
                                 data, sizeof(data), &received);
    if (err != ESP_OK || received != count) {
        uint8_t packet[4] = { 3, TMX_REPORT_I2C_TOO_FEW_BYTES, 1, address };
        tmx_core_send(packet, sizeof(packet));
        return;
    }

    /* 长度 = 数据字节数 + 4 (报告类型/字节数/地址/寄存器) */
    uint8_t packet[5 + sizeof(data)];
    packet[0] = (uint8_t)(received + 4);
    packet[1] = TMX_REPORT_I2C_READ;
    packet[2] = (uint8_t)received;
    packet[3] = address;
    packet[4] = reg;
    memcpy(&packet[5], data, received);
    tmx_core_send(packet, (size_t)received + 5);
}

static void cmd_i2c_write(void)
{
    uint8_t count = s_cmd_buffer[0];
    uint8_t address = s_cmd_buffer[1];

    tmx_log_command("i2c_write: addr 0x%02X n %d", address, count);

    if (count > (TMX_MAX_COMMAND_LEN - 2)) {
        count = TMX_MAX_COMMAND_LEN - 2;
    }
    tmx_i2c_write(address, &s_cmd_buffer[2], count);
}

static void cmd_sonar_new(void)
{
    int trigger = s_cmd_buffer[0];
    int echo = s_cmd_buffer[1];

    tmx_log_command("sonar: trig %d echo %d", trigger, echo);

    if (s_sonar_count >= TMX_MAX_SONARS) {
        ESP_LOGW(TAG, "sonar_new: max sonars reached (%d)", TMX_MAX_SONARS);
        return;
    }
    if (tmx_sonar_register(trigger, echo) != ESP_OK) {
        return;
    }
    s_sonars[s_sonar_count].trigger_pin = trigger;
    s_sonars[s_sonar_count].echo_pin = echo;
    s_sonars[s_sonar_count].last_value = 0xffff;
    s_sonar_count++;
}

static void cmd_stop_all_reports(void)
{
    s_stop_reports = true;
}

static void cmd_enable_all_reports(void)
{
    s_stop_reports = false;
}

static void cmd_set_analog_scanning_interval(void)
{
    s_analog_interval_ms = s_cmd_buffer[0];
}

static void cmd_dac(void)
{
    ESP_LOGW(TAG, "ESP32-S3 has no DAC, command ignored");
}

/* ---------------- 屏幕 (LCD) ---------------- */

static void cmd_lcd_backlight(void)
{
    static int last_value = -1;
    uint8_t value = s_cmd_buffer[0];
    if (value > 100) {
        value = 100;
    }
    if (display_ili9341_panel() == NULL) {
        ESP_LOGW(TAG, "lcd backlight: screen is not initialized");
        return;
    }
    display_ili9341_set_backlight(value);
    if ((int)value != last_value) {
        last_value = (int)value;
        ESP_LOGI(TAG, "lcd backlight: %s", value == 0 ? "off" : "on");
    }
}

static void cmd_lcd_color(void)
{
    static int last_rgb = -1;
    uint8_t red = s_cmd_buffer[0];
    uint8_t green = s_cmd_buffer[1];
    uint8_t blue = s_cmd_buffer[2];

    if (display_ili9341_panel() == NULL) {
        ESP_LOGW(TAG, "lcd color: screen is not initialized");
        return;
    }
    esp_err_t err = display_ili9341_fill_rgb(red, green, blue);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lcd color: fill failed (%s)", esp_err_to_name(err));
        return;
    }
    int rgb = ((int)red << 16) | ((int)green << 8) | (int)blue;
    if (rgb != last_rgb) {
        last_rgb = rgb;
        ESP_LOGI(TAG, "lcd color: #%02x%02x%02x", red, green, blue);
    }
}

/* ---------------- 音频 (ES8311) ---------------- */

static void cmd_audio_tone(void)
{
    int freq = ((int)s_cmd_buffer[0] << 8) | s_cmd_buffer[1];
    int duration_ms = ((int)s_cmd_buffer[2] << 8) | s_cmd_buffer[3];
    int volume = s_cmd_buffer[4];

    esp_err_t err = tmx_audio_play_tone(freq, duration_ms, volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio tone failed: %s", esp_err_to_name(err));
        return;
    }
    tmx_log_command("audio: tone %dHz %dms %d%%", freq, duration_ms, volume);
}

static void cmd_audio_stop(void)
{
    tmx_audio_stop();
    tmx_log_command("audio: stop");
}

static void cmd_audio_mic(void)
{
    bool enable = (s_cmd_buffer[0] != 0);
    tmx_audio_set_mic_reporting(enable);
    tmx_log_command("audio: mic %s", enable ? "on" : "off");
}

/* ---------------- 语音合成 (TTS) ---------------- */

static void cmd_tts_text(void)
{
#if CONFIG_TMX_TTS_ENABLE
    bool last = (s_cmd_buffer[0] & 0x01) != 0;
    const char *text = (const char *)&s_cmd_buffer[1];
    size_t len = strnlen(text, sizeof(s_cmd_buffer) - 1);

    if (len > 0) {
        size_t space = sizeof(s_tts_text) - 1 - s_tts_len;
        if (len > space) {
            len = space;
            ESP_LOGW(TAG, "朗读文字超过 %d 字节, 后面的被截断", CONFIG_TMX_TTS_TEXT_MAX);
        }
        memcpy(s_tts_text + s_tts_len, text, len);
        s_tts_len += len;
        s_tts_text[s_tts_len] = '\0';
    }

    if (!last) {
        return;
    }

    if (s_tts_len == 0) {
        ESP_LOGW(TAG, "收到空文字, 不朗读");
    } else {
        esp_err_t err = tmx_audio_say_text(s_tts_text, CONFIG_TMX_TTS_SPEED);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "朗读失败: %s", esp_err_to_name(err));
        } else {
            tmx_log_command("tts: 朗读 %u 字节", (unsigned)s_tts_len);
        }
    }
    s_tts_len = 0;
    s_tts_text[0] = '\0';
#endif
}

static void cmd_tts_stop(void)
{
    tmx_audio_stop();
    tmx_log_command("tts: stop");
}

static void cmd_tts_mirror(void)
{
    bool enable = (s_cmd_buffer[0] != 0);
    tmx_audio_set_mirror(enable);
    tmx_log_command("tts: mirror %s", enable ? "on" : "off");
}

/* ---------------- 摄像头 (OV2640) ---------------- */

static void cmd_camera_config(void)
{
    int frame_size = s_cmd_buffer[0];
    int quality = s_cmd_buffer[1];
    int pixformat = s_cmd_buffer[2];   /* 0 = 不改 (老的两字节命令也走这里) */
    if (frame_size == 0xFF) {
        frame_size = -1;
    }
    if (quality == 0xFF) {
        quality = -1;
    }

    if (!tmx_camera_is_ready()) {
        ESP_LOGW(TAG, "摄像头没就绪 (menuconfig -> 摄像头 (OV2640 DVP))");
        return;
    }
    esp_err_t err = tmx_camera_set_format(frame_size, quality, pixformat);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera config failed: %s", esp_err_to_name(err));
    }
}

static void cmd_camera_snapshot(void)
{
    int frames = s_cmd_buffer[0];
    uint32_t interval_ms = ((uint32_t)s_cmd_buffer[1] << 8) | s_cmd_buffer[2];
    if (interval_ms == 0) {
        interval_ms = CONFIG_TMX_CAMERA_STREAM_INTERVAL_MS;
    }
    tmx_camera_snapshot(frames, interval_ms);
}

static void cmd_camera_stop(void)
{
    tmx_camera_stop();
}

static void cmd_camera_info(void)
{
    tmx_camera_send_info();
}

static void cmd_camera_probe(void)
{
    esp_log_level_set("tmx_camera", ESP_LOG_NONE);   /* 同上, 先静音再探针 */
    tmx_camera_send_info();                          /* 心跳: 让 PC 知道命令到了 */
    ESP_LOGW(TAG, "camera probe: 开始量 DVP 各信号线 (看串口后续几行)");
    tmx_camera_probe_pins();
}

static void cmd_camera_tune(void)
{
    /* 日志通道开机一段后会卡住, 而这两个命令一进来就写日志 —— 那会把命令处理
     * 任务自己卡住, 探针/调参干脆跑不到。先把 tmx_camera 这个 tag 静音;
     * 探针结果本身是 ets_printf 直连输出的, 不受影响。 */
    esp_log_level_set("tmx_camera", ESP_LOG_NONE);
    int field = s_cmd_buffer[0];
    int value = (int)(int16_t)((s_cmd_buffer[1] << 8) | s_cmd_buffer[2]);
    if (tmx_camera_tune(field, value) != ESP_OK) {
        ESP_LOGW(TAG, "camera tune: field %d value %d failed", field, value);
    }
    tmx_camera_send_info();                          /* 心跳: 让 PC 知道命令到了 */
}

static void warn_unsupported(uint8_t command)
{
    if (command < 64) {
        if (s_warned_commands & (1ULL << command)) {
            return;
        }
        s_warned_commands |= (1ULL << command);
    }
    ESP_LOGW(TAG, "command %u is not implemented in this firmware (SPI/OneWire/stepper...)", command);
}

/* ------------------------------------------------------------------ */
/* 上报扫描                                                            */
/* ------------------------------------------------------------------ */

static void send_digital_report(int pin, int value)
{
    uint8_t packet[4] = { 3, TMX_REPORT_DIGITAL, (uint8_t)pin, (uint8_t)value };
    tmx_core_send(packet, sizeof(packet));
}

static void send_analog_report(int pin, int value)
{
    uint8_t packet[5] = { 4, TMX_REPORT_ANALOG, (uint8_t)pin,
                          (uint8_t)((value >> 8) & 0xff), (uint8_t)(value & 0xff) };
    tmx_core_send(packet, sizeof(packet));
}

static void scan_digital_inputs(void)
{
    for (int pin = 0; pin < TMX_MAX_PINS; pin++) {
        pin_state_t *state = &s_digital_pins[pin];
        if (state->mode != TMX_MODE_INPUT &&
            state->mode != TMX_MODE_INPUT_PULLUP &&
            state->mode != TMX_MODE_INPUT_PULLDOWN) {
            continue;
        }
        if (!state->reporting) {
            continue;
        }
        int value = tmx_gpio_read(pin);
        if (value != state->last_value) {
            state->last_value = value;
            send_digital_report(pin, value);
        }
    }
}

static void scan_analog_inputs(void)
{
    uint64_t now = now_ms();
    if ((now - s_analog_last_ms) < s_analog_interval_ms) {
        return;
    }
    s_analog_last_ms = now;

    for (int pin = 0; pin < TMX_MAX_PINS; pin++) {
        pin_state_t *state = &s_analog_pins[pin];
        if (state->mode != TMX_MODE_ANALOG || !state->reporting) {
            continue;
        }
        int value = tmx_adc_read(pin);
        int differential = value - state->last_value;
        if (differential < 0) {
            differential = -differential;
        }
        if (differential >= state->differential) {
            state->last_value = value;
            send_analog_report(pin, value);
        }
    }
}

static void scan_sonars(void)
{
    if (s_sonar_count == 0) {
        return;
    }

    uint64_t now = now_ms();
    if ((now - s_sonar_last_ms) < TMX_SONAR_SCAN_MS) {
        return;
    }
    s_sonar_last_ms = now;

    sonar_state_t *sonar = &s_sonars[s_sonar_next];
    uint16_t distance = tmx_sonar_read_cm(sonar->trigger_pin, sonar->echo_pin);

    if (distance != sonar->last_value) {
        sonar->last_value = distance;
        uint8_t packet[5] = { 4, TMX_REPORT_SONAR_DISTANCE,
                              (uint8_t)sonar->trigger_pin,
                              (uint8_t)(distance >> 8),
                              (uint8_t)(distance & 0xff) };
        tmx_core_send(packet, sizeof(packet));
    }

    s_sonar_next++;
    if (s_sonar_next >= s_sonar_count) {
        s_sonar_next = 0;
    }
}

/* 麦克风响度: 只在打开上报、且数值变化时发 (与模拟输入的行为一致) */
static void scan_audio_input(void)
{
#if CONFIG_TMX_AUDIO_ENABLE
    static int      last_level = -1;
    static uint64_t last_ms;

    if (!tmx_audio_mic_reporting()) {
        last_level = -1;
        return;
    }

    uint64_t now = now_ms();
    if ((now - last_ms) < CONFIG_TMX_AUDIO_MIC_REPORT_MS) {
        return;
    }

    int level = tmx_audio_mic_level();
    if (level == last_level) {
        return;
    }
    last_level = level;
    last_ms = now;

    uint8_t packet[3] = { 2, TMX_REPORT_AUDIO_LEVEL, (uint8_t)level };
    tmx_core_send(packet, sizeof(packet));
#endif
}

/* TTS 回传: 把音频任务攒下的合成 PCM 按包发给 PC (PC 侧存成 wav) */
static void scan_tts_mirror(void)
{
#if CONFIG_TMX_TTS_ENABLE
    static uint16_t seq;

    if (!tmx_audio_mirror_on()) {
        return;
    }

    uint8_t chunk[240];
    int n = tmx_audio_mirror_read(chunk, sizeof(chunk));
    if (n > 0) {
        uint8_t packet[4 + sizeof(chunk)];
        /* 长度 = 报告码(1) + 序号(2) + PCM 数据(n) */
        packet[0] = (uint8_t)(3 + n);
        packet[1] = TMX_REPORT_TTS_PCM;
        packet[2] = (uint8_t)(seq >> 8);
        packet[3] = (uint8_t)(seq & 0xff);
        memcpy(&packet[4], chunk, (size_t)n);
        tmx_core_send(packet, (size_t)n + 4);
        seq++;
    } else if (tmx_audio_mirror_end_pending()) {
        uint8_t packet[4] = { 3, TMX_REPORT_TTS_PCM, 0xFF, 0xFF };
        tmx_core_send(packet, sizeof(packet));
        tmx_audio_mirror_clear_end();
        seq = 0;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* 主循环                                                              */
/* ------------------------------------------------------------------ */

/* 读取并执行一条命令; 返回 false 表示连接已断开 */
static bool read_one_command(void)
{
    static const uint8_t command_table_size = TMX_CMD_TABLE_SIZE;
    uint8_t packet_length;
    uint8_t command;
    int result;

    result = recv_nonblock(&packet_length, 1);
    if (result == 0) {
        return true; /* 暂无命令 */
    }
    if (result < 0) {
        return false; /* 连接断开 */
    }

    if (!recv_with_timeout(&command, 1, TMX_CMD_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "incomplete command packet, dropped");
        return true;
    }

    int data_len = (int)packet_length - 1;
    if (data_len > (int)sizeof(s_cmd_buffer)) {
        ESP_LOGW(TAG, "command packet too long (%d bytes), dropped", data_len);
        /* 丢弃多余字节, 避免协议错位 */
        uint8_t scratch[64];
        int left = data_len;
        while (left > 0) {
            int chunk = left > (int)sizeof(scratch) ? (int)sizeof(scratch) : left;
            if (!recv_with_timeout(scratch, (size_t)chunk, TMX_CMD_TIMEOUT_MS)) {
                return true;
            }
            left -= chunk;
        }
        return true;
    }

    memset(s_cmd_buffer, 0, sizeof(s_cmd_buffer));
    if (data_len > 0 && !recv_with_timeout(s_cmd_buffer, (size_t)data_len,
                                           TMX_CMD_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "incomplete command data, dropped");
        return true;
    }

    if (command >= command_table_size) {
        ESP_LOGW(TAG, "illegal command byte %u", command);
        return true;
    }

#if CONFIG_TMX_DEBUG_REPORTS
    send_debug(packet_length, command);
#endif

    switch (command) {
        case TMX_CMD_LOOPBACK:                     cmd_loopback(); break;
        case TMX_CMD_SET_PIN_MODE:                 cmd_set_pin_mode(); break;
        case TMX_CMD_DIGITAL_WRITE:                cmd_digital_write(); break;
        case TMX_CMD_ANALOG_WRITE:                 cmd_analog_write(); break;
        case TMX_CMD_MODIFY_REPORTING:             cmd_modify_reporting(); break;
        case TMX_CMD_GET_FIRMWARE_VERSION:         cmd_get_firmware_version(); break;
        case TMX_CMD_SERVO_ATTACH:                 cmd_servo_attach(); break;
        case TMX_CMD_SERVO_WRITE:                  cmd_servo_write(); break;
        case TMX_CMD_SERVO_DETACH:                 cmd_servo_detach(); break;
        case TMX_CMD_I2C_BEGIN:                    cmd_i2c_begin(); break;
        case TMX_CMD_I2C_READ:                     cmd_i2c_read(); break;
        case TMX_CMD_I2C_WRITE:                    cmd_i2c_write(); break;
        case TMX_CMD_SONAR_NEW:                    cmd_sonar_new(); break;
        case TMX_CMD_DHT_NEW:
            ESP_LOGW(TAG, "DHT sensor is not implemented (pin %u)", s_cmd_buffer[0]);
            break;
        case TMX_CMD_STOP_ALL_REPORTS:             cmd_stop_all_reports(); break;
        case TMX_CMD_SET_ANALOG_SCANNING_INTERVAL: cmd_set_analog_scanning_interval(); break;
        case TMX_CMD_ENABLE_ALL_REPORTS:           cmd_enable_all_reports(); break;
        case TMX_CMD_ANALOG_OUT_ATTACH:            break; /* ESP32 Core v3 已不需要 */
        case TMX_CMD_ANALOG_OUT_DETACH:            tmx_pwm_detach(s_cmd_buffer[0]); break;
        case TMX_CMD_DAC_WRITE:
        case TMX_CMD_DAC_DISABLE:                  cmd_dac(); break;
        case TMX_CMD_RESET:                        esp_restart(); break;
        case TMX_CMD_LCD_BACKLIGHT:                cmd_lcd_backlight(); break;
        case TMX_CMD_LCD_COLOR:                    cmd_lcd_color(); break;
        case TMX_CMD_AUDIO_TONE:                   cmd_audio_tone(); break;
        case TMX_CMD_AUDIO_STOP:                   cmd_audio_stop(); break;
        case TMX_CMD_AUDIO_MIC:                    cmd_audio_mic(); break;
        case TMX_CMD_TTS_TEXT:                     cmd_tts_text(); break;
        case TMX_CMD_TTS_STOP:                     cmd_tts_stop(); break;
        case TMX_CMD_TTS_MIRROR:                   cmd_tts_mirror(); break;
        case TMX_CMD_CAMERA_CONFIG:                cmd_camera_config(); break;
        case TMX_CMD_CAMERA_SNAPSHOT:              cmd_camera_snapshot(); break;
        case TMX_CMD_CAMERA_STOP:                  cmd_camera_stop(); break;
        case TMX_CMD_CAMERA_INFO:                  cmd_camera_info(); break;
        case TMX_CMD_CAMERA_PROBE:                 cmd_camera_probe(); break;
        case TMX_CMD_CAMERA_TUNE:                  cmd_camera_tune(); break;
        default:                                   warn_unsupported(command); break;
    }

    return true;
}

bool tmx_core_poll(void)
{
    if (!read_one_command()) {
        return false;
    }

    if (!s_stop_reports) {
        scan_digital_inputs();
        scan_analog_inputs();
        scan_sonars();
        scan_audio_input();
    }
    scan_tts_mirror();
    /* 摄像头: 该拍就拍, 有帧要发就分片发给 PC */
    tmx_camera_poll();

    /* 与 Arduino 版 loop() 中的 delay(1) 对应, 同时让出 CPU */
    vTaskDelay(1);
    return true;
}
