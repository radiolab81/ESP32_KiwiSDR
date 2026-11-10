/* Modultests der Spektrumanzeige: Zustand/Autoskalierung, Renderer, Treiber gegen simulierte Panels. */
#include <stdio.h>
#include <string.h>

#include "disp_ssd1306.h"
#include "disp_st7735.h"
#include "kiwi_disp.h"
#include "kiwi_spec.h"
#include "sim_panel.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* Zeile mit Rauschen und Spitze an Spalte pk (Rohwert = dBm + 255) */
static void make_line(kiwi_wf_line_t *ln, unsigned cols, unsigned pk, uint32_t t)
{
    memset(ln, 0, sizeof *ln);
    ln->ncols = (uint16_t)cols; ln->f_start_hz = 1000000u - 25000u; ln->span_hz = 50000u; ln->t_ms = t;
    for (unsigned i = 0; i < cols; i++) ln->cols[i] = (uint8_t)(145 + (i * 7u) % 3u);
    if (pk < cols) ln->cols[pk] = 205;
}

static void feed(kiwi_spec_t *s, unsigned cols, unsigned pk, unsigned n, uint32_t t0)
{
    kiwi_wf_line_t ln;
    for (unsigned i = 0; i < n; i++) { make_line(&ln, cols, pk, t0 + i * 100u); ln.seq = i; kiwi_spec_push(s, &ln); }
}

static void test_state(void)
{
    printf("Spektrumzustand\n");
    static kiwi_spec_t s; kiwi_spec_view_t v;
    kiwi_spec_init(&s, 1, 1, -110, -20);
    CHECK(kiwi_spec_snapshot(&s, &v, 0, 3000) == 1 && !v.valid, "ohne Daten: valid = 0");
    kiwi_spec_set_tune(&s, 1000000u, "usb");
    feed(&s, 128, 90, 40, 1000);
    CHECK(kiwi_spec_snapshot(&s, &v, 5000, 3000) == 1 && v.valid, "mit Daten: valid = 1");
    unsigned mx = 0, at = 0;
    for (unsigned i = 0; i < v.cols; i++) if (v.lvl[i] > mx) { mx = v.lvl[i]; at = i; }
    CHECK(at == 90 && mx > 200, "Spitze an Spalte 90 (%u), Pegel %u", at, mx);
    CHECK(v.lvl[10] < 90, "Rauschen liegt unten (lvl %u)", v.lvl[10]);
    CHECK(v.f_tune_hz == 1000000u && !strcmp(v.mode, "usb") && v.pb_lo_hz < v.pb_hi_hz, "Abstimmung/Modus/Durchlassbereich uebernommen");
    kiwi_spec_snapshot(&s, &v, 9000, 3000);
    CHECK(!v.valid, "Daten aelter als 3 s gelten als ungueltig");
    uint32_t ver = v.ver;
    kiwi_spec_set_tune(&s, 1000000u, "usb");
    kiwi_spec_snapshot(&s, &v, 9000, 3000);
    CHECK(v.ver == ver, "unveraenderte Abstimmung erzeugt keine neue Version");
    kiwi_spec_set_tune(&s, 1000500u, "usb");
    kiwi_spec_snapshot(&s, &v, 9000, 3000);
    CHECK(v.ver != ver, "geaenderte Abstimmung erzeugt neue Version");
    kiwi_spec_reset(&s);
    kiwi_spec_snapshot(&s, &v, 1000, 3000);
    CHECK(!v.valid, "reset -> NO DATA");
    /* feste Skala */
    kiwi_spec_init(&s, 0, 0, -100, -50);
    feed(&s, 64, 30, 3, 100);
    kiwi_spec_snapshot(&s, &v, 400, 3000);
    CHECK(v.lvl[30] >= 250 && v.lvl[5] < 150, "feste Skala -100..-50 dBm: Spitze (-50) oben (%u), Rauschen (-110) unten (%u)", v.lvl[30], v.lvl[5]);
}

static int count_px(const sim_panel_t *p) { int n = 0; for (unsigned y = 0; y < p->h; y++) for (unsigned x = 0; x < p->w; x++) if (sim_panel_rgb(p, x, y)) n++; return n; }

static void fill_spec(kiwi_spec_t *s, unsigned cols)
{
    kiwi_spec_init(s, 1, 1, -110, -20);
    kiwi_spec_set_tune(s, 1000000u, "usb");
    feed(s, cols, cols * 7 / 10, 40, 1000);
}

