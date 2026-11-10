#include "kiwi_disp.h"

int kiwi_spec_refresh(kiwi_spec_t *s, kiwi_disp_t *d, kiwi_spec_ui_t *ui, uint32_t now_ms, uint32_t stale_ms)
{
    if (!ui->opened) {
        if (d->open(d) != 0) { ui->errors++; return -1; }
        ui->opened = 1;
        ui->drawn = 0;
    }
    kiwi_spec_view_t v;
    if (!kiwi_spec_snapshot(s, &v, now_ms, stale_ms)) return 0;
    if (ui->drawn && v.ver == ui->last_ver && v.valid == ui->last_valid) return 0;   /* unveraendert */

    if (d->present(d, &v) != 0) {
        ui->errors++;
        ui->opened = 0;                          /* naechstes Mal komplett neu initialisieren */
        return -1;
    }
    ui->last_ver = v.ver;
    ui->last_valid = v.valid;
    ui->drawn = 1;
    ui->frames++;
    return 1;
}
