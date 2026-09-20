/*
 * ILI9341 SPI 屏幕驱动实现 (见 display_ili9341.h)
 *
 * 几点硬件/驱动约定, 出问题时先看这里:
 *  1. 本板屏幕是 ILI9342C (不是 ILI9341)。两者的显存尺寸不同, 这决定了
 *     swap 要不要开, 搞反了就是错位的条纹:
 *       ILI9342C: 320(列) x 240(行)  -> 320x240 横屏用 MV=0, swap_xy(false)
 *       ILI9341 : 240(列) x 320(行)  -> 320x240 横屏必须 MV=1, swap_xy(true)
 *     ILI9342C 的寄存器跟 ILI9341 基本兼容, 所以直接用 esp_lcd 的 ILI9341
 *     驱动 + 默认初始化表即可, 只有方向 (MADCTL) 要按 9342C 来设。
 *  2. SPI 屏按大端收 16bit 像素, 而 ESP32 是小端, 所以写显存前要把
 *     RGB565 的高低字节交换, 否则红蓝/颜色整体会不对。
 *  3. 背光是 LEDC PWM。打开 TMX_LCD_BACKLIGHT_PWM 后本模块独占
 *     LEDC_TIMER_2 + 一个通道, tmx_io.c 会把它们从 PWM/舵机池里让出来。
 */

#include "display_ili9341.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "lcd";

#define LCD_HOST          SPI2_HOST
#define LCD_W             CONFIG_TMX_LCD_WIDTH
#define LCD_H             CONFIG_TMX_LCD_HEIGHT
#define LCD_CLK_HZ        CONFIG_TMX_LCD_SPI_CLOCK_HZ

/* 每次刷多少行: 16 行 = 320*16*2 = 10 KB, 放内部 DMA RAM */
#define LCD_CHUNK_LINES   16
#define LCD_CHUNK_BYTES   (LCD_W * LCD_CHUNK_LINES * (int)sizeof(uint16_t))

#define LCD_BL_FREQ_HZ    25000
#define LCD_BL_RES_BITS   LEDC_TIMER_10_BIT
#define LCD_BL_DUTY_MAX   ((1u << 10) - 1)

#if CONFIG_TMX_LCD_BGR
#define LCD_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_BGR
#else
#define LCD_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_RGB
#endif

/* 注意: Kconfig 里选 n 的 bool 在 C 里是"未定义", 不能直接当表达式用 */
#if CONFIG_TMX_LCD_SWAP_XY
#define LCD_SWAP_XY       true
#else
#define LCD_SWAP_XY       false
#endif

#if CONFIG_TMX_LCD_MIRROR_X
#define LCD_MIRROR_X      true
#else
#define LCD_MIRROR_X      false
#endif

#if CONFIG_TMX_LCD_MIRROR_Y
#define LCD_MIRROR_Y      true
#else
#define LCD_MIRROR_Y      false
#endif

#if CONFIG_TMX_LCD_BACKLIGHT_INVERT
#define LCD_BL_INVERT     true
#else
#define LCD_BL_INVERT     false
#endif

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static uint16_t                 *s_chunk;
static SemaphoreHandle_t         s_flush_done;
static bool                      s_backlight_ready;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

/* RGB565 大小端交换: 送屏的数据必须是 高字节在前 */
static inline uint16_t lcd_be16(uint16_t color)
{
    return (uint16_t)((color >> 8) | (color << 8));
}

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    BaseType_t higher_woken = pdFALSE;
    if (s_flush_done) {
        xSemaphoreGiveFromISR(s_flush_done, &higher_woken);
    }
    return higher_woken == pdTRUE;
}

/* 等 DMA 把这一块数据真正发完, 之后才能复用 s_chunk */
static esp_err_t lcd_draw_bitmap(int x0, int y0, int x1, int y1, const void *data)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, data);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "wait color transfer done timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 背光                                                                */
/* ------------------------------------------------------------------ */

static void backlight_init(void)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_TMX_LCD_BACKLIGHT_PIN;

