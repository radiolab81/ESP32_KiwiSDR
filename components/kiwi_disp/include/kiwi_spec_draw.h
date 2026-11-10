/**
 * @file kiwi_spec_draw.h
 * @brief Zeilenweiser Renderer fuer die Spektrumansicht (display-unabhaengig).
 *
 * Der Renderer liefert fuer jede Bildzeile y ein Feld von "Palettenindizes" (KIWI_PX_*).
 * Die Treiber setzen daraus Pixel: SSD1306 (1 Bit) packt 8 Zeilen zu einer Page,
 * ST7735 (RGB565) mappt auf Farben. Es gibt dadurch KEINEN Framebuffer - der Speicher
 * betraegt nur eine Zeile (<= 160 Byte) plus ~350 Byte Kontext.
 *
 * Layout (Hoehe H >= 32):  y 0..7 Kopfzeile (Frequenz, Modus) | Kurve | Achse H-11 |
 *   Durchlassbereich H-10..H-9 | Beschriftung H-8..H-2 (Randfrequenzen in kHz).
 */
#ifndef KIWI_SPEC_DRAW_H
#define KIWI_SPEC_DRAW_H

#include <stdint.h>

#include "kiwi_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KIWI_PX_BG = 0,   /**< Hintergrund                                        */
    KIWI_PX_TRACE,    /**< Spektrumkurve                                      */
    KIWI_PX_FILL,     /**< Flaeche unter der Kurve (nur Farbdisplays)         */
    KIWI_PX_AXIS,     /**< Achse und Teilstriche                              */
    KIWI_PX_MARK,     /**< Markierung der Abstimmfrequenz                     */
    KIWI_PX_PASS,     /**< Durchlassbereich des Demodulators (Balken an der Achse) */
    KIWI_PX_TEXT,     /**< Text                                               */
    KIWI_PX_TINT,     /**< getoentes Durchlassband hinter der Kurve (Farbdisplays) */
    KIWI_PX_COUNT
};

#define KIWI_DRAW_MIN_H 32u
#define KIWI_DRAW_MAX_TICKS 12u

typedef struct {
    uint16_t w, h;
    uint16_t plot_top, plot_bot, y_axis, y_label;
    int16_t  x_mark;                         /**< -1 = ausserhalb                     */
    int16_t  x_lo, x_hi;                     /**< Durchlassbereich in Display-Spalten, -1 = keiner */
    uint8_t  have;                           /**< Spektrumdaten vorhanden             */
    uint8_t  nticks;
    int16_t  tick_x[KIWI_DRAW_MAX_TICKS];
    uint8_t  yt[KIWI_WF_COLS_MAX];           /**< y der Kurve je Display-Spalte       */
    char     hdr_l[16], hdr_r[8], ftr_l[10], ftr_r[10];
} kiwi_spec_rctx_t;

/** Rechnet Geometrie, Kurvenhoehen, Teilstriche und Texte einmal je Bild. 0 = ok. */
int kiwi_spec_rctx_init(kiwi_spec_rctx_t *r, const kiwi_spec_view_t *v, uint16_t w, uint16_t h);

/** Schreibt die Zeile y als KIWI_PX_*-Indizes nach px[0..w-1]. */
void kiwi_spec_render_row(const kiwi_spec_rctx_t *r, uint16_t y, uint8_t *px);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_SPEC_DRAW_H */
