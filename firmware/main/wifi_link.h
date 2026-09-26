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
