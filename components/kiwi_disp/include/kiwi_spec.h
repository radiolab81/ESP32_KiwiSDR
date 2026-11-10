/**
 * @file kiwi_spec.h
 * @brief Spektrum-Zustand: Glaettung, Autoskalierung und lock-freier Austausch Schreiber -> Anzeige.
 *
 * Der W/F-Task schreibt (kiwi_spec_push / kiwi_spec_set_tune), der Anzeige-Task liest
 * (kiwi_spec_snapshot). Der Austausch ist ein "Seqlock": kein Mutex, keine Betriebssystem-
 * abhaengigkeit, der Schreiber wird nie blockiert (wichtig: er laeuft im Netz-Takt).
 * Es darf genau EINEN Schreiber geben.
 */
#ifndef KIWI_SPEC_H
#define KIWI_SPEC_H

#include <stdatomic.h>
#include <stdint.h>

#include "kiwi_wf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fertige Ansicht (Pegel normiert), Grundlage fuer jeden Renderer. */
typedef struct {
    uint8_t  lvl[KIWI_WF_COLS_MAX];  /**< 0..255 je Spalte (nach Glaettung und Skalierung)      */
    uint16_t cols;
    uint8_t  valid;                  /**< 1 = frische Spektrumdaten vorhanden                   */
    uint32_t f_start_hz, span_hz;    /**< Frequenzachse der Spalten                              */
    uint32_t f_tune_hz;              /**< eingestellte Frequenz (0 = unbekannt)                  */
    int16_t  pb_lo_hz, pb_hi_hz;     /**< Durchlassbereich relativ zu f_tune_hz                  */
    char     mode[8];
    uint32_t ver;                    /**< zaehlt jede Aenderung (zum Erkennen "neu zeichnen")    */
} kiwi_spec_view_t;

typedef struct {
    /* --- Konfiguration (vor kiwi_spec_init setzen ist nicht noetig, siehe init) --- */
    uint8_t  smooth;                 /**< 0 aus, 1..3: exponentielle Glaettung mit 1/2, 1/4, 1/8 */
    uint8_t  autoscale;              /**< 1 = Bereich folgt Rauschteppich und Spitzen            */
    int16_t  lo_dbm, hi_dbm;         /**< fester Bereich, wenn autoscale == 0                    */
    /* --- Zustand --- */
    atomic_uint guard;               /**< Seqlock-Zaehler (ungerade = Schreiber aktiv)           */
    kiwi_spec_view_t v;              /**< veroeffentlichte Ansicht                               */
    uint16_t sm[KIWI_WF_COLS_MAX];   /**< geglaettete Rohwerte, 4 Nachkommabits                  */
    uint16_t floor16, peak16;        /**< Rauschteppich / Spitzenwert (x16), fuer Autoskalierung */
    uint32_t t_line_ms;              /**< Zeitpunkt der letzten Zeile                            */
    uint32_t f_start_prev;
    uint8_t  have;                   /**< mindestens eine Zeile verarbeitet                      */
    kiwi_spec_sink_t sink;           /**< fertig verdrahtet: an kiwi_wf_config_t::sink uebergeben */
} kiwi_spec_t;

void kiwi_spec_init(kiwi_spec_t *s, uint8_t smooth, uint8_t autoscale, int lo_dbm, int hi_dbm);

/** Neue Sitzung/Verbindungsabbruch: Anzeige zeigt wieder "NO DATA". */
void kiwi_spec_reset(kiwi_spec_t *s);

/** Eine Zeile verarbeiten und veroeffentlichen (wird ueber s->sink vom W/F-Client gerufen). */
void kiwi_spec_push(kiwi_spec_t *s, const kiwi_wf_line_t *ln);

/** Abstimmfrequenz und Modus aktualisieren (nur bei Aenderung wird etwas veroeffentlicht). */
void kiwi_spec_set_tune(kiwi_spec_t *s, uint32_t f_hz, const char *mode);

/**
 * Konsistente Kopie der Ansicht holen. Daten aelter als @p stale_ms gelten als ungueltig
 * (valid = 0, z. B. Verbindung weg). @return 1 = Kopie fertig.
 */
int kiwi_spec_snapshot(kiwi_spec_t *s, kiwi_spec_view_t *out, uint32_t now_ms, uint32_t stale_ms);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_SPEC_H */
