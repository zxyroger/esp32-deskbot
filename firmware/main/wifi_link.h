#pragma once

/* 启动 WiFi (STA, 失败可选 SoftAP 兜底), 并打印板子 IP 地址 */
void wifi_link_start(void);

/* 最近一次获取到的 IPv4 地址字符串, 未连接时返回 "0.0.0.0" */
const char *wifi_link_ip_string(void);
