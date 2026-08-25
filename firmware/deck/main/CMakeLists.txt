idf_component_register(
    SRCS
        "app_main.c"
        "audio_task.c"
        "wav_reader.c"
        "track_meta.c"
        "sd_card.c"
        "bus_link.c"
        "espnow_link.c"
        "net_task.c"
        "led_ring.c"
        "haptics.c"
        "ui_task.c"
        # portable core shared with the controller and the host test suite
        "../../../common/src/espdj_proto.c"
        "../../../common/src/espdj_resampler.c"
        "../../../common/src/espdj_ringbuf.c"
        "../../../common/src/espdj_mix.c"
        "../../../common/src/espdj_sync.c"
    INCLUDE_DIRS "." "../../../common/include"
    REQUIRES
        driver esp_driver_i2s esp_driver_uart esp_driver_gpio esp_driver_spi
        esp_adc esp_timer esp_wifi esp_netif esp_http_server esp_event
        nvs_flash fatfs sdmmc vfs json
    PRIV_REQUIRES esp_psram
)
