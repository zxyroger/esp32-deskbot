/*
 * Telemetrix 协议引擎: 命令解析 / 引脚状态表 / 输入上报扫描
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 初始化状态表与外设 */
esp_err_t tmx_core_init(void);

/* 有客户端连上 / 断开时通知协议引擎 */
void tmx_core_set_client(int sock);
void tmx_core_clear_client(void);

/*
 * 处理一条命令并执行一次输入扫描。
 * 返回 false 表示连接已经断开, 调用方应关闭 socket。
 */
bool tmx_core_poll(void);

/* 向 PC 端发送一个报告包 (包长度字节由调用方填好) */
bool tmx_core_send(const uint8_t *packet, size_t len);
