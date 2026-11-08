# =============================================================================
# KiwiSDR-Client fuer Raspberry Pi Pico W (RP2040) und Pico 2 W (RP2350)
#
# Dieses Projekt baut den UNVERAENDERTEN portablen Kern aus ../components/
# (kiwi_core, kiwi_dir) und ersetzt nur die Plattformschicht (siehe README.md).
#
#   Pico W   : cmake -S . -B build-pico  -G Ninja -DPICO_BOARD=pico_w
#   Pico 2 W : cmake -S . -B build-pico2 -G Ninja -DPICO_BOARD=pico2_w
#   cmake --build build-pico            ->  build-pico/kiwi_pico.uf2
#
# Vorausgesetzt: PICO_SDK_PATH (>= 2.1) und FREERTOS_KERNEL_PATH (>= V11.3,
# mit initialisiertem Submodul "Community-Supported-Ports" fuer den RP2350).
# =============================================================================
cmake_minimum_required(VERSION 3.13)

# Voreinstellung: Pico W. Fuer den Pico 2 W:  -DPICO_BOARD=pico2_w
if(NOT PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "pico_w (RP2040) | pico2_w (RP2350)")
endif()
if(NOT PICO_BOARD MATCHES "^pico2?_w$")
    message(FATAL_ERROR "PICO_BOARD muss pico_w oder pico2_w sein (WLAN-Chip CYW43 noetig), ist: ${PICO_BOARD}")
endif()

include(pico_sdk_import.cmake)           # findet das SDK ueber PICO_SDK_PATH
include(FreeRTOS_Kernel_import.cmake)    # waehlt RP2040- bzw. RP2350-Port automatisch

# ---------------------------------------------------------------------------
# Flash-Aufteilung: Serverdatenbank + Einstellungen liegen am ENDE des Flashs.
# Damit das Programm nie in diesen Bereich hineinwachsen kann, wird die dem
# Linker bekannte Flash-Groesse entsprechend verkleinert (Linkfehler statt
# stiller Datenzerstoerung).
# ---------------------------------------------------------------------------
set(KIWI_DIR_SLOT_KB 192 CACHE STRING "Groesse EINES Datenbank-Slots in KB (Vielfaches von 4; A/B => 2 Slots)")
if(NOT KIWI_FLASH_SIZE_KB)
    if(PICO_BOARD STREQUAL "pico2_w")
        set(_kiwi_flash_default_kb 4096)     # Pico 2 W: 4 MB
    else()
        set(_kiwi_flash_default_kb 2048)     # Pico W: 2 MB
    endif()
    set(KIWI_FLASH_SIZE_KB ${_kiwi_flash_default_kb} CACHE STRING "Flash-Groesse des Boards in KB (Pico W: 2048, Pico 2 W: 4096; bei Nachbauten anpassen)")
endif()
math(EXPR _kiwi_store_bytes "(2 * ${KIWI_DIR_SLOT_KB} + 4) * 1024")   # 2 Slots + 1 Einstellungs-Sektor
math(EXPR _kiwi_flash_total "${KIWI_FLASH_SIZE_KB} * 1024")
math(EXPR _kiwi_flash_app   "${_kiwi_flash_total} - ${_kiwi_store_bytes}")
set(PICO_FLASH_SIZE_BYTES "${_kiwi_flash_app}")        # nur fuer das Linker-Skript (Programmgroesse)

