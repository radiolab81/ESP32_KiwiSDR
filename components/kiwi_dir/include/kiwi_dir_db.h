/**
 * @file kiwi_dir_db.h
 * @brief Kompakte, flashfaehige Datenbank der oeffentlichen KiwiSDR-Server.
 *
 * LEHRBUCH-HINTERGRUND: Warum keine JSON-Datei und kein malloc?
 * -------------------------------------------------------------
 * Die Liste des Verzeichnisdienstes ist als Text/JSON mehrere hundert KB gross
 * (lange Antennenbeschreibungen, E-Mail-Adressen ...). Ein ESP32 hat nur ca.
 * 200 KB freien RAM. Daher wird der Text beim Empfang SOFORT zerlegt (Streaming,
 * siehe kiwi_dir_parse.h) und nur das Wichtige in einen Datensatz FESTER Groesse
 * (kiwi_dir_rec_t, 120 Byte) uebernommen:
 *
 *   - feste Satzgroesse   -> Zugriff auf Satz i per Rechnung: offset = i * 120
 *   - kein RAM-Abbild     -> gelesen wird direkt aus dem Flash (Satz fuer Satz)
 *   - Laender als Index   -> "Germany" steht nur EINMAL in einer Laendertabelle,
 *                            der Satz speichert 1 Byte (Filter = Byte-Vergleich)
 *
 * SPEICHERLAYOUT EINES SLOTS (siehe kiwi_dir_store.h fuer A/B):
 *
 *   0x0000  Header (28 Byte, belegt einen ganzen Sektor)   <- wird ZULETZT geschrieben
 *   0x1000  Laendertabelle (max. 255 * 24 Byte)
 *   0x3000  Datensaetze 0 .. count-1  (je 120 Byte)
 *
 * Bei 192 KB pro Slot passen so ueber 1500 Server (der Dienst listet heute
 * einige hundert). Ueberzaehlige Eintraege werden gezaehlt und verworfen.
 *
 * THREAD-SICHERHEIT: Lesefunktionen duerfen aus beliebigen Tasks laufen,
 * auch waehrend ein Update in den INAKTIVEN Slot schreibt. Nur ein Update zur
 * gleichen Zeit (siehe kiwi_dir_update.h).
 */
#ifndef KIWI_DIR_DB_H
#define KIWI_DIR_DB_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "kiwi_dir_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Fehlercodes (0 = OK, alles andere negativ) - gemeinsam fuer kiwi_dir_* */
/* ------------------------------------------------------------------ */
typedef enum {
    KIWI_DIR_OK          =  0,
    KIWI_DIR_ERR_ARG     = -1,  /**< ungueltiger Parameter                       */
    KIWI_DIR_ERR_NET     = -2,  /**< Netzwerkfehler / Verbindung abgebrochen     */
    KIWI_DIR_ERR_HTTP    = -3,  /**< Server antwortet nicht mit 200 OK           */
    KIWI_DIR_ERR_TLS     = -4,  /**< https:// angefordert - hier nicht moeglich  */
    KIWI_DIR_ERR_PARSE   = -5,  /**< Antwort enthaelt keine brauchbare Liste     */
    KIWI_DIR_ERR_STORE   = -6,  /**< Lese-/Schreib-/Loeschfehler im Speicher     */
    KIWI_DIR_ERR_TOO_BIG = -7,  /**< Antwort groesser als erlaubt                */
    KIWI_DIR_ERR_TIMEOUT = -8,  /**< Gesamtzeitlimit ueberschritten              */
    KIWI_DIR_ERR_EMPTY   = -9,  /**< keine gueltige Datenbank vorhanden          */
    KIWI_DIR_ERR_BUSY    = -10, /**< es laeuft bereits ein Update                */
    KIWI_DIR_ERR_RANGE   = -11, /**< Index ausserhalb der Datenbank              */
    KIWI_DIR_ERR_SUSPECT = -12  /**< neue Liste auffaellig klein -> verworfen    */
} kiwi_dir_err_t;

/** Klartext zu einem Fehlercode. */
const char *kiwi_dir_strerror(int err);

