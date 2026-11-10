/**
 * @file spec_hal_pico.c
 * @brief Pico-SDK-Anbindung der Spektrum-Displays (hardware_i2c / hardware_spi / hardware_gpio).
 *
 * Nur kompiliert, wenn KIWI_SPEC in CMake eingeschaltet ist (siehe CMakeLists.txt).
 * Gegenstueck zu main/spec_hal_esp32.c; der Treibercode darueber ist identisch.
 * Chip-Select und D/C werden per GPIO gesteuert (SPI-Peripherie-CS ist hier unpassend,
 * weil der Treiber CS ueber ganze Befehls-/Datenfolgen halten muss).
 */
#include "kiwi_config.h"

#ifdef CONFIG_KIWI_SPEC

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "klog.h"
#include "kiwi_spec_app.h"

static const char *TAG = "spec_hal";

static kiwi_disp_hal_t g_hal;

#ifdef CONFIG_KIWI_SPEC_PANEL_SSD1306_I2C
/* ------------------------------- I2C ------------------------------------ */
#define I2C_INST (CONFIG_KIWI_SPEC_I2C_INST ? i2c1 : i2c0)

static int hal_i2c_write(void *ctx, uint8_t addr7, const uint8_t *buf, size_t n)
{
    (void)ctx;
    int r = i2c_write_timeout_us(I2C_INST, addr7, buf, n, false, 50000);
    return r == (int)n ? 0 : -1;
}

static int bus_init(void)
{
    i2c_init(I2C_INST, CONFIG_KIWI_SPEC_BUS_HZ);
    gpio_set_function(CONFIG_KIWI_SPEC_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(CONFIG_KIWI_SPEC_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(CONFIG_KIWI_SPEC_PIN_SDA);          /* besser externe 4,7 kOhm; intern nur Notbehelf */
    gpio_pull_up(CONFIG_KIWI_SPEC_PIN_SCL);
    g_hal.i2c_write = hal_i2c_write;
    return 0;
}
#else
/* ------------------------------- SPI ------------------------------------ */
#define SPI_INST (CONFIG_KIWI_SPEC_SPI_INST ? spi1 : spi0)
#define PIN_DC   CONFIG_KIWI_SPEC_PIN_DC
#define PIN_CS   CONFIG_KIWI_SPEC_PIN_CS
#define PIN_RST  CONFIG_KIWI_SPEC_PIN_RST            /* -1 = nicht angeschlossen */

static int hal_spi_write(void *ctx, int dc, const uint8_t *buf, size_t n)
{
    (void)ctx;
    gpio_put(PIN_DC, dc ? 1 : 0);
    int r = spi_write_blocking(SPI_INST, buf, n);
    while (spi_is_busy(SPI_INST)) tight_loop_contents();   /* erst wenn alles draussen ist, darf D/C oder CS wechseln */
    return r == (int)n ? 0 : -1;
}

static void hal_spi_cs(void *ctx, int level) { (void)ctx; gpio_put(PIN_CS, level ? 1 : 0); }

static void hal_reset(void *ctx, int level) { (void)ctx; if (PIN_RST >= 0) gpio_put(PIN_RST, level ? 1 : 0); }

static void out_pin(int pin, int level)
{
    if (pin < 0) return;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, level);
}

static int bus_init(void)
{
    spi_init(SPI_INST, CONFIG_KIWI_SPEC_BUS_HZ);
    spi_set_format(SPI_INST, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(CONFIG_KIWI_SPEC_PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(CONFIG_KIWI_SPEC_PIN_MOSI, GPIO_FUNC_SPI);
    out_pin(PIN_DC, 0);
    out_pin(PIN_CS, 1);
    out_pin(PIN_RST, 1);
    g_hal.spi_write = hal_spi_write;
    g_hal.spi_cs = hal_spi_cs;
    g_hal.reset = hal_reset;
    return 0;
}
#endif

static void hal_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

const kiwi_disp_hal_t *kiwi_spec_hal_create(void)
{
    memset(&g_hal, 0, sizeof g_hal);
    g_hal.delay_ms = hal_delay;
    if (bus_init() != 0) { KLOGE(TAG, "Bus-Initialisierung fehlgeschlagen"); return NULL; }
    return &g_hal;
}

#endif /* CONFIG_KIWI_SPEC */
