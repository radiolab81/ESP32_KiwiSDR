/**
 * @file flash_util.c
 * @brief Siehe flash_util.h.
 */
#include "flash_util.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#include "flash_layout.h"
#include "klog.h"

#define FLASH_SAFE_TIMEOUT_MS 2000   /* so lange darf das Anhalten des Systems dauern */

typedef struct {
    uint32_t off;
    size_t   len;
    const uint8_t *data;
} flash_job_t;

static void do_erase(void *p)
{
    const flash_job_t *j = (const flash_job_t *)p;
    flash_range_erase(j->off, j->len);
}

static void do_program(void *p)
{
    const flash_job_t *j = (const flash_job_t *)p;
    flash_range_program(j->off, j->data, j->len);
}

int kiwi_flash_erase(uint32_t off, size_t len)
{
    if ((off % KIWI_FLASH_SECTOR) || (len % KIWI_FLASH_SECTOR)) return -1;
    flash_job_t j = { off, len, NULL };
    int rc = flash_safe_execute(do_erase, &j, FLASH_SAFE_TIMEOUT_MS);
    if (rc != PICO_OK) KLOGE("flash", "Loeschen @0x%06x fehlgeschlagen (SDK-Code %d)", (unsigned)off, rc);
    return rc == PICO_OK ? 0 : -1;
}

int kiwi_flash_write(uint32_t off, const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    uint8_t page[KIWI_FLASH_PAGE];

    while (len) {
        uint32_t page_base = off & ~(KIWI_FLASH_PAGE - 1u);
        uint32_t in_page   = off - page_base;
        size_t   n         = KIWI_FLASH_PAGE - in_page;
        if (n > len) n = len;

        /* Seite mit 0xFF vorbelegen: "programmieren" von 0xFF aendert keine Flash-Zelle. So
         * koennen mehrere kleine Schreibvorgaenge (120-Byte-Datensaetze) dieselbe Seite nutzen. */
        memset(page, 0xFF, sizeof page);
        memcpy(page + in_page, src, n);

        flash_job_t j = { page_base, KIWI_FLASH_PAGE, page };
        int rc = flash_safe_execute(do_program, &j, FLASH_SAFE_TIMEOUT_MS);
        if (rc != PICO_OK) {
            KLOGE("flash", "Schreiben @0x%06x fehlgeschlagen (SDK-Code %d)", (unsigned)page_base, rc);
            return -1;
        }

        off += (uint32_t)n;
        src += n;
        len -= n;
    }
    return 0;
}

void kiwi_flash_read(uint32_t off, void *buf, size_t len)
{
    memcpy(buf, (const void *)(XIP_BASE + off), len);
}
