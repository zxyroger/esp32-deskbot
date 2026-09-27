#pragma once

#include <stdbool.h>

/* 启动 WiFi (STA, 失败可选 SoftAP 兜底), 并打印板子 IP 地址 */
void wifi_link_start(void);

/* 最近一次获取到的 IPv4 地址字符串, 未连接时返回 "0.0.0.0" */
const char *wifi_link_ip_string(void);

/*
 * WiFi 省电: on = WIFI_PS_MIN_MODEM (跟着 AP 的 DTIM 睡觉, 空闲时省几十 mA,
 * 但下行会晚 ~100ms 醒来); off = WIFI_PS_NONE (默认, 延迟最低)。
 */
void wifi_link_set_power_save(bool on);

/*
 * WiFi 发射功率: 电池供电时调低 (menuconfig -> "省电 (电池供电)")。
 *
 * 板子上最大的电流尖峰来自 WiFi 发射 + 摄像头: 小电池 / 保护板限流 1A 时,
 * 一开摄像头整机就掉电, 路由器那边表现为"板子直接消失"。把发射功率从 20dBm
 * 降到 ~11dBm, 尖峰能砍掉一两百 mA, 代价只是穿墙差一点。
 * on_battery=true 用 TMX_WIFI_TX_POWER_BATTERY_DBM, 否则用 TMX_WIFI_TX_POWER_DBM。
 */
void wifi_link_set_tx_power(bool on_battery);
