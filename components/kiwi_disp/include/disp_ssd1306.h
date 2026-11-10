/**
 * @file disp_ssd1306.h
 * @brief Treiber SSD1306 / SH1106 (monochrome OLED), I2C oder SPI, ohne Framebuffer.
 *
 * Es wird im Page-Adressierungsmodus gearbeitet (laeuft auf beiden Controllern); die Pages
 * werden direkt aus dem Zeilenrenderer gepackt. SH1106 (typisch 1,3"): col_offset = 2.
 */
#ifndef DISP_SSD1306_H
#define DISP_SSD1306_H

#include "kiwi_disp.h"
#include "kiwi_spec_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    kiwi_disp_bus_t bus;
    uint8_t  i2c_addr;       /**< 7 Bit: 0x3C (Standard) oder 0x3D                     */
    uint16_t width;          /**< 128 (bis 132)                                         */
    uint16_t height;         /**< 64 oder 32                                            */
    uint8_t  col_offset;     /**< 0 = SSD1306, 2 = SH1106                               */
    uint8_t  rotate180;      /**< 1 = Bild um 180 Grad gedreht                          */
    uint8_t  contrast;       /**< 0..255, 0 -> 0xCF                                      */
} disp_ssd1306_cfg_t;

typedef struct {
    kiwi_disp_t base;                    /**< MUSS erstes Mitglied sein */
    const kiwi_disp_hal_t *hal;
    disp_ssd1306_cfg_t cfg;
    uint8_t px[KIWI_WF_COLS_MAX];        /**< aktuelle Bildzeile (Palettenindizes) */
    uint8_t buf[1 + 132];                /**< Steuerbyte + eine Page              */
} disp_ssd1306_t;

/** Treiberobjekt vorbereiten (keine Hardwarezugriffe). NULL bei ungueltiger Konfiguration. */
kiwi_disp_t *disp_ssd1306_init(disp_ssd1306_t *d, const kiwi_disp_hal_t *hal, const disp_ssd1306_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
#endif
