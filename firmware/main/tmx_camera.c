/*
 * OV2640 DVP 摄像头实现 (见 tmx_camera.h)
 *
 * 几个关键点:
 *  1. SCCB (摄像头的"I2C"控制口) 复用板载 I2C 总线: esp32-camera 支持
 *     pin_sccb_sda = -1 + sccb_i2c_port = 已初始化的端口, 这时它不再自己建总线,
 *     而是直接挂到 tmx_i2c 那条总线上去 (上面还挂着 ES8311, 地址不同不冲突)。
 *  2. XCLK 是 LEDC 输出 (20MHz), 会占一个 LEDC 定时器 + 通道; tmx_io.c 会把
 *     它们从 Scratch 的 PWM/舵机池里让出来 (见那里的 ledc_*_is_reserved)。
 *  3. 本板没开 PSRAM, 帧缓冲放在内部 DRAM: 分辨率越高、fb_count 越大越吃内存。
 *     VGA 的 JPEG 缓冲约 61KB/份 (宽x高/5), QVGA 约 15KB/份。
 *  4. 拍照是"按需"的: PC 发了 CAMERA_SNAPSHOT 才去取帧, 平时传感器照常出流,
 *     只是没人取, 不占 CPU。
 */

#include "tmx_camera.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tmx_i2c.h"
#include "tmx_protocol.h"

static const char *TAG = "tmx_camera";

#if CONFIG_TMX_CAMERA_ENABLE

#include "esp_camera.h"
#include "sensor.h"

#if CONFIG_TMX_CAMERA_PMIC_AXP2101
/*
 * 板载摄像头供电: AXP2101 的 BLDO1 = AVDD, BLDO2 = DVDD
 *
 * 冷启动时 AXP2101 只开 BLDO1 (0x90 = 0x55), DVDD 那一路是关的。这时候
 * SCCB 还能认出 0x30, 但寄存器读写会时好时坏 (写 NACK / 读回错值),
 * esp_camera_init 最后以 ESP_FAIL 收场, 日志里是 "Camera probe failed"。
 * 所以初始化摄像头之前先把两路都打开。
 *
 * 这里只动 0x90 的两个使能位, 不改电压: 电压沿用芯片默认值/板子当前的值,
 * 要按数据手册 (AVDD 2.8V / DVDD 1.2V) 调就用 tools/axp2101_power.py。
 */
#define TMX_PMIC_ADDR          0x34
#define TMX_PMIC_REG_ID        0x03
#define TMX_PMIC_REG_LDO_EN0   0x90
#define TMX_PMIC_ID_AXP2101    0x4A
#define TMX_PMIC_BLDO1_BIT     4     /* BLDO1 = AVDD */
#define TMX_PMIC_BLDO2_BIT     5     /* BLDO2 = DVDD */
#define TMX_PMIC_REG_BLDO1_VOL 0x96  /* BLDO1 电压 = 500mV + N*100mV */
#define TMX_PMIC_REG_BLDO2_VOL 0x97  /* BLDO2 电压 = 同上 */

