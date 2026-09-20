/*
 * I2C 主站封装 (ESP-IDF 新版 i2c_master 驱动)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 建立 I2C 总线; sda/scl 为 0 时使用 Kconfig 中的默认引脚 */
esp_err_t tmx_i2c_begin(int sda_pin, int scl_pin);

/* 已完成 begin 才可用 */
bool tmx_i2c_is_ready(void);

/* 取总线句柄 (i2c_master_bus_handle_t); 未 begin 时返回 NULL。
 * 音频 codec 驱动 (esp_codec_dev) 需要挂在同一条总线上, 所以把句柄让出来。 */
void *tmx_i2c_get_bus(void);

/* I2C 端口号 (I2C_NUM_x); 未 begin 时返回 -1。
 * 摄像头 SCCB 要用它挂到同一条总线上 (见 tmx_camera.c)。 */
int tmx_i2c_port_num(void);

/* 写: addr 为 7 位地址, data 为要写入的字节 */
esp_err_t tmx_i2c_write(uint8_t addr, const uint8_t *data, size_t len);

/* 读: 先写寄存器地址, 再读 count 个字节到 out */
esp_err_t tmx_i2c_read(uint8_t addr, uint8_t reg, uint8_t count,
                       bool stop_between, uint8_t *out, size_t out_size,
                       size_t *out_len);