/* ------------------------------------------------------------------ */
/* Datensatz                                                          */
/* ------------------------------------------------------------------ */
#define KIWI_DIR_HOST_LEN      44   /**< Hostname/IPv4 inkl. Endnull            */
#define KIWI_DIR_NAME_LEN      32   /**< Anzeigename inkl. Endnull (UTF-8)      */
#define KIWI_DIR_PLACE_LEN     24   /**< Ortsname inkl. Endnull                 */
#define KIWI_DIR_COUNTRY_LEN   24   /**< Landesname inkl. Endnull               */
#define KIWI_DIR_MAX_COUNTRIES 255  /**< Land-IDs 1..255 (0 = unbekannt)        */
#define KIWI_DIR_NO_GEO        INT16_MIN  /**< "keine Koordinaten bekannt"      */

/** Bits in kiwi_dir_rec_t.flags */
#define KIWI_DIR_F_OFFLINE 0x01  /**< Server meldet sich als offline            */
#define KIWI_DIR_F_TLS     0x02  /**< Adresse ist https:// (Client kann kein TLS) */
#define KIWI_DIR_F_PRIVATE 0x04  /**< Server ist nicht oeffentlich (Passwort)    */
/** Standardmaessig ausgeblendet: nicht verbindbar. */
#define KIWI_DIR_F_UNUSABLE (KIWI_DIR_F_OFFLINE | KIWI_DIR_F_TLS | KIWI_DIR_F_PRIVATE)

/**
 * Ein Server. Feste Groesse, Felder nach Groesse sortiert (keine Fuellbytes).
 * Bewusst NICHT gespeichert: Antennenbeschreibung, SDR-Hardware, E-Mail ... -
 * sie sind lang und fuer die Verbindung unwichtig.
 */
typedef struct {
    char     host[KIWI_DIR_HOST_LEN]; /**< Hostname oder IPv4, NUL-terminiert   */
    uint16_t port;                    /**< TCP-Port (meist 8073)                */
    uint8_t  country;                 /**< ID in der Laendertabelle, 0 = ?      */
    uint8_t  flags;                   /**< KIWI_DIR_F_*                         */
    int16_t  lat_cdeg;                /**< Breite  in 0,01 Grad (N positiv)     */
    int16_t  lon_cdeg;                /**< Laenge  in 0,01 Grad (O positiv)     */
    uint8_t  users;                   /**< aktuell verbundene Nutzer            */
    uint8_t  users_max;               /**< Kanaele (0 = unbekannt)              */
    uint8_t  snr_all;                 /**< SNR 0..30 MHz in dB (0 = unbekannt)  */
    uint8_t  snr_hf;                  /**< SNR 1,8..30 MHz in dB (0 = unbekannt)*/
    uint32_t band_lo_hz;              /**< Empfangsbereich von ...              */
    uint32_t band_hi_hz;              /**< ... bis (0/0 = unbekannt)            */
    char     name[KIWI_DIR_NAME_LEN]; /**< Anzeigename                          */
    char     place[KIWI_DIR_PLACE_LEN]; /**< Ort ("Boulder, CO")                */
} kiwi_dir_rec_t;

_Static_assert(sizeof(kiwi_dir_rec_t) == 120, "kiwi_dir_rec_t: Layout geaendert -> KIWI_DIR_FMT_VERSION erhoehen");

/* ------------------------------------------------------------------ */
/* Speicherformat                                                     */
/* ------------------------------------------------------------------ */
#define KIWI_DIR_MAGIC         0x4449574Bu  /* "KWID" (little endian) */
#define KIWI_DIR_FMT_VERSION   1u
#define KIWI_DIR_OFF_HDR       0u
#define KIWI_DIR_OFF_COUNTRIES 0x1000u
#define KIWI_DIR_OFF_RECORDS   0x3000u