static void test_renderer(void)
{
    printf("Renderer\n");
    static kiwi_spec_t s; kiwi_spec_view_t v; kiwi_spec_rctx_t r;
    fill_spec(&s, 128); kiwi_spec_snapshot(&s, &v, 5000, 3000);
    CHECK(kiwi_spec_rctx_init(&r, &v, 128, 31) != 0, "Hoehe < 32 wird abgelehnt");
    CHECK(kiwi_spec_rctx_init(&r, &v, 128, 64) == 0, "128x64 akzeptiert");
    CHECK(r.x_mark == 64, "Markierung in Spaltenmitte (%d)", r.x_mark);
    uint8_t px[KIWI_WF_COLS_MAX]; int bad = 0, trace = 0;
    for (unsigned y = 0; y < 64; y++) { kiwi_spec_render_row(&r, (uint16_t)y, px); for (unsigned x = 0; x < 128; x++) { if (px[x] >= KIWI_PX_COUNT) bad++; if (px[x] == KIWI_PX_TRACE) trace++; } }
    CHECK(!bad && trace >= 128, "nur gueltige Palettenindizes, Kurve gezeichnet (%d Pixel)", trace);
    v.valid = 0;
    kiwi_spec_rctx_init(&r, &v, 128, 64);
    trace = 0;
    for (unsigned y = 0; y < 64; y++) { kiwi_spec_render_row(&r, (uint16_t)y, px); for (unsigned x = 0; x < 128; x++) if (px[x] == KIWI_PX_TRACE) trace++; }
    CHECK(trace == 0, "ohne Daten keine Kurve (Text 'NO DATA')");
}

typedef struct { int ssd; kiwi_disp_bus_t bus; unsigned w, h; unsigned coff, xoff, yoff; const char *name; } dcase_t;

