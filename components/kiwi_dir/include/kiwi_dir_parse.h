/**
 * @file kiwi_dir_parse.h
 * @brief Streaming-Parser fuer die Serverliste (kiwisdr_com.js).
 *
 * FORMAT DER QUELLE (so gesehen am 2. 11. 2026, ca. 890 KB, 836 Server)
 * ---------------------------------------------------------------------
 *     // KiwiSDR.com receiver list for dyatlov map maker      <- Kommentarzeilen
 *     var kiwisdr_com =                                         <- JavaScript, kein reines JSON
 *     [
 *         {
 *             "status":"active", "offline":"no",
 *             "name":"0-30 MHz SDR | Dortmund, Germany",
 *             "bands":"0-30000000", "users":"0", "users_max":"4",
 *             "gps":"(-34.273700, 138.771000)",                 <- Koordinaten (Breite, Laenge)
 *             "loc":"Near Tarlee, South Australia",             <- FREITEXT, kein Koordinatenfeld!
 *             "antenna":"...", "snr":"40,41",                   <- SNR 0-30 MHz, 1,8-30 MHz
 *             "url":"http://host:8075"                          <- Port kann fehlen (z. B. Proxy-Server): dann 8073
 *             ... ca. 35 weitere Felder, alle Werte sind Strings ...
 *         },                                                    <- Komma vor "]" ist erlaubt
 *     ]
 *     ;
 *
 * LEHRBUCH-HINTERGRUND: Warum ein Zustandsautomat statt JSON-Bibliothek?
 * ----------------------------------------------------------------------
 * Eine JSON-Bibliothek baut den ganzen Baum im RAM auf (hier >1 MB). Dieser
 * Parser dagegen verarbeitet JEDES BYTE EINZELN beim Eintreffen aus dem Netz,
 * merkt sich nur die ~10 benoetigten Felder des gerade gelesenen Objekts und
 * liefert beim "}" einen fertigen, kompakten Datensatz. RAM-Bedarf konstant
 * (~2,6 KB), egal wie lang die Liste ist; Datenhaeppchen duerfen an JEDER
 * beliebigen Stelle enden (TCP garantiert keine Grenzen).
 *
 * Er ist absichtlich TOLERANT: kein exaktes JSON noetig, unbekannte Felder
 * (auch verschachtelte) werden uebersprungen, ueberlange Werte gekuerzt,
 * Trailing-Commas und Kommentare vor der Liste stoeren nicht.
 */
#ifndef KIWI_DIR_PARSE_H
#define KIWI_DIR_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "kiwi_dir_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ein fertig zerlegter Server. */
typedef struct {
    kiwi_dir_rec_t rec;                     /**< rec.country ist hier noch 0 ...    */
    char country[KIWI_DIR_COUNTRY_LEN];     /**< ... der Landesname steht hier (leer = unbekannt) */
} kiwi_dir_item_t;

/** Wird je gueltigem Server aufgerufen. Rueckgabe != 0 bricht das Parsen ab. */
typedef int (*kiwi_dir_item_cb)(void *user, const kiwi_dir_item_t *item);

/** Zaehler (zur Anzeige/Diagnose). */
typedef struct {
    uint32_t objects;      /**< gesehene { } Objekte                              */
    uint32_t accepted;     /**< an den Callback gelieferte Server                 */
    uint32_t skipped;      /**< verworfen: keine/ungueltige URL, Host zu lang, IPv6 ... */
} kiwi_dir_parse_stats_t;

typedef struct kiwi_dir_parser kiwi_dir_parser_t;

/** Interner Zustand. Felder nicht direkt benutzen (Groesse ca. 2,8 KB). */
#define KIWI_DIR_VAL_W 240
struct kiwi_dir_parser {
    kiwi_dir_item_cb cb;
    void *user;
    kiwi_dir_parse_stats_t stats;
    uint8_t  state, esc, hexn, in_key, nest_str, nest_esc, keylen, closed;
    int8_t   field;
    uint16_t uni, hi, vlen, nest;
    char     key[24];
    char     val[10][KIWI_DIR_VAL_W];
};

void kiwi_dir_parser_init(kiwi_dir_parser_t *p, kiwi_dir_item_cb cb, void *user);

/**
 * Fuettert Bytes ein (beliebig gestueckelt).
 * @return 0 = ok, >0 = Callback hat Abbruch verlangt (dessen Wert)
 */
int kiwi_dir_parser_push(kiwi_dir_parser_t *p, const uint8_t *data, size_t n);

/** 1, wenn das schliessende "]" der Liste gesehen wurde (Liste vollstaendig). */
int kiwi_dir_parser_finished(const kiwi_dir_parser_t *p);

/* ---- Hilfsfunktionen, einzeln testbar ---- */

/** Zerlegt "http://host:port/..." (fehlt der Port, gilt der KiwiSDR-Standardport 8073). 0 = OK, <0 = unbrauchbar. */
int kiwi_dir_parse_hostport(const char *url, char host[KIWI_DIR_HOST_LEN], uint16_t *port, uint8_t *flags);
/** "(-34.27, 138.77)" -> Zehntel... in 0,01 Grad. 0 = OK, <0 = kein Koordinatenpaar. */
int kiwi_dir_parse_gps(const char *s, int16_t *lat_cdeg, int16_t *lon_cdeg);
/** Kopiert UTF-8 gekuerzt auf dstcap-1 Bytes, ohne ein Zeichen zu zerschneiden. */
void kiwi_dir_copy_utf8(char *dst, size_t dstcap, const char *src);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_PARSE_H */