/* 设一路 BLDO 的电压 (寄存器低 5 位 = (mV-500)/100) */
static bool pmic_set_ldo_mv(uint8_t vol_reg, int millivolt)
{
    if (millivolt < 500 || millivolt > 3500 || ((millivolt - 500) % 100) != 0) {
        ESP_LOGW(TAG, "PMIC: 电压 %d mV 非法 (500~3500mV, 100mV 一档)", millivolt);
        return false;
    }
    uint8_t cur = 0;
    size_t len = 0;
    if (tmx_i2c_read(TMX_PMIC_ADDR, vol_reg, 1, false, &cur, sizeof(cur), &len) != ESP_OK ||
        len != 1) {
        ESP_LOGW(TAG, "PMIC: 读电压寄存器 0x%02X 失败", vol_reg);
        return false;
    }
    uint8_t val = (uint8_t)((cur & 0xE0) | ((millivolt - 500) / 100));
    uint8_t buf[2] = { vol_reg, val };
    if (tmx_i2c_write(TMX_PMIC_ADDR, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC: 写电压寄存器 0x%02X 失败", vol_reg);
        return false;
    }
    return true;
}

/* 把摄像头两路 LDO 关掉 (真正的掉电, 连 I/O 一起), 用于自检失败时的"硬复位" */
static void camera_rails_off(void)
{
    uint8_t value = 0;
    size_t len = 0;
    if (tmx_i2c_read(TMX_PMIC_ADDR, TMX_PMIC_REG_LDO_EN0, 1, false,
                     &value, sizeof(value), &len) != ESP_OK || len != 1) {
        return;
    }
    uint8_t target = (uint8_t)(value & ~((1u << TMX_PMIC_BLDO1_BIT) |
                                         (1u << TMX_PMIC_BLDO2_BIT)));
    uint8_t buf[2] = { TMX_PMIC_REG_LDO_EN0, target };
    if (tmx_i2c_write(TMX_PMIC_ADDR, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW(TAG, "PMIC: 关摄像头供电失败");
    }
}

static void camera_power_on(void)
{
    uint8_t value = 0;
    size_t  len = 0;

    if (tmx_i2c_read(TMX_PMIC_ADDR, TMX_PMIC_REG_ID, 1, false,
                     &value, sizeof(value), &len) != ESP_OK ||
        len != 1 || value != TMX_PMIC_ID_AXP2101) {
        ESP_LOGW(TAG, "PMIC: 0x%02X 上的 AXP2101 没应答 (0x%02X 读回 0x%02X), 跳过摄像头供电",
                 TMX_PMIC_ADDR, TMX_PMIC_REG_ID, value);
        return;
    }

    /* 电压每次开机都设成确定值: AVDD 2.8V / DVDD 1.2V (OV2640 数据手册, Kconfig 可调)。
     * DVDD 填大了 (比如 2.8V) 内核过压, 摄像头会明显发烫。 */
    bool vol_ok = pmic_set_ldo_mv(TMX_PMIC_REG_BLDO1_VOL, CONFIG_TMX_CAMERA_PMIC_AVDD_MV);
    vol_ok &= pmic_set_ldo_mv(TMX_PMIC_REG_BLDO2_VOL, CONFIG_TMX_CAMERA_PMIC_DVDD_MV);
    if (vol_ok) {
        ESP_LOGI(TAG, "PMIC: 摄像头供电 AVDD(BLDO1)=%dmV, DVDD(BLDO2)=%dmV",
                 CONFIG_TMX_CAMERA_PMIC_AVDD_MV, CONFIG_TMX_CAMERA_PMIC_DVDD_MV);
    }

    if (tmx_i2c_read(TMX_PMIC_ADDR, TMX_PMIC_REG_LDO_EN0, 1, false,
                     &value, sizeof(value), &len) != ESP_OK || len != 1) {
        ESP_LOGW(TAG, "PMIC: 读 LDO 使能寄存器 0x%02X 失败", TMX_PMIC_REG_LDO_EN0);
        return;
    }

    uint8_t target = (uint8_t)(value |
                               (1u << TMX_PMIC_BLDO1_BIT) |
                               (1u << TMX_PMIC_BLDO2_BIT));
    if (target != value) {
        uint8_t buf[2] = { TMX_PMIC_REG_LDO_EN0, target };
        if (tmx_i2c_write(TMX_PMIC_ADDR, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "PMIC: 写 0x%02X 失败, 摄像头供电可能没打开", TMX_PMIC_REG_LDO_EN0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (tmx_i2c_read(TMX_PMIC_ADDR, TMX_PMIC_REG_LDO_EN0, 1, false,
                     &value, sizeof(value), &len) == ESP_OK && len == 1) {
        ESP_LOGI(TAG, "PMIC AXP2101: LDO 使能 0x90 = 0x%02X (BLDO1/AVDD=%s, BLDO2/DVDD=%s)",
                 value,
                 (value & (1u << TMX_PMIC_BLDO1_BIT)) ? "on" : "off",
                 (value & (1u << TMX_PMIC_BLDO2_BIT)) ? "on" : "off");
    }
}
#else
static void camera_power_on(void)
{
    ESP_LOGI(TAG, "PMIC 供电开关没编译进来 (menuconfig -> 摄像头 -> 开机自动打开 AXP2101 摄像头供电)");
}
#endif /* CONFIG_TMX_CAMERA_PMIC_AXP2101 */

/* LEDC 资源冲突在编译期就挡掉, 免得运行时才发现 */
#if CONFIG_TMX_LCD_ENABLE && CONFIG_TMX_LCD_BACKLIGHT_PWM && \
    (CONFIG_TMX_CAMERA_XCLK_LEDC_TIMER == 2)
#error "摄像头 XCLK 和 LCD 背光都占了 LEDC_TIMER_2, 请在 menuconfig 里把摄像头 XCLK 定时器改成 0 或 1"
#endif
#if (CONFIG_TMX_CAMERA_XCLK_LEDC_TIMER == 3)
#error "LEDC_TIMER_3 固定给舵机 (50Hz/14bit), 摄像头 XCLK 不能占用它"
#endif
#if CONFIG_TMX_LCD_ENABLE && CONFIG_TMX_LCD_BACKLIGHT_PWM && \
    (CONFIG_TMX_CAMERA_XCLK_LEDC_CHANNEL == CONFIG_TMX_LCD_BACKLIGHT_LEDC_CHANNEL)
#error "摄像头 XCLK 和 LCD 背光用了同一个 LEDC 通道, 请在 menuconfig 里把其中一个改掉"
#endif

#define CAM_CHUNK_MAX        240  /* 单包 JPEG 数据上限 (包长字节最大 255) */
#define CAM_CHUNKS_PER_POLL  16   /* 每轮最多发几片, 免得长时间占住主循环 */

static const framesize_t s_frame_sizes[] = {
    FRAMESIZE_QVGA,   /* 0: 320x240  */
    FRAMESIZE_VGA,    /* 1: 640x480  */
    FRAMESIZE_SVGA,   /* 2: 800x600  */
    FRAMESIZE_XGA,    /* 3: 1024x768 */
    FRAMESIZE_SXGA,   /* 4: 1280x1024*/
    FRAMESIZE_UXGA,   /* 5: 1600x1200*/
};
#define CAM_SIZE_COUNT ((int)(sizeof(s_frame_sizes) / sizeof(s_frame_sizes[0])))

static const char *s_frame_names[] = {
    "QVGA", "VGA", "SVGA", "XGA", "SXGA", "UXGA"
};

#if CONFIG_TMX_CAMERA_FRAMESIZE_QVGA
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_QVGA
#elif CONFIG_TMX_CAMERA_FRAMESIZE_SVGA
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_SVGA
#elif CONFIG_TMX_CAMERA_FRAMESIZE_XGA
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_XGA
#elif CONFIG_TMX_CAMERA_FRAMESIZE_SXGA
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_SXGA
#elif CONFIG_TMX_CAMERA_FRAMESIZE_UXGA
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_UXGA
#else
#define CAM_DEFAULT_SIZE TMX_CAMERA_SIZE_VGA
#endif

static bool               s_ready;
static tmx_camera_send_fn s_send;
static camera_config_t    s_cfg;            /* 自检失败要重新初始化时用 */
static bool               s_sensor_on;      /* 传感器现在是不是开着 (开着就发热) */

static bool frame_header_ok(const camera_fb_t *fb);

static camera_fb_t       *s_fb;             /* 正在发给 PC 的那一帧 */
static size_t             s_offset;         /* 已经发到帧内的哪个偏移 */
static uint8_t            s_frame_index;    /* 帧序号 (0~255 回绕) */
static uint32_t           s_frames_sent;    /* 累计发给 PC 的帧数 */
static int                s_frames_left;    /* -1 = 一直拍, 0 = 不拍, >0 = 还要拍几帧 */
static uint32_t           s_interval_ms;    /* 两帧之间的最小间隔 */
static uint64_t           s_last_frame_ms;  /* 上一帧发完的时刻 */
static int                s_errors;         /* 连续失败次数 */

static int                s_quality;
static int                s_size_index;

#if CONFIG_TMX_CAMERA_PIN_PROBE
/*
 * 引脚活动探针 (排障用, menuconfig 里默认关)
 *
 * 摄像头初始化完之后传感器就在出流了, 这时用硬件脉冲计数器 (PCNT) 数一遍
 * 各根信号线上的边沿, 就能知道每根线"到底在跑多快":
 *
 *   VSYNC : 一帧一次            -> 几十 Hz
 *   HREF  : 每行一次            -> 几千 Hz (行数 x 帧率)
 *   PCLK  : 每个像素一次        -> 几 MHz
 *   数据线 : 随像素变化          -> 与 PCLK 同量级, 但会低一些
 *
 * 用软件采样是量不准的 (信号频率和采样率接近时会混叠, 看起来"很慢"),
 * 所以这里用 PCNT 硬件计数器, 数出来是多少就是多少。
 *
 * 判断方法:
 *   IO3 (VSYNC) 数出来是几千 Hz 以上 -> 它实际接的是 HREF/PCLK, 同步线接反了;
 *   IO16 (PCLK) 数出来是 0          -> 像素时钟没过来 (线没接 / XCLK 没到传感器)。
 */
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_sig_map.h"
#include "soc/lcd_cam_struct.h"
#include "soc/soc.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_io_reg.h"

static void cam_probe_pin(const char *name, int pin, int window_ms, int glitch_ns)
{
    if (pin < 0) {
        return;
    }

    /* 先看这根线有没有被"外部驱动":
     *   开内部上拉/下拉各测一次电平 —— 电平跟着我们变 = 悬空(没人驱动);
     *   电平咬死不变 = 摄像头在驱动它。 */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(1);
    int with_pullup = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(1);
    int with_pulldown = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);

    const char *drive;
    if (with_pullup == 0 && with_pulldown == 0) {
        drive = "被驱动为低";
    } else if (with_pullup == 1 && with_pulldown == 1) {
        drive = "被驱动为高";
    } else {
        drive = "悬空/没被驱动";
    }

    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32000,
        .low_limit = -1,
    };
    if (pcnt_new_unit(&unit_cfg, &unit) != ESP_OK) {
        ESP_LOGW(TAG, "探针: PCNT 单元创建失败");
        return;
    }
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = pin,
        .level_gpio_num = -1,
    };
    if (pcnt_new_channel(unit, &chan_cfg, &chan) != ESP_OK) {
        ESP_LOGW(TAG, "探针: %s (IO%d) 通道创建失败", name, pin);
        return;
    }
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    /* 毛刺滤波: glitch_ns=0 表示不过滤 (数 MHz 的 PCLK 必须这样),
     * 1000 = 1us, 用来滤掉线上拾到的噪声毛刺, 只看真正的信号 */
    if (glitch_ns > 0) {
        pcnt_glitch_filter_config_t filter_cfg = { .max_glitch_ns = glitch_ns };
        pcnt_unit_set_glitch_filter(unit, &filter_cfg);
    }

    pcnt_unit_clear_count(unit);
    pcnt_unit_enable(unit);
    pcnt_unit_start(unit);
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    pcnt_unit_stop(unit);
    int edges = 0;
    pcnt_unit_get_count(unit, &edges);
    pcnt_unit_disable(unit);
    pcnt_del_channel(chan);
    pcnt_del_unit(unit);

    /* 删单元时驱动可能把引脚输入关了, 恢复成普通输入 */
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    ESP_LOGW(TAG, "探针 %-5s (IO%-2d): %7d 沿/%-4dms = %8d Hz  %s [%s]",
             name, pin, edges, window_ms,
             (int)((int64_t)edges * 1000 / window_ms),
             glitch_ns > 0 ? "滤波1us" : "不过滤 ",
             drive);
}

/*
 * 数到足够多边沿就收工, 用来判断"这根线到底有没有数据"。
 *
 * 为什么不能只用一个固定的小窗口 (比如 1ms): OV2640 输出 JPEG 是"一阵一阵"的,
 * 一帧 (VGA/10fps 约 100ms) 里真正在吐数据的时间只有十几 ms, 固定 1ms 窗口很容易
 * 正好落在空闲期 —— 看起来就像"这根数据线死了", 下次换个窗口死的又是别的线。
 * 所以这里一直数到 enough 个边沿或者超时, 超时还是一根毛都没有才是真的没信号。
 */
static int cam_count_edges_until(int pin, int enough, int timeout_ms, int *elapsed_ms)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32000,
        .low_limit = -1,
    };
    if (pcnt_new_unit(&unit_cfg, &unit) != ESP_OK) {
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return -1;
    }
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = pin,
        .level_gpio_num = -1,
    };
    if (pcnt_new_channel(unit, &chan_cfg, &chan) != ESP_OK) {
        pcnt_del_unit(unit);
        if (elapsed_ms) {
            *elapsed_ms = 0;
        }
        return -1;
    }
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    pcnt_unit_clear_count(unit);
    pcnt_unit_enable(unit);
    pcnt_unit_start(unit);

    uint64_t t0 = (uint64_t)esp_timer_get_time();
    int edges = 0;
    int used = 0;
    while (1) {
        pcnt_unit_get_count(unit, &edges);
        used = (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
        if (edges >= enough || used >= timeout_ms) {
            break;
        }
        vTaskDelay(1);
    }

    pcnt_unit_stop(unit);
    pcnt_unit_disable(unit);
    pcnt_del_channel(chan);
    pcnt_del_unit(unit);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    if (elapsed_ms) {
        *elapsed_ms = used;
    }
    return edges;
}