static void test_drivers(void)
{
    printf("Treiber gegen simulierte Panels\n");
    static const dcase_t cases[] = {
        {1, KIWI_DISP_BUS_I2C, 128, 64, 0, 0, 0, "SSD1306 I2C 128x64"},
        {1, KIWI_DISP_BUS_I2C, 128, 32, 0, 0, 0, "SSD1306 I2C 128x32"},
        {1, KIWI_DISP_BUS_SPI, 128, 64, 0, 0, 0, "SSD1306 SPI 128x64"},
        {1, KIWI_DISP_BUS_I2C, 128, 64, 2, 2, 0, "SH1106 I2C (Spaltenversatz 2)"},
        {0, KIWI_DISP_BUS_SPI, 128, 160, 0, 0, 0, "ST7735 128x160"},
        {0, KIWI_DISP_BUS_SPI, 128, 128, 0, 2, 3, "ST7735 128x128 mit Versatz"},
        {0, KIWI_DISP_BUS_SPI, 160, 80, 0, 1, 26, "ST7735 160x80 mit Versatz"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        const dcase_t *c = &cases[i];
        static sim_panel_t p; static kiwi_spec_t s; static kiwi_spec_ui_t ui;
        static union { disp_ssd1306_t a; disp_st7735_t b; } drv;
        sim_panel_init(&p, c->ssd ? SIM_SSD1306 : SIM_ST7735, c->bus, (uint16_t)c->w, (uint16_t)c->h);
        p.xoff = (uint8_t)(c->ssd ? c->coff : c->xoff); p.yoff = (uint8_t)c->yoff;
        kiwi_disp_t *d;
        if (c->ssd) { disp_ssd1306_cfg_t cfg = { .bus = c->bus, .i2c_addr = 0x3C, .width = (uint16_t)c->w, .height = (uint16_t)c->h, .col_offset = (uint8_t)c->coff }; d = disp_ssd1306_init(&drv.a, sim_panel_hal(&p), &cfg); }
        else { disp_st7735_cfg_t cfg = { .width = (uint16_t)c->w, .height = (uint16_t)c->h, .xoff = (uint8_t)c->xoff, .yoff = (uint8_t)c->yoff }; d = disp_st7735_init(&drv.b, sim_panel_hal(&p), &cfg); }
        CHECK(d != NULL, "%s: init", c->name);
        if (!d) continue;
        fill_spec(&s, c->w); memset(&ui, 0, sizeof ui);
        int rc = kiwi_spec_refresh(&s, d, &ui, 5000, 3000);
        CHECK(rc == 1 && p.display_on, "%s: erstes Bild gezeichnet, Display an (rc=%d)", c->name, rc);
        CHECK(p.errors == 0, "%s: keine Protokollfehler%s%s", c->name, p.errors ? ": " : "", p.err_msg);
        if (!c->ssd) CHECK(p.frames >= 1, "%s: Bild vollstaendig uebertragen", c->name);
        CHECK(count_px(&p) > 200, "%s: sichtbarer Inhalt (%d Pixel)", c->name, count_px(&p));
        unsigned wr = p.i2c_writes + p.spi_writes;
        CHECK(kiwi_spec_refresh(&s, d, &ui, 5050, 3000) == 0 && p.i2c_writes + p.spi_writes == wr, "%s: unveraendert -> nichts gesendet", c->name);
        /* Spitze muss an der erwarteten Spalte erscheinen (oberster Kurvenpixel je Spalte) */
        unsigned best = 99999, col = 0;
        for (unsigned x = 0; x < c->w; x++) { if (x >= c->w / 2 - 2 && x <= c->w / 2 + 2) continue; for (unsigned y = 9; y < c->h - 12; y++) { uint32_t px = sim_panel_rgb(&p, x, y); int on = c->ssd ? px != 0 : ((px >> 8) & 255) > 200 && (px & 255) > 200 && ((px >> 16) & 255) < 60; if (on) { if (y < best) { best = y; col = x; } break; } } }
        unsigned exp = c->w * 7 / 10;
        CHECK(col + 2 >= exp && col <= exp + 2, "%s: Spitze bei Spalte %u (erwartet %u)", c->name, col, exp);
        d->close(d);
        CHECK(!p.display_on, "%s: close schaltet das Display aus", c->name);
    }
}

/* HAL mit Fehlern */
static int g_fail; static sim_panel_t *g_p; static const kiwi_disp_hal_t *g_real;
static int flaky_i2c(void *ctx, uint8_t a, const uint8_t *b, size_t n) { (void)ctx; return g_fail ? -1 : g_real->i2c_write(g_real->ctx, a, b, n); }

static void test_errors(void)
{
    printf("Fehlerpfade\n");
    static sim_panel_t p; static kiwi_spec_t s; static kiwi_spec_ui_t ui; static disp_ssd1306_t drv;
    sim_panel_init(&p, SIM_SSD1306, KIWI_DISP_BUS_I2C, 128, 64); g_p = &p; g_real = sim_panel_hal(&p);
    kiwi_disp_hal_t hal = *g_real; hal.i2c_write = flaky_i2c;
    disp_ssd1306_cfg_t cfg = { .bus = KIWI_DISP_BUS_I2C, .i2c_addr = 0x3C, .width = 128, .height = 64 };
    kiwi_disp_t *d = disp_ssd1306_init(&drv, &hal, &cfg);
    fill_spec(&s, 128); memset(&ui, 0, sizeof ui);
    g_fail = 1;
    CHECK(kiwi_spec_refresh(&s, d, &ui, 5000, 3000) < 0, "Busfehler wird gemeldet (kein Absturz)");
    CHECK(kiwi_spec_refresh(&s, d, &ui, 5100, 3000) < 0, "erneuter Versuch schlaegt weiter sauber fehl");
    g_fail = 0;
    CHECK(kiwi_spec_refresh(&s, d, &ui, 5200, 3000) == 1 && p.display_on, "nach Wiederkehr: Display neu initialisiert und gezeichnet");
    /* falsche Adresse = NACK */
    sim_panel_init(&p, SIM_SSD1306, KIWI_DISP_BUS_I2C, 128, 64); g_real = sim_panel_hal(&p);
    cfg.i2c_addr = 0x3D; hal = *g_real; hal.i2c_write = flaky_i2c;
    d = disp_ssd1306_init(&drv, &hal, &cfg); memset(&ui, 0, sizeof ui);
    CHECK(kiwi_spec_refresh(&s, d, &ui, 5000, 3000) < 0, "falsche I2C-Adresse -> Fehler statt Fehlzeichnung");
    /* ungueltige Konfiguration */
    cfg.i2c_addr = 0x3C; cfg.height = 48;
    CHECK(disp_ssd1306_init(&drv, g_real, &cfg) == NULL, "SSD1306-Hoehe 48 abgelehnt");
    static disp_st7735_t st; disp_st7735_cfg_t sc = { .width = 200, .height = 128 };
    CHECK(disp_st7735_init(&st, g_real, &sc) == NULL, "ST7735-Breite 200 abgelehnt");
    kiwi_disp_hal_t nohal; memset(&nohal, 0, sizeof nohal);
    cfg.height = 64;
    CHECK(disp_ssd1306_init(&drv, &nohal, &cfg) == NULL, "HAL ohne i2c_write abgelehnt");
}

int main(void)
{
    test_state(); test_renderer(); test_drivers(); test_errors();
    if (fails) { printf("\n%d FEHLER\n", fails); return 1; }
    printf("\nAlle Spektrum-Tests bestanden.\n");
    return 0;
}
