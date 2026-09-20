# 板载中文语音合成 (TTS)

Scratch 里「朗读文字 [ ]」积木把文字发到板子, 板子**本地**合成语音, 从 ES8311 喇叭念出来。
用的是 Espressif 的 [esp-sr](https://components.espressif.com/components/espressif/esp-sr) 里的
`esp-tts` (中文小乐音色), 不需要联网、不经过云端。

```
Scratch「朗读文字」 ──► Banyan ──► esp32gw ──► 0x75 (UTF-8 文字, 拆包) ──► 板子
                                                                          │
                          esp-tts (16kHz/16bit 单声道) ──► 3 倍升采样 ──► I2S ──► ES8311 ──► 喇叭
```

## 为什么改了 flash 分区

esp-tts 需要一份 2.9MB 的中文音色数据 (`esp_tts_voice_data_xiaole.dat`)。
本工程把它放在 flash 的 **voice_data 分区**, 运行时 mmap 出来给 TTS 用:

* 不占 RAM (只映射地址, 按需读 flash)
* 不进 app 镜像 (app 还是 ~1MB)

所以 flash 从 2MB 换成了 **16MB**, 用自定义分区表 `firmware/partitions_16m.csv`:

| 分区 | 类型 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24K | WiFi 配置 |
| phy_init | data/phy | 0xf000 | 4K | 射频校准 |
| factory | app | 0x10000 | 4M | 固件 (现在 ~978KB, 余量很大) |
| voice_data | data/fat | 0x410000 | 3M | TTS 音色数据 |

烧录**不用手工**烧音色数据: `firmware/main/CMakeLists.txt` 里用
`esptool_py_flash_to_partition(flash "voice_data" ...)` 把它挂到了 `idf.py flash`,
所以:

```powershell
D:\esp\onegpio\tools\idf.ps1 -Port COM15 flash monitor
```

一次就把 bootloader / 分区表 / 固件 / 音色数据全写进去。手工用 esptool 时要自己加:

```
... 0x410000 managed_components\espressif__esp-sr\esp-tts\esp_tts_chinese\esp_tts_voice_data_xiaole.dat
```

开机日志里能看到:

```
init voice set:template
ESP Chinese TTS v1.7 (Sep 22 2022 14:35:13, 1)
I (548) tmx_tts: esp-tts ready: 音色数据 3145728 字节 (分区 voice_data), 输出 16kHz/16bit 单声道
```

## Scratch 积木

| 积木 | 板子上的动作 |
| --- | --- |
| 朗读文字 [文本] | 0x75: 文字按 UTF-8 拆包发到板子, 板子拼好后开始合成并播放 |
| 停止朗读 | 0x76: 立刻停 (同时停掉正在播的音调) |

* 文字上限 512 字节 (约 170 个汉字, `TMX_TTS_TEXT_MAX`); 超了会被截断并在串口告警。
* 语速固定用 `TMX_TTS_SPEED` (默认 4, 0 最慢 / 5 最快);
  音量用 `TMX_TTS_VOLUME` (默认 90%)。
* 「朗读文字」会打断正在播放的「播放音调」; 反过来放音调也会停掉朗读。
* 生僻字、纯英文/数字会解析不出来, 串口会打 `解析失败, 这段文字念不出来`, 属于正常现象
  (esp-tts 是中文音色, 只认常用汉字/数字/符号)。

## PC 侧自检 (不经过 Scratch)

`tools/pc_tts_check.py` 可以让板子念一段文字, 并把板子合成的 PCM **回传**回来存成 wav,
方便确认"念的到底是不是这几个字"(不依赖耳朵, 也不用录屏):

```powershell
# 先让出板子 (板子同时只服务一个客户端)
D:\esp\onegpio\tools\stop_s3extend.ps1

# 发文字, 存 wav; --play 会用 PC 喇叭再放一遍
python D:\esp\onegpio\tools\pc_tts_check.py 192.168.0.103 "你好，我是小乐" --play

# 命令行编码不稳时用 UTF-8 十六进制
python D:\esp\onegpio\tools\pc_tts_check.py 192.168.0.103 --hex-utf8 e4bda0e5a5bd
```

实测 (2026-09-18, 本板):

```
朗读文字: 你好，我是小乐，很高兴认识你
已发出 1 个文字包 (每个 ≤250 字节 UTF-8)
收到合成 PCM: 113184 字节 = 3.54 秒 (丢包 0), 已存 tts_out.wav
```

## 排障

| 现象 | 处理 |
| --- | --- |
| 串口 `找不到音色数据分区 "voice_data"` | 烧录时没写 voice_data 分区: 用 `idf.py flash` (会自动带上), 不要只烧 app |
| 串口 `音色数据不合法 (voice_set_init 失败)` | 分区里是空数据或长度不对: 重新 `idf.py flash` |
| 串口 `TTS 不可用: 朗读积木会被忽略` | `TMX_TTS_ENABLE` 关了, 或者上面两种初始化失败 |
| 积木点了没反应 | 网关没打**补丁 8**: `python tools\apply_local_patches.py`, 然后重启网关 |
| 念出来是"数字一个个念"或跳过 | 文字里有 esp-tts 不认的字符 (英文单词/生僻字), 换常用中文说法 |
| 声音太小 | `TMX_TTS_VOLUME` 调大 (默认 90%), 或查 [audio-es8311.md](audio-es8311.md) |
| 想换音色 | esp-sr 还带 `xiaoxin` 音色: 把 `idf_component.yml` 对应的 `.dat` 换成 `esp_tts_voice_data_xiaoxin.dat` 并改分区烧录内容 |

## 已知限制

* 只支持中文 (esp-tts 不带英文音素); 数字会按中文念 (如 `123` → 一百二十三)。
* 合成是**离线**的, 语调是拼接式音色, 比不上云端 TTS。
* 朗读和音调共用一路 I2S/喇叭, 同一时刻只能有一个在响。
