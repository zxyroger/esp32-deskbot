/*
 * WiFi 连接管理
 *
 *  - 默认使用 DHCP 连接 Kconfig 中配置的路由器
 *  - 可在 menuconfig 中改用静态 IP
 *  - 连接失败若干次后可启动 SoftAP 热点, 便于没有路由器时直接连板子
 *  - 关闭 WiFi 省电模式, 保证 Scratch 控制的实时性
 *  - 拿到 IP 后周期性 UDP 广播自己的地址, PC 端无需去串口抄 IP
 */

#include "wifi_link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

static esp_netif_t *s_sta_netif;
static char         s_ip_string[16] = "0.0.0.0";
static int          s_retry_count;
static bool         s_ap_started;
#if CONFIG_TMX_UDP_BEACON_ENABLE
static bool         s_beacon_started;
#endif

static void set_ip_string(esp_ip4_addr_t addr)
{
    snprintf(s_ip_string, sizeof(s_ip_string), IPSTR, IP2STR(&addr));
}

const char *wifi_link_ip_string(void)
{
    return s_ip_string;
}

#if CONFIG_TMX_USE_STATIC_IP
static void apply_static_ip(void)
{
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_dhcpc_stop(s_sta_netif);

    esp_netif_str_to_ip4(CONFIG_TMX_STATIC_IP, &ip_info.ip);
    esp_netif_str_to_ip4(CONFIG_TMX_STATIC_GATEWAY, &ip_info.gw);
    esp_netif_str_to_ip4(CONFIG_TMX_STATIC_NETMASK, &ip_info.netmask);

    esp_err_t err = esp_netif_set_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set static IP: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "using static IP: %s", CONFIG_TMX_STATIC_IP);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* IP 自动发现: 板子主动广播自己的地址                                  */
/* ------------------------------------------------------------------ */
#if CONFIG_TMX_UDP_BEACON_ENABLE

#define TMX_BEACON_MAGIC    "ONEGPIO"
#define TMX_BEACON_PROBE    "ONEGPIO?"
#define TMX_BEACON_STACK    3072
#define TMX_BEACON_PRIO     3
#define TMX_BEACON_POLL_MS  200

static void beacon_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "beacon: socket() failed, UDP broadcast disabled");
        vTaskDelete(NULL);
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(CONFIG_TMX_UDP_BEACON_PORT),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGW(TAG, "beacon: bind port %d failed", CONFIG_TMX_UDP_BEACON_PORT);
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = TMX_BEACON_POLL_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int polls_per_interval = (1000 / TMX_BEACON_POLL_MS) *
                             CONFIG_TMX_UDP_BEACON_INTERVAL_S;
    int poll_count = polls_per_interval; /* 第一次直接广播 */

    for (;;) {
        char buf[32];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            buf[n] = '\0';
            if (strncmp(buf, TMX_BEACON_PROBE, strlen(TMX_BEACON_PROBE)) == 0) {
                /* PC 主动探测: 立刻回它一条 (单播) */
                char msg[48];
                int len = snprintf(msg, sizeof(msg), "%s %s %d", TMX_BEACON_MAGIC,
                                   s_ip_string, CONFIG_TMX_TCP_PORT);
                sendto(sock, msg, len, 0, (struct sockaddr *)&from, from_len);
                continue;
            }
        }

        if (++poll_count < polls_per_interval) {
            continue;
        }
        poll_count = 0;

        esp_netif_ip_info_t info = { 0 };
        if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
            continue;
        }

        char msg[48];
        int len = snprintf(msg, sizeof(msg), "%s %s %d", TMX_BEACON_MAGIC,
                           s_ip_string, CONFIG_TMX_TCP_PORT);
        struct sockaddr_in to = {
            .sin_family = AF_INET,
            .sin_port = htons(CONFIG_TMX_UDP_BEACON_PORT),
            /* 子网广播地址, 比 255.255.255.255 更容易穿过家用路由器 */
            .sin_addr.s_addr = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr,
        };
        sendto(sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}
#endif /* CONFIG_TMX_UDP_BEACON_ENABLE */

#if CONFIG_TMX_ENABLE_SOFTAP_FALLBACK
static void start_softap(void)
{
    if (s_ap_started) {
        return;
    }
    s_ap_started = true;

    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = { 0 };
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s",
             CONFIG_TMX_SOFTAP_SSID);
    ap_config.ap.ssid_len = strlen(CONFIG_TMX_SOFTAP_SSID);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    if (strlen(CONFIG_TMX_SOFTAP_PASSWORD) >= 8) {
        snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s",
                 CONFIG_TMX_SOFTAP_PASSWORD);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    set_ip_string((esp_ip4_addr_t){ .addr = 0x0104A8C0 }); /* 192.168.4.1 */

    ESP_LOGW(TAG, "cannot reach the router, SoftAP started: \"%s\"", CONFIG_TMX_SOFTAP_SSID);
    ESP_LOGW(TAG, "connect the PC to that AP, board address is %s:%d", s_ip_string,
             CONFIG_TMX_TCP_PORT);
}
#endif

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
#if CONFIG_TMX_USE_STATIC_IP
                apply_static_ip();
#endif
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                s_retry_count++;
                ESP_LOGW(TAG, "connect to WiFi \"%s\" failed (%d/%d)", CONFIG_TMX_WIFI_SSID,
                         s_retry_count, CONFIG_TMX_WIFI_MAX_RETRY);
#if CONFIG_TMX_ENABLE_SOFTAP_FALLBACK
                if (CONFIG_TMX_WIFI_MAX_RETRY > 0 &&
                    s_retry_count >= CONFIG_TMX_WIFI_MAX_RETRY) {
                    start_softap();
                    /* 热点已开, 但 STA 继续尝试, 连上路由器后热点依然可用 */
                    s_retry_count = 0;
                }
#endif
                esp_wifi_connect();
                break;
            }

            default:
                break;
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        set_ip_string(event->ip_info.ip);
        ESP_LOGI(TAG, "connected to WiFi \"%s\"", CONFIG_TMX_WIFI_SSID);
        ESP_LOGI(TAG, "board IP address: %s   port: %d", s_ip_string,
                 CONFIG_TMX_TCP_PORT);
        ESP_LOGI(TAG, "put this IP address into the Scratch 'IP address' block");
#if CONFIG_TMX_UDP_BEACON_ENABLE
        if (!s_beacon_started) {
            s_beacon_started = true;
            xTaskCreate(beacon_task, "tmx_beacon", TMX_BEACON_STACK, NULL,
                        TMX_BEACON_PRIO, NULL);
            ESP_LOGI(TAG, "UDP beacon started: broadcasting %s:%d on port %d every %ds",
                     s_ip_string, CONFIG_TMX_TCP_PORT, CONFIG_TMX_UDP_BEACON_PORT,
                     CONFIG_TMX_UDP_BEACON_INTERVAL_S);
            ESP_LOGI(TAG, "PC side can auto-discover the board (tools/find_board.py)");
        }
#endif
    }
}

void wifi_link_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t sta_config = { 0 };
    snprintf((char *)sta_config.sta.ssid, sizeof(sta_config.sta.ssid), "%s",
             CONFIG_TMX_WIFI_SSID);
    snprintf((char *)sta_config.sta.password, sizeof(sta_config.sta.password), "%s",
             CONFIG_TMX_WIFI_PASSWORD);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Scratch 控制对延迟敏感, 关闭省电 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

    ESP_LOGI(TAG, "connecting to WiFi \"%s\" ...", CONFIG_TMX_WIFI_SSID);
}