#if CONFIG_TMX_LCD_BACKLIGHT_PWM
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LCD_BL_RES_BITS,
        .timer_num       = LEDC_TIMER_2,   /* 见文件头说明, 与 tmx_io 约定 */
        .freq_hz         = LCD_BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t chan_cfg = {
        .gpio_num   = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = (ledc_channel_t)CONFIG_TMX_LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_2,
        .duty       = 0,                   /* 先全灭, 面板初始化完再点亮 */
        .hpoint     = 0,
        .flags      = { .output_invert = LCD_BL_INVERT },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
#else
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(pin, LCD_BL_INVERT ? 0 : 1);
#endif

    s_backlight_ready = true;
}

void display_ili9341_set_backlight(uint8_t percent)
{
    if (!s_backlight_ready) {
        return;   /* 屏幕没初始化 (或没启用), 直接忽略 */
    }

    if (percent > 100) {
        percent = 100;
    }

#if CONFIG_TMX_LCD_BACKLIGHT_PWM
    uint32_t duty = (LCD_BL_DUTY_MAX * percent) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)CONFIG_TMX_LCD_BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)CONFIG_TMX_LCD_BACKLIGHT_LEDC_CHANNEL);
#else
    int level;
    if (percent == 0) {
        level = LCD_BL_INVERT ? 1 : 0;
    } else {
        level = LCD_BL_INVERT ? 0 : 1;
    }
    gpio_set_level((gpio_num_t)CONFIG_TMX_LCD_BACKLIGHT_PIN, level);
#endif
}

/* ------------------------------------------------------------------ */
/* 绘图                                                                */
/* ------------------------------------------------------------------ */

