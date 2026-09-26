/*
 * AXP2101 电池电压 / 电量 / 充电状态
 *
 * 板子上的 AXP2101 除了给摄像头供两路 LDO, 还管着电池和充电, 内部自带电量计。
 * 寄存器表 (AXP2101 数据手册 / XPowersLib, 与 Waveshare 的 AXP2101 驱动一致):
 *
 *   0x00 STATUS1: bit3 = 电池在位; bit5 = 1 表示现在由外部(USB/VBUS)供电
 *   0x01 STATUS2: 低 3 位 = 充电状态
 *                 0 涓流 1 预充 2 恒流 3 恒压 4 充满 5 未充电
 *   0x30 ADC 通道使能: bit0/bit2/bit3 打开 VBAT / VBUS / VSYS 的 ADC
 *   0x34 VBAT / 0x38 VBUS / 0x3A VSYS: 14 位, 高字节低 6 位有效, 单位 mV
 *   0xA4 电量百分比 (0~100, 超过 100 视为无效)
 *
 * 摄像头那边 (tmx_camera.c) 用的是同一颗 PMIC 的另外几个寄存器 (0x90/0x93/0x96/0x97),
 * 互不影响; 两边都挂在同一条 I2C 总线上。
 */

#include "tmx_power.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "tmx_i2c.h"

static const char *TAG = "tmx_power";

#define AXP_ADDR          0x34
#define REG_STATUS1       0x00
#define REG_STATUS2       0x01
#define REG_ADC_ENABLE    0x30
#define REG_VBAT_H        0x34
#define REG_VBUS_H        0x38
#define REG_VSYS_H        0x3A
#define REG_PERCENT       0xA4

#define ADC_EN_MASK       ((1u << 0) | (1u << 2) | (1u << 3))

#define POLL_INTERVAL_MS  2000
#define HEARTBEAT_MS      30000
#define MV_DELTA_REPORT   20      /* 电压变化超过这么多 mV 就补一条 */

static bool     s_ready;
static uint64_t s_last_poll_ms;
static uint64_t s_last_report_ms;
static int      s_last_mv = -1;
static int      s_last_percent = -2;
static bool     s_last_external;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool read_reg(uint8_t reg, uint8_t *out)
{
    size_t len = 0;
    return tmx_i2c_read(AXP_ADDR, reg, 1, false, out, 1, &len) == ESP_OK && len == 1;
}

/* 14 位 ADC: 高字节低 6 位 + 低字节, 单位 mV */
static bool read_u14(uint8_t high_reg, int *millivolt)
{
    uint8_t data[2] = { 0, 0 };
    size_t len = 0;
    if (tmx_i2c_read(AXP_ADDR, high_reg, 2, false, data, sizeof(data), &len) != ESP_OK ||
            len != 2) {
        return false;
    }
    *millivolt = (int)(((uint16_t)(data[0] & 0x3F) << 8) | data[1]);
    return true;
}

static bool write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return tmx_i2c_write(AXP_ADDR, buf, sizeof(buf)) == ESP_OK;
}

esp_err_t tmx_power_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!tmx_i2c_is_ready()) {
        return ESP_ERR_INVALID_STATE;      /* I2C 还没建起来, 下次轮询再试 */
    }

    /* 打开 VBAT / VBUS / VSYS 三个 ADC 通道 (保留其它位的原值) */
    uint8_t cur = 0;
    if (!read_reg(REG_ADC_ENABLE, &cur)) {
        ESP_LOGW(TAG, "读 ADC 使能寄存器 (0x30) 失败");
        return ESP_FAIL;
    }
    uint8_t want = (uint8_t)(cur | ADC_EN_MASK);
    if (want != cur && !write_reg(REG_ADC_ENABLE, want)) {
        ESP_LOGW(TAG, "写 ADC 使能寄存器 (0x30) 失败");
        return ESP_FAIL;
    }

    s_ready = true;

    tmx_power_info_t info;
    if (tmx_power_read(&info) == ESP_OK) {
        ESP_LOGI(TAG, "电池: %d mV, 电量 %d%%, 电池%s, %s, VBUS %d mV",
                 info.battery_mv, info.percent,
                 info.battery_present ? "在位" : "未接",
                 info.charging ? "充电中"
                               : (info.external_power ? "外部供电" : "电池供电"),
                 info.vbus_mv);
    }
    return ESP_OK;
}

esp_err_t tmx_power_read(tmx_power_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    if (!read_reg(REG_STATUS1, &status1) || !read_reg(REG_STATUS2, &status2)) {
        return ESP_FAIL;
    }

    out->battery_present = (status1 & (1u << 3)) != 0;
    out->external_power  = (status1 & (1u << 5)) != 0;
    out->charge_state    = (uint8_t)(status2 & 0x07);
    out->charging        = out->charge_state <= 3;      /* 涓流/预充/恒流/恒压 */

    int mv = 0;
    if (out->battery_present && read_u14(REG_VBAT_H, &mv)) {
        out->battery_mv = mv;
    } else {
        out->battery_mv = 0;
    }
    if (read_u14(REG_VBUS_H, &mv)) {
        out->vbus_mv = mv;
    } else {
        out->vbus_mv = 0;
    }
    (void)REG_VSYS_H;   /* VSYS 暂时不用, 留着以后要显示系统电压时读 */

    uint8_t percent = 0;
    if (out->battery_present && read_reg(REG_PERCENT, &percent) && percent <= 100) {
        out->percent = (int)percent;
    } else {
        out->percent = -1;
    }
    return ESP_OK;
}

bool tmx_power_poll(tmx_power_info_t *out)
{
    uint64_t now = now_ms();
    if (s_last_poll_ms != 0 && (now - s_last_poll_ms) < POLL_INTERVAL_MS) {
        return false;
    }
    s_last_poll_ms = now;

    if (!s_ready && tmx_power_init() != ESP_OK) {
        return false;
    }

    tmx_power_info_t info;
    if (tmx_power_read(&info) != ESP_OK) {
        return false;
    }

    bool changed = (info.percent != s_last_percent) ||
                   (info.external_power != s_last_external) ||
                   (s_last_mv < 0) ||
                   (info.battery_mv > 0 && (info.battery_mv - s_last_mv > MV_DELTA_REPORT ||
                                            s_last_mv - info.battery_mv > MV_DELTA_REPORT));
    bool heartbeat = (s_last_report_ms == 0) ||
                     ((now - s_last_report_ms) >= HEARTBEAT_MS);

    s_last_mv = info.battery_mv;
    s_last_percent = info.percent;
    s_last_external = info.external_power;

    if (!changed && !heartbeat) {
        return false;
    }
    s_last_report_ms = now;
    if (out != NULL) {
        *out = info;
    }
    return true;
}
