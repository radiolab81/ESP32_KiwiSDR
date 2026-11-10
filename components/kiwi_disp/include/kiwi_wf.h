/**
 * @file kiwi_wf.h
 * @brief Optionaler Wasserfall-/Spektrum-Client (zweiter WebSocket /W/F) fuer KiwiSDR.
 *
 * HINTERGRUND (aus Browser-Mitschnitt an KiwiSDR v1.902 belegt)
 *   Der Audiostrom (/SND) enthaelt nur das demodulierte Audio (12 kHz Rate, also max.
 *   +-6 kHz). Das Spektrum um die Abstimmfrequenz liefert ein ZWEITER WebSocket auf
 *   demselben Port: /ws/kiwi/<ts>/W/F. Der Browser benutzt fuer beide Verbindungen
 *   DENSELBEN Zeitstempel <ts>; der Kiwi ordnet sie so einer Sitzung zu.
 *
 *   WICHTIG (Hoeflichkeit/Zulaessigkeit): Ein zweiter, unabhaengiger Client derselben
 *   IP wird von KiwiSDR-Servern i. d. R. abgelehnt. Dieser Client ist deshalb KEIN
 *   eigener Nutzer, sondern haengt sich ueber kiwi_client_t::ts_id an die laufende
 *   Audio-Sitzung. Er ist optional: wird er abgelehnt, laeuft das Audio unveraendert weiter.
 *
 *   Frame: "W/F" + 1 Byte + x_bin(u32 LE) + flags_zoom(u32 LE) + seq(u32 LE) + Daten.
 *   Daten = 1024 Bins (Byte = dBm + 255, wenn "SET send_dB=1"), entweder roh oder
 *   (Flag 0x10000) IMA-ADPCM mit 10 Vorlauf-Samples AM ANFANG der Zeile (siehe .c).
 *   Fenster: Zoom z zeigt bandwidth/2^z; Startbin x_bin zaehlt im 2^24-Raster.
 *
 * Der Client reduziert jede Zeile sofort auf die Spaltenzahl des Displays (Max-Hold je
 * Spalte) und uebergibt nur diese kleine Zeile - es gibt keinen 1024-Byte-Zeilenpuffer.
 */
#ifndef KIWI_WF_H
#define KIWI_WF_H

#include <stddef.h>
#include <stdint.h>

#include "kiwi_client.h"
#include "kiwi_ws.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIWI_WF_BINS      1024u   /**< Bins pro Zeile (wf_fft_size des Kiwi)          */
#define KIWI_WF_COLS_MAX  160u    /**< groesste unterstuetzte Spaltenzahl (Displaybreite) */

/** Empfangspuffer des W/F-Sockets. Muss >= 1040 + 1 sein; groessere MSG werden verworfen. */
#ifndef KIWI_WF_RX_BUF_SIZE
#define KIWI_WF_RX_BUF_SIZE 1152u
#endif

/** Eine auf Display-Spalten reduzierte Spektrumzeile. */
typedef struct {
    uint8_t  cols[KIWI_WF_COLS_MAX]; /**< Rohwerte je Spalte (dBm + 255), Max-Hold ueber die Bins */
    uint16_t ncols;
    uint32_t f_start_hz;             /**< Frequenz am linken Rand von Spalte 0 (inkl. Kiwi-Offset) */
    uint32_t span_hz;                /**< Breite aller Spalten                                      */
    uint32_t seq;                    /**< Sequenznummer aus dem Frame-Kopf                          */
    uint32_t t_ms;                   /**< Empfangszeit (kiwi_time_ms)                               */
} kiwi_wf_line_t;

/** Abnehmer der Zeilen (Strategie-Muster wie audio_sink_t). Wird im Kontext des W/F-Tasks gerufen. */
typedef struct {
    void (*line)(void *ctx, const kiwi_wf_line_t *ln);
    void *ctx;
} kiwi_spec_sink_t;

typedef struct {
    const kiwi_client_t *master;     /**< Audio-Client: Server, Passwort, ts_id, Abstimmung (muss verbunden sein) */
    uint16_t cols;                   /**< Spaltenzahl 16..KIWI_WF_COLS_MAX (= Displaybreite)                      */
    uint32_t span_hz;                /**< gewuenschte Anzeigebreite, z. B. 50000 (+-25 kHz)                       */
    uint8_t  speed;                  /**< wf_speed 1..4 (1 = ca. 1 Zeile/s, 4 = Maximum); 0 -> 2                  */
    uint32_t connect_timeout_ms;     /**< 0 -> 10000                                                              */
    uint32_t idle_timeout_ms;        /**< Zeit ohne jedes Byte bis Timeout; 0 -> 10000                            */
    const kiwi_spec_sink_t *sink;
} kiwi_wf_config_t;

typedef struct kiwi_wf {
    kiwi_wf_config_t cfg;
    kiwi_net_t *net;
    kiwi_ws_rx_t ws;
    kiwi_err_t err;                  /**< Fehler aus dem Nachrichten-Callback (sticky)     */
    int setup_done;
    uint32_t bw_hz;                  /**< Bandbreite des Kiwi (MSG bandwidth=, Standard 32 MHz) */
    uint8_t  zoom, zoom_cap;
    uint16_t visible;                /**< Bins, die auf cfg.cols Spalten verteilt werden    */
    int      req_valid;
    uint32_t req_start_hz;           /**< zuletzt angefordertes Fensterende links (Basisband) */
    uint32_t t_last_rx, t_last_ka;
    struct { uint32_t lines, stale, bad; } st;
    kiwi_wf_line_t line;
    uint8_t rxbuf[KIWI_WF_RX_BUF_SIZE];
    uint8_t tmp[512];
} kiwi_wf_t;

/** Konfiguration pruefen und Zustand anlegen. */
kiwi_err_t kiwi_wf_init(kiwi_wf_t *w, const kiwi_wf_config_t *cfg);

/** Verbindet /W/F mit der ts_id des Audio-Clients und meldet sich an. Konfiguration folgt
 *  automatisch, sobald der Kiwi "wf_setup" meldet (in kiwi_wf_poll). */
kiwi_err_t kiwi_wf_connect(kiwi_wf_t *w);

/** Nachrichten verarbeiten, Zeilen liefern, Fenster nachfuehren, Keepalive senden.
 *  Wartet hoechstens timeout_ms auf das erste Byte, liest danach nur noch Vorhandenes. */
kiwi_err_t kiwi_wf_poll(kiwi_wf_t *w, uint32_t timeout_ms);

/** Close-Frame senden und Socket schliessen (idempotent). */
void kiwi_wf_close(kiwi_wf_t *w);

/** Fuer Tests/Wiedergabe: rohe WebSocket-Bytes (Server->Client) wie vom Socket einspeisen. */
kiwi_err_t kiwi_wf_feed(kiwi_wf_t *w, const uint8_t *data, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_WF_H */