project(kiwi_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Konfiguration (Gegenstueck zu Kconfig/menuconfig im ESP-IDF-Projekt).
# Aendern per:  cmake -DKIWI_HOST=... -DKIWI_WIFI_SSID=... ..., ccmake oder cmake-gui.
# Zugangsdaten bitte NICHT ins Repository: kiwi_secrets.cmake (siehe .example).
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/kiwi_secrets.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/kiwi_secrets.cmake")
endif()

set(KIWI_WIFI_SSID        "meine-ssid"          CACHE STRING "WLAN SSID")
set(KIWI_WIFI_PASSWORD    ""                    CACHE STRING "WLAN Passwort (leer = offenes WLAN)")
set(KIWI_WIFI_COUNTRY     "DE"                  CACHE STRING "WLAN-Laendercode (2 Buchstaben, ISO 3166) - bestimmt erlaubte Kanaele/Leistung")
set(KIWI_HOST             "kiwisdr.adresse.org" CACHE STRING "KiwiSDR Hostname / IPv4")
set(KIWI_PORT             "8073"                CACHE STRING "KiwiSDR Port")
set(KIWI_PASSWORD         ""                    CACHE STRING "KiwiSDR Passwort (leer = oeffentlich)")
set(KIWI_USER_NAME        "pico"                CACHE STRING "Anzeigename in der Kiwi-Nutzerliste (nur A-Z a-z 0-9 _ -)")
set(KIWI_FREQ_KHZ         "225"                 CACHE STRING "Startfrequenz in kHz (z. B. 225 oder 7055.5)")
set(KIWI_MODE             "am"                  CACHE STRING "Demodulation (am amn amw sam usb lsb cw nbfm ...)")
option(KIWI_ADPCM         "ADPCM-Kompression anfordern (spart WLAN-Bandbreite)" OFF)
option(KIWI_STATUS_LOG    "Status alle 10 s automatisch loggen" OFF)

set(KIWI_AUDIO_PIN          "14"   CACHE STRING "PWM-Audioausgang: GPIO-Nummer (GERADE, A-Kanal; GPIO+1 liefert dasselbe Signal)")
set(KIWI_AUDIO_GAIN         "2"    CACHE STRING "Software-Verstaerkung vor dem PWM (1..8)")
set(KIWI_AUDIO_BUFFER_MS    "1000" CACHE STRING "Jitterpuffer in Millisekunden (200..4000)")
set(KIWI_AUDIO_PREBUFFER_MS "400"  CACHE STRING "Vorpufferung vor Wiedergabestart in Millisekunden (100..2000)")

set(KIWI_DIR_URL            "http://rx.linkfanel.net/kiwisdr_com.js" CACHE STRING "URL der Serverliste (nur http://)")
set(KIWI_DIR_MIN_INTERVAL_S "60"  CACHE STRING "Mindestabstand zwischen zwei Verzeichnis-Updates in Sekunden")
option(KIWI_DIR_FAILOVER    "Bei nicht erreichbarem Server automatisch wechseln" ON)
set(KIWI_DIR_FAILOVER_MAX   "5"   CACHE STRING "Maximale Wechsel hintereinander")

option(KIWI_WIFI_NO_POWERSAVE "WLAN-Energiesparmodus aus (weniger Jitter, wie WIFI_PS_NONE im ESP32-Projekt)" ON)
set(KIWI_WATCHDOG_MS        "8000" CACHE STRING "Hardware-Watchdog in ms (0 = aus). Startet bei Haenger/Absturz neu.")
option(KIWI_STDIO_USB       "Konsole ueber USB (CDC)" ON)
option(KIWI_STDIO_UART      "Konsole ueber UART0 (GPIO0/1, 115200 Baud)" ON)

# --- Plausibilitaetspruefungen (frueh scheitern statt kryptisch zur Laufzeit) ---
if(NOT KIWI_AUDIO_PIN MATCHES "^[0-9]+$" OR KIWI_AUDIO_PIN GREATER 28 OR (KIWI_AUDIO_PIN GREATER 21 AND KIWI_AUDIO_PIN LESS 26))
    message(FATAL_ERROR "KIWI_AUDIO_PIN=${KIWI_AUDIO_PIN} ungueltig: GPIO0..22 oder 26..28 (GPIO23-25/29 gehoeren dem WLAN-Chip)")
endif()
math(EXPR _pin_odd "${KIWI_AUDIO_PIN} % 2")
if(_pin_odd)
    message(FATAL_ERROR "KIWI_AUDIO_PIN muss GERADE sein (A-Kanal eines PWM-Slices; der B-Kanal GPIO+1 gibt dasselbe Signal aus)")
endif()
if(NOT KIWI_WATCHDOG_MS MATCHES "^[0-9]+$" OR KIWI_WATCHDOG_MS GREATER 8300)
    message(FATAL_ERROR "KIWI_WATCHDOG_MS muss 0 (aus) oder 1..8300 sein (Hardware-Grenze ca. 8,3 s)")
endif()
if(NOT KIWI_WIFI_COUNTRY MATCHES "^[A-Z][A-Z]$")
    message(FATAL_ERROR "KIWI_WIFI_COUNTRY muss aus zwei Grossbuchstaben bestehen, z. B. DE")
endif()
math(EXPR _slot_mod "${KIWI_DIR_SLOT_KB} % 4")
if(_slot_mod OR KIWI_DIR_SLOT_KB LESS 64)
    message(FATAL_ERROR "KIWI_DIR_SLOT_KB muss ein Vielfaches von 4 und >= 64 sein")
endif()
string(REGEX MATCH "^[0-9]+$" _ok "${KIWI_PORT}")
if(NOT _ok OR KIWI_PORT LESS 1 OR KIWI_PORT GREATER 65535)
    message(FATAL_ERROR "KIWI_PORT ungueltig: ${KIWI_PORT}")
endif()

# Text fuer ein C-Stringliteral aufbereiten (Backslash und Anfuehrungszeichen escapen).
foreach(_v KIWI_WIFI_SSID KIWI_WIFI_PASSWORD KIWI_HOST KIWI_PASSWORD KIWI_USER_NAME
           KIWI_FREQ_KHZ KIWI_MODE KIWI_DIR_URL)
    string(REPLACE "\\" "\\\\" _c "${${_v}}")
    string(REPLACE "\"" "\\\"" _c "${_c}")
    set(${_v}_C "${_c}")
endforeach()
string(SUBSTRING "${KIWI_WIFI_COUNTRY}" 0 1 KIWI_WIFI_COUNTRY_1)
string(SUBSTRING "${KIWI_WIFI_COUNTRY}" 1 1 KIWI_WIFI_COUNTRY_2)

# kiwi_config.h entsteht im BUILD-Verzeichnis (nicht im Quellbaum -> keine Passwoerter im Repo).
configure_file(${CMAKE_CURRENT_LIST_DIR}/kiwi_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/kiwi_config.h @ONLY)

# ---------------------------------------------------------------------------
# Ziel
# ---------------------------------------------------------------------------
set(KIWI_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_executable(kiwi_pico
    # --- UNVERAENDERTER portabler Kern (identische Dateien wie ESP-IDF und PC) ---
    ${KIWI_ROOT}/components/kiwi_core/src/kiwi_ws.c
    ${KIWI_ROOT}/components/kiwi_core/src/kiwi_adpcm.c
    ${KIWI_ROOT}/components/kiwi_core/src/kiwi_client.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_db.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_geo.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_geo_data.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_parse.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_http.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_update.c
    ${KIWI_ROOT}/components/kiwi_dir/src/kiwi_dir_cmd.c
    # --- Pico-spezifische Plattformschicht (nur diese Dateien sind neu) ---
    port_lwip.c          # ersetzt port_sockets.c (kiwi_port.h auf lwIP-Sockets/FreeRTOS)
    sink_pwm.c           # ersetzt sink_dac.c    (PWM + DMA statt 8-Bit-DAC)
    dir_store_flash.c    # ersetzt main/dir_store_flash.c (XIP-Flash statt esp_partition)
    flash_util.c         # sicheres Loeschen/Programmieren (flash_safe_execute)
    settings.c           # ersetzt NVS (zuletzt gewaehlter Server)
    app_dir.c            # Port von main/app_dir.c
    main_pico.c          # Port von main/app_main.c
)

target_include_directories(kiwi_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}                       # FreeRTOSConfig.h, lwipopts.h, klog.h ...
    ${CMAKE_CURRENT_BINARY_DIR}/generated           # kiwi_config.h
    ${KIWI_ROOT}/components/kiwi_core/include
    ${KIWI_ROOT}/components/kiwi_dir/include
)

# FreeRTOS braucht FreeRTOSConfig.h (liegt in diesem Ordner).
target_compile_definitions(kiwi_pico PRIVATE
    KIWI_FLASH_TOTAL_BYTES=${_kiwi_flash_total}   # echte Flash-Groesse (Datenbank liegt an deren Ende)
    CYW43_HOST_NAME="kiwi-pico"                 # Hostname im Router (DHCP)
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0    # nicht auf ein USB-Terminal warten
    # Kern 1 wird nie gestartet => beim Flash-Schreiben genuegt "Interrupts aus" auf Kern 0.
    # OHNE dies verweigert flash_safe_execute() unter FreeRTOS (1 Kern) jeden Schreibzugriff
    # mit PICO_ERROR_NOT_PERMITTED. Wer Kern 1 spaeter benutzt, muss dort
    # flash_safe_execute_core_init() aufrufen und diese Zeile entfernen.
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)

target_compile_options(kiwi_pico PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(kiwi_pico PRIVATE
    pico_stdlib
    pico_rand
    pico_atomic                              # __atomic_*-Funktionen fuer den RP2040 (stdatomic im Kern)
    pico_flash                               # flash_safe_execute()
    pico_cyw43_arch_lwip_sys_freertos        # WLAN + lwIP-Sockets im FreeRTOS-Betrieb
    FreeRTOS-Kernel-Heap4
    hardware_pwm
    hardware_dma
    hardware_flash
    hardware_watchdog
    hardware_clocks
    hardware_irq
)

pico_enable_stdio_usb(kiwi_pico  $<IF:$<BOOL:${KIWI_STDIO_USB}>,1,0>)
pico_enable_stdio_uart(kiwi_pico $<IF:$<BOOL:${KIWI_STDIO_UART}>,1,0>)

pico_add_extra_outputs(kiwi_pico)            # erzeugt .uf2 / .bin / .hex

# Zusammenfassung beim Konfigurieren
message(STATUS "KiwiSDR-Pico: Board=${PICO_BOARD} Plattform=${PICO_PLATFORM} Flash gesamt=${_kiwi_flash_total} B, "
               "Programm max. ${_kiwi_flash_app} B, Datenspeicher ${_kiwi_store_bytes} B, Audio-PWM auf GPIO${KIWI_AUDIO_PIN}")
