/**
 * @file flash_layout.h
 * @brief Aufteilung des QSPI-Flashs: [ Programm ... | kiwidb Slot A | kiwidb Slot B | Einstellungen ].
 *
 * Daten liegen am ENDE des Flashs. CMakeLists.txt verkleinert die dem Linker bekannte Flashgroesse
 * um genau diesen Bereich - wuchse das Programm hinein, scheitert der LINK (nicht erst der Betrieb).
 */
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include "pico/stdlib.h"
#include "kiwi_config.h"

#define KIWI_FLASH_SECTOR      4096u
#define KIWI_FLASH_PAGE        256u
#define KIWI_DB_SLOT_BYTES     ((uint32_t)CONFIG_KIWI_DIR_SLOT_KB * 1024u)
#define KIWI_SETTINGS_OFFSET   ((uint32_t)KIWI_FLASH_TOTAL_BYTES - KIWI_FLASH_SECTOR)
#define KIWI_DB_OFFSET         (KIWI_SETTINGS_OFFSET - 2u * KIWI_DB_SLOT_BYTES)

#endif
