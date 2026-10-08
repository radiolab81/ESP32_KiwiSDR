/**
 * @file kiwi_ws.h
 * @brief Minimaler WebSocket-Client (RFC 6455), frei von I/O und Betriebssystem.
 *
 * LEHRBUCH-HINTERGRUND: Was ist ein WebSocket?
 * --------------------------------------------
 * Ein WebSocket beginnt als gewoehnliche HTTP-Anfrage ("GET ... Upgrade:
 * websocket"). Antwortet der Server mit "101 Switching Protocols", wird die
 * TCP-Verbindung zu einem bidirektionalen Strom von *Frames*:
 *
 *      Byte 0:  FIN(1) RSV(3) OPCODE(4)
 *      Byte 1:  MASK(1) LEN7(7)
 *      [2 Byte LEN16, falls LEN7 == 126]
 *      [8 Byte LEN64, falls LEN7 == 127]
 *      [4 Byte Maskierschluessel, falls MASK == 1]
 *      Nutzdaten (bei MASK == 1: jedes Byte i XOR key[i % 4])
 *
 * Regeln fuer Clients: Frames Richtung Server MUESSEN maskiert sein, Frames
 * vom Server sind NICHT maskiert.
 *
 * Diese Datei enthaelt nur reine Datenverarbeitung (Bytes rein, Bytes raus).
 * Das Lesen/Schreiben des Sockets macht der Aufrufer. Dadurch ist der Code
 * trivial auf PC testbar (siehe pc/test_core.c).
 */
#ifndef KIWI_WS_H
#define KIWI_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (RFC 6455, Abschnitt 5.2) */
#define KIWI_WS_OP_CONT   0x0
#define KIWI_WS_OP_TEXT   0x1
#define KIWI_WS_OP_BINARY 0x2
#define KIWI_WS_OP_CLOSE  0x8
#define KIWI_WS_OP_PING   0x9
#define KIWI_WS_OP_PONG   0xA

/** Maximale Nutzlast eines Steuer-Frames (Close/Ping/Pong) laut RFC. */
#define KIWI_WS_MAX_CTL 125

/** Groesse eines Base64-kodierten 16-Byte-Schluessels inkl. Endnull. */
#define KIWI_WS_KEY_B64_LEN 25

/* ------------------------------------------------------------------ */
/* Sendeseite                                                         */
/* ------------------------------------------------------------------ */

/**
 * Baut die HTTP-Upgrade-Anfrage ("Opening Handshake").
 * @param origin  Wert des Origin-Headers (z. B. "http://host:8073") oder NULL
 * @param key  16 Zufallsbytes (werden Base64-kodiert als Sec-WebSocket-Key)
 * @return Laenge des Textes oder 0, falls @p cap zu klein ist.
 */
size_t kiwi_ws_build_request(char *out, size_t cap, const char *host,
                             uint16_t port, const char *path,
                             const char *origin, const uint8_t key[16]);

/**
 * Liest den HTTP-Statuscode aus der Serverantwort ("HTTP/1.1 101 ...").
 * @return Statuscode (z. B. 101) oder -1 bei unlesbarer Antwort.
 */
int kiwi_ws_parse_status(const char *resp, size_t len);

/**
 * Kodiert einen *maskierten* Client-Frame (FIN=1, keine Fragmentierung).
 * @param mask  4 Maskierbytes (Zufall)
 * @return Gesamtlaenge des Frames oder 0, falls @p cap zu klein ist.
 */
size_t kiwi_ws_encode_frame(uint8_t *out, size_t cap, uint8_t opcode,
                            const uint8_t *payload, size_t n,
                            const uint8_t mask[4]);

/** Base64-Kodierung (Hilfsfunktion; schreibt Endnull). Liefert Zeichenzahl. */
size_t kiwi_ws_base64(const uint8_t *in, size_t n, char *out);

/* ------------------------------------------------------------------ */
/* Empfangsseite: inkrementeller Frame-Parser                         */
/* ------------------------------------------------------------------ */

/**
 * Callback fuer jede vollstaendig empfangene Nachricht.
 * Bei Steuer-Frames ist @p opcode 0x8/0x9/0xA. Daten-Nachrichten (Text/
 * Binaer) werden bei Fragmentierung vor dem Aufruf zusammengesetzt.
 * @return 0 = weitermachen, != 0 = Verarbeitung abbrechen (Wert wird
 *         von kiwi_ws_rx_push() zurueckgegeben).
 */
typedef int (*kiwi_ws_msg_cb)(void *user, uint8_t opcode,
                              const uint8_t *payload, size_t len);

/** Zustand des Frame-Parsers. Felder nicht direkt benutzen. */
typedef struct {
    uint8_t *buf;        /**< Puffer fuer eine Daten-Nachricht (vom Aufrufer). */
    size_t   cap;        /**< Kapazitaet von buf.                              */
    uint8_t  in_payload; /**< 0 = lese Header, 1 = lese Nutzdaten.             */
    uint8_t  hdr[14];    /**< Header-Bytes (max. 2 + 8 + 4).                   */
    uint8_t  hdr_have, hdr_need;
    uint8_t  opcode, fin, masked, mask[4];
    uint64_t plen, pgot; /**< Frame-Laenge / bisher gelesene Nutzbytes.        */
    uint8_t  msg_opcode; /**< Opcode der gerade zusammengesetzten Nachricht.   */
    size_t   msg_len;
    uint8_t  overflow;   /**< Nachricht groesser als buf -> wird verworfen.    */
    uint8_t  ctl[KIWI_WS_MAX_CTL];
    size_t   ctl_len;
    uint32_t dropped;    /**< Zaehler: wegen Platzmangel verworfene Nachrichten */
} kiwi_ws_rx_t;

/** Initialisiert den Parser; @p buf muss waehrend der Nutzung gueltig bleiben. */
void kiwi_ws_rx_init(kiwi_ws_rx_t *rx, uint8_t *buf, size_t cap);

/**
 * Fuettert empfangene Rohbytes in den Parser. Beliebige Stueckelung erlaubt
 * (1 Byte bis mehrere Frames pro Aufruf) - TCP garantiert keine Grenzen!
 * @return 0 = ok, <0 = Protokollfehler, >0 = vom Callback verlangter Abbruch.
 */
int kiwi_ws_rx_push(kiwi_ws_rx_t *rx, const uint8_t *data, size_t n,
                    kiwi_ws_msg_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_WS_H */
