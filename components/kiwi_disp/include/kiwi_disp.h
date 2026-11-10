/**
 * @file kiwi_disp.h
 * @brief Display-Schnittstelle und minimale Hardware-Abstraktion (HAL) fuer I2C/SPI.
 *
 * Die Treiber (SSD1306, ST7735) sind reiner, portabler C-Code: sie kennen weder ESP-IDF
 * noch das Pico-SDK, sondern sprechen nur ueber kiwi_disp_hal_t. Jede Plattform liefert
 * eine HAL (main/spec_hal_esp32.c, pico/spec_hal_pico.c, pc/sim_panel.c). Dasselbe
 * Prinzip wie kiwi_port.h und audio_sink_t im Rest des Projekts.
 */
#ifndef KIWI_DISP_H
#define KIWI_DISP_H

#include <stddef.h>
#include <stdint.h>

#include "kiwi_spec.h"
#include "kiwi_spec_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Funktionen, die eine Plattform fuer die Anzeige bereitstellen muss (nicht benoetigte = NULL). */
typedef struct kiwi_disp_hal {
    /** I2C: schreibt @p n Bytes (Steuerbyte bereits enthalten) an die 7-Bit-Adresse. 0 = ok. */
    int  (*i2c_write)(void *ctx, uint8_t addr7, const uint8_t *buf, size_t n);
    /** SPI: sendet @p n Bytes; @p dc = 0 Befehl, 1 Daten. CS wird NICHT hier bewegt. 0 = ok. */
    int  (*spi_write)(void *ctx, int dc, const uint8_t *buf, size_t n);
    /** SPI-Chip-Select: level 0 = aktiv. Der Treiber haelt CS ueber zusammengehoerende Folgen. */
    void (*spi_cs)(void *ctx, int level);
    /** Reset-Leitung (optional). level 0 = Reset aktiv. */
    void (*reset)(void *ctx, int level);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} kiwi_disp_hal_t;

typedef enum { KIWI_DISP_BUS_I2C = 0, KIWI_DISP_BUS_SPI = 1 } kiwi_disp_bus_t;

/** Gemeinsame Schnittstelle aller Treiber (Instanz ist das erste Mitglied des Treiberobjekts). */
typedef struct kiwi_disp {
    uint16_t width, height;
    int  (*open)(struct kiwi_disp *d);                                       /**< Hardware initialisieren, 0 = ok */
    int  (*present)(struct kiwi_disp *d, const kiwi_spec_view_t *v);          /**< Ansicht zeichnen, 0 = ok        */
    void (*close)(struct kiwi_disp *d);                                       /**< Anzeige ausschalten            */
} kiwi_disp_t;

/** Merkt sich, was zuletzt gezeichnet wurde (damit unveraenderte Bilder nicht gesendet werden). */
typedef struct {
    uint32_t last_ver;
    uint8_t  last_valid;
    uint8_t  drawn;          /**< seit (Re-)Open mindestens ein Bild gezeichnet */
    uint8_t  opened;         /**< 0 = vor dem naechsten Zeichnen (erneut) initialisieren */
    uint32_t frames, errors;
} kiwi_spec_ui_t;

/**
 * Ein Anzeige-Zyklus: Snapshot holen, nur bei Aenderung zeichnen, bei Fehlern das Display
 * beim naechsten Mal neu initialisieren (z. B. nach Wackelkontakt).
 * @return 1 = gezeichnet, 0 = nichts zu tun, <0 = Fehler (naechster Aufruf versucht es erneut)
 */
int kiwi_spec_refresh(kiwi_spec_t *s, kiwi_disp_t *d, kiwi_spec_ui_t *ui, uint32_t now_ms, uint32_t stale_ms);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DISP_H */
