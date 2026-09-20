# 板载 ES8311 音频输入/输出

本板 (ESP32-S3 + ILI9342C 屏幕 + ES8311 codec) 的音频通路：

```
                    ┌──────────────┐
   I2S0 (48kHz)     │   ES8311     │
   16bit 立体声      │   codec      │
   ─── MCLK/BCLK/WS ─►             │
   ─── DOUT ─────────►  DAC ──► PA ──► 喇叭
   ◄── DIN ──────────  ADC ◄── 麦克风
                    └──────┬───────┘
                           │ I2C (地址 0x30)
                     ESP32-S3 配置寄存器
```

固件启动时按下面的接线初始化 I2S + codec + PA，之后 Scratch 就能：

* **输出**：播放指定频率/时长/音量的音调（正弦波，走 DAC → PA → 喇叭）
* **输入**：读麦克风响度 0~100（走 ADC，按变化上报给 Scratch）

## 接线

以下就是板级描述文件里的实际接线，固件默认值与之完全一致
（menuconfig → `音频 (ES8311 Codec)` 可逐项修改）：

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| I2S MCLK | 38 | codec 主时钟，采样率 × 256 = 12.288 MHz |
| I2S BCLK | 14 | 位时钟 |
| I2S WS / LRCK | 13 | 声道选择 |
| I2S DOUT | 45 | 板子 → codec（放音数据） |
| I2S DIN | 12 | codec → 板子（录音数据） |
| PA 使能 | 47 | 功放使能，高电平开（active_level=1），增益 6 dB |
| I2C SDA | 1 | codec 控制总线（与 Scratch 的 I2C 积木共用）|
| I2C SCL | 2 | 同上 |

对应的板级配置（`board_devices.yaml` / `board_peripherals.yaml`）：

```yaml
devices:
  - name: audio_dac
    chip: es8311
    type: audio_codec
    version: default
    config:
      dac_enabled: true
      dac_max_channel: 1
      dac_channel_mask: "1"      # 左声道
    peripherals:
      - name: gpio_pa_control    # -> PA 引脚 47, gain 6, active_level 1
        gain: 6
        active_level: 1
      - name: i2s_audio_out      # -> I2S0 主机, 48kHz/16bit/立体声/MCLK×256
      - name: i2c_master         # -> I2C0, 地址 0x30
        address: 0x30
        frequency: 400000

  - name: audio_adc
    chip: es8311
    type: audio_codec
    version: default
    config:
      adc_enabled: true
      adc_max_channel: 2
      adc_channel_mask: "11"
      adc_channel_labels: [RE, FC]   # 左=麦克风(FC), 右=播放回采(RE)
    peripherals:
      - name: i2s_audio_in
      - name: i2c_master
        address: 0x30
        frequency: 400000
```

固件里这套配置的落点：

| 板级配置 | 固件里的位置 |
| --- | --- |
| `dac_enabled` + `adc_enabled` | `codec_mode = BOTH`，DAC/ADC 同时打开 |
| `dac_channel_mask` / `adc_channel_mask` | I2S 立体声 + `channel_mask = 0x03`，两路都收 |
| `adc_channel_labels: [RE, FC]` | `TMX_AUDIO_ADC_DAC_REF`（REG44 = ADCL + DACR）|
| `adc_channel_labels` 里的 FC | `TMX_AUDIO_MIC_CHANNEL`（默认 0 = 左声道）|
| `gpio_pa_control` 的 gain / active_level | `TMX_AUDIO_PA_GAIN_DB` / `TMX_AUDIO_PA_ACTIVE_LEVEL` |
| `i2s_audio_out` / `i2s_audio_in` 的引脚与 48kHz/16bit/MCLK×256 | `TMX_AUDIO_I2S_*` 系列选项 |
| `i2c_master` 的 address 0x30 | `TMX_AUDIO_I2C_ADDR` |

> I2C 实际跑 100 kHz（ESP-IDF 新版 i2c_master 驱动的默认速率）。
> ES8311 在 100 kHz 下工作正常，400 kHz 只是它的能力上限。

## Scratch 积木

| 积木 | 板子上的动作 |
| --- | --- |
| 播放音调 频率 `440` Hz 时长 `500` 毫秒 音量 `60` % | 0x72 AUDIO_TONE，固件实时合成正弦波 |
| 停止播放音频 | 0x73 AUDIO_STOP，DAC 静音 |
| 麦克风响度 | 读最近一次 0x0D AUDIO_LEVEL 上报（0~100）|
| 麦克风检测 `开` / `关` | 0x74 AUDIO_MIC，打开后固件按变化上报响度 |

“麦克风响度” 需要先用「麦克风检测 开」打开采集，读数才会刷新；
关掉之后读数固定为 0（也不再占用 I2S 采集）。

## menuconfig 选项

