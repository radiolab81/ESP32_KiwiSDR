/**
 * @file kiwi_dir_http.h
 * @brief Minimaler HTTP/1.1-GET-Client (nur http://) mit Streaming-Ausgabe.
 *
 * Warum kein esp_http_client? Dieser Client setzt - wie der Rest des Projekts -
 * ausschliesslich auf die Portschicht kiwi_port.h auf und laeuft dadurch
 * UNVERAENDERT auf PC und ESP32 (und damit testbar ohne Hardware). Er kann nur,
 * was fuer die Serverliste noetig ist:
 *   - GET mit "Connection: close", Hostname/Port aus der URL
 *   - Umleitungen 301/302/303/307/308 auf http:// (max. 3)
 *   - Antwortkoerper mit Content-Length, "Transfer-Encoding: chunked" oder bis Verbindungsende
 *   - Zeitlimit insgesamt, Groessenlimit, Fortschritt Stueck fuer Stueck an einen Callback
 * Nicht moeglich: TLS (https:// -> KIWI_DIR_ERR_TLS), Kompression (wird nicht angefordert).
 */
#ifndef KIWI_DIR_HTTP_H
#define KIWI_DIR_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Empfaenger fuer den Antwortkoerper. Rueckgabe != 0 bricht ab (wird als Fehler gemeldet). */
typedef int (*kiwi_dir_http_body_cb)(void *user, const uint8_t *data, size_t n);

typedef struct {
    const char *url;          /**< "http://host[:port]/pfad"                      */
    uint32_t    timeout_ms;   /**< Gesamtzeitlimit, 0 -> 120000                   */
    uint32_t    max_body;     /**< Hoechstgroesse des Koerpers, 0 -> 4 MB         */
    const char *user_agent;   /**< NULL -> "kiwi_esp32"                           */
} kiwi_dir_http_cfg_t;

typedef struct {
    int      status;          /**< letzter HTTP-Statuscode                        */
    uint32_t body_bytes;      /**< empfangene Koerperbytes                        */
    uint8_t  redirects;       /**< befolgte Umleitungen                           */
} kiwi_dir_http_info_t;

/**
 * Holt eine URL und reicht den Koerper an @p cb weiter.
 * @return KIWI_DIR_OK, sonst KIWI_DIR_ERR_NET/HTTP/TLS/TIMEOUT/TOO_BIG/ARG
 *         (oder KIWI_DIR_ERR_PARSE, wenn der Callback abbricht)
 */
int kiwi_dir_http_get(const kiwi_dir_http_cfg_t *cfg, kiwi_dir_http_body_cb cb, void *user,
                      kiwi_dir_http_info_t *info);

/* ---- Hilfsfunktionen, einzeln testbar ---- */

typedef struct {
    char     host[64];
    char     path[128];       /**< beginnt immer mit "/" */
    uint16_t port;
    uint8_t  tls;             /**< 1 = https:// */
} kiwi_dir_url_t;

/** Zerlegt eine URL. 0 = OK, <0 = unbrauchbar/zu lang. */
int kiwi_dir_url_split(const char *url, kiwi_dir_url_t *out);

/**
 * Decoder fuer "Transfer-Encoding: chunked": entfernt die Laengenzeilen und
 * liefert nur die Nutzdaten. Beliebig gestueckelte Eingabe erlaubt.
 */
typedef struct {
    uint8_t  state;
    uint32_t left;            /**< noch fehlende Bytes im aktuellen Chunk */
    uint8_t  digits;
} kiwi_dir_chunk_t;

void kiwi_dir_chunk_init(kiwi_dir_chunk_t *c);
/**
 * @return 0 = mehr Daten noetig, 1 = letzter Chunk gesehen (fertig),
 *         <0 = Formatfehler oder Callback hat abgebrochen
 */
int kiwi_dir_chunk_push(kiwi_dir_chunk_t *c, const uint8_t *data, size_t n,
                        kiwi_dir_http_body_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_HTTP_H */
