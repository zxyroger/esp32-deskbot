/*
 * I2C 主站封装 (ESP-IDF v5.x 新版 i2c_master 驱动)
 *
 * 与 Arduino Wire 行为对应关系:
 *   i2c_read : beginTransmission(addr) -> write(reg) -> endTransmission(stop)
 *              -> requestFrom(addr, count)
 *   i2c_write: beginTransmission(addr) -> write(data...) -> endTransmission()
 */

#include "tmx_i2c.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "tmx_i2c";

#define TMX_I2C_PORT        I2C_NUM_0
#define TMX_I2C_SPEED_HZ    100000
#define TMX_I2C_TIMEOUT_MS  100
#define TMX_I2C_MAX_DEVICES 8

typedef struct {
    uint8_t                 addr;
    i2c_master_dev_handle_t handle;
} i2c_dev_entry_t;

static i2c_master_bus_handle_t s_bus;
static i2c_dev_entry_t         s_devices[TMX_I2C_MAX_DEVICES];
static int                     s_device_count;

bool tmx_i2c_is_ready(void)
{
    return s_bus != NULL;
}

void *tmx_i2c_get_bus(void)
{
    return (void *)s_bus;
}

int tmx_i2c_port_num(void)
{
    return s_bus ? (int)TMX_I2C_PORT : -1;
}

esp_err_t tmx_i2c_begin(int sda_pin, int scl_pin)
{
    if (s_bus) {
        /* 与 Arduino Wire.begin() 一样, 重复调用不再重新初始化 */
        return ESP_OK;
    }

    if (sda_pin == 0 && scl_pin == 0) {
        sda_pin = CONFIG_TMX_I2C_SDA_PIN;
        scl_pin = CONFIG_TMX_I2C_SCL_PIN;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TMX_I2C_PORT,
        .sda_io_num = (gpio_num_t)sda_pin,
        .scl_io_num = (gpio_num_t)scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed (SDA=%d SCL=%d): %s",
                 sda_pin, scl_pin, esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }

    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    ESP_LOGI(TAG, "I2C ready: SDA=%d SCL=%d @ %dHz", sda_pin, scl_pin, TMX_I2C_SPEED_HZ);
    return ESP_OK;
}

static esp_err_t get_device(uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (!s_bus) {
        ESP_LOGW(TAG, "I2C not initialized yet (call set_pin_mode_i2c first)");
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].addr == addr) {
            *out = s_devices[i].handle;
            return ESP_OK;
        }
    }

    if (s_device_count >= TMX_I2C_MAX_DEVICES) {
        ESP_LOGW(TAG, "too many I2C devices (max %d)", TMX_I2C_MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = TMX_I2C_SPEED_HZ,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add I2C device 0x%02X failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    s_devices[s_device_count].addr = addr;
    s_devices[s_device_count].handle = handle;
    s_device_count++;
    *out = handle;
    return ESP_OK;
}

esp_err_t tmx_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    if (len == 0) {
        return ESP_OK;
    }
    return i2c_master_transmit(dev, data, len, TMX_I2C_TIMEOUT_MS);
}

esp_err_t tmx_i2c_read(uint8_t addr, uint8_t reg, uint8_t count,
                       bool stop_between, uint8_t *out, size_t out_size,
                       size_t *out_len)
{
    *out_len = 0;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = get_device(addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    if (count == 0) {
        return ESP_OK;
    }
    if (count > out_size) {
        count = (uint8_t)out_size;
    }

    /* 写寄存器地址 + 读数据。
     * stop_between == false: 一次"重复起始位"事务 (START, 地址W, reg,
     *                        START, 地址R, 数据...), 与 Arduino 的
     *                        endTransmission(false) + requestFrom 一致。
     *                        OV2640 这类 SCCB 器件必须用这种读法, 否则会
     *                        ACK 但数据全是 0x00。
     * stop_between == true : 先写 (带 STOP), 再单独发起一次读。 */
    if (!stop_between) {
        err = i2c_master_transmit_receive(dev, &reg, 1, out, count,
                                          TMX_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C repeated-start read 0x%02X (device 0x%02X) failed: %s",
                     reg, addr, esp_err_to_name(err));
            return err;
        }
        *out_len = count;
        return ESP_OK;
    }

    err = i2c_master_transmit(dev, &reg, 1, TMX_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C write register 0x%02X (device 0x%02X) failed: %s",
                 reg, addr, esp_err_to_name(err));
        return err;
    }

    err = i2c_master_receive(dev, out, count, TMX_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C read device 0x%02X failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    *out_len = count;
    return ESP_OK;
}