/* 内部上拉/下拉各测一次: 两次都读 0 = 被外部低阻钳在低电平 */
static const char *cam_pull_verdict(int pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(1);
    int with_pullup = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(1);
    int with_pulldown = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);

    if (with_pullup == 0 && with_pulldown == 0) {
        return "被外部拉低";
    }
    if (with_pullup == 1 && with_pulldown == 1) {
        return "被外部拉高";
    }
    return "悬空(信号在跳)";
}

static void cam_probe_data_pin(int index, int pin)
{
    const char *pull = cam_pull_verdict(pin);
    int ms = 0;
    int edges = cam_count_edges_until(pin, 1000, 800, &ms);

    if (edges <= 0) {
        ESP_LOGW(TAG, "D%d (IO%-2d): %d 沿 / %4d ms  [%s]  <- 这一段时间完全没信号",
                 index, pin, edges, ms, pull);
    } else {
        ESP_LOGW(TAG, "D%d (IO%-2d): %d 沿 / %4d ms  [%s]  <- 有数据 (数够就收工)",
                 index, pin, edges, ms, pull);
    }
}

static void cam_probe_restore_pins(void)
{
    /* 把摄像头用的引脚重新接到 LCD_CAM 上 (万一 PCNT 动过路由) */
    esp_rom_gpio_connect_in_signal(CONFIG_TMX_CAMERA_PCLK_PIN, CAM_PCLK_IDX, false);
    esp_rom_gpio_connect_in_signal(CONFIG_TMX_CAMERA_VSYNC_PIN, CAM_V_SYNC_IDX, true);
    esp_rom_gpio_connect_in_signal(CONFIG_TMX_CAMERA_HREF_PIN, CAM_H_ENABLE_IDX, false);
    const int data_pins[8] = {
        CONFIG_TMX_CAMERA_D0_PIN, CONFIG_TMX_CAMERA_D1_PIN,
        CONFIG_TMX_CAMERA_D2_PIN, CONFIG_TMX_CAMERA_D3_PIN,
        CONFIG_TMX_CAMERA_D4_PIN, CONFIG_TMX_CAMERA_D5_PIN,
        CONFIG_TMX_CAMERA_D6_PIN, CONFIG_TMX_CAMERA_D7_PIN,
    };
    for (int i = 0; i < 8; i++) {
        esp_rom_gpio_connect_in_signal(data_pins[i], CAM_DATA_IN0_IDX + i, false);
    }
}

/* 量一根脚在窗口里跳了多少个边沿 (硬件计数器, 软件采样会混叠) */
static int cam_count_edges(int pin, int window_ms, int glitch_ns)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    pcnt_unit_config_t unit_cfg = {
        .high_limit = 32000,
        .low_limit = -1,
    };
    if (pcnt_new_unit(&unit_cfg, &unit) != ESP_OK) {
        return -1;
    }
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = pin,
        .level_gpio_num = -1,
    };
    if (pcnt_new_channel(unit, &chan_cfg, &chan) != ESP_OK) {
        pcnt_del_unit(unit);
        return -1;
    }
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    if (glitch_ns > 0) {
        pcnt_glitch_filter_config_t filter_cfg = { .max_glitch_ns = glitch_ns };
        pcnt_unit_set_glitch_filter(unit, &filter_cfg);
    }

    pcnt_unit_clear_count(unit);
    pcnt_unit_enable(unit);
    pcnt_unit_start(unit);
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    pcnt_unit_stop(unit);
    int edges = 0;
    pcnt_unit_get_count(unit, &edges);
    pcnt_unit_disable(unit);
    pcnt_del_channel(chan);
    pcnt_del_unit(unit);

    /* 删单元时驱动可能把引脚输入关了, 恢复成普通输入 */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    return edges;
}

/*
 * 全引脚扫描 (排障用): 摄像头在出流的时候, 把板上"没被外设占用"的 GPIO
 * 逐个量 1ms 的边沿数, 只打有活动的脚。
 *
 * 用途: 怀疑某根数据线在板子上被走到别的 IO 时, 一眼能看出来 ——
 * 比如 D4/D5 在 IO15/IO17 上量不到东西, 而 IO23/IO24 上有 300kHz 的信号,
 * 那就是走线跟原理图对不上 (改 Kconfig 就行), 而不是线断了 (要动烙铁)。
 *
 * 跳过: 摄像头自己那几根 / I2C / 屏幕 / 音频 / USB / Flash+PSRAM / 控制台串口。
 */
static void cam_scan_free_pins(void)
{
    static const int skip[] = {
        1, 2,                        /* I2C: 音频 codec + 摄像头 SCCB */
        3, 4, 5, 6, 7, 8, 9,         /* DVP: VSYNC / D2 / D1 / D3 / D0 / XCLK / D7 */
        12, 13, 14, 38, 45, 47,      /* 音频 I2S + PA */
        15, 16, 17, 18, 46, 48,      /* DVP: D4 / PCLK / D5 / D6 / HREF / PWDN */
        19, 20,                      /* USB */
        21, 39, 40, 41, 42,          /* 屏幕 SPI + DC / CS / 背光 */
        43, 44,                      /* 控制台串口 */
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,  /* Flash / PSRAM */
    };

    ESP_LOGW(TAG, "=== 全引脚扫描: 每根 1ms, 只列有活动的脚 ===");
    for (int pin = 0; pin <= 48; pin++) {
        bool used = false;
        for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
            if (skip[i] == pin) {
                used = true;
                break;
            }
        }
        if (used) {
            continue;
        }

        int edges = cam_count_edges(pin, 1, 0);
        if (edges >= 20) {
            ESP_LOGW(TAG, "  扫描 IO%-2d: %7d 沿/1ms = %8d Hz", pin, edges, edges * 1000);
        }
    }
    ESP_LOGW(TAG, "=== 扫描结束 ===");
}

/*
 * 引脚"归属"诊断 (只读, 一个寄存器都不写)
 *
 * 用来回答: 这根脚到底被谁占着 —— 普通数字 GPIO, 还是 RTC 域 (32K 晶振 / 触摸),
 * 还是某个外设通过 GPIO 矩阵在驱动它 (UART 的 RTS/TXD、LEDC...)?
 *
 *   IOMUX MCU_SEL : 1 = 普通数字 GPIO; 0 = 模拟功能 (ADC/触摸/32K 晶振); 其它 = 外设固定功能
 *   out_sel       : 256 = 自己的 GPIO 输出寄存器; 别的值是外设信号号 (下面会翻成名字)
 *   oen_sel = 1   : 输出使能由矩阵里那条信号的 OE 决定 —— 这种脚"设成输入"也照样被外设驱动
 *   RTC mux = 1   : 这根脚被 RTC_IO 接管 (32K 晶振 / 模拟), 数字 GPIO 说了不算
 *
 * ESP32-S3 上: IO15 = XTAL_32K_P, IO16 = XTAL_32K_N, IO17 = DAC_1 (RTC pad)。
 */
static const char *cam_out_src_name(uint32_t out_sel)
{
    switch (out_sel) {
    case SIG_GPIO_OUT_IDX: return "GPIO 输出寄存器";
    case U0TXD_OUT_IDX:    return "U0TXD(UART0 TX)";
    case U0RTS_OUT_IDX:    return "U0RTS(UART0 RTS)";
    case U1TXD_OUT_IDX:    return "U1TXD(UART1 TX)";
    case U1RTS_OUT_IDX:    return "U1RTS(UART1 RTS)";
    case U2TXD_OUT_IDX:    return "U2TXD(UART2 TX)";
    default:               return "其它外设信号";
    }
}

