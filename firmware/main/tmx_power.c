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
 *   0x34 VBAT: 13 位 (高字节低 5 位) / 0x38 VBUS: 14 位 (高字节低 6 位), 单位 mV
 *   0x36 VTST: 电池 NTC 温度通道 —— 注意, 这**不是**电池电流 (见下)
 *   0xA4 电量百分比 (0~100, 超过 100 视为无效)
 *
 * 为什么"电池电流"是算出来的:
 *   AXP2101 的 ADC 只有 VBAT / TS / VBUS / VSYS / 芯片温度五路, **没有电池电流
 *   寄存器** (XPowersLib 里 0x36/0x37 是 TS 引脚, 量的是板上那颗 10kΩ NTC)。
 *   早期版本把 0x36/0x37 当 IBAT 读, 于是"电流"永远在 500mA 上下, 插不插 USB、
 *   开不开摄像头都一样 —— 那是假数据 (2026-09-27 定位并删除)。
 *   现在改成用内置电量计的百分比变化率 × 电池容量换算平均放电电流, 它是**一段
 *   时间的平均值**; 电量计只有 1% 分辨率, 所以要攒够几分钟才有意义。
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
#define REG_VTST_H        0x36        /* TS 引脚 (电池 NTC), 不是电流 */
#define REG_VBUS_H        0x38
#define REG_VSYS_H        0x3A
#define REG_PERCENT       0xA4

/* VBAT 是 13 位, VBUS/VSYS 是 14 位 */
#define ADC_MASK_VBAT     0x1F
#define ADC_MASK_V14      0x3F

#define ADC_EN_MASK       ((1u << 0) | (1u << 2) | (1u << 3))

#define POLL_INTERVAL_MS  2000
#define HEARTBEAT_MS      30000
#define MV_DELTA_REPORT   20      /* 电压变化超过这么多 mV 就补一条 */
#define MA_DELTA_REPORT   50      /* (估算)电流变化超过这么多 mA 就补一条 */

/* 放电速率: 用内置电量计的百分比推算, 单位 0.1%/h (负 = 放电) */
#define RATE_SAMPLE_MS    30000ULL        /* 每 30 秒记一个电量点 */
#define RATE_MIN_SPAN_MS  60000ULL        /* 只要跨过 1 分钟, 而且电量掉过 1% 就出数 */
#define RATE_ZERO_SPAN_MS 600000ULL       /* 跨过 10 分钟还一点没掉, 才敢说 "基本没在掉" */
#define RATE_MAX_SPAN_MS  3600000ULL      /* 最多回看 1 小时 */
#define RATE_HISTORY      128
#define RATE_STEP_X10     36000000LL      /* Δ% × 3.6e7 ÷ 窗口ms = 速率×10 */

/*
 * 还测不出来时的哨兵值 (见 tmx_core.c 的包格式说明):
 * 电量计只有 1% 分辨率, 板子刚上电 / 刚拔 USB 时还没有任何变化可看, 这时候
 * 报 0 是假数据 (板子明明在耗电)。所以用哨兵告诉扩展"还不知道", 让积木显示
 * "测量中", 而不是骗人的 0。
 */
#define RATE_UNKNOWN      ((int)0x7FFF)
#define MA_UNKNOWN        ((int)(-0x8000))

static bool     s_ready;
static uint64_t s_last_poll_ms;
static uint64_t s_last_report_ms;
static int      s_last_mv = -1;
static int      s_last_percent = -2;
static bool     s_last_external;
static int      s_last_ma = 0;
static int      s_last_rate = 0;
static bool     s_external_seen = true;     /* 还没读到过时按"外部供电"算 */

/* 电量计百分比的滑动历史 (算平均放电速率用) */
static struct {
    uint64_t ms;
    int      percent;
} s_hist[RATE_HISTORY];
static int      s_hist_count;
static int      s_hist_head;
static int      s_rate_x10;                 /* 平均放电速率 0.1%/h, 负 = 放电 */
static bool     s_rate_valid;               /* false = 还没测出来 (别报 0) */
static int      s_fail_streak;              /* 连续读失败次数 (用来发现 I2C 掉线) */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool read_reg(uint8_t reg, uint8_t *out)
{
    size_t len = 0;
    return tmx_i2c_read(AXP_ADDR, reg, 1, false, out, 1, &len) == ESP_OK && len == 1;
}

