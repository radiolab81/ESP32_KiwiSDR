/**
 * @file kiwi_client.h
 * @brief KiwiSDR-Audioclient (Protokoll "SND"): verbinden, abstimmen, PCM liefern.
 *
 * PROTOKOLL-UEBERBLICK (aus kiwiclient/kiwi/client.py abgeleitet)
 * ===============================================================
 *
 *  1. TCP-Verbindung zum Kiwi (Standardport 8073).
 *  2. HTTP-Upgrade auf WebSocket mit Pfad  "/ws/kiwi/<zeitstempel>/SND"
 *     (KiwiSDR v1.9xx, per Browser-Mitschnitt belegt; aeltere Kiwis nutzen
 *     "/<zeitstempel>/SND" ohne Praefix). Mitgesendet wird "Origin: http://host:port".
 *     (Jede Verbindung braucht einen eigenen Zeitstempel; er dient dem Server
 *      zur Zuordnung. Der Wert selbst ist beliebig.)
 *  3. Client sendet als Text-Frame:  "SET auth t=kiwi p=<passwort>"
 *     (ohne Passwort sendet der Browser "p=#"), dann "SERVER DE CLIENT <name> SND"
 *  4. Server schickt "MSG"-Nachrichten mit Parametern ("name=wert name=wert"),
 *     u. a.  version_maj, version_min, audio_rate=12000, sample_rate=12000.xxx
 *     und Fehler wie too_busy=, badp=, down=, redirect=.
 *  5. Auf "audio_rate=N" antwortet der Client:  "SET AR OK in=N out=44100"
 *     Auf "sample_rate=F" folgt die Konfiguration:
 *        SET squelch=0 max=0 / SET genattn=0 / SET gen=0 mix=-1
 *        SET ident_user=<name>
 *        SET mod=<modus> low_cut=<Hz> high_cut=<Hz> freq=<kHz>   <- ABSTIMMEN
 *        SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50
 *        SET compression=0   (optional: unkomprimiertes PCM anfordern)
 *        SET keepalive
 *  6. Danach schickt der Server laufend binaere "SND"-Frames:
 *
 *        Byte 0..2  "SND"                    (Tag)
 *        Byte 3     flags   (0x02 ADC-Ueberlauf, 0x08 Stereo, 0x10 ADPCM,
 *                            0x80 little endian - nur "camp")
 *        Byte 4..7  Sequenznummer, little endian
 *        Byte 8..9  S-Meter, BIG endian;  RSSI[dBm] = 0.1 * Wert - 127
 *        Byte 10..  Audiodaten: ADPCM (flags&0x10) oder int16 BIG endian
 *
 *  7. Mindestens 1x pro Sekunde "SET keepalive", sonst trennt der Server.
 *  8. Beim Beenden: WebSocket-Close-Frame senden, dann TCP schliessen.
 *
 * Der erste SND-Frame nach dem Start enthaelt noch Audio-Reste des vorigen
 * Nutzers dieses Empfangskanals und wird deshalb verworfen.
 */
#ifndef KIWI_CLIENT_H
#define KIWI_CLIENT_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "kiwi_adpcm.h"
#include "kiwi_audio_sink.h"
#include "kiwi_port.h"
#include "kiwi_ws.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Groesse des Nachrichtenpuffers (groessere Nachrichten werden verworfen). */
#ifndef KIWI_RX_BUF_SIZE
/* KiwiSDR v1.9xx: SND-Frame = 10 Byte Kopf + 1024 Nutzbytes (ADPCM: 2048
 * Samples, roh: 512 Samples). 6 KB lassen Reserve fuer groessere Varianten. */
#define KIWI_RX_BUF_SIZE 6144
#endif

/** Fehlercodes (0 = OK, alles andere negativ). */
typedef enum {
    KIWI_OK              =  0,
    KIWI_ERR_ARG         = -1,  /**< ungueltiger Parameter                    */
    KIWI_ERR_NET         = -2,  /**< Netzwerk-/Socket-Fehler                  */
    KIWI_ERR_HANDSHAKE   = -3,  /**< WebSocket-Handshake abgelehnt            */
    KIWI_ERR_PROTO       = -4,  /**< Protokollverletzung                      */
    KIWI_ERR_BUSY        = -5,  /**< Server voll / IP schon verbunden         */
    KIWI_ERR_PASSWORD    = -6,  /**< Passwort falsch / Zugang verweigert      */
    KIWI_ERR_DOWN        = -7,  /**< Server meldet "down"                     */
    KIWI_ERR_REDIRECT    = -8,  /**< Server verlangt anderen Host              */
    KIWI_ERR_CLOSED      = -9,  /**< Server hat die Verbindung beendet        */
    KIWI_ERR_TIMEOUT     = -10, /**< zu lange keine Daten                     */
    KIWI_ERR_SINK        = -11, /**< Audio-Senke meldet Fehler                */
    KIWI_ERR_UNSUPPORTED = -12  /**< z. B. Stereo-/IQ-Modus                   */
} kiwi_err_t;

/** Klartext zu einem Fehlercode. */
const char *kiwi_strerror(int err);

