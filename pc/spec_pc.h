/**
 * @file spec_pc.h
 * @brief Spektrumanzeige am PC: simuliertes SSD1306/ST7735 (sim_panel) hinter dem ECHTEN Treibercode.
 *
 * Einthread-Variante der FreeRTOS-Glue (components/kiwi_disp/src/kiwi_spec_app.c): spec_pc_step()
 * wird aus der Hauptschleife gerufen. Optional und nicht-fatal - bei abgelehntem /W/F laeuft Audio weiter.
 */
#ifndef SPEC_PC_H
#define SPEC_PC_H

#include <stdint.h>

#include "kiwi_client.h"

typedef struct spec_pc spec_pc_t;

/** panel: "ssd1306" (I2C), "ssd1306spi", "st7735". ppm: Datei fuer das letzte Bild (oder NULL). term: 1 = im Terminal zeichnen.
 *  @return Objekt oder NULL (Meldung auf stderr). */
spec_pc_t *spec_pc_open(const char *panel, unsigned span_khz, const char *ppm, int term);

/** Aus der Hauptschleife aufrufen (nach kiwi_client_poll). Wartet nie. */
void spec_pc_step(spec_pc_t *s, kiwi_client_t *master);

/** W/F schliessen; muss VOR kiwi_client_close(master) gerufen werden. */
void spec_pc_stop(spec_pc_t *s);

void spec_pc_free(spec_pc_t *s);

#endif
