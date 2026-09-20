/*
 * Telemetrix TCP 服务器
 *
 * 与官方 Arduino 版行为一致: 同一时刻只服务一个客户端, 客户端断开后
 * 自动回到 accept() 等待下一次连接。PC 端的 s3-extend / telemetrix_aio_esp32
 * 就是这么连上来的。
 */

#include "tmx_server.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tmx_core.h"

static const char *TAG = "tmx_server";

#define TMX_SERVER_TASK_STACK   6144
#define TMX_SERVER_TASK_PRIO    5

static void tmx_server_task(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(CONFIG_TMX_TCP_PORT),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind port %d failed: errno=%d", CONFIG_TMX_TCP_PORT, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Telemetrix server started, listening on port %d", CONFIG_TMX_TCP_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* 本服务器同一时刻只服务一个客户端, 所以"半死"连接(对端掉电/掉线但没发 FIN)
         * 会让板子在旧 socket 上一直等下去, 新连接进不来 —— 表现就是"连不上/很慢"。
         * 打开 TCP keepalive: 对端没了大约 10 + 5*3 = 25 秒就能测出来并回收。 */
        int keepalive = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#ifdef TCP_KEEPIDLE
        int keep_idle = 10;    /* 空闲 10 秒开始探测 */
        int keep_intvl = 5;    /* 每 5 秒探一次 */
        int keep_cnt = 3;      /* 连续 3 次无响应就断开 */
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
        setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));
#endif

        /* 避免 PC 端不读数据时把服务器任务永久卡住 */
        struct timeval send_timeout = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout));

        ESP_LOGI(TAG, "client connected: %s:%d", inet_ntoa(peer.sin_addr),
                 ntohs(peer.sin_port));

        tmx_core_set_client(client_fd);
        while (tmx_core_poll()) {
            /* 协议引擎内部会处理命令与输入扫描 */
        }

        tmx_core_clear_client();
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        ESP_LOGI(TAG, "client disconnected, waiting for the next connection");
    }
}

void tmx_server_start(void)
{
    xTaskCreate(tmx_server_task, "tmx_server", TMX_SERVER_TASK_STACK, NULL,
                TMX_SERVER_TASK_PRIO, NULL);
}
