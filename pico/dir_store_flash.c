/**
 * @file dir_store_flash.c
 * @brief Pico-Flash als kiwi_dir_store_t (A/B-Slots) - Gegenstueck zu main/dir_store_flash.c.
 *
 * Dieselbe Schnittstelle (main/dir_store_flash.h) wie beim ESP32; statt esp_partition_* arbeiten
 * flash_util.c (Schreiben/Loeschen) und XIP (Lesen). Die Datenbank liegt am Flash-Ende.
 */
#include "../main/dir_store_flash.h"

#include "FreeRTOS.h"
#include "task.h"

#include "flash_layout.h"
#include "flash_util.h"
#include "klog.h"

static const char *TAG = "dir_flash";

extern char __flash_binary_end;          /* Linker-Symbol: Ende des Programms im Flash */

static int fl_read(void *ctx, int slot, size_t off, void *buf, size_t len)
{
    (void)ctx;
    if (off + len > KIWI_DB_SLOT_BYTES) return -1;
    kiwi_flash_read(KIWI_DB_OFFSET + (uint32_t)slot * KIWI_DB_SLOT_BYTES + (uint32_t)off, buf, len);
    return 0;
}

static int fl_erase(void *ctx, int slot)
{
    (void)ctx;
    for (uint32_t o = 0; o < KIWI_DB_SLOT_BYTES; o += KIWI_FLASH_SECTOR) {
        if (kiwi_flash_erase(KIWI_DB_OFFSET + (uint32_t)slot * KIWI_DB_SLOT_BYTES + o, KIWI_FLASH_SECTOR) != 0)
            return -1;
        vTaskDelay(1);                   /* CPU freigeben: Audio-/WLAN-Tasks duerfen dazwischen laufen */
    }
    return 0;
}

static int fl_write(void *ctx, int slot, size_t off, const void *buf, size_t len)
{
    (void)ctx;
    if (off + len > KIWI_DB_SLOT_BYTES) return -1;
    return kiwi_flash_write(KIWI_DB_OFFSET + (uint32_t)slot * KIWI_DB_SLOT_BYTES + (uint32_t)off, buf, len);
}

static kiwi_dir_store_t s_store = { 0, KIWI_FLASH_SECTOR, fl_read, fl_erase, fl_write, NULL };

const kiwi_dir_store_t *dir_store_flash_get(void)
{
    /* Zusatzsicherung zum Linker-Limit (CMakeLists.txt): niemals Programmcode ueberschreiben. */
    uint32_t prog_end = (uint32_t)(uintptr_t)&__flash_binary_end - XIP_BASE;
    if (prog_end > KIWI_DB_OFFSET) {
        KLOGE(TAG, "Programm (%u B) reicht in den Datenbankbereich (ab %u B) - Datenbank abgeschaltet",
              (unsigned)prog_end, (unsigned)KIWI_DB_OFFSET);
        return NULL;
    }
    s_store.slot_size = KIWI_DB_SLOT_BYTES;
    KLOGI(TAG, "Datenbank-Flash: %u KB ab Offset 0x%06x, 2 Slots zu %u KB (Programm endet bei 0x%06x)",
          (unsigned)(2u * KIWI_DB_SLOT_BYTES / 1024u), (unsigned)KIWI_DB_OFFSET,
          (unsigned)(KIWI_DB_SLOT_BYTES / 1024u), (unsigned)prog_end);
    return &s_store;
}
