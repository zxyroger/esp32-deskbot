/*
 * 硬件抽象层: GPIO / ADC / PWM / 舵机 / 超声波
 * 目标芯片: ESP32-S3 (ESP-IDF v5.5.4)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 初始化 GPIO/ADC/LEDC 等外设（上电调用一次） */
esp_err_t tmx_io_init(void);

/* 引脚是否属于 Flash/PSRAM/Strapping 等保留引脚 */
bool tmx_pin_is_reserved(int pin);

/* 引脚号是否在合法范围内 */
bool tmx_pin_is_valid(int pin);

/* ---------------- GPIO ---------------- */
esp_err_t tmx_gpio_input(int pin, bool pullup, bool pulldown);
esp_err_t tmx_gpio_output(int pin);
esp_err_t tmx_gpio_write(int pin, int level);
int       tmx_gpio_read(int pin);

/* ---------------- ADC ---------------- */
/* 该引脚对应的实际 GPIO (含 Scratch 的 32~39 别名映射); 无 ADC 能力返回 -1 */
int       tmx_adc_gpio_for_pin(int pin);
/* 返回该引脚对应的 ADC1 通道号, 无 ADC 能力时返回 -1 */
int       tmx_adc_channel_for_pin(int pin);
esp_err_t tmx_adc_configure(int pin);
/* 读取原始值 (0~4095, 12bit), 失败返回 0 */
int       tmx_adc_read(int pin);

/* ---------------- PWM ---------------- */
/* resolution 为占空比位数 (1~16), frequency 为频率 (Hz) */
esp_err_t tmx_pwm_configure(int pin, uint8_t resolution, double frequency);
esp_err_t tmx_pwm_write(int pin, uint32_t duty);
esp_err_t tmx_pwm_detach(int pin);

/* ---------------- 舵机 ---------------- */
esp_err_t tmx_servo_attach(int pin, uint16_t min_pulse_us, uint16_t max_pulse_us);
esp_err_t tmx_servo_write(int pin, int angle);
esp_err_t tmx_servo_detach(int pin);

/* ---------------- HC-SR04 超声波 ---------------- */
esp_err_t tmx_sonar_register(int trigger_pin, int echo_pin);
/* 返回厘米值, 超时/未接传感器返回 0 */
uint16_t  tmx_sonar_read_cm(int trigger_pin, int echo_pin);
