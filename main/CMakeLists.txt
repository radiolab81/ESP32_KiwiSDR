idf_component_register(
    SRCS "app_main.c" "sink_dac.c"
    INCLUDE_DIRS "."
    REQUIRES kiwi_core
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_dac esp_driver_uart freertos log
)
