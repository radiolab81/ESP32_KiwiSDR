# KiwiSDR-Client für Raspberry Pi Pico W (RP2040) und Pico 2 W (RP2350)

Port des ESP32-Projekts. **Keine Datei außerhalb von `pico/` wurde verändert**; der Protokollkern
(`components/kiwi_core`, `components/kiwi_dir`) wird als identischer Quellcode mitkompiliert.

## Was ersetzt wurde (Portierungsschichten)

| ESP32 / ESP-IDF | Pico (dieser Ordner) | Anmerkung |
|---|---|---|
| `port_sockets.c` | `port_lwip.c` | `kiwi_port.h` auf lwIP-Sockets (`lwip_*`), `lwip_poll`, kein `errno` |
| `sink_dac.c` (8-Bit-DAC) | `sink_pwm.c` | PWM ~11 Bit, 48 kHz, DMA-Ring aus 4 Blöcken, Takt per PWM-DREQ |
| `dir_store_flash.c` (`esp_partition`) | `dir_store_flash.c`, `flash_util.c` | gleiche Schnittstelle, `flash_safe_execute` |
| NVS | `settings.c` | letzter Server, 1 Flash-Sektor mit CRC |
| `app_dir.c` | `app_dir.c` | ~50 geänderte Zeilen (Log, Heap, Settings, Stackeinheit) |
| `app_main.c` | `main_pico.c` | `kiwi_task`/Konsole fast identisch; WLAN über CYW43 |
| Kconfig/menuconfig | CMake-Variablen → `kiwi_config.h` | gleiche `CONFIG_KIWI_*`-Namen |
| `sdkconfig.defaults` | `FreeRTOSConfig.h`, `lwipopts.h` | |

Entscheidungen: FreeRTOS (1 Kern) + `pico_cyw43_arch_lwip_sys_freertos`, weil die unveränderte
Sockets-Logik des Kerns einen OS-Rahmen braucht. Hardware-Watchdog (Standard 8 s).
Datenbank am Flash-Ende; das Linker-Limit wird entsprechend verkleinert (Programm wächst nie hinein).

## Bauen

Benötigt: Pico SDK ≥ 2.1, FreeRTOS-Kernel ≥ V11.3 (für Pico 2 W mit Submodul
`portable/ThirdParty/Community-Supported-Ports`), `arm-none-eabi-gcc`, CMake, Ninja.

```sh
export PICO_SDK_PATH=~/pico-sdk FREERTOS_KERNEL_PATH=~/FreeRTOS-Kernel
cp pico/kiwi_secrets.cmake.example pico/kiwi_secrets.cmake      # WLAN-Daten eintragen (nicht einchecken)
cmake -S pico -B build-pico  -G Ninja -DPICO_BOARD=pico_w  -DKIWI_HOST=mein.kiwi.org
cmake -S pico -B build-pico2 -G Ninja -DPICO_BOARD=pico2_w
cmake --build build-pico      # -> build-pico/kiwi_pico.uf2 (BOOTSEL-Laufwerk kopieren)
```
Weitere Optionen: `KIWI_FREQ_KHZ`, `KIWI_MODE`, `KIWI_ADPCM`, `KIWI_AUDIO_PIN/GAIN/BUFFER_MS`,
`KIWI_WIFI_COUNTRY`, `KIWI_WATCHDOG_MS`, `KIWI_DIR_*` (siehe `CMakeLists.txt`).

## Hardware

Audio an **GPIO14** (und GPIO15, gleiches Signal; per `KIWI_AUDIO_PIN` änderbar, gerade Zahl, nicht
GPIO23–25/29). Pro Pin: RC-Tiefpass (z. B. 2,2 kΩ + 10 nF), danach 10 µF Koppel-C zu einem
Verstärker. Die GPIOs treiben keine Kopfhörer direkt.
Konsole: USB-CDC und UART0 (115200). Befehle wie beim ESP32 (`f`, `m`, `s`, `q`, `d …`), neu: `h` (Heap/Stack).

## Teststatus

* **Geprüft:** Kompilieren und Linken für `pico_w` und `pico2_w` ohne Warnungen
  (SDK 2.1.1, FreeRTOS V11.3.3, GCC 13.2); Konfigurationsvarianten; Originaldateien byteidentisch.
* **Geprüft:** Lauf auf echter Hardware (Pico W/2040, WLAN, PWM/DMA-Audio, Flash-Schreiben).
* **Grenzen:** PWM-Takt 125 MHz/2604 ≙ 48003 Hz (RP2040), also minimal schneller als 4×12 kHz – die
  Drift fängt der Jitterpuffer ab. Während eines Flash-Sektor-Löschens (45–400 ms) steht die CPU;
  der DMA-Ring überbrückt ca. 64 ms.
