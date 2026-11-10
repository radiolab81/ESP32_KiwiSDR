# KiwiSDR Client for Raspberry Pi Pico W (RP2040) and Pico 2 W (RP2350)

Port of the ESP32 project. The port lives in `pico/` (plus small `CONFIG_KIWI_SPEC`-guarded additions for the optional spectrum display, see main README 7a); the protocol core
(`components/kiwi_core`, `components/kiwi_dir`) is compiled from the identical source code.

## What was replaced (porting layers)

| ESP32 / ESP-IDF | Pico (this folder) | Notes |
|---|---|---|
| `port_sockets.c` | `port_lwip.c` | `kiwi_port.h` on top of lwIP sockets (`lwip_*`), `lwip_poll`, no `errno` |
| `sink_dac.c` (8-bit DAC) | `sink_pwm.c` | PWM, ~11 bit, 48 kHz, DMA ring of 4 blocks, paced by the PWM DREQ |
| `dir_store_flash.c` (`esp_partition`) | `dir_store_flash.c`, `flash_util.c` | same interface, uses `flash_safe_execute` |
| NVS | `settings.c` | last used server, one flash sector with CRC |
| `app_dir.c` | `app_dir.c` | ~50 changed lines (logging, heap, settings, stack units) |
| `app_main.c` | `main_pico.c` | `kiwi_task`/console almost identical; Wi-Fi via the CYW43 chip |
| Kconfig/menuconfig | CMake variables → `kiwi_config.h` | same `CONFIG_KIWI_*` names |
| `sdkconfig.defaults` | `FreeRTOSConfig.h`, `lwipopts.h` | |

Design decisions: FreeRTOS (single core) + `pico_cyw43_arch_lwip_sys_freertos`, because the
unmodified socket logic of the core requires an OS framework. Hardware watchdog (default 8 s).
The database lives at the end of the flash; the linker limit is reduced accordingly, so the
program can never grow into the data area.

## Building

Requirements: Pico SDK ≥ 2.1, FreeRTOS-Kernel ≥ V11.3 (for the Pico 2 W, with the submodule
`portable/ThirdParty/Community-Supported-Ports` initialised), `arm-none-eabi-gcc`, CMake, Ninja.

```sh
export PICO_SDK_PATH=~/pico-sdk FREERTOS_KERNEL_PATH=~/FreeRTOS-Kernel
cp pico/kiwi_secrets.cmake.example pico/kiwi_secrets.cmake      # enter Wi-Fi credentials (do not commit)
cmake -S pico -B build-pico  -G Ninja -DPICO_BOARD=pico_w  -DKIWI_HOST=my.kiwi.org
cmake -S pico -B build-pico2 -G Ninja -DPICO_BOARD=pico2_w
cmake --build build-pico      # -> build-pico/kiwi_pico.uf2 (copy to the BOOTSEL drive)
screen /dev/ttyACM0 115200    # monitor and control
```

Further options: `KIWI_FREQ_KHZ`, `KIWI_MODE`, `KIWI_ADPCM`, `KIWI_AUDIO_PIN/GAIN/BUFFER_MS`,
`KIWI_WIFI_COUNTRY`, `KIWI_WATCHDOG_MS`, `KIWI_DIR_*` (see `CMakeLists.txt`).

Note: `PICO_FLASH_ASSUME_CORE1_SAFE=1` is set in `CMakeLists.txt` because core 1 is never started.
Without it, `flash_safe_execute()` refuses all flash writes under FreeRTOS (single core). If you
use core 1 later, call `flash_safe_execute_core_init()` there and remove this definition.

## Hardware

Audio is output on **GPIO14** (and GPIO15, same signal; changeable via `KIWI_AUDIO_PIN`, must be an
even number, not GPIO23–25/29). For each pin use an RC low-pass filter (e.g. 2.2 kΩ + 10 nF),
followed by a 10 µF coupling capacitor to an amplifier. The GPIOs cannot drive headphones directly.
Console: USB CDC and UART0 (115200 baud). Commands as on the ESP32 (`f`, `m`, `s`, `q`, `d …`);
new: `h` (heap/stack).

## Test status

* **Verified:** compiles and links for `pico_w` and `pico2_w` without warnings 
  (SDK 2.1.1, FreeRTOS V11.3.3, GCC 13.2); several configuration variants; original files
  byte-identical. 
* **Verified:** long-term operation, real hardware (Pico W/2040, Wi-Fi, PWM/DMA-audio, flash-r/w).
* **Limitations:** the PWM clock of 125 MHz / 2604 corresponds to 48003 Hz (RP2040), i.e. marginally
  faster than 4 × 12 kHz; the jitter buffer absorbs the drift. While a flash sector is being erased
  (45–400 ms) the CPU stalls; the DMA ring bridges about 64 ms.
