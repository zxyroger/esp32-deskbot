/*
 * ESP32-S3 Scratch (OneGPIO / s3-extend) 服务器
 *
 * 用途:
 *   ESP32-S3 作为 Telemetrix TCP 服务器, PC 上的 s3-extend (s32 命令或
 *   esp32gw 网关) 连上来以后, 就可以用 Scratch 3 离线版的
 *   "OneGpio ESP32" 积木控制板子 (数字输入输出 / PWM / 舵机 / 模拟输入 / 超声波)。
 *
 * 使用步骤:
 *   1. idf.py menuconfig  -> "ESP32-S3 Scratch 服务器配置" 填写 WiFi 名称与密码
 *   2. idf.py -p COMx flash monitor   -> 记下串口打印的板子 IP
 *   3. PC 上运行 s32 (s3-extend), 在 Scratch 的 IP 地址积木里填这个 IP
 */

#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "tmx_core.h"
#include "tmx_server.h"
#include "wifi_link.h"
#include "display_ili9341.h"
#include "tmx_audio.h"
#include "tmx_camera.h"

static const char *TAG = "app";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/*
 * 上次为什么复位 —— 电池供电时最有用的一条日志。
 * "切到电池一开摄像头板子就没了"如果真是电池带不动, 这里会看到 BROWNOUT;
 * 如果是 AXP2101 被拉到欠压关机, 下一句会是 POWERON (从零上电)。
 */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:  return "POWERON (刚上电/掉电重启)";
        case ESP_RST_EXT:      return "EXT (外部复位脚)";
        case ESP_RST_SW:       return "SW (软件 esp_restart)";
        case ESP_RST_PANIC:    return "PANIC (程序崩溃)";
        case ESP_RST_INT_WDT:  return "INT_WDT (中断看门狗)";
        case ESP_RST_TASK_WDT: return "TASK_WDT (任务看门狗)";
        case ESP_RST_WDT:      return "WDT (看门狗)";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT (电压被拉垮!)";
        case ESP_RST_SDIO:     return "SDIO";
        case ESP_RST_USB:      return "USB (USB 外设复位)";
        case ESP_RST_JTAG:     return "JTAG";
        default:               return "UNKNOWN";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " ESP32-S3 Scratch (OneGPIO / s3-extend) server");
    ESP_LOGI(TAG, " ESP-IDF: %s   chip: %s", esp_get_idf_version(), CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, " TCP port: %d", CONFIG_TMX_TCP_PORT);
    ESP_LOGI(TAG, " 上次复位: %s", reset_reason_name(esp_reset_reason()));
    ESP_LOGI(TAG, "=================================================");

    init_nvs();
    ESP_ERROR_CHECK(tmx_core_init());

#if CONFIG_TMX_LCD_ENABLE
    /* 板载 ILI9341 SPI 屏幕 (引脚/方向见 menuconfig -> "LCD (ILI9341)") */
    ESP_ERROR_CHECK(display_ili9341_init());
#if CONFIG_TMX_LCD_ORIENT_TEST
    display_ili9341_orientation_test();
#endif
#if CONFIG_TMX_LCD_TEST_PATTERN
    display_ili9341_show_test_pattern();
#endif
#endif

#if CONFIG_TMX_AUDIO_ENABLE
    /* 板载 ES8311 音频 codec (引脚/声道见 menuconfig -> "音频 (ES8311 Codec)")。
     * 初始化失败(比如没焊 codec)只影响音频积木, 其它功能照常。 */
    esp_err_t audio_err = tmx_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "audio (ES8311) init failed: %s; 音频积木不可用",
                 esp_err_to_name(audio_err));
    }
#endif

#if CONFIG_TMX_CAMERA_ENABLE
    /* 板载 OV2640 DVP 摄像头 (引脚见 menuconfig -> "摄像头 (OV2640 DVP)")。
     * SCCB 复用上面那条 I2C 总线; 初始化失败只影响拍照, 其它功能照常。 */
    esp_err_t camera_err = tmx_camera_init();
    if (camera_err != ESP_OK) {
        ESP_LOGE(TAG, "camera (OV2640) init failed: %s; 拍照功能不可用",
                 esp_err_to_name(camera_err));
    }
#endif

    wifi_link_start();

    /* 服务器先起来, 等 PC 端连接; 此时 WiFi 可能还在连接中 */
    tmx_server_start();
}