/* 电压 ADC: 高字节低位 + 低字节, 单位 mV (mask 见 ADC_MASK_xxx) */
static bool read_adc(uint8_t high_reg, uint8_t mask, int *millivolt)
{
    uint8_t data[2] = { 0, 0 };
    size_t len = 0;
    if (tmx_i2c_read(AXP_ADDR, high_reg, 2, false, data, sizeof(data), &len) != ESP_OK ||
            len != 2) {
        return false;
    }
    *millivolt = (int)(((uint16_t)(data[0] & mask) << 8) | data[1]);
    return true;
}

static bool write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return tmx_i2c_write(AXP_ADDR, buf, sizeof(buf)) == ESP_OK;
}

/* 长时间不用电时把"电量计历史"清掉, 免得拿充电时的点去算放电速率 */
static void rate_history_reset(void)
{
    s_hist_count = 0;
    s_hist_head = 0;
    s_rate_x10 = 0;
    s_rate_valid = false;
}

static void rate_history_push(uint64_t now, int percent)
{
    if (percent < 0) {
        return;
    }
    if (s_hist_count > 0) {
        int last = (s_hist_head + RATE_HISTORY - 1) % RATE_HISTORY;
        if (now - s_hist[last].ms < RATE_SAMPLE_MS) {
            return;                     /* 还没到下一个采样点 */
        }
    }
    s_hist[s_hist_head].ms = now;
    s_hist[s_hist_head].percent = percent;
    s_hist_head = (s_hist_head + 1) % RATE_HISTORY;
    if (s_hist_count < RATE_HISTORY) {
        s_hist_count++;
    }
}

/*
 * 用"最老的那个还在窗口内的点"到"最新点"的差值算平均速率。
 *
 * 电量计 1% 一跳, 所以:
 *   - 窗口里有变化 (Δ≠0) 且跨过 RATE_MIN_SPAN_MS  -> 直接算 (通常第一次掉 1% 就出数);
 *   - 窗口里一点没变, 但已经跨过 RATE_ZERO_SPAN_MS -> 说明真的掉得极慢, 报 0;
 *   - 其他情况 -> 还不知道 (valid = false), 让积木显示"测量中"。
 */
static int rate_estimate_x10(uint64_t now, bool *valid)
{
    *valid = false;
    if (s_hist_count < 2) {
        return 0;
    }

    int newest = (s_hist_head + RATE_HISTORY - 1) % RATE_HISTORY;
    int oldest = newest;
    for (int i = 1; i < s_hist_count; i++) {
        int idx = (s_hist_head + RATE_HISTORY - 1 - i) % RATE_HISTORY;
        if (now - s_hist[idx].ms > RATE_MAX_SPAN_MS) {
            break;
        }
        oldest = idx;
    }

    uint64_t span = now - s_hist[oldest].ms;
    int delta = s_hist[newest].percent - s_hist[oldest].percent;
    if (delta == 0) {
        if (span >= RATE_ZERO_SPAN_MS) {
            *valid = true;
            return 0;
        }
        return 0;
    }
    if (span < RATE_MIN_SPAN_MS) {
        return 0;
    }
    *valid = true;
    return (int)((int64_t)delta * RATE_STEP_X10 / (int64_t)span);
}

/*
 * 平均放电电流 (mA): 速率(%)/h ÷ 100 × 容量(mAh)。
 * 符号跟原来一致: 正 = 充电 (电量在涨), 负 = 放电。
 */
