/**
 * @file store_file.h
 * @brief PC-Store fuer die Serverdatenbank: eine Datei mit zwei Slots (emuliert den Flash).
 */
#ifndef STORE_FILE_H
#define STORE_FILE_H

#include "kiwi_dir_store.h"

/** Oeffnet/legt die Datei an (Groesse 2 * slot_size, neu = 0xFF). NULL bei Fehler. */
kiwi_dir_store_t *store_file_open(const char *path, size_t slot_size);
void store_file_close(kiwi_dir_store_t *st);

/** Standard-Slotgroesse: wie die ESP32-Partition (192 KB pro Slot). */
#define STORE_FILE_DEFAULT_SLOT (192u * 1024u)

#endif
