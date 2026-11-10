/**
 * @file disp_st7735.h
 * @brief Treiber ST7735 (farbiges TFT, RGB565, SPI), ohne Framebuffer.
 *
 * Das Bild wird als zusammenhaengender Strom in einem Adressfenster uebertragen; gezeichnet
 * wird zeilenweise in einen 2-KB-Bandpuffer (8 Zeilen). Typische Module:
 *   1,8" 128x160 (rot/schwarz/gruen): xoff 0/2, yoff 0/1,  madctl 0xC0 (RGB) oder 0xC8 (BGR)
 *   1,44" 128x128:                     xoff 2,   yoff 3
 *   0,96" 160x80:                      xoff 1,   yoff 26,  invert 1
 */
#ifndef DISP_ST7735_H
#define DISP_ST7735_H

#include "kiwi_disp.h"
#include "kiwi_spec_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_ST7735_BAND_ROWS 8u

typedef struct {
    uint16_t width, height;  /**< Pixel (Breite <= KIWI_WF_COLS_MAX)                    */
    uint8_t  xoff, yoff;     /**< Versatz im Controller-RAM                              */
    uint8_t  madctl;         /**< MADCTL-Register (Ausrichtung/RGB-Reihenfolge); 0 -> 0xC8 */
    uint8_t  invert;         /**< 1 = Farben invertieren (manche Panels brauchen INVON)   */
} disp_st7735_cfg_t;

typedef struct {
    kiwi_disp_t base;                    /**< MUSS erstes Mitglied sein */
    const kiwi_disp_hal_t *hal;
    disp_st7735_cfg_t cfg;
    uint16_t pal[KIWI_PX_COUNT];
    uint8_t px[KIWI_WF_COLS_MAX];
    uint8_t band[KIWI_WF_COLS_MAX * 2u * DISP_ST7735_BAND_ROWS];   /**< 2560 Byte */
} disp_st7735_t;

kiwi_disp_t *disp_st7735_init(disp_st7735_t *d, const kiwi_disp_hal_t *hal, const disp_st7735_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
#endif
