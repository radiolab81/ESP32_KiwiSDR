# ACHTUNG: REQUIRES/PRIV_REQUIRES werden von ESP-IDF in einem fruehen Durchlauf ausgewertet, in dem die
# CONFIG_*-Variablen (sdkconfig) noch NICHT gesetzt sind. Sie duerfen deshalb nicht von CONFIG_KIWI_SPEC
# abhaengen. kiwi_disp und die Treiberkomponenten sind daher immer eingetragen; ungenutzter Code wird vom
# Linker verworfen. Nur die Quelldatei spec_hal_esp32.c (SRCS) wird bedingt aufgenommen.
set(kiwi_main_srcs "app_main.c" "sink_dac.c" "app_dir.c" "dir_store_flash.c")
if(CONFIG_KIWI_SPEC)
    list(APPEND kiwi_main_srcs "spec_hal_esp32.c")      # Optionale Spektrumanzeige (menuconfig)
endif()

idf_component_register(
    SRCS ${kiwi_main_srcs}
    INCLUDE_DIRS "."
    REQUIRES kiwi_core kiwi_dir kiwi_disp
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_dac esp_driver_uart freertos log esp_partition
                  esp_driver_i2c esp_driver_spi esp_driver_gpio
)