/** Kopf einer gueltigen Datenbank (steht am Anfang des Slots). */
typedef struct {
    uint32_t magic;          /**< KIWI_DIR_MAGIC                                  */
    uint16_t version;        /**< KIWI_DIR_FMT_VERSION                            */
    uint16_t rec_size;       /**< sizeof(kiwi_dir_rec_t)                          */
    uint32_t generation;     /**< wird je Update um 1 groesser; hoechste gewinnt  */
    uint32_t count;          /**< Anzahl Datensaetze                              */
    uint16_t country_count;  /**< Anzahl belegter Laendereintraege                */
    uint16_t country_len;    /**< KIWI_DIR_COUNTRY_LEN                            */
    uint32_t body_crc;       /**< CRC-32 ueber Datensaetze + Laendertabelle       */
    uint32_t hdr_crc;        /**< CRC-32 ueber die Felder davor                   */
} kiwi_dir_hdr_t;

/** CRC-32 (zlib-kompatibel). Aufrufe lassen sich verketten: crc = f(crc, ...). */
uint32_t kiwi_dir_crc32(uint32_t crc, const void *data, size_t n);

/* ------------------------------------------------------------------ */
/* Lesen                                                              */
/* ------------------------------------------------------------------ */

/** Handle auf die aktuell gueltige Datenbank. Kann statisch angelegt werden. */
typedef struct kiwi_dir_db {
    const kiwi_dir_store_t *st;
    kiwi_dir_hdr_t hdr[KIWI_DIR_SLOTS]; /**< zwischengespeicherte Header        */
    atomic_int     active;              /**< Slot 0/1, oder -1 = keine DB       */
} kiwi_dir_db_t;

/**
 * Sucht die gueltige Datenbank mit der hoechsten Generation und merkt sie sich.
 * Auch zum Neu-Einlesen nach einem Update zu benutzen.
 * @param verify 1 = zusaetzlich die Pruefsumme ueber den ganzen Inhalt pruefen
 *               (liest einmal den Slot, ca. 100 ms) - beim Start sinnvoll.
 * @return KIWI_DIR_OK, oder KIWI_DIR_ERR_EMPTY, wenn keine gueltige DB existiert
 *         (das ist beim allerersten Start normal; die DB ist dann leer, aber
 *         das Handle bleibt benutzbar).
 */
int kiwi_dir_db_open(kiwi_dir_db_t *db, const kiwi_dir_store_t *st, int verify);

/** 1, wenn eine gueltige Datenbank geladen ist. */
int kiwi_dir_db_valid(const kiwi_dir_db_t *db);
/** Anzahl der Server (0, wenn leer). */
uint32_t kiwi_dir_db_count(const kiwi_dir_db_t *db);
/** Generation (Update-Zaehler, 0, wenn leer). */
uint32_t kiwi_dir_db_generation(const kiwi_dir_db_t *db);
/** Slot (0/1) der aktuellen DB, -1 wenn leer. */
int kiwi_dir_db_active_slot(const kiwi_dir_db_t *db);
/** Maximale Anzahl Datensaetze, die ein Slot dieses Stores fasst. */
uint32_t kiwi_dir_capacity(const kiwi_dir_store_t *st);

/** Liest Datensatz @p idx (0-basiert). */
int kiwi_dir_db_get(const kiwi_dir_db_t *db, uint32_t idx, kiwi_dir_rec_t *out);

/** Anzahl belegter Laendereintraege (IDs 1..n). */
unsigned kiwi_dir_db_country_count(const kiwi_dir_db_t *db);
/** Kopiert den Namen von Land @p id (1..n). */
int kiwi_dir_db_country_name(const kiwi_dir_db_t *db, unsigned id, char out[KIWI_DIR_COUNTRY_LEN]);
/** Sucht ein Land (ohne Beachtung der Gross-/Kleinschreibung, exakter Name). */
int kiwi_dir_db_country_find(const kiwi_dir_db_t *db, const char *name, uint8_t *id);
/**
 * Zaehlt Server je Land. @p counts muss 256 Eintraege haben; Index = Land-ID,
 * Index 0 = ohne Land. Es zaehlen nur nutzbare Server (kein offline/https/privat).
 */
int kiwi_dir_db_count_by_country(const kiwi_dir_db_t *db, uint16_t counts[256]);

