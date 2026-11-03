idf_component_register(
    SRCS "app_main.c" "sink_dac.c" "app_dir.c" "dir_store_flash.c"
    INCLUDE_DIRS "."
    REQUIRES kiwi_core kiwi_dir
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_dac esp_driver_uart freertos log esp_partition
)