/* 填充矩形 (用同一个 DMA 缓冲按行分批刷) */
static esp_err_t lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565_color)
{
    if (s_panel == NULL || s_chunk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > LCD_W) { x1 = LCD_W; }
    if (y1 > LCD_H) { y1 = LCD_H; }
    if (x0 >= x1 || y0 >= y1) {
        return ESP_OK;
    }

    const int width = x1 - x0;
    const uint16_t be = lcd_be16(rgb565_color);
    for (int i = 0; i < width * LCD_CHUNK_LINES; i++) {
        s_chunk[i] = be;
    }

    for (int y = y0; y < y1; y += LCD_CHUNK_LINES) {
        int lines = (y1 - y) < LCD_CHUNK_LINES ? (y1 - y) : LCD_CHUNK_LINES;
        esp_err_t err = lcd_draw_bitmap(x0, y, x1, y + lines, s_chunk);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t display_ili9341_fill(uint16_t rgb565_color)
{
    return lcd_fill_rect(0, 0, LCD_W, LCD_H, rgb565_color);
}

esp_err_t display_ili9341_fill_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint16_t color = (uint16_t)(((uint16_t)(red & 0xF8) << 8) |
                                      ((uint16_t)(green & 0xFC) << 3) |
                                      ((uint16_t)blue >> 3));
    return display_ili9341_fill(color);
}

/*
 * 方向测试图: 4 个象限 + 左上角 marker_count 个白方块
 *   左上=红   右上=绿
 *   左下=蓝   右下=黄
 * 图是正的时候, 红块一定在左上角 (屏幕横过来看)。
 * 白方块数量 = 第几张候选配置, 方便"报数"而不是靠记时间。
 */
static esp_err_t lcd_draw_orient_pattern(int marker_count)
{
    if (s_panel == NULL || s_chunk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int hw = LCD_W / 2;
    const int hh = LCD_H / 2;
    esp_err_t err;

    err = lcd_fill_rect(0, 0, hw, hh, 0xF800);                                /* 左上 红 */
    if (err == ESP_OK) { err = lcd_fill_rect(hw, 0, LCD_W, hh, 0x07E0); }     /* 右上 绿 */
    if (err == ESP_OK) { err = lcd_fill_rect(0, hh, hw, LCD_H, 0x001F); }     /* 左下 蓝 */
    if (err == ESP_OK) { err = lcd_fill_rect(hw, hh, LCD_W, LCD_H, 0xFFE0); } /* 右下 黄 */

    /* 左上角的白方块: 个数 = 候选编号 */
    for (int i = 0; i < marker_count && err == ESP_OK; i++) {
        int x = 8 + i * 30;
        err = lcd_fill_rect(x, 8, x + 22, 30, 0xFFFF);
    }
    return err;
}

esp_err_t display_ili9341_show_test_pattern(void)
{
    return lcd_draw_orient_pattern(0);
}

/*
 * 启动时轮播几种方向组合, 每种停 2.5 秒, 左上角画 N 个白方块。
 * 只要看"哪一张方向正常 + 有几个白方块", 就能一次把方向定死。
 */
esp_err_t display_ili9341_orientation_test(void)
{
    static const struct {
        bool swap_xy;
        bool mirror_x;
        bool mirror_y;
        const char *name;
    } candidates[] = {
        {false, true,  true,  "board cfg: swap=0 mx=1 my=1"},
        {false, false, false, "swap=0 mx=0 my=0"},
        {false, true,  false, "swap=0 mx=1 my=0"},
        {false, false, true,  "swap=0 mx=0 my=1"},
        {true,  true,  true,  "swap=1 mx=1 my=1 (ILI9341 landscape)"},
    };
    const int count = sizeof(candidates) / sizeof(candidates[0]);

    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "orient test %d/%d: %s -> %d white blocks",
                 i + 1, count, candidates[i].name, i + 1);
        esp_lcd_panel_swap_xy(s_panel, candidates[i].swap_xy);
        esp_lcd_panel_mirror(s_panel, candidates[i].mirror_x, candidates[i].mirror_y);
        lcd_draw_orient_pattern(i + 1);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    /* 轮播完回到配置里的方向 */
    esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    ESP_LOGI(TAG, "orient test done, back to board cfg (swap=%d mx=%d my=%d)",
             LCD_SWAP_XY, LCD_MIRROR_X, LCD_MIRROR_Y);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

esp_lcd_panel_handle_t display_ili9341_panel(void)
{
    return s_panel;
}

esp_err_t display_ili9341_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "ILI9342C init: SCK=%d MOSI=%d DC=%d CS=%d BLK=%d, %dx%d swap=%d mx=%d my=%d",
             CONFIG_TMX_LCD_SPI_SCK_PIN, CONFIG_TMX_LCD_SPI_MOSI_PIN,
             CONFIG_TMX_LCD_DC_PIN, CONFIG_TMX_LCD_CS_PIN,
             CONFIG_TMX_LCD_BACKLIGHT_PIN, LCD_W, LCD_H,
             LCD_SWAP_XY, LCD_MIRROR_X, LCD_MIRROR_Y);

    s_flush_done = xSemaphoreCreateBinary();
    if (s_flush_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_chunk = heap_caps_malloc(LCD_CHUNK_BYTES, MALLOC_CAP_DMA);
    if (s_chunk == NULL) {
        ESP_LOGE(TAG, "no DMA memory for %d bytes", LCD_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    backlight_init();
    display_ili9341_set_backlight(0);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = CONFIG_TMX_LCD_SPI_MOSI_PIN,
        .miso_io_num     = -1,                 /* 只写屏, 不读 */
        .sclk_io_num     = CONFIG_TMX_LCD_SPI_SCK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num          = CONFIG_TMX_LCD_CS_PIN,
        .dc_gpio_num          = CONFIG_TMX_LCD_DC_PIN,
        .spi_mode             = 0,
        .pclk_hz              = LCD_CLK_HZ,
        .trans_queue_depth    = 4,
        .on_color_trans_done  = lcd_color_trans_done,
        .user_ctx             = NULL,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,                  /* 模块没接 RESET, 用软件复位 */
        .rgb_ele_order  = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
        .flags          = { .reset_active_high = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

#if CONFIG_TMX_LCD_INVERT_COLOR
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_ERROR_CHECK(display_ili9341_fill(0x0000));   /* 清成黑屏 */
    display_ili9341_set_backlight(CONFIG_TMX_LCD_BACKLIGHT_PERCENT);

    ESP_LOGI(TAG, "ILI9341 ready");
    return ESP_OK;
}
