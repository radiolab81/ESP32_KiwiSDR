/**
 * @file spec_hal_esp32.c
 * @brief ESP-IDF-Anbindung der Spektrum-Displays (I2C: i2c_master, SPI: spi_master).
 *
 * Nur kompiliert, wenn CONFIG_KIWI_SPEC gesetzt ist. Pins/Takt kommen aus menuconfig
 * ("KiwiSDR Client" -> "Spektrumanzeige"). Chip-Select und D/C werden per GPIO gesteuert,
 * damit der portable Treiber CS ueber zusammengehoerende Folgen halten kann.
 */
#include "sdkconfig.h"

#ifdef CONFIG_KIWI_SPEC

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kiwi_spec_app.h"

static const char *TAG = "spec_hal";

#if defined(CONFIG_KIWI_SPEC_PANEL_SSD1306_I2C)
#define BUS_I2C 1
#else
#define BUS_I2C 0
#endif

static kiwi_disp_hal_t g_hal;

#if BUS_I2C
/* ------------------------------- I2C ------------------------------------ */
static i2c_master_dev_handle_t g_i2c;

static int hal_i2c_write(void *ctx, uint8_t addr7, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (addr7 != CONFIG_KIWI_SPEC_I2C_ADDR) return -1;
    return i2c_master_transmit(g_i2c, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int bus_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_KIWI_SPEC_PIN_SDA,
        .scl_io_num = CONFIG_KIWI_SPEC_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,       /* besser externe 4,7 kOhm; intern nur Notbehelf */
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return -1;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_KIWI_SPEC_I2C_ADDR,
        .scl_speed_hz = CONFIG_KIWI_SPEC_BUS_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dc, &g_i2c) != ESP_OK) return -1;
    g_hal.i2c_write = hal_i2c_write;
    return 0;
}
#else
/* ------------------------------- SPI ------------------------------------ */
static spi_device_handle_t g_spi;

#define PIN_DC  CONFIG_KIWI_SPEC_PIN_DC
#define PIN_CS  CONFIG_KIWI_SPEC_PIN_CS
#define PIN_RST CONFIG_KIWI_SPEC_PIN_RST    /* -1 = nicht angeschlossen */

static int hal_spi_write(void *ctx, int dc, const uint8_t *buf, size_t n)
{
    (void)ctx;
    gpio_set_level(PIN_DC, dc ? 1 : 0);
    while (n) {
        size_t c = n > 4000 ? 4000 : n;             /* unter max_transfer_sz */
        spi_transaction_t t = { .length = c * 8u, .tx_buffer = buf };
        if (spi_device_polling_transmit(g_spi, &t) != ESP_OK) return -1;
        buf += c; n -= c;
    }
    return 0;
}

static void hal_spi_cs(void *ctx, int level) { (void)ctx; gpio_set_level(PIN_CS, level ? 1 : 0); }

static void hal_reset(void *ctx, int level) { (void)ctx; if (PIN_RST >= 0) gpio_set_level(PIN_RST, level ? 1 : 0); }

static int out_pin(int pin, int level)
{
    if (pin < 0) return 0;
    gpio_config_t c = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_OUTPUT };
    if (gpio_config(&c) != ESP_OK) return -1;
    return gpio_set_level(pin, level) == ESP_OK ? 0 : -1;
}

static int bus_init(void)
{
    if (out_pin(PIN_DC, 0) || out_pin(PIN_CS, 1) || out_pin(PIN_RST, 1)) return -1;
    spi_bus_config_t bc = {
        .mosi_io_num = CONFIG_KIWI_SPEC_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_KIWI_SPEC_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    spi_device_interface_config_t dc = {
        .clock_speed_hz = CONFIG_KIWI_SPEC_BUS_HZ,
        .mode = 0,
        .spics_io_num = -1,                          /* CS manuell (haelt ueber ganze Folgen) */
        .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dc, &g_spi) != ESP_OK) return -1;
    g_hal.spi_write = hal_spi_write;
    g_hal.spi_cs = hal_spi_cs;
    g_hal.reset = hal_reset;
    return 0;
}
#endif

static void hal_delay(void *ctx, uint32_t ms) { (void)ctx; vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

const kiwi_disp_hal_t *kiwi_spec_hal_create(void)
{
    memset(&g_hal, 0, sizeof g_hal);
    g_hal.delay_ms = hal_delay;
    if (bus_init() != 0) {
        ESP_LOGE(TAG, "Bus-Initialisierung fehlgeschlagen (Pins in menuconfig pruefen)");
        return NULL;
    }
    return &g_hal;
}

#endif /* CONFIG_KIWI_SPEC */