static void cam_dump_pin_owner(const char *label, int pin)
{
    uint32_t iomux;
    uint32_t rtc = 0;
    const char *rtc_name = "-";
    int rtc_mux = -1, rtc_fun = -1, rtc_ie = -1, rtc_rue = -1, rtc_rde = -1;

    switch (pin) {
    case 15:
        iomux = REG_READ(IO_MUX_GPIO15_REG);
        rtc = REG_READ(RTC_IO_XTAL_32P_PAD_REG);
        rtc_name = "XTAL_32K_P";
        rtc_mux = (int)((rtc >> RTC_IO_X32P_MUX_SEL_S) & RTC_IO_X32P_MUX_SEL_V);
        rtc_fun = (int)((rtc >> RTC_IO_X32P_FUN_SEL_S) & RTC_IO_X32P_FUN_SEL_V);
        rtc_ie  = (int)((rtc >> RTC_IO_X32P_FUN_IE_S) & RTC_IO_X32P_FUN_IE_V);
        rtc_rue = (int)((rtc >> RTC_IO_X32P_RUE_S) & RTC_IO_X32P_RUE_V);
        rtc_rde = (int)((rtc >> RTC_IO_X32P_RDE_S) & RTC_IO_X32P_RDE_V);
        break;
    case 16:
        iomux = REG_READ(IO_MUX_GPIO16_REG);
        rtc = REG_READ(RTC_IO_XTAL_32N_PAD_REG);
        rtc_name = "XTAL_32K_N";
        rtc_mux = (int)((rtc >> RTC_IO_X32N_MUX_SEL_S) & RTC_IO_X32N_MUX_SEL_V);
        rtc_fun = (int)((rtc >> RTC_IO_X32N_FUN_SEL_S) & RTC_IO_X32N_FUN_SEL_V);
        rtc_ie  = (int)((rtc >> RTC_IO_X32N_FUN_IE_S) & RTC_IO_X32N_FUN_IE_V);
        rtc_rue = (int)((rtc >> RTC_IO_X32N_RUE_S) & RTC_IO_X32N_RUE_V);
        rtc_rde = (int)((rtc >> RTC_IO_X32N_RDE_S) & RTC_IO_X32N_RDE_V);
        break;
    case 17:
        iomux = REG_READ(IO_MUX_GPIO17_REG);
        rtc = REG_READ(RTC_IO_PAD_DAC1_REG);
        rtc_name = "DAC_1";
        rtc_mux = (int)((rtc >> RTC_IO_PDAC1_MUX_SEL_S) & RTC_IO_PDAC1_MUX_SEL_V);
        rtc_fun = (int)((rtc >> RTC_IO_PDAC1_FUN_SEL_S) & RTC_IO_PDAC1_FUN_SEL_V);
        rtc_ie  = (int)((rtc >> RTC_IO_PDAC1_FUN_IE_S) & RTC_IO_PDAC1_FUN_IE_V);
        rtc_rue = (int)((rtc >> RTC_IO_PDAC1_RUE_S) & RTC_IO_PDAC1_RUE_V);
        rtc_rde = (int)((rtc >> RTC_IO_PDAC1_RDE_S) & RTC_IO_PDAC1_RDE_V);
        break;
    default:
        return;            /* 其它脚没有对应的 RTC pad 结构, 不在这儿看 */
    }

    uint32_t iomux_raw = iomux;
    /* 注意: 结构体里这个字段叫 func_sel, 值就是 out_sel (256 = 自己的 GPIO 输出) */
    uint32_t out_sel = GPIO.func_out_sel_cfg[pin].func_sel;
    uint32_t oen_sel = GPIO.func_out_sel_cfg[pin].oen_sel;
    uint32_t oen_inv = GPIO.func_out_sel_cfg[pin].oen_inv_sel;
    int      gpio_oe = (int)((GPIO.enable >> pin) & 1);

    /* 注意: 这里只能用移位解字段 —— REG_GET_FIELD() 是拿第一个参数当"寄存器地址"
     * 去读的, 把读回来的值传进去会变成"按这个值当地址读内存", 直接跑飞。 */
    ESP_LOGW(TAG, "PIN %-4s IO%-2d IOMUX=0x%08X MCU_SEL=%u IE=%u PU=%u PD=%u",
             label, pin, (unsigned)iomux_raw,
             (unsigned)((iomux_raw >> MCU_SEL_S) & MCU_SEL_V),
             (unsigned)((iomux_raw >> FUN_IE_S) & FUN_IE_V),
             (unsigned)((iomux_raw >> FUN_PU_S) & FUN_PU_V),
             (unsigned)((iomux_raw >> FUN_PD_S) & FUN_PD_V));
    ESP_LOGW(TAG, "PIN %-4s IO%-2d 矩阵 out_sel=%u(%s) oen_sel=%u oen_inv=%u GPIO_OE=%d",
             label, pin, (unsigned)out_sel, cam_out_src_name(out_sel),
             (unsigned)oen_sel, (unsigned)oen_inv, gpio_oe);
    ESP_LOGW(TAG, "PIN %-4s IO%-2d RTC %s=0x%08X mux=%d fun=%d ie=%d rue=%d rde=%d",
             label, pin, rtc_name, (unsigned)rtc,
             rtc_mux, rtc_fun, rtc_ie, rtc_rue, rtc_rde);
}

static void cam_probe_all_pins(void)
{
    /* 驱动在 VSYNC 中断里刷告警, 但日志等级压不住它 (ESP_DRAM_LOGW 直写),
     * 所以探针期间直接把 LCD_CAM 的 VSYNC 中断关掉, 完了再打开。 */
    LCD_CAM.lc_dma_int_ena.cam_vsync_int_ena = 0;
    LCD_CAM.lc_dma_int_clr.cam_vsync_int_clr = 1;

    ESP_LOGW(TAG, "=== 引脚探针: 期望 PCLK 几MHz / VSYNC 几十Hz / HREF 几kHz / 数据线 与 PCLK 同量级 ===");
    /* 先看这几根脚的"归属" (只读), 判断是不是被 RTC 域 / 外设矩阵占着 */
    cam_dump_pin_owner("D4", CONFIG_TMX_CAMERA_D4_PIN);
    cam_dump_pin_owner("PCLK", CONFIG_TMX_CAMERA_PCLK_PIN);
    cam_dump_pin_owner("D5", CONFIG_TMX_CAMERA_D5_PIN);
    cam_probe_pin("PCLK", CONFIG_TMX_CAMERA_PCLK_PIN, 1, 0);
    cam_probe_pin("VSYNC", CONFIG_TMX_CAMERA_VSYNC_PIN, 200, 1000);
    cam_probe_pin("HREF", CONFIG_TMX_CAMERA_HREF_PIN, 20, 1000);
    cam_probe_data_pin(0, CONFIG_TMX_CAMERA_D0_PIN);
    cam_probe_data_pin(1, CONFIG_TMX_CAMERA_D1_PIN);
    cam_probe_data_pin(2, CONFIG_TMX_CAMERA_D2_PIN);
    cam_probe_data_pin(3, CONFIG_TMX_CAMERA_D3_PIN);
    cam_probe_data_pin(4, CONFIG_TMX_CAMERA_D4_PIN);
    cam_probe_data_pin(5, CONFIG_TMX_CAMERA_D5_PIN);
    cam_probe_data_pin(6, CONFIG_TMX_CAMERA_D6_PIN);
    cam_probe_data_pin(7, CONFIG_TMX_CAMERA_D7_PIN);
    cam_probe_restore_pins();
    LCD_CAM.lc_dma_int_ena.cam_vsync_int_ena = 1;
    ESP_LOGW(TAG, "=== 探针结束 ===");
}

void tmx_camera_probe_pins(void)
{
    cam_probe_all_pins();
}
#endif /* CONFIG_TMX_CAMERA_PIN_PROBE */

#if !CONFIG_TMX_CAMERA_PIN_PROBE
void tmx_camera_probe_pins(void)
{
    ESP_LOGW(TAG, "引脚探针没编译进来 (menuconfig -> 摄像头 -> 启动时量一遍 DVP 各根信号线的活动)");
}
#endif

/* 摄像头引脚和屏幕/音频引脚撞车的话, 两边都跑不起来, 上电先把话说清楚。
 * (SCCB 的 IO1/IO2 是故意和音频共用 I2C 的, 不算冲突) */
