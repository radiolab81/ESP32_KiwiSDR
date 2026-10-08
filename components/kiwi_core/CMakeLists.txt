# Portabler KiwiSDR-Kern als ESP-IDF-Komponente.
# Dieselben Quelldateien baut auch das PC-Makefile (pc/Makefile).
idf_component_register(
    SRCS "src/kiwi_ws.c"
         "src/kiwi_adpcm.c"
         "src/kiwi_client.c"
         "src/port_sockets.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip
    PRIV_REQUIRES esp_hw_support
)
