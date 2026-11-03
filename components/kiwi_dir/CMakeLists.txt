# Portabler Verzeichnisdienst-Client (KiwiSDR-Serverliste) als ESP-IDF-Komponente.
# Dieselben Quelldateien baut auch das PC-Makefile (pc/Makefile).
# Nutzt nur die Portschicht (kiwi_port.h) von kiwi_core -> keine eigene Netzwerk-API.
idf_component_register(
    SRCS "src/kiwi_dir_db.c"
         "src/kiwi_dir_geo.c"
         "src/kiwi_dir_geo_data.c"
         "src/kiwi_dir_parse.c"
         "src/kiwi_dir_http.c"
         "src/kiwi_dir_update.c"
         "src/kiwi_dir_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES kiwi_core
)
