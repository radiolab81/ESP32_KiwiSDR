/**
 * @file kiwi_dir_update.h
 * @brief Datenbank aktualisieren: Liste laden -> parsen -> in den freien Slot schreiben.
 *
 * ABLAUF (alles im Strom, nichts wird zwischengespeichert)
 *
 *   Netz --> kiwi_dir_http --> kiwi_dir_parse --> kiwi_dir_writer --> Flash (inaktiver Slot)
 *                                                                 \--> am Ende: Commit + db neu oeffnen
 *
 * SICHERHEIT / "BEST PRACTICE"
 *   - Der inaktive Slot wird erst geloescht, wenn der erste gueltige Server
 *     eintrifft: schlaegt schon der Download fehl (kein WLAN, DNS ...), bleibt
 *     der Flash unberuehrt (Schonung der Schreibzyklen).
 *   - Die alte Datenbank bleibt bis zum Commit in Benutzung. Jeder Abbruch
 *     (Fehler, Stromausfall) hinterlaesst sie unveraendert.
 *   - Plausibilitaetspruefung: eine Liste, die weniger als ein Viertel der alten
 *     Eintraege hat, wird verworfen (KIWI_DIR_ERR_SUSPECT) - z. B. wenn der Dienst
 *     eine Fehlerseite oder eine abgeschnittene Datei liefert.
 *   - Nur EIN Update gleichzeitig (KIWI_DIR_ERR_BUSY).
 *   - Der Aufrufer sollte Updates nicht oft ausloesen (siehe app_dir.c: Mindestabstand);
 *     Flash vertraegt ca. 100.000 Loeschzyklen, der Dienst moechte nicht gehaemmert werden.
 *
 * SPEICHER: kiwi_dir_update_t ist ca. 9 KB gross -> NICHT auf einen kleinen Stack legen
 * (statisch oder per malloc fuer die Dauer des Updates).
 */
#ifndef KIWI_DIR_UPDATE_H
#define KIWI_DIR_UPDATE_H

#include "kiwi_dir_db.h"
#include "kiwi_dir_http.h"
#include "kiwi_dir_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t entries;      /**< gespeicherte Server                                */
    uint32_t skipped;      /**< verworfen (keine brauchbare Adresse)               */
    uint32_t overflow;     /**< verworfen, weil der Slot voll war                  */
    uint32_t countries;    /**< verschiedene Laender                               */
    uint32_t bytes;        /**< heruntergeladene Bytes                             */
    uint32_t generation;   /**< neue Generation der Datenbank                      */
    uint32_t old_entries;  /**< Anzahl vorher                                      */
} kiwi_dir_result_t;

typedef struct kiwi_dir_update {
    kiwi_dir_parser_t parser;
    kiwi_dir_writer_t writer;
    const kiwi_dir_store_t *st;
    kiwi_dir_db_t *db;
    int      writer_open, err, locked;
    uint32_t bytes;
} kiwi_dir_update_t;

/** Fortschrittsmeldung waehrend des Downloads (optional, darf NULL sein). */
typedef void (*kiwi_dir_progress_cb)(void *user, uint32_t bytes, uint32_t servers);

/** Startet ein Update (Zustand zuruecksetzen, Sperre nehmen). KIWI_DIR_ERR_BUSY, wenn schon eines laeuft. */
int kiwi_dir_update_begin(kiwi_dir_update_t *u, const kiwi_dir_store_t *st, kiwi_dir_db_t *db);
/** Fuettert Rohbytes der Liste ein (beliebig gestueckelt). */
int kiwi_dir_update_push(kiwi_dir_update_t *u, const uint8_t *data, size_t n);
/** Beendet: prueft, schreibt den Commit, oeffnet die DB neu. Gibt in jedem Fall die Sperre frei. */
int kiwi_dir_update_end(kiwi_dir_update_t *u, kiwi_dir_result_t *res);
/** Bricht ab (alte DB bleibt). Gibt die Sperre frei; mehrfach aufrufbar. */
void kiwi_dir_update_abort(kiwi_dir_update_t *u);

/**
 * Komplettes Update ueber HTTP: begin -> GET -> end.
 * @param progress  optional; wird ca. alle 32 KB aufgerufen
 */
int kiwi_dir_update_http(kiwi_dir_update_t *u, const kiwi_dir_store_t *st, kiwi_dir_db_t *db,
                         const char *url, kiwi_dir_progress_cb progress, void *user,
                         kiwi_dir_result_t *res);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_UPDATE_H */
