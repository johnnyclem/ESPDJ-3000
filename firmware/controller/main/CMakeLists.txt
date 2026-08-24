idf_component_register(
    SRCS
        "app_main.c"
        "controls.c"
        "bus_master.c"
        "pad_leds.c"
        "../../../common/src/espdj_proto.c"
        "../../../common/src/espdj_mix.c"
    INCLUDE_DIRS "." "../../../common/include"
    REQUIRES driver esp_driver_uart esp_driver_gpio esp_driver_i2c
             esp_adc esp_timer esp_event nvs_flash
)
