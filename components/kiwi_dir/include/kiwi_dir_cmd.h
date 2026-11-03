/**
 * @file kiwi_dir_cmd.h
 * @brief Textbefehle "d ..." fuer Konsole/stdin (auf PC und ESP32 identisch).
 *
 * Ergaenzt kiwi_client_command() (f, m, s, q) um die Serverauswahl. Alle Befehle
 * beginnen mit "d" (wie "directory"), damit sie nicht mit den bestehenden
 * Ein-Buchstaben-Befehlen kollidieren:
 *
 *   d                       Hilfe
 *   d update [force]        Serverliste neu laden (u). "force" ignoriert den Mindestabstand
 *   d info                  Datenbank-Info und aktueller Filter (i)
 *   d countries             Laender mit Anzahl nutzbarer Server (c)
 *   d filter <Land|Text>    Filter setzen (f): Landesname ("Germany", auch Anfang "Ger"),
 *                           sonst Teilstring in Name/Ort/Host. Ohne Argument: Filter loeschen
 *   d free [on|off]         nur Server mit freiem Kanal
 *   d snr <dB>              Mindest-SNR (0 = aus)
 *   d freq <kHz|off>        nur Server, deren Empfangsbereich diese Frequenz abdeckt
 *   d list [Seite]          Treffer des Filters anzeigen (l), 10 pro Seite
 *   d go <Nr|best|host[:port]>   zu diesem Server wechseln (g); "best" = bester Treffer
 *
 * Die Befehle selbst kennen weder Netz noch Flash noch FreeRTOS: Ausgabe, Update
 * und Serverwechsel erledigen Callbacks der Anwendung (app_dir.c bzw. pc/kiwi_dirtool.c).
 */
#ifndef KIWI_DIR_CMD_H
#define KIWI_DIR_CMD_H

#include "kiwi_dir_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kiwi_dir_cmd {
    kiwi_dir_db_t *db;
    /** Gibt eine Textzeile aus (ohne Zeilenende). */
    void (*out)(void *user, const char *line);
    /** Startet ein Update (darf asynchron laufen). 0 = gestartet, sonst Fehlercode; Meldungen macht der Callback. */
    int (*update)(void *user, int force);
    /** Wechselt den Server (host/port in rec; die DB-Daten sind nur Zusatz). 0 = ok. */
    int (*select)(void *user, const kiwi_dir_rec_t *rec);
    void *user;
    kiwi_dir_query_t q;           /**< aktueller Filter (wird von der Anwendung fuer Failover mitgenutzt) */
} kiwi_dir_cmd_t;

void kiwi_dir_cmd_init(kiwi_dir_cmd_t *c, kiwi_dir_db_t *db,
                       void (*out)(void *, const char *),
                       int (*update)(void *, int), int (*select)(void *, const kiwi_dir_rec_t *), void *user);

/**
 * Fuehrt eine Zeile aus, wenn sie mit "d" als eigenem Wort beginnt.
 * @return 1 = war ein d-Befehl (behandelt), 0 = nicht unser Befehl (Zeile unveraendert weiterreichen)
 */
int kiwi_dir_command(kiwi_dir_cmd_t *c, const char *line);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_CMD_H */
