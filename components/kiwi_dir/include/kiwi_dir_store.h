/**
 * @file kiwi_dir_store.h
 * @brief Speicher-Abstraktion fuer die Server-Datenbank ("Store").
 *
 * LEHRBUCH-HINTERGRUND: A/B-Slots (Doppelpufferung im Flash)
 * ----------------------------------------------------------
 * Flash laesst sich nur sektorweise LOESCHEN (4 KB) und danach einmalig
 * beschreiben (Bits gehen nur von 1 auf 0). Eine Datenbank "an Ort und Stelle"
 * zu aktualisieren waere gefaehrlich: Faellt waehrend des Updates der Strom
 * aus oder bricht das WLAN ab, waere die alte Liste weg und die neue unvollstaendig.
 *
 * Deshalb gibt es ZWEI gleich grosse Bereiche ("Slots"):
 *    - Slot A enthaelt die gueltige, aktuelle Datenbank (wird nur gelesen),
 *    - das Update wird komplett in Slot B geschrieben,
 *    - erst ganz zum Schluss wird in B ein Kopf (Header) mit Pruefsumme
 *      geschrieben. Das ist der "Commit": Ab jetzt ist B gueltig und neuer.
 * Bricht das Update vorher ab, fehlt in B der Header -> B ist ungueltig, A bleibt
 * unveraendert in Benutzung. Beim naechsten Update ist dann B wieder dran.
 * (Zusatznutzen: Flash-Abnutzung verteilt sich auf beide Slots.)
 *
 * Wie beim Audio-"Sink" und der Netz-"Port" ist der Store eine Struktur aus
 * Funktionszeigern: Auf dem ESP32 liegt er in einer Flash-Partition
 * (main/dir_store_flash.c), auf dem PC in einer Datei (pc/store_file.c), im
 * Test im RAM. Die Datenbanklogik selbst kennt keine Hardware.
 */
#ifndef KIWI_DIR_STORE_H
#define KIWI_DIR_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Anzahl der Slots (A/B). Fest, damit die Logik einfach bleibt. */
#define KIWI_DIR_SLOTS 2

typedef struct kiwi_dir_store {
    /** Groesse EINES Slots in Bytes (Vielfaches von erase_size). */
    size_t slot_size;
    /** Loeschgranularitaet (ESP32-Flash: 4096). */
    size_t erase_size;

    /** Liest @p len Bytes ab Offset @p off im Slot. 0 = OK. */
    int (*read)(void *ctx, int slot, size_t off, void *buf, size_t len);
    /**
     * Loescht den GANZEN Slot (alle Bits auf 1). Darf mehrere Sekunden dauern;
     * die Flash-Variante gibt zwischen den Sektoren die CPU frei, damit das
     * Audio nicht aussetzt. 0 = OK.
     */
    int (*erase)(void *ctx, int slot);
    /**
     * Schreibt in einen zuvor geloeschten Bereich. Jede Stelle darf nach dem
     * Loeschen nur EINMAL beschrieben werden. 0 = OK.
     */
    int (*write)(void *ctx, int slot, size_t off, const void *buf, size_t len);

    void *ctx; /**< Beliebige Nutzdaten des Stores. */
} kiwi_dir_store_t;

#ifdef __cplusplus
}
#endif
#endif /* KIWI_DIR_STORE_H */
