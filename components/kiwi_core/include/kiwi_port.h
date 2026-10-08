/**
 * @file kiwi_port.h
 * @brief Portabilitaetsschicht ("Port") zwischen Protokoll-Kern und Betriebssystem.
 *
 * LEHRBUCH-HINTERGRUND
 * --------------------
 * Gute portable Software trennt *was* gemacht wird (Protokoll) von *womit* es
 * gemacht wird (Betriebssystem, Netzwerk-Stack). Der Protokoll-Kern
 * (kiwi_ws.c, kiwi_client.c, kiwi_adpcm.c) ruft ausschliesslich die wenigen
 * Funktionen dieser Datei auf. Wer das Projekt auf ein neues System (z. B.
 * Windows/Winsock, Zephyr, FreeRTOS+TCP) portieren will, muss nur diese
 * Funktionen neu implementieren.
 *
 * Unsere Referenzimplementierung (port_sockets.c) nutzt die BSD-Socket-API.
 * Diese gibt es sowohl unter Linux/macOS als auch im ESP-IDF (lwIP), daher
 * laeuft EXAKT DIESELBE Datei auf PC und ESP32.
 */
#ifndef KIWI_PORT_H
#define KIWI_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Rueckgabewerte von kiwi_net_recv() (Werte > 0 = Anzahl gelesener Bytes). */
#define KIWI_NET_TIMEOUT   0   /**< Innerhalb der Frist kamen keine Daten.      */
#define KIWI_NET_ERR      (-1) /**< Socket-Fehler.                              */
#define KIWI_NET_CLOSED   (-2) /**< Gegenstelle hat die Verbindung beendet.     */

/** Undurchsichtiger Verbindungs-Handle (Inhalt ist Sache des Ports). */
typedef struct kiwi_net kiwi_net_t;

/**
 * Baut eine TCP-Verbindung auf (DNS-Aufloesung + connect), mit Zeitlimit.
 * @return 0 bei Erfolg, sonst negativ.
 */
int kiwi_net_connect(kiwi_net_t **out, const char *host, uint16_t port,
                     uint32_t timeout_ms);

/** Sendet genau @p len Bytes (blockiert bis fertig oder Zeitlimit). 0 = OK. */
int kiwi_net_send_all(kiwi_net_t *n, const uint8_t *data, size_t len,
                      uint32_t timeout_ms);

/** Liest bis zu @p cap Bytes; wartet hoechstens @p timeout_ms. */
int kiwi_net_recv(kiwi_net_t *n, uint8_t *buf, size_t cap, uint32_t timeout_ms);

/** Schliesst den Socket und gibt den Handle frei (NULL-sicher). */
void kiwi_net_close(kiwi_net_t *n);

/** Monotone Zeit in Millisekunden (darf ueberlaufen, nur Differenzen nutzen). */
uint32_t kiwi_time_ms(void);

/** Zufallszahl (fuer WebSocket-Schluessel/Masken; Kryptoqualitaet nicht noetig). */
uint32_t kiwi_random_u32(void);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_PORT_H */
