/**
 * @file sim_panel.h
 * @brief Virtuelle Displays (SSD1306 / ST7735) fuer den PC: dekodieren das I2C-/SPI-Protokoll
 *        des Treibers und halten das Ergebnisbild. So laeuft der ECHTE Treibercode ohne Hardware.
 *
 * Geprueft wird auch die Protokolldisziplin: unbekannte Befehle, falsche I2C-Adresse, Daten
 * ohne Fenster, SPI-Zugriff bei inaktivem CS, abgebrochene Bildubertragung -> sim_panel_t::errors.
 */
#ifndef SIM_PANEL_H
#define SIM_PANEL_H

#include <stdio.h>
#include <stdint.h>

#include "kiwi_disp.h"

typedef enum { SIM_SSD1306 = 0, SIM_ST7735 = 1 } sim_kind_t;

typedef struct {
    sim_kind_t kind;
    kiwi_disp_bus_t bus;
    uint16_t w, h;               /* sichtbare Groesse */
    uint8_t  i2c_addr;
    uint8_t  xoff, yoff;         /* ST7735: Versatz im Controller-RAM; SSD1306: Spaltenversatz (SH1106) */
    kiwi_disp_hal_t hal;

    /* Ergebnis */
    uint8_t  gdd[8][132];        /* SSD1306-Bildspeicher (Pages) */
    uint16_t fb[160 * 160];      /* ST7735 sichtbarer Bereich (RGB565) */
    int      display_on;
    unsigned frames;             /* ST7735: abgeschlossene RAMWR-Uebertragungen; SSD1306: Page-Daten / Pages */
    unsigned i2c_writes, spi_writes, resets;
    unsigned errors;
    char     err_msg[96];

    /* Protokollzustand */
    int cs_low, dc_last;
    uint8_t cmd, args[16]; unsigned nargs, need;
    int page, col;
    unsigned x0, x1, y0, y1;     /* ST7735-Fenster */
    unsigned wpix;               /* Pixel seit RAMWR */
    int in_ramwr, hi_byte_valid; uint8_t hi_byte;
    int win_set;
} sim_panel_t;

void sim_panel_init(sim_panel_t *p, sim_kind_t kind, kiwi_disp_bus_t bus, uint16_t w, uint16_t h);
const kiwi_disp_hal_t *sim_panel_hal(sim_panel_t *p);

/** Sichtbarer Pixel als 0xRRGGBB. */
uint32_t sim_panel_rgb(const sim_panel_t *p, unsigned x, unsigned y);
/** Terminalausgabe (ANSI, 2 Pixelzeilen je Textzeile). */
void sim_panel_print(const sim_panel_t *p, FILE *out);
/** Bild als PPM (P6) schreiben. 0 = ok. */
int  sim_panel_write_ppm(const sim_panel_t *p, const char *path);

#endif
