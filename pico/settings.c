/**
 * @file settings.c
 * @brief Einstellungsspeicher: ein 4-KB-Sektor am Flash-Ende, mit Magic, Version und CRC-32.
 *
 * Absichtlich minimal (statt eines Wear-Leveling-Dateisystems): Es wird nur beim manuellen
 * Serverwechsel ("d go") geschrieben, also selten. Ein Abbruch waehrend des Schreibens hinterlaesst
 * einen Sektor mit falscher CRC -> wird beim Lesen ignoriert, es gilt der Kconfig/CMake-Server.
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "kiwi_dir_db.h"          /* KIWI_DIR_HOST_LEN, kiwi_dir_crc32() */

#include "flash_layout.h"
#include "flash_util.h"

#define SET_MAGIC   0x4B495731u   /* "KIW1" */

typedef struct {
    uint32_t magic;
    uint16_t port;
    uint16_t reserved;
    char     host[KIWI_DIR_HOST_LEN];
    uint32_t crc;                 /* CRC-32 ueber alles davor */
} settings_rec_t;

_Static_assert(sizeof(settings_rec_t) <= KIWI_FLASH_PAGE, "Einstellungen muessen in eine Flash-Seite passen");

static uint32_t rec_crc(const settings_rec_t *r)
{
    return kiwi_dir_crc32(0, r, offsetof(settings_rec_t, crc));
}

int settings_load_server(char *host, size_t cap, uint16_t *port)
{
    settings_rec_t r;
    kiwi_flash_read(KIWI_SETTINGS_OFFSET, &r, sizeof r);
    if (r.magic != SET_MAGIC || r.crc != rec_crc(&r) || r.port == 0) return 0;
    r.host[sizeof r.host - 1] = '\0';
    if (!r.host[0]) return 0;
    snprintf(host, cap, "%s", r.host);
    *port = r.port;
    return 1;
}

int settings_save_server(const char *host, uint16_t port)
{
    settings_rec_t r;
    memset(&r, 0, sizeof r);
    r.magic = SET_MAGIC;
    r.port = port;
    snprintf(r.host, sizeof r.host, "%s", host);
    r.crc = rec_crc(&r);
    if (kiwi_flash_erase(KIWI_SETTINGS_OFFSET, KIWI_FLASH_SECTOR) != 0) return -1;
    return kiwi_flash_write(KIWI_SETTINGS_OFFSET, &r, sizeof r);
}