菜单：`音频 (ES8311 Codec)`

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `TMX_AUDIO_ENABLE` | y | 总开关；关掉后音频模块完全不初始化，音频积木无效 |
| `TMX_AUDIO_I2C_ADDR` | 0x30 | codec 的 I2C 地址（8 位写法）|
| `TMX_AUDIO_I2S_MCLK_PIN` 等 5 个 | 38/14/13/45/12 | I2S 引脚 |
| `TMX_AUDIO_PA_PIN` | 47 | 功放使能脚 |
| `TMX_AUDIO_PA_ACTIVE_LEVEL` | 1 | 1 = 高电平开 |
| `TMX_AUDIO_PA_GAIN_DB` | 6 | 功放增益，用于音量换算 |
| `TMX_AUDIO_SAMPLE_RATE` | 48000 | 采样率 |
| `TMX_AUDIO_MCLK_MULTIPLE` | 256 | MCLK = 采样率 × N |
| `TMX_AUDIO_VOLUME` | 80 | 默认音量 % |
| `TMX_AUDIO_MIC_GAIN_DB` | 30 | 麦克风增益 |
| `TMX_AUDIO_ADC_DAC_REF` | y | 录音带一路 DAC 回采（ADCL + DACR）|
| `TMX_AUDIO_MIC_CHANNEL` | 0 | 哪一路是麦克风（0=左）|
| `TMX_AUDIO_MIC_REPORT_MS` | 100 | 响度最快多久上报一次 |

## 排障

| 现象 | 处理 |
| --- | --- |
| 串口报 `ES8311 没有应答 (I2C 地址 0x30)` | 检查 SDA=1 / SCL=2 接线与上拉；确认 `TMX_I2C_SDA_PIN`/`TMX_I2C_SCL_PIN` 没被改掉 |
| 板子日志里没有 `ES8311 ready` | 说明音频没初始化成功（`TMX_AUDIO_ENABLE` 关掉了，或者上面的初始化失败）|
| 放音没声音 | 先确认喇叭接在 PA 输出；再把音量给到 100；PA=47 在放音时应为高电平（万用表可量）|
| 音调频率不对/沙哑 | 确认采样率与 MCLK 倍数是 48000/256；杜邦线太长时音质会变差 |
| 麦克风响度一直是 0 | 打开「麦克风检测 开」；对着麦克风说话；必要时把 `TMX_AUDIO_MIC_GAIN_DB` 调大 |
| 响度跟着喇叭里的声音走，不跟环境声走 | 说明读到的是 DAC 回采那一路，把 `TMX_AUDIO_MIC_CHANNEL` 改成 1 |
| 只想录麦克风、不要回采 | 把 `TMX_AUDIO_ADC_DAC_REF` 关掉（REG44 只留麦克风）|

## 实测结论（本板，2026-09-18）

烧录后在串口抓到的两个声道 RMS 很能说明问题：

```
安静时:              ch0(麦克风)=90~170    ch1(DAC 回采)=0
放 1kHz/100% 音调时:  ch0(麦克风)=~29000   ch1(DAC 回采)=21212
```

`21212 = 30000 / √2`，正好是我们生成的 30000 幅度正弦波的 RMS，
说明 I2S TX → ES8311 DAC 的数字通路完全正确；同时麦克风那边从 ~100 涨到 ~29000，
说明功放和喇叭确实在出声。也就是说：

* 输出：Scratch/网关 → 0x72 → DAC → PA → 喇叭 ✅
* 输入：麦克风 → ADC → 响度 → 0x0D → 网关 → Banyan/Scratch ✅
* 声道：ch0 = 麦克风（FC，默认 `TMX_AUDIO_MIC_CHANNEL=0`），ch1 = DAC 回采（RE）✅

不用人耳判断的办法（跑之前先 `tools\stop_s3extend.ps1`）：

```powershell
python tools\pc_audio_loopback_check.py 192.168.0.103
```

它会先量环境本底、再放一段音调同时读麦克风：本底 ~20、放音时 ~97 就说明
输入输出两条路都通了（不依赖 Scratch，也不依赖耳朵）。

## 已知限制

* **不做 PCM 音频流**：Scratch ↔ 板子的通道是 Telemetrix 二进制协议，
  单包最长 255 字节，还要经过 Banyan/ZMQ/WebSocket 转发，
  实时传 PCM（48kHz 立体声 ≈ 192 KB/s）不现实。
  因此板子侧提供的是“音调输出 + 响度输入”，PC 侧想放整段音乐用 Scratch 自己的声音积木即可。
* **I2S 全双工是软同步**：ESP-IDF 的 I2S 全双工不保证收发严格同刻，
  本固件只用来放音调和测响度，不做回声消除。
* **I2C 与模拟引脚 32/33 冲突**：codec 用的 GPIO1/GPIO2 同时是 ADC1 的
  CH0/CH1（Scratch 的模拟引脚 32/33）。用了音频/I2C 之后不要再读这两个模拟引脚，
  需要模拟输入请用 34/35/36/39。