static void warn_pin_conflicts(void)
{
    static const struct {
        int         pin;
        const char *name;
    } cam_pins[] = {
        { CONFIG_TMX_CAMERA_XCLK_PIN,  "XCLK" },
        { CONFIG_TMX_CAMERA_PCLK_PIN,  "PCLK" },
        { CONFIG_TMX_CAMERA_VSYNC_PIN, "VSYNC" },
        { CONFIG_TMX_CAMERA_HREF_PIN,  "HREF" },
        { CONFIG_TMX_CAMERA_PWDN_PIN,  "PWDN" },
        { CONFIG_TMX_CAMERA_D0_PIN,    "D0" },
        { CONFIG_TMX_CAMERA_D1_PIN,    "D1" },
        { CONFIG_TMX_CAMERA_D2_PIN,    "D2" },
        { CONFIG_TMX_CAMERA_D3_PIN,    "D3" },
        { CONFIG_TMX_CAMERA_D4_PIN,    "D4" },
        { CONFIG_TMX_CAMERA_D5_PIN,    "D5" },
        { CONFIG_TMX_CAMERA_D6_PIN,    "D6" },
        { CONFIG_TMX_CAMERA_D7_PIN,    "D7" },
    };
    static const struct {
        int         pin;
        const char *name;
    } other_pins[] = {
#if CONFIG_TMX_LCD_ENABLE
        { CONFIG_TMX_LCD_SPI_SCK_PIN,  "LCD SCK" },
        { CONFIG_TMX_LCD_SPI_MOSI_PIN, "LCD MOSI" },
        { CONFIG_TMX_LCD_DC_PIN,       "LCD DC" },
        { CONFIG_TMX_LCD_CS_PIN,       "LCD CS" },
        { CONFIG_TMX_LCD_BACKLIGHT_PIN, "LCD 背光" },
#endif
#if CONFIG_TMX_AUDIO_ENABLE
        { CONFIG_TMX_AUDIO_I2S_MCLK_PIN, "音频 MCLK" },
        { CONFIG_TMX_AUDIO_I2S_BCLK_PIN, "音频 BCLK" },
        { CONFIG_TMX_AUDIO_I2S_WS_PIN,   "音频 WS" },
        { CONFIG_TMX_AUDIO_I2S_DOUT_PIN, "音频 DOUT" },
        { CONFIG_TMX_AUDIO_I2S_DIN_PIN,  "音频 DIN" },
        { CONFIG_TMX_AUDIO_PA_PIN,       "音频 PA" },
#endif
        { -1, NULL },
    };

    for (size_t i = 0; i < sizeof(cam_pins) / sizeof(cam_pins[0]); i++) {
        if (cam_pins[i].pin < 0) {
            continue;
        }
        for (size_t j = 0; other_pins[j].pin >= 0; j++) {
            if (cam_pins[i].pin == other_pins[j].pin) {
                ESP_LOGW(TAG, "引脚冲突: 摄像头 %s = IO%d, 但 %s 也用它; "
                              "请在 menuconfig 里给其中一个换脚",
                         cam_pins[i].name, cam_pins[i].pin, other_pins[j].name);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static sensor_t *sensor_or_null(void)
{
    return s_ready ? esp_camera_sensor_get() : NULL;
}

static void send_status(int state, int value)
{
    if (s_send == NULL) {
        return;
    }
    uint8_t packet[4] = { 3, TMX_REPORT_CAMERA_STATUS,
                          (uint8_t)state, (uint8_t)value };
    s_send(packet, sizeof(packet));
}

static void drop_current_frame(void)
{
    if (s_fb != NULL) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
    s_offset = 0;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

/*
 * 给摄像头断电 (降温)。
 *
 * OV2640 只要初始化过就会一直出流: XCLK 不停、JPEG 编码器一直干活, 摸上去很烫。
 * 所以拍完就把 esp_camera 反初始化 (XCLK 停) 并且把 PWDN 拉高 (传感器掉电),
 * 下次拍照前再上电初始化。Kconfig TMX_CAMERA_IDLE_POWER_OFF 可以关掉这个行为。
 */
static void camera_sensor_off(void)
{
#if !CONFIG_TMX_CAMERA_IDLE_POWER_OFF
    return;
#else
    if (!s_sensor_on) {
        return;
    }

    esp_camera_deinit();
    s_sensor_on = false;

    if (CONFIG_TMX_CAMERA_PWDN_PIN >= 0) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = 1ULL << CONFIG_TMX_CAMERA_PWDN_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_cfg);
        gpio_set_level(CONFIG_TMX_CAMERA_PWDN_PIN, 1);   /* PWDN: 1 = 掉电 (与 RESET 反相) */
    }
    ESP_LOGI(TAG, "空闲降温: 摄像头已断电 (PWDN=1, XCLK 停), 下次拍照前重新初始化");
#endif
}

esp_err_t tmx_camera_init(void)
{
    if (s_ready && s_sensor_on) {
        return ESP_OK;          /* 已经开着 */
    }

    /* SCCB 走板载 I2C 总线: 音频那一侧可能已经建好了, 没有就按摄像头引脚建 */
    if (!tmx_i2c_is_ready()) {
        esp_err_t err = tmx_i2c_begin(CONFIG_TMX_CAMERA_SIOD_PIN,
                                      CONFIG_TMX_CAMERA_SIOC_PIN);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C 总线建立失败, 摄像头不可用: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_ready) {             /* 只在第一次初始化时套用 Kconfig 默认值;
                                 * 之后重新上电要保留在线改过的分辨率/质量 */
        s_size_index = CAM_DEFAULT_SIZE;
        s_quality = CONFIG_TMX_CAMERA_JPEG_QUALITY;
    }

    /* 冷启动时 AXP2101 只开了 AVDD, DVDD 是关的 -> SCCB 读写会时好时坏 */
    camera_power_on();

    warn_pin_conflicts();

    camera_config_t cfg = {
        .pin_pwdn     = CONFIG_TMX_CAMERA_PWDN_PIN,
        .pin_reset    = CONFIG_TMX_CAMERA_RESET_PIN,
        .pin_xclk     = CONFIG_TMX_CAMERA_XCLK_PIN,
        /* -1 = 不自己建总线, 挂到 sccb_i2c_port 上已有的那条 */
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d7       = CONFIG_TMX_CAMERA_D7_PIN,
        .pin_d6       = CONFIG_TMX_CAMERA_D6_PIN,
        .pin_d5       = CONFIG_TMX_CAMERA_D5_PIN,
        .pin_d4       = CONFIG_TMX_CAMERA_D4_PIN,
        .pin_d3       = CONFIG_TMX_CAMERA_D3_PIN,
        .pin_d2       = CONFIG_TMX_CAMERA_D2_PIN,
        .pin_d1       = CONFIG_TMX_CAMERA_D1_PIN,
        .pin_d0       = CONFIG_TMX_CAMERA_D0_PIN,
        .pin_vsync    = CONFIG_TMX_CAMERA_VSYNC_PIN,
        .pin_href     = CONFIG_TMX_CAMERA_HREF_PIN,
        .pin_pclk     = CONFIG_TMX_CAMERA_PCLK_PIN,

        .xclk_freq_hz = CONFIG_TMX_CAMERA_XCLK_FREQ_HZ,
        .ledc_timer   = (ledc_timer_t)CONFIG_TMX_CAMERA_XCLK_LEDC_TIMER,
        .ledc_channel = (ledc_channel_t)CONFIG_TMX_CAMERA_XCLK_LEDC_CHANNEL,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = s_frame_sizes[s_size_index],
        .jpeg_quality = s_quality,
        .fb_count     = CONFIG_TMX_CAMERA_FB_COUNT,
#if CONFIG_SPIRAM
        .fb_location  = CAMERA_FB_IN_PSRAM,
#else
        .fb_location  = CAMERA_FB_IN_DRAM,
#endif
        .grab_mode    = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = tmx_i2c_port_num(),
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OV2640 初始化失败: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "  检查: 摄像头供电 / PWDN=IO%d / XCLK=IO%d / SIOD=IO%d / SIOC=IO%d",
                 CONFIG_TMX_CAMERA_PWDN_PIN, CONFIG_TMX_CAMERA_XCLK_PIN,
                 CONFIG_TMX_CAMERA_SIOD_PIN, CONFIG_TMX_CAMERA_SIOC_PIN);
        if (err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "  内存不够: 把 TMX_CAMERA_FB_COUNT 改成 1, 或把分辨率降到 QVGA");
        } else {
            ESP_LOGE(TAG, "  SCCB 上没认到传感器时, 用 tools/pc_camera_check.py --scan 或 i2c 扫描确认 0x30 是否应答");
        }
#if CONFIG_TMX_CAMERA_PIN_PROBE
        /* 初始化失败也要量: 摄像头没供电/断线的时候, 这几根线的状态最能说明问题 */
        cam_probe_all_pins();
#endif
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    s_ready = true;
    s_sensor_on = true;
    s_cfg = cfg;

    /* 开机自检: 抓一帧看看 JPEG 头好不好。这块板子偶尔会碰上"初始化少写进去
     * 几个寄存器"的状态: DVP 采样错位 -> 帧头里的 DHT (FF C4) 被采成 DAC (FF C5)
     * -> PC 侧谁也解不开, 人眼看就是满屏噪点。碰上就重新初始化再来 (最多 3 次)。 */
    for (int attempt = 1; attempt <= 3; attempt++) {
        camera_fb_t *fb = esp_camera_fb_get();
        bool ok = frame_header_ok(fb);
        if (fb != NULL) {
            esp_camera_fb_return(fb);
        }
        if (ok) {
            ESP_LOGI(TAG, "开机自检: 第 %d 帧 JPEG 头正常", attempt);
            break;
        }
        ESP_LOGW(TAG, "开机自检: 第 %d 帧 JPEG 头不对 (采样错位), 重新初始化摄像头", attempt);
        esp_camera_deinit();
        /* 光重新 init 不够: 传感器内部状态没复位。把 PWDN 拉高做一次真正的掉电再上电,
         * 这样"采样错位"的状态才能清掉 (实测有效)。 */
        if (CONFIG_TMX_CAMERA_PWDN_PIN >= 0) {
            gpio_set_level(CONFIG_TMX_CAMERA_PWDN_PIN, 1);      /* 掉电 */
        }
#if CONFIG_TMX_CAMERA_PMIC_AXP2101
        camera_rails_off();                                     /* 连 AVDD/DVDD 一起断 */
#endif
        vTaskDelay(pdMS_TO_TICKS(200));
#if CONFIG_TMX_CAMERA_PMIC_AXP2101
        camera_power_on();                                      /* 重新设电压 + 打开 */
#endif
        if (CONFIG_TMX_CAMERA_PWDN_PIN >= 0) {
            gpio_set_level(CONFIG_TMX_CAMERA_PWDN_PIN, 0);      /* 上电 */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        if (esp_camera_init(&s_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "重新初始化摄像头失败, 拍照功能不可用");
            s_ready = false;
            return ESP_FAIL;
        }
        sensor = esp_camera_sensor_get();
    }

    /* 弱光降噪: 压住自动增益上限, 让 AEC 用曝光时间(而不是增益)去补偿。
     * 上限越小噪点越少、画面越暗; 想要原厂行为把 Kconfig 填 6 (128X)。 */
    if (sensor != NULL) {
        sensor->set_gainceiling(sensor, (gainceiling_t)CONFIG_TMX_CAMERA_GAIN_CEILING);
        ESP_LOGI(TAG, "自动增益上限设为 %d (0=2X 3=16X 6=128X)", CONFIG_TMX_CAMERA_GAIN_CEILING);
    }

#if CONFIG_TMX_CAMERA_PIN_PROBE
    cam_probe_all_pins();
    cam_scan_free_pins();
#endif

    ESP_LOGI(TAG, "OV2640 ready: %s %dx%d, JPEG 质量 %d, XCLK %dMHz, fb=%d, PID=0x%02X",
             s_frame_names[s_size_index],
             sensor ? resolution[sensor->status.framesize].width : 0,
             sensor ? resolution[sensor->status.framesize].height : 0,
             s_quality, CONFIG_TMX_CAMERA_XCLK_FREQ_HZ / 1000000,
             CONFIG_TMX_CAMERA_FB_COUNT, sensor ? sensor->id.PID : 0);
    ESP_LOGI(TAG, "引脚: XCLK=%d PCLK=%d VSYNC=%d HREF=%d D0..D7=%d,%d,%d,%d,%d,%d,%d,%d PWDN=%d SCCB=IO%d/IO%d(共用 I2C)",
             CONFIG_TMX_CAMERA_XCLK_PIN, CONFIG_TMX_CAMERA_PCLK_PIN,
             CONFIG_TMX_CAMERA_VSYNC_PIN, CONFIG_TMX_CAMERA_HREF_PIN,
             CONFIG_TMX_CAMERA_D0_PIN, CONFIG_TMX_CAMERA_D1_PIN,
             CONFIG_TMX_CAMERA_D2_PIN, CONFIG_TMX_CAMERA_D3_PIN,
             CONFIG_TMX_CAMERA_D4_PIN, CONFIG_TMX_CAMERA_D5_PIN,
             CONFIG_TMX_CAMERA_D6_PIN, CONFIG_TMX_CAMERA_D7_PIN,
             CONFIG_TMX_CAMERA_PWDN_PIN,
             CONFIG_TMX_CAMERA_SIOD_PIN, CONFIG_TMX_CAMERA_SIOC_PIN);

    /* 开机验完就断电: 平时不拍照时板子上的摄像头应该是凉的 */
    camera_sensor_off();
    return ESP_OK;
}

bool tmx_camera_is_ready(void)
{
    return s_ready;
}

void tmx_camera_set_sender(tmx_camera_send_fn send)
{
    s_send = send;
}

/* ------------------------------------------------------------------ */
/* 参数 / 状态                                                         */
/* ------------------------------------------------------------------ */

esp_err_t tmx_camera_set_format(int frame_size, int quality, int pixformat)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sensor_on) {
        /* 空闲时传感器是断电的: 只记下新设置, 下次拍照上电时生效 */
        if (frame_size >= 0) {
            if (frame_size >= CAM_SIZE_COUNT) {
                return ESP_ERR_INVALID_ARG;
            }
            s_size_index = frame_size;
        }
        if (quality >= 0) {
            s_quality = quality > 63 ? 63 : quality;
        }
        ESP_LOGI(TAG, "摄像头空闲, 已记下设置: %s 质量 %d (下次拍照生效)",
                 s_frame_names[s_size_index], s_quality);
        return ESP_OK;
    }
    sensor_t *sensor = sensor_or_null();
    if (sensor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (frame_size >= 0) {
        if (frame_size >= CAM_SIZE_COUNT) {
            ESP_LOGW(TAG, "分辨率索引 %d 非法 (0~%d)", frame_size, CAM_SIZE_COUNT - 1);
            return ESP_ERR_INVALID_ARG;
        }
        if (sensor->set_framesize(sensor, s_frame_sizes[frame_size]) != 0) {
            ESP_LOGW(TAG, "设置分辨率失败 (索引 %d)", frame_size);
            return ESP_FAIL;
        }
        s_size_index = frame_size;
    }

    if (pixformat > 0) {
        pixformat_t target;
        switch (pixformat) {
            case TMX_CAMERA_PIX_JPEG:   target = PIXFORMAT_JPEG;   break;
            case TMX_CAMERA_PIX_YUV422: target = PIXFORMAT_YUV422; break;
            default:
                ESP_LOGW(TAG, "像素格式 %d 不支持 (1=JPEG, 2=YUV422)", pixformat);
                return ESP_ERR_INVALID_ARG;
        }
        /* 帧缓冲是初始化时按 JPEG 大小分配的 (VGA 只有 61KB), 在线切到 YUV422 需要
         * w*h*2 字节 (VGA 600KB+), 驱动会写穿缓冲区。要用 YUV422 就改 Kconfig 里的
         * 像素格式重新烧录; 这里直接拒绝, 免得把内存写坏。 */
        if (target != sensor->pixformat) {
            ESP_LOGW(TAG, "不支持在线切像素格式 (%d -> %d): 帧缓冲大小对不上, 请改 Kconfig 重新烧录",
                     (int)sensor->pixformat, (int)target);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (sensor->set_pixformat(sensor, target) != 0) {
            ESP_LOGW(TAG, "切换像素格式失败 (%d)", pixformat);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "像素格式 -> %s (排障用, 帧数据不再是 JPEG)",
                 target == PIXFORMAT_JPEG ? "JPEG" : "YUV422");
    }

    if (quality >= 0) {
        if (quality > 63) {
            quality = 63;
        }
        if (sensor->set_quality(sensor, quality) != 0) {
            ESP_LOGW(TAG, "设置 JPEG 质量失败 (%d)", quality);
            return ESP_FAIL;
        }
        s_quality = quality;
    }

    ESP_LOGI(TAG, "摄像头参数: %dx%d, JPEG 质量 %d",
             resolution[sensor->status.framesize].width,
             resolution[sensor->status.framesize].height, s_quality);
    return ESP_OK;
}

/* 把当前传感器参数打到串口 (在线调试用, 不改变任何东西) */
static void log_sensor_status(void)
{
    sensor_t *sensor = sensor_or_null();
    if (sensor == NULL) {
        ESP_LOGW(TAG, "传感器没就绪, 没有参数可看");
        return;
    }
    const camera_status_t *st = &sensor->status;
    ESP_LOGW(TAG, "传感器: %s %dx%d 质量 %d | 亮度 %d 对比 %d 饱和 %d 锐度 %d | "
                  "AE档 %d AEC值 %u AGC增益 %u 增益上限 %u(0=2X 3=16X 6=128X) | "
                  "AEC=%u AGC=%u AEC2=%u AWB=%u 镜像=%u 翻转=%u",
             s_frame_names[s_size_index],
             resolution[st->framesize].width, resolution[st->framesize].height, st->quality,
             st->brightness, st->contrast, st->saturation, st->sharpness,
             st->ae_level, st->aec_value, st->agc_gain, st->gainceiling,
             st->aec, st->agc, st->aec2, st->awb, st->hmirror, st->vflip);
}

/* 在线调传感器参数 (协议 0x7D) */
esp_err_t tmx_camera_tune(int field, int value)
{
    sensor_t *sensor = sensor_or_null();
    if (sensor == NULL) {
        ESP_LOGW(TAG, "摄像头没就绪, 调不了");
        return ESP_ERR_INVALID_STATE;
    }

    int ret = 0;
    switch (field) {
    case TMX_CAM_FIELD_STATUS:     log_sensor_status(); return ESP_OK;
    case TMX_CAM_FIELD_BRIGHTNESS: ret = sensor->set_brightness(sensor, value); break;
    case TMX_CAM_FIELD_CONTRAST:   ret = sensor->set_contrast(sensor, value); break;
    case TMX_CAM_FIELD_SATURATION: ret = sensor->set_saturation(sensor, value); break;
    case TMX_CAM_FIELD_AE_LEVEL:   ret = sensor->set_ae_level(sensor, value); break;
    case TMX_CAM_FIELD_AGC_GAIN:   ret = sensor->set_agc_gain(sensor, value); break;
    case TMX_CAM_FIELD_AEC_VALUE:  ret = sensor->set_aec_value(sensor, value); break;
    case TMX_CAM_FIELD_GAINCEILING:/* 0=2X 1=4X 2=8X 3=16X 4=32X 5=64X 6=128X */
        if (value < GAINCEILING_2X || value > GAINCEILING_128X) {
            ESP_LOGW(TAG, "增益上限 %d 非法 (0~6)", value);
            return ESP_ERR_INVALID_ARG;
        }
        ret = sensor->set_gainceiling(sensor, (gainceiling_t)value);
        break;
    case TMX_CAM_FIELD_HMIRROR:    ret = sensor->set_hmirror(sensor, value); break;
    case TMX_CAM_FIELD_VFLIP:      ret = sensor->set_vflip(sensor, value); break;
    case TMX_CAM_FIELD_AWB_GAIN:   ret = sensor->set_awb_gain(sensor, value); break;
    case TMX_CAM_FIELD_AEC2:       ret = sensor->set_aec2(sensor, value); break;
    case TMX_CAM_FIELD_AGC_CTRL:   ret = sensor->set_gain_ctrl(sensor, value); break;
    case TMX_CAM_FIELD_AEC_CTRL:   ret = sensor->set_exposure_ctrl(sensor, value); break;
    case TMX_CAM_FIELD_SET_REG_DSP:
    case TMX_CAM_FIELD_SET_REG_SEN: {
        int bank = (field == TMX_CAM_FIELD_SET_REG_DSP) ? 1 : 0;
        int reg = (value >> 8) & 0xFF;
        int val = value & 0xFF;
        ret = sensor->set_reg(sensor, (bank << 8) | reg, 0xFF, val);
        if (ret == 0) {
            ESP_LOGW(TAG, "写寄存器 bank%d[0x%02X] = 0x%02X", bank, reg, val);
        }
        break;
    }
    case TMX_CAM_FIELD_GET_REG: {
        int bank = (value >> 8) & 0x01;
        int reg = value & 0xFF;
        int got = sensor->get_reg(sensor, (bank << 8) | reg, 0xFF);
        ESP_LOGW(TAG, "读寄存器 bank%d[0x%02X] = 0x%02X", bank, reg, got & 0xFF);
        return ESP_OK;
    }
    default:
        ESP_LOGW(TAG, "没有这个字段: %d", field);
        return ESP_ERR_INVALID_ARG;
    }

    if (ret != 0) {
        ESP_LOGW(TAG, "字段 %d 设为 %d 失败", field, value);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "字段 %d -> %d 已生效", field, value);
    log_sensor_status();
    return ESP_OK;
}

/*
 * 抓来的这一帧, JPEG 头正常吗?
 *
 * 这块板子偶尔会碰上"传感器初始化少写进去几个寄存器"的情况, 表现是 DVP 采样错位:
 * 帧头里的 DHT 标记 (FF C4) 被采成 DAC (FF C5), 于是 PC 侧所有解码器都打不开,
 * 人眼看就是"一屏噪点"。正常帧里不会出现 FF C5, 而且要有一两个 FF C4 (DHT)。
 * 复位板子以后重新初始化就正常了, 所以这里做一个开机自检 + 自动重来。
 */
static bool frame_header_ok(const camera_fb_t *fb)
{
    const size_t probe = 700;   /* VGA 的 JPEG 头大约 600 字节 */
    if (fb == NULL || fb->len < probe) {
        return false;
    }
    if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        return false;
    }
    bool has_dht = false;
    for (size_t i = 0; i + 1 < probe; i++) {
        if (fb->buf[i] != 0xFF) {
            continue;
        }
        if (fb->buf[i + 1] == 0xC5) {
            return false;                 /* DAC: 采样错位 */
        }
        if (fb->buf[i + 1] == 0xC4) {
            has_dht = true;
        }
    }
    return has_dht;
}

int tmx_camera_state(void)
{
    if (!s_ready) {
        return TMX_CAMERA_STATE_ERROR;
    }
    if (!s_sensor_on) {
        return TMX_CAMERA_STATE_IDLE;      /* 空闲断电状态 */
    }
    return (s_fb != NULL || s_frames_left != 0) ? TMX_CAMERA_STATE_STREAM
                                                : TMX_CAMERA_STATE_IDLE;
}

uint32_t tmx_camera_frames_sent(void)
{
    return s_frames_sent;
}

int tmx_camera_quality(void)
{
    return s_quality;
}

void tmx_camera_send_info(void)
{
    if (s_send == NULL) {
        return;
    }
    sensor_t *sensor = sensor_or_null();
    uint16_t width = 0;
    uint16_t height = 0;
    if (sensor != NULL) {
        width = resolution[sensor->status.framesize].width;
        height = resolution[sensor->status.framesize].height;
    } else {
        /* 空闲时传感器是断电的, 按配置值上报 */
        width = resolution[s_frame_sizes[s_size_index]].width;
        height = resolution[s_frame_sizes[s_size_index]].height;
    }

    /* 数据: 状态(1) 宽(2) 高(2) 质量(1) 分辨率索引(1) XCLK MHz(1) */
    uint8_t packet[10] = {
        9, TMX_REPORT_CAMERA_INFO,
        (uint8_t)tmx_camera_state(),
        (uint8_t)(width >> 8), (uint8_t)(width & 0xff),
        (uint8_t)(height >> 8), (uint8_t)(height & 0xff),
        (uint8_t)s_quality, (uint8_t)s_size_index,
        (uint8_t)(CONFIG_TMX_CAMERA_XCLK_FREQ_HZ / 1000000),
    };
    s_send(packet, sizeof(packet));
}

/* ------------------------------------------------------------------ */
/* 拍照 / 发帧                                                         */
/* ------------------------------------------------------------------ */

esp_err_t tmx_camera_snapshot(int frames, uint32_t interval_ms)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "摄像头没就绪, 忽略拍照请求");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sensor_on) {
        /* 空闲时摄像头是断电的 (降温), 拍之前重新上电初始化 */
        esp_err_t err = tmx_camera_init();
        if (err != ESP_OK || !s_sensor_on) {
            ESP_LOGW(TAG, "拍照前给摄像头上电失败: %s", esp_err_to_name(err));
            return (err == ESP_OK) ? ESP_FAIL : err;
        }
    }
    if (interval_ms > 10000) {
        interval_ms = 10000;
    }

    drop_current_frame();
    s_frames_left = (frames > 0) ? frames : -1;
    s_interval_ms = interval_ms;
    s_last_frame_ms = 0;
    s_errors = 0;

    ESP_LOGI(TAG, "拍照: %s, 间隔 %ums",
             frames > 0 ? "指定帧数" : "连续", (unsigned)interval_ms);
    send_status(TMX_CAMERA_STATE_STREAM, frames > 0 ? frames : 0);
    return ESP_OK;
}