/** Sucht Server nach Host und Port. @return KIWI_DIR_OK und Index, sonst KIWI_DIR_ERR_RANGE. */
int kiwi_dir_db_lookup(const kiwi_dir_db_t *db, const char *host, uint16_t port, uint32_t *idx);

/* ------------------------------------------------------------------ */
/* Filtern                                                            */
/* ------------------------------------------------------------------ */

/** Suchfilter. Mit kiwi_dir_query_init() vorbelegen, dann Felder anpassen. */
typedef struct {
    uint8_t  country;            /**< 0 = egal, sonst Land-ID                     */
    uint8_t  flags_excl;         /**< Server mit einem dieser Flags ausblenden    */
    uint8_t  only_free;          /**< 1 = nur Server mit freiem Kanal             */
    uint8_t  min_snr;            /**< 0 = egal, sonst Mindest-SNR in dB           */
    uint32_t freq_hz;            /**< 0 = egal, sonst muss der Bereich dies abdecken */
    char     text[24];           /**< "" = egal, sonst Teilstring in Name/Ort/Host/Land */
} kiwi_dir_query_t;

/** Standardfilter: alles ausser nicht verbindbaren Servern (KIWI_DIR_F_UNUSABLE). */
void kiwi_dir_query_init(kiwi_dir_query_t *q);

/** Prueft einen Datensatz gegen den Filter (1 = passt). */
int kiwi_dir_query_match(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, const kiwi_dir_rec_t *r);

/**
 * Liefert den naechsten passenden Server ab *cursor.
 * Typische Schleife:   uint32_t cur = 0;  while (kiwi_dir_db_find(db,&q,&cur,&r,&i) == 1) { ... }
 * @return 1 = gefunden (*cursor steht danach HINTER dem Treffer), 0 = Ende, <0 = Fehler
 */
int kiwi_dir_db_find(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, uint32_t *cursor,
                     kiwi_dir_rec_t *out, uint32_t *idx);

/** Zaehlt die Treffer eines Filters. */
uint32_t kiwi_dir_db_count_matches(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q);

/**
 * Bester Treffer: hoechster SNR, bei Gleichstand die wenigsten Nutzer.
 * @return 1 = gefunden, 0 = kein Treffer, <0 = Fehler
 */
int kiwi_dir_db_best(const kiwi_dir_db_t *db, const kiwi_dir_query_t *q, kiwi_dir_rec_t *out, uint32_t *idx);

/** Der fuer die Rangfolge benutzte SNR (HF-Wert, sonst Gesamtwert). */
uint8_t kiwi_dir_rec_snr(const kiwi_dir_rec_t *r);

/* ------------------------------------------------------------------ */
/* Schreiben (nur fuer kiwi_dir_update.c; in Tests auch direkt nutzbar) */
/* ------------------------------------------------------------------ */

typedef struct kiwi_dir_writer {
    const kiwi_dir_store_t *st;
    int      slot;
    uint32_t generation, count, capacity, crc, overflow;
    uint16_t ncountry;
    char     country[KIWI_DIR_MAX_COUNTRIES][KIWI_DIR_COUNTRY_LEN]; /**< ~6 KB RAM waehrend des Updates */
} kiwi_dir_writer_t;

/** Loescht @p slot und bereitet das Schreiben vor. Der andere Slot bleibt unberuehrt. */
int kiwi_dir_writer_begin(kiwi_dir_writer_t *w, const kiwi_dir_store_t *st, int slot, uint32_t generation);
/** Liefert die ID eines Landes (legt es bei Bedarf an). 0 = leer/Tabelle voll. */
uint8_t kiwi_dir_writer_country(kiwi_dir_writer_t *w, const char *name);
/** Haengt einen Datensatz an. @return 0 = ok, 1 = Platz voll (verworfen, gezaehlt), <0 Fehler. */
int kiwi_dir_writer_add(kiwi_dir_writer_t *w, const kiwi_dir_rec_t *rec);
/** Schreibt Laendertabelle und Header = COMMIT. Erst danach ist der Slot gueltig. */
int kiwi_dir_writer_finish(kiwi_dir_writer_t *w);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_DB_H */
