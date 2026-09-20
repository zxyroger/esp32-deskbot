/*
 * ILI9341 SPI 屏幕 (320x240 横屏) 初始化
 * 目标芯片: ESP32-S3 / ESP-IDF v5.5.4
 *
 * 默认接线 (可在 menuconfig -> "LCD (ILI9341)" 里改):
 *   SCK/CLK = GPIO41     MOSI/DIN = GPIO40
 *   DC/RS   = GPIO39     CS       = GPIO21
 *   BLK     = GPIO42     RESET    = 未接 (软件复位)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 SPI 总线 + ILI9341 面板 + 背光, 上电调用一次 */
esp_err_t display_ili9341_init(void);

/* 面板句柄 (未初始化时返回 NULL), 方便接 LVGL 或自己画图 */
esp_lcd_panel_handle_t display_ili9341_panel(void);

/* 背光亮度 0 ~ 100 */
void display_ili9341_set_backlight(uint8_t percent);

/* 全屏填充 RGB565 颜色, 例如 0xF800 = 红 */
esp_err_t display_ili9341_fill(uint16_t rgb565_color);

/* 全屏填充 RGB888 颜色 (Scratch 的颜色积木用这个) */
esp_err_t display_ili9341_fill_rgb(uint8_t red, uint8_t green, uint8_t blue);

/* 方向测试图: 左上红 / 右上绿 / 左下蓝 / 右下黄, 用来检查接线/方向/颜色 */
esp_err_t display_ili9341_show_test_pattern(void);

/* 轮播几种 swap/mirror 组合 (每种 2.5 秒), 用来一眼定好屏幕方向 */
esp_err_t display_ili9341_orientation_test(void);

#ifdef __cplusplus
}
#endif