void tmx_camera_stop(void)
{
    bool was_active = (s_fb != NULL) || (s_frames_left != 0);

    drop_current_frame();
    s_frames_left = 0;

    if (was_active) {
        ESP_LOGI(TAG, "拍照停止 (累计 %u 帧)", (unsigned)s_frames_sent);
        send_status(TMX_CAMERA_STATE_IDLE, (int)(s_frames_sent & 0xff));
    }
}

static bool send_frame_info(const camera_fb_t *fb)
{
    /* 数据: 序号(1) 格式(1) 宽(2) 高(2) 长度(4, 小端) */
    uint8_t packet[12] = {
        11, TMX_REPORT_CAMERA_FRAME_INFO,
        s_frame_index,
        (uint8_t)fb->format,
        (uint8_t)(fb->width >> 8), (uint8_t)(fb->width & 0xff),
        (uint8_t)(fb->height >> 8), (uint8_t)(fb->height & 0xff),
        (uint8_t)(fb->len & 0xff),
        (uint8_t)((fb->len >> 8) & 0xff),
        (uint8_t)((fb->len >> 16) & 0xff),
        (uint8_t)((fb->len >> 24) & 0xff),
    };
    return s_send(packet, sizeof(packet));
}

/*
 * 帧对齐: 本板传感器吐出来的 JPEG 不是从第 0 字节开始的 (前面带了一截垃圾),
 * 所以驱动上游那种"只认开头 SOI"的做法会一直丢帧 (NO-SOI)。
 * 这里在整帧里找 FF D8 FF (SOI) 和最后一处 FF D9 (EOI), 只把中间那段干净的
 * JPEG 交给 PC; 找不到就丢掉这一帧。
 */