/** Konfiguration. Die Zeiger muessen waehrend der Nutzung gueltig bleiben. */
typedef struct {
    const char *host;        /**< Hostname oder IPv4-Adresse                   */
    uint16_t    port;        /**< normalerweise 8073                           */
    const char *password;    /**< "" fuer oeffentliche Kiwis                   */
    const char *user_name;   /**< wird in der Nutzerliste des Kiwi angezeigt   */
    uint32_t    freq_hz;     /**< Startfrequenz in Hz (z. B. 225000 = 225 kHz) */
    const char *mode;        /**< "am", "amn", "usb", "lsb", "cw", "nbfm" ...  */
    int         compression; /**< 1 = ADPCM anfordern, 0 = rohes PCM (Standard)*/
    uint32_t    connect_timeout_ms; /**< 0 -> 10000                            */
    uint32_t    idle_timeout_ms;    /**< 0 -> 10000: Watchdog "keine Daten"    */
    const char *path_prefix; /**< NULL = "/ws/kiwi" (KiwiSDR v1.9xx); "" = alter Pfad "/<ts>/SND" */
    const audio_sink_t *sink;       /**< Ziel der PCM-Samples (darf NULL sein) */
} kiwi_config_t;

/** Laufende Statistik. */
typedef struct {
    uint32_t frames;       /**< verarbeitete SND-Frames                       */
    uint32_t samples;      /**< an die Senke gelieferte Samples               */
    uint32_t lost_frames;  /**< anhand der Sequenznummer erkannte Luecken     */
    uint32_t dropped_msgs; /**< zu grosse Nachrichten (z. B. load_cfg)        */
    int      rssi_tenths_dbm; /**< letzte Feldstaerke in 0,1 dBm              */
    uint32_t audio_rate;   /**< Abtastrate in Hz (meist 12000)                */
} kiwi_stats_t;

/** Zustand des Clients. Darf statisch angelegt werden (ca. 5,5 KB). */
typedef struct kiwi_client {
    kiwi_config_t cfg;
    kiwi_net_t   *net;
    kiwi_ws_rx_t  ws;
    kiwi_adpcm_t  adpcm;
    uint8_t       rxbuf[KIWI_RX_BUF_SIZE];
    uint8_t       tmp[1024];

    int      err;            /**< erster Fehler aus dem Nachrichten-Callback   */
    int      sink_open;
    int      setup_done;     /**< Abstimmung nach "sample_rate" gesendet?      */
    int      first_dropped;  /**< ersten SND-Frame bereits verworfen?          */
    int      have_seq;
    uint32_t expect_seq;
    uint32_t audio_rate;
    uint32_t sample_rate_mhz; /**< "sample_rate=" in Milli-Hz (z. B. 11998895) */
    uint32_t foff_hz;        /**< Frequenzversatz des Kiwi (Konverter), Hz     */
    uint32_t ver_maj, ver_min;
    uint32_t t_last_rx, t_last_ka;

    uint32_t cur_freq_hz;    /**< aktuell eingestellte Anzeigefrequenz         */
    char     cur_mode[8];

    /* Abstimmwunsch aus einem anderen Task (z. B. Konsole). Zuerst Felder
     * schreiben, dann Flag setzen; der Netzwerk-Task liest erst nach dem Flag. */
    uint32_t    pend_freq_hz;
    char        pend_mode[8];
    atomic_int  pend;

    kiwi_stats_t st;
} kiwi_client_t;

/** Initialisiert den Zustand (noch keine Verbindung). */
kiwi_err_t kiwi_client_init(kiwi_client_t *c, const kiwi_config_t *cfg);

/**
 * Verbindet (TCP + WebSocket + Anmeldung). Blockiert hoechstens ca.
 * connect_timeout_ms. Danach muss kiwi_client_poll() regelmaessig laufen.
 */
kiwi_err_t kiwi_client_connect(kiwi_client_t *c);

/**
 * Hauptschleife: wartet bis @p timeout_ms auf Daten, verarbeitet sie,
 * liefert Audio an die Senke, sendet Keepalives, setzt Abstimmwuensche um.
 * @return KIWI_OK = weiterlaufen; sonst Fehlercode -> kiwi_client_close().
 */
kiwi_err_t kiwi_client_poll(kiwi_client_t *c, uint32_t timeout_ms);

/**
 * Stimmt neu ab. Darf aus einem anderen Task aufgerufen werden.
 * @param mode  z. B. "am"; NULL = Modus beibehalten
 */
kiwi_err_t kiwi_client_set_tune(kiwi_client_t *c, uint32_t freq_hz,
                                const char *mode);

/** Beendet sauber (Close-Frame, TCP zu, Senke schliessen). Idempotent. */
void kiwi_client_close(kiwi_client_t *c);

/**
 * Einfacher Befehlsinterpreter fuer Konsole/stdin (auf PC und ESP32 identisch):
 *   "f 225"      Frequenz in kHz (auch "f 7055.5")
 *   "m am"       Modus
 *   "s"          Status
 *   "q"          Beenden
 * @return 0 = ok, 1 = Beenden gewuenscht, <0 = Fehler. Antworttext in @p reply.
 */
int kiwi_client_command(kiwi_client_t *c, const char *line, char *reply,
                        size_t reply_cap);

/** Wandelt "225" / "7055.5" (kHz) nach Hz ohne Gleitkomma. 0 = OK. */
int kiwi_parse_khz(const char *s, uint32_t *hz);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_CLIENT_H */
