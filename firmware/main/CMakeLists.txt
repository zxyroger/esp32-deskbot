idf_component_register(
    SRCS
        "app_main.c"
        "wifi_link.c"
        "tmx_core.c"
        "tmx_io.c"
        "tmx_i2c.c"
        "tmx_server.c"
        "display_ili9341.c"
        "tmx_audio.c"
        "tmx_tts.c"
        "tmx_camera.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        nvs_flash
        esp_adc
        esp_timer
        esp_lcd
        esp_driver_i2s
        esp_codec_dev
        espressif__esp32-camera
        espressif__esp-sr
        driver
)

# 把 TTS 音色数据 (esp-sr 的中文小乐音色, 2.9MB) 烧进 voice_data 分区:
# 这样 `idf.py flash` 会连它一起写进去, 不用手工 esptool write_flash。
idf_component_get_property(espsr_dir espressif__esp-sr COMPONENT_DIR)
set(TTS_VOICE_DATA "${espsr_dir}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat")
if(EXISTS "${TTS_VOICE_DATA}")
    esptool_py_flash_to_partition(flash "voice_data" "${TTS_VOICE_DATA}")
else()
    message(WARNING "找不到 TTS 音色数据: ${TTS_VOICE_DATA} (voice_data 分区不会被烧写)")
endif()
