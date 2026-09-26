/*
 * AXP2101 电池电压 / 电量读取 (板载 PMIC 自带电量计)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool    battery_present;    /* STATUS1 bit3 */
    bool    external_power;     /* STATUS1 bit5: 1 = 现在由 USB/VBUS 供电 */
    uint8_t charge_state;       /* STATUS2 低 3 位: 0 涓流 1 预充 2 恒流 3 恒压 4 充满 5 未充电 */
    bool    charging;           /* 涓流/预充/恒流/恒压 都算充电中 */
    int     battery_mv;         /* 电池电压 mV, 0 = 读不到 */
    int     vbus_mv;            /* VBUS(USB) 电压 mV, 0 = 没插 */
    int     percent;            /* 电量 0~100, -1 = 读不到 */
    int     rate_pph_x10;       /* 平均放电速率 0.1%/h: 负 = 放电, 正 = 在涨 */
    int     current_ma;         /* 估算的平均电流 mA: 正 = 充电, 负 = 放电。
                                 * AXP2101 没有电流 ADC, 这是 速率×容量 换算出来的
                                 * (容量见 menuconfig TMX_BATTERY_CAPACITY_MAH)。 */
} tmx_power_info_t;

/* 打开 AXP2101 的 VBAT/VBUS/VSYS ADC 通道并打一条初始日志 (幂等, 未就绪时返回错误) */
esp_err_t tmx_power_init(void);

/* 立刻读一次 */
esp_err_t tmx_power_read(tmx_power_info_t *out);

/* 最近一次读到的供电状态: true = 外部(USB)供电, false = 电池供电。
 * 还没读到过时返回 true (宁可保守, 不做省电)。 */
bool tmx_power_external_power(void);

/*
 * 由主循环调用: 默认每 2 秒看一次, 只有"电量变了 / 电压变了 20mV 以上 / 供电状态变了"
 * 才返回 true (另外每 30 秒补一条心跳), 并把读数填进 out —— 免得每 2 秒刷一条上报。
 */
bool tmx_power_poll(tmx_power_info_t *out);