static bool jpeg_align_frame(camera_fb_t *fb)
{
    size_t soi = 0;
    size_t eoi = 0;
    bool have_soi = false;
    bool have_eoi = false;

    for (size_t i = 0; i + 3 <= fb->len; i++) {
        if (fb->buf[i] == 0xFF && fb->buf[i + 1] == 0xD8 && fb->buf[i + 2] == 0xFF) {
            soi = i;
            have_soi = true;
            break;
        }
    }
    if (!have_soi) {
        ESP_LOGW(TAG, "帧对齐: %u 字节里没有 FF D8 FF, 丢掉这一帧", (unsigned)fb->len);
        return false;
    }

    for (size_t i = fb->len; i >= 2; i--) {
        if (fb->buf[i - 2] == 0xFF && fb->buf[i - 1] == 0xD9) {
            eoi = i;
            have_eoi = true;
            break;
        }
    }

    size_t out_len = (have_eoi && eoi > soi) ? (eoi - soi) : (fb->len - soi);
    ESP_LOGW(TAG, "帧对齐: 收到 %u 字节, SOI@%u, EOI:%s -> 送 %u 字节",
             (unsigned)fb->len, (unsigned)soi,
             have_eoi ? "有" : "无", (unsigned)out_len);

    if (soi != 0) {
        memmove(fb->buf, fb->buf + soi, out_len);
    }
    fb->len = out_len;
    return true;
}

