#include "spec_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disp_ssd1306.h"
#include "disp_st7735.h"
#include "kiwi_port.h"
#include "kiwi_wf.h"
#include "sim_panel.h"

struct spec_pc {
    kiwi_spec_t spec;
    kiwi_wf_t wf;
    kiwi_spec_ui_t ui;
    sim_panel_t panel;
    union { disp_ssd1306_t ssd; disp_st7735_t st; } drv;
    kiwi_disp_t *disp;
    char ppm[256];
    int term, active, giveup, fails;
    uint32_t span_hz, t_frame, t_retry;
};

spec_pc_t *spec_pc_open(const char *panel, unsigned span_khz, const char *ppm, int term)
{
    spec_pc_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->span_hz = span_khz * 1000u;
    s->term = term;
    if (ppm) snprintf(s->ppm, sizeof s->ppm, "%s", ppm);
    if (!strcmp(panel, "ssd1306") || !strcmp(panel, "ssd1306spi")) {
        kiwi_disp_bus_t bus = !strcmp(panel, "ssd1306") ? KIWI_DISP_BUS_I2C : KIWI_DISP_BUS_SPI;
        sim_panel_init(&s->panel, SIM_SSD1306, bus, 128, 64);
        disp_ssd1306_cfg_t c = { .bus = bus, .i2c_addr = 0x3C, .width = 128, .height = 64 };
        s->disp = disp_ssd1306_init(&s->drv.ssd, sim_panel_hal(&s->panel), &c);
    } else if (!strcmp(panel, "st7735")) {
        sim_panel_init(&s->panel, SIM_ST7735, KIWI_DISP_BUS_SPI, 128, 160);
        disp_st7735_cfg_t c = { .width = 128, .height = 160 };
        s->disp = disp_st7735_init(&s->drv.st, sim_panel_hal(&s->panel), &c);
    }
    if (!s->disp) { fprintf(stderr, "Unbekanntes Display '%s' (ssd1306 | ssd1306spi | st7735)\n", panel); free(s); return NULL; }
    kiwi_spec_init(&s->spec, 1, 1, -110, -20);
    return s;
}

static void connect_wf(spec_pc_t *s, kiwi_client_t *m)
{
    kiwi_wf_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.master = m; cfg.cols = s->disp->width; cfg.span_hz = s->span_hz; cfg.speed = 4; cfg.sink = &s->spec.sink;
    kiwi_err_t rc = kiwi_wf_init(&s->wf, &cfg);
    if (rc == KIWI_OK) rc = kiwi_wf_connect(&s->wf);
    if (rc != KIWI_OK) {
        fprintf(stderr, "\nSpektrum nicht verfuegbar (%s) - Audio laeuft weiter\n", kiwi_strerror(rc));
        kiwi_wf_close(&s->wf);
        s->giveup = 1;                          /* kein erneuter Versuch (Kiwi lehnt Zweitverbindungen oft ab) */
        return;
    }
    s->active = 1;
}

void spec_pc_step(spec_pc_t *s, kiwi_client_t *m)
{
    if (!s) return;
    if (!s->active && !s->giveup && m->setup_done) connect_wf(s, m);
    if (s->active) {
        kiwi_err_t rc = kiwi_wf_poll(&s->wf, 0);
        kiwi_spec_set_tune(&s->spec, m->cur_freq_hz, m->cur_mode);
        if (rc != KIWI_OK) {
            fprintf(stderr, "\nSpektrum beendet (%s) - Audio laeuft weiter\n", kiwi_strerror(rc));
            kiwi_wf_close(&s->wf);
            kiwi_spec_reset(&s->spec);
            s->active = 0; s->giveup = 1;
        }
    }
    uint32_t now = kiwi_time_ms();
    if (now - s->t_frame >= 200u) {
        s->t_frame = now;
        if (kiwi_spec_refresh(&s->spec, s->disp, &s->ui, now, 3000u) == 1) {
            if (s->ppm[0]) sim_panel_write_ppm(&s->panel, s->ppm);
            if (s->term) sim_panel_print(&s->panel, stdout);
        }
    }
}

void spec_pc_stop(spec_pc_t *s)
{
    if (!s) return;
    if (s->active) kiwi_wf_close(&s->wf);
    s->active = 0;
}

void spec_pc_free(spec_pc_t *s)
{
    if (!s) return;
    if (s->disp) s->disp->close(s->disp);
    free(s);
}