static int rate_to_ma(int rate_x10)
{
    int capacity = CONFIG_TMX_BATTERY_CAPACITY_MAH;
    if (capacity <= 0) {
        return 0;
    }
    return (int)((int64_t)rate_x10 * (int64_t)capacity / 1000);
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
        ESP_LOGI(TAG, "电池: %d mV, 电量 %d%%, 电池%s, %s, VBUS %d mV "
                      "(电流是电量计换算的, 要先跑几分钟)",
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
    if (out->battery_present && read_adc(REG_VBAT_H, ADC_MASK_VBAT, &mv)) {
        out->battery_mv = mv;
    } else {
        out->battery_mv = 0;
    }
    /*
     * VBUS: 没插 USB 的时候这一路的 ADC 是个悬空值 (实测 16.3V), 直接当 0 处理,
     * 免得积木上显示一个"USB 16V"的假电压。
     */
    if (!out->external_power) {
        out->vbus_mv = 0;
    } else if (read_adc(REG_VBUS_H, ADC_MASK_V14, &mv)) {
        out->vbus_mv = mv;
    } else {
        out->vbus_mv = 0;
    }
    (void)REG_VSYS_H;   /* VSYS 暂时不用, 留着以后要显示系统电压时读 */
    (void)REG_VTST_H;   /* TS 是电池 NTC 温度, 不是电流 */

    uint8_t percent = 0;
    if (out->battery_present && read_reg(REG_PERCENT, &percent) && percent <= 100) {
        out->percent = (int)percent;
    } else {
        out->percent = -1;
    }

    /* 电流是算出来的 (见文件头), 这里只填最近一次算出的平均值 */
    out->rate_valid = s_rate_valid;
    out->rate_pph_x10 = s_rate_valid ? s_rate_x10 : RATE_UNKNOWN;
    out->current_ma = s_rate_valid ? rate_to_ma(s_rate_x10) : MA_UNKNOWN;
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
        /*
         * 读不到时**不能一声不吭**: 以前这里直接 return, 于是"电池积木再也没数据"
         * 这种故障在串口上一点痕迹都没有, 只能靠猜。现在连续 5 次 (~10 秒) 读不到
         * 就报一条警告, 并且把模块退回未初始化 —— 下一轮会重新初始化 PMIC
         * (重新打开 VBAT/VBUS/VSYS 的 ADC 通道), 能自愈的就自愈。
         */
        if (++s_fail_streak == 5) {
            ESP_LOGW(TAG, "电池读数连续 %d 次失败 (I2C 没应答 / PMIC 状态异常?), "
                          "重新初始化 PMIC", s_fail_streak);
            rate_history_reset();
            s_ready = false;
        }
        return false;
    }
    if (s_fail_streak > 0) {
        ESP_LOGI(TAG, "电池读数恢复 (之前连续失败 %d 次)", s_fail_streak);
        s_fail_streak = 0;
    }
    s_external_seen = info.external_power;

    /*
     * 电量计的滑动历史: 插着 USB / 正在充电时清空 (那时候电量在涨, 不能拿去
     * 算放电速率); 电池供电时每分钟记一个点。
     */
    if (info.external_power || info.charging) {
        rate_history_reset();
    } else {
        rate_history_push(now, info.percent);
        bool valid = false;
        int rate = rate_estimate_x10(now, &valid);
        if (valid) {
            s_rate_x10 = rate;          /* 记下最近一次真正测出来的值 */
        }
        s_rate_valid = valid;
        info.rate_valid = valid;
        info.rate_pph_x10 = valid ? s_rate_x10 : RATE_UNKNOWN;
        info.current_ma = valid ? rate_to_ma(s_rate_x10) : MA_UNKNOWN;
    }

    bool changed = (info.percent != s_last_percent) ||
                   (info.external_power != s_last_external) ||
                   (s_last_mv < 0) ||
                   (info.rate_pph_x10 - s_last_rate > 10 ||
                    s_last_rate - info.rate_pph_x10 > 10) ||
                   (info.battery_mv > 0 && (info.battery_mv - s_last_mv > MV_DELTA_REPORT ||
                                            s_last_mv - info.battery_mv > MV_DELTA_REPORT)) ||
                   (info.current_ma - s_last_ma > MA_DELTA_REPORT ||
                    s_last_ma - info.current_ma > MA_DELTA_REPORT);
    bool heartbeat = (s_last_report_ms == 0) ||
                     ((now - s_last_report_ms) >= HEARTBEAT_MS);

    s_last_mv = info.battery_mv;
    s_last_percent = info.percent;
    s_last_external = info.external_power;
    s_last_rate = info.rate_pph_x10;
    s_last_ma = info.current_ma;

    if (!changed && !heartbeat) {
        return false;
    }
    s_last_report_ms = now;
    if (out != NULL) {
        *out = info;
    }
    return true;
}

bool tmx_power_external_power(void)
{
    return s_external_seen;
}