static bool start_frame(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        s_errors++;
        ESP_LOGW(TAG, "取帧失败 (第 %d 次, 传感器没有输出?)", s_errors);
        send_status(TMX_CAMERA_STATE_ERROR, s_errors);
        if (s_errors >= 3) {
            ESP_LOGW(TAG, "连续失败, 停止拍照");
            s_frames_left = 0;
        }
        return false;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        ESP_LOGW(TAG, "帧格式不支持 (format=%d, len=%u)", (int)fb->format,
                 (unsigned)fb->len);
        esp_camera_fb_return(fb);
        s_frames_left = 0;
        send_status(TMX_CAMERA_STATE_ERROR, 1);
        return false;
    }

    s_errors = 0;
    s_frame_index++;
    if (!jpeg_align_frame(fb)) {
        esp_camera_fb_return(fb);
        s_errors++;
        /* 单帧对不齐只是丢掉这一帧, 别立刻报错: PC 那边收到"出错"会直接放弃这次拍照。
         * 连续几帧都对不齐才算真出问题。 */
        if (s_errors >= 3) {
            ESP_LOGW(TAG, "连续 %d 帧都对不齐, 停止拍照", s_errors);
            s_frames_left = 0;
            send_status(TMX_CAMERA_STATE_ERROR, s_errors);
        }
        return false;
    }
    if (!send_frame_info(fb)) {
        /* 连接断了: 直接收工 */
        esp_camera_fb_return(fb);
        s_frames_left = 0;
        return false;
    }

    s_fb = fb;
    s_offset = 0;
    return true;
}

static void finish_frame(void)
{
    camera_fb_t *fb = s_fb;
    s_fb = NULL;
    s_offset = 0;
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }

    s_frames_sent++;
    s_last_frame_ms = now_ms();

    if (s_frames_left > 0) {
        s_frames_left--;
        if (s_frames_left == 0) {
            ESP_LOGI(TAG, "拍完 (累计 %u 帧)", (unsigned)s_frames_sent);
            send_status(TMX_CAMERA_STATE_IDLE, (int)(s_frames_sent & 0xff));
        }
    }
}

static void send_frame_chunks(void)
{
    uint8_t packet[6 + CAM_CHUNK_MAX];
    int chunks = 0;

    while (s_fb != NULL && s_offset < s_fb->len && chunks < CAM_CHUNKS_PER_POLL) {
        size_t remaining = s_fb->len - s_offset;
        size_t n = remaining > CAM_CHUNK_MAX ? CAM_CHUNK_MAX : remaining;

        /* 数据: 序号(1) 偏移(3, 大端) JPEG 数据(n)。
         * 偏移给 3 字节: XGA 以上的 JPEG 会超过 64KB, 2 字节会回绕。 */
        packet[0] = (uint8_t)(5 + n);
        packet[1] = TMX_REPORT_CAMERA_FRAME;
        packet[2] = s_frame_index;
        packet[3] = (uint8_t)((s_offset >> 16) & 0xff);
        packet[4] = (uint8_t)((s_offset >> 8) & 0xff);
        packet[5] = (uint8_t)(s_offset & 0xff);
        memcpy(&packet[6], s_fb->buf + s_offset, n);

        if (!s_send(packet, n + 6)) {
            /* PC 掉线了, 别把帧缓冲借走不还 */
            drop_current_frame();
            s_frames_left = 0;
            return;
        }

        s_offset += n;
        chunks++;
    }

    if (s_fb != NULL && s_offset >= s_fb->len) {
        finish_frame();
    }
}

void tmx_camera_poll(void)
{
    if (!s_ready || s_send == NULL) {
        return;
    }
    if (!s_sensor_on) {
        return;                 /* 空闲断电状态, 等拍照命令再上电 */
    }

    if (s_fb != NULL) {
        send_frame_chunks();
        return;
    }

    if (s_frames_left == 0) {
        /* 拍完/停止后把摄像头断电降温 (下次拍照前会重新初始化) */
        camera_sensor_off();
        return;
    }

    uint64_t now = now_ms();
    if (s_last_frame_ms != 0 && (now - s_last_frame_ms) < s_interval_ms) {
        return;
    }

    start_frame();
}

#else /* !CONFIG_TMX_CAMERA_ENABLE */

/* 摄像头关掉时保留同样的接口, 免得 tmx_core 里到处是 #if */

esp_err_t tmx_camera_init(void)
{
    ESP_LOGI(TAG, "摄像头未启用 (menuconfig -> 摄像头 (OV2640 DVP))");
    return ESP_ERR_NOT_SUPPORTED;
}

bool tmx_camera_is_ready(void) { return false; }
void tmx_camera_set_sender(tmx_camera_send_fn send) { (void)send; }
void tmx_camera_poll(void) {}
void tmx_camera_stop(void) {}
void tmx_camera_send_info(void) {}
void tmx_camera_probe_pins(void) {}

esp_err_t tmx_camera_snapshot(int frames, uint32_t interval_ms)
{
    (void)frames;
    (void)interval_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tmx_camera_set_format(int frame_size, int quality, int pixformat)
{
    (void)frame_size;
    (void)quality;
    (void)pixformat;
    return ESP_ERR_NOT_SUPPORTED;
}

int tmx_camera_state(void) { return TMX_CAMERA_STATE_IDLE; }
esp_err_t tmx_camera_tune(int field, int value)
{
    (void)field;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}
uint32_t tmx_camera_frames_sent(void) { return 0; }
int tmx_camera_quality(void) { return 0; }

#endif /* CONFIG_TMX_CAMERA_ENABLE */
