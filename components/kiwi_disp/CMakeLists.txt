# Optionale Spektrumanzeige (SSD1306 / ST7735). Der portable Teil (kiwi_wf, kiwi_spec, Renderer,
# Treiber) ist reines C und wird auch vom PC-Makefile gebaut; die FreeRTOS-Anbindung
# (kiwi_spec_app.c) nur, wenn die Funktion in menuconfig eingeschaltet ist.
set(kiwi_disp_srcs
    "src/kiwi_wf.c" "src/kiwi_spec.c" "src/kiwi_spec_draw.c"
    "src/kiwi_disp.c" "src/disp_ssd1306.c" "src/disp_st7735.c")
if(CONFIG_KIWI_SPEC)
    list(APPEND kiwi_disp_srcs "src/kiwi_spec_app.c")
endif()

idf_component_register(
    SRCS ${kiwi_disp_srcs}
    INCLUDE_DIRS "include"
    REQUIRES kiwi_core
    PRIV_REQUIRES freertos log
)
