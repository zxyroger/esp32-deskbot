# 第三方组件与协议 (Third-party notices)

本仓库整体按 **AGPL-3.0** 发布，完整条文见根目录 [LICENSE](../LICENSE)。
下面说明为什么选 AGPL、以及仓库里/构建中涉及的第三方部分。

> 一句话总结：上游 MrYsLab 那一套工具链（s3-extend / Telemetrix4Esp32 / telemetrix-esp32）
> 都是 AGPL-3.0，本工程又直接改用了其中的 Scratch 扩展、并会 import / 打补丁到这些包，
> 所以整个仓库跟着用 AGPL-3.0 最省事也最一致。
> 固件里的协议实现是照协议**独立重写**的 C 代码（协议本身不受版权保护）。
> AGPL 的第 13 条（网络条款）：如果把改过的这套东西作为网络服务提供给别人用，
> 需要向使用者提供对应源码；本仓库公开在 GitHub 上，这一条自然满足。

## 上游项目

| 项目 | 在本工程里的角色 | 协议 |
| --- | --- | --- |
| [MrYsLab/s3-extend](https://github.com/MrYsLab/s3-extend) | PC 端扩展服务器（backplane + wsgw + esp32gw），`tools/start_s3extend.ps1` 启动的就是它 | AGPL-3.0 |
| [MrYsLab/Telemetrix4Esp32](https://github.com/MrYsLab/Telemetrix4Esp32) | 固件所实现协议的参考实现（Arduino / WiFi 版） | AGPL-3.0 |
| [MrYsLab/telemetrix-esp32](https://github.com/MrYsLab/telemetrix-esp32) | Python 客户端 `telemetrix_aio_esp32`，`tools/pc_telemetrix_demo.py` 会 import 它 | AGPL-3.0 |
| [MrYsLab/s3onegpio](https://github.com/MrYsLab/s3onegpio) | `scratch/esp32s3.js` 的上游（本仓库那份是在它基础上改的） | **该仓库目前没有 LICENSE 文件**；同一作者的其它项目均为 AGPL-3.0 |
| [Scratch GUI / VM](https://github.com/scratchfoundation/scratch-gui) | 上游 s3onegpio 基于 Scratch 编辑器构建（间接） | BSD-3-Clause |
| [Banyan](https://github.com/MrYsLab/banyan)（随 s3-extend 一起装） | PC 端 backplane 总线 | MIT |

`scratch/esp32s3.js` 文件头已注明：改自 MrYsLab 的 OneGPIO 扩展，按 AGPL-3.0 发布。

## 打进本仓库的第三方文件

| 文件 | 来源 | 协议 | 说明 |
| --- | --- | --- | --- |
| `firmware/patches/cam_hal.c.local` | `espressif/esp32-camera` 2.x 的 `driver/cam_hal.c` | Apache-2.0 | **已修改**：文件头有 NOTICE 声明，正文里搜 `local debug patch` 能看到两处改动；`firmware/patches/README.md` 有说明。它只是"重新拉组件后照着改"的参照，不参与编译 |
| `tools/apply_local_patches.py` | 本工程自己写的补丁脚本，内部含少量上游代码片段用于文本定位/替换 | AGPL-3.0 | 只修改本地已安装的 AGPL 包 |

## 通过 ESP-IDF 组件管理器引用的组件（不在仓库里，构建时下载）

| 组件 | 协议 |
| --- | --- |
| `espressif/esp32-camera` | Apache-2.0 |
| `espressif/esp_codec_dev`（ES8311 驱动） | Apache-2.0 |
| `espressif/esp_lcd_ili9341` | Apache-2.0 |
| `espressif/esp_jpeg` | Apache-2.0 |
| `espressif/esp-dsp`、`espressif/cmake_utilities` | Apache-2.0 |
| `espressif/esp-sr`（含中文 TTS 音色数据 `esp_tts_voice_data_xiaole.dat`） | Espressif MIT |
| `espressif/cjson` | MIT |
| ESP-IDF v5.5.4 本身 | Apache-2.0 |

如果要发布二进制（固件 `.bin`、打包好的 exe），请把这些组件的 LICENSE 文本一并附上。

## 硬件资料

| 内容 | 来源 | 说明 |
| --- | --- | --- |
| OV2640 寄存器表 | `espressif/esp32-camera`（Apache-2.0）| `docs/camera-ov2640.md`、`tools/ov2640_regs.py` 里的寄存器说明来自该组件与 OV2640 数据手册 |
| AXP2101 寄存器说明 | [XPowersLib](https://github.com/lewisxhe/XPowersLib)（MIT）+ AXP2101 数据手册 | `tools/axp2101_power.py` |
