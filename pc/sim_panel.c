#include "sim_panel.h"

#include <string.h>

static void fail(sim_panel_t *p, const char *m)
{
    if (!p->errors) snprintf(p->err_msg, sizeof p->err_msg, "%s", m);
    p->errors++;
}

/* ------------------------------ SSD1306 -------------------------------- */

static void ssd_data(sim_panel_t *p, uint8_t b)
{
    if (p->page < 0 || p->page > 7 || p->col < 0 || p->col >= 132) { fail(p, "SSD1306: Daten ausserhalb des RAM"); return; }
    p->gdd[p->page][p->col++] = b;
}

static void ssd_cmd_byte(sim_panel_t *p, uint8_t b)
{
    if (p->need) {                                   /* Argument eines laufenden Befehls */
        p->args[p->nargs++] = b;
        if (p->nargs == p->need) { p->need = 0; if (p->cmd == 0x20 && b != 0x02) fail(p, "SSD1306: nur Page-Adressierung erwartet"); }
        return;
    }
    p->cmd = b; p->nargs = 0;
    if (b == 0xAE) p->display_on = 0;
    else if (b == 0xAF) p->display_on = 1;
    else if (b >= 0xB0 && b <= 0xB7) p->page = b & 7;
    else if (b <= 0x0F) p->col = (p->col & 0xF0) | (b & 0x0F);
    else if (b >= 0x10 && b <= 0x1F) p->col = (p->col & 0x0F) | ((b & 0x0F) << 4);
    else if (b == 0x81 || b == 0x8D || b == 0xD5 || b == 0xD9 || b == 0xDA || b == 0xDB || b == 0xD3 || b == 0xA8 || b == 0x20) p->need = 1;
    else if (b == 0xA4 || b == 0xA5 || b == 0xA6 || b == 0xA7 || b == 0xA0 || b == 0xA1 || b == 0xC0 || b == 0xC8 || (b >= 0x40 && b <= 0x7F)) { }
    else fail(p, "SSD1306: unbekannter Befehl");
}

static void ssd_stream(sim_panel_t *p, int dc, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { if (dc) ssd_data(p, b[i]); else ssd_cmd_byte(p, b[i]); }
    if (dc) p->frames++;                             /* zaehlt Page-Datenbloecke */
}

/* ------------------------------- ST7735 -------------------------------- */

static int st_nargs(uint8_t c)
{
    switch (c) {
    case 0x01: case 0x11: case 0x13: case 0x20: case 0x21: case 0x28: case 0x29: case 0x2C: return 0;
    case 0x2A: case 0x2B: return 4;
    case 0x36: case 0x3A: case 0xB4: case 0xC1: case 0xC5: return 1;
    case 0xC2: case 0xC3: case 0xC4: return 2;
    case 0xB1: case 0xB2: case 0xC0: return 3;
    case 0xB3: return 6;
    case 0xE0: case 0xE1: return 16;
    default: return -1;
    }
}

static void st_finish_cmd(sim_panel_t *p)
{
    if (p->cmd == 0x2A) { p->x0 = (unsigned)(p->args[0] << 8 | p->args[1]); p->x1 = (unsigned)(p->args[2] << 8 | p->args[3]); p->win_set |= 1; }
    if (p->cmd == 0x2B) { p->y0 = (unsigned)(p->args[0] << 8 | p->args[1]); p->y1 = (unsigned)(p->args[2] << 8 | p->args[3]); p->win_set |= 2; }
    if (p->cmd == 0x28) p->display_on = 0;
    if (p->cmd == 0x29) p->display_on = 1;
}

static void st_pixel(sim_panel_t *p, uint16_t c)
{
    unsigned ww = p->x1 - p->x0 + 1, wh = p->y1 - p->y0 + 1;
    if (p->wpix >= ww * wh) { fail(p, "ST7735: mehr Pixel als im Fenster"); return; }
    unsigned x = p->x0 + p->wpix % ww, y = p->y0 + p->wpix / ww;
    p->wpix++;
    if (x >= p->xoff && y >= p->yoff && x - p->xoff < p->w && y - p->yoff < p->h)
        p->fb[(y - p->yoff) * p->w + (x - p->xoff)] = c;
}

static void st_stream(sim_panel_t *p, int dc, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!dc) {
            if (p->in_ramwr) { p->in_ramwr = 0; if (p->wpix == (p->x1 - p->x0 + 1) * (p->y1 - p->y0 + 1)) p->frames++; }
            p->cmd = b[i]; p->nargs = 0;
            int na = st_nargs(p->cmd);
            if (na < 0) { fail(p, "ST7735: unbekannter Befehl"); p->need = 0; continue; }
            p->need = (unsigned)na;
            if (p->cmd == 0x2C) {
                if (p->win_set != 3) fail(p, "ST7735: RAMWR ohne CASET/RASET");
                p->in_ramwr = 1; p->wpix = 0; p->hi_byte_valid = 0;
            }
            if (!p->need && p->cmd != 0x2C) st_finish_cmd(p);
        } else if (p->in_ramwr) {
            if (!p->hi_byte_valid) { p->hi_byte = b[i]; p->hi_byte_valid = 1; }
            else { st_pixel(p, (uint16_t)(p->hi_byte << 8 | b[i])); p->hi_byte_valid = 0; }
        } else if (p->need) {
            if (p->nargs < sizeof p->args) p->args[p->nargs] = b[i];
            p->nargs++;
            if (p->nargs == p->need) { p->need = 0; st_finish_cmd(p); }
        } else {
            fail(p, "ST7735: Daten ohne Befehl");
        }
    }
}

/* -------------------------------- HAL ---------------------------------- */

static int hal_i2c(void *ctx, uint8_t addr, const uint8_t *buf, size_t n)
{
    sim_panel_t *p = ctx;
    p->i2c_writes++;
    if (p->kind != SIM_SSD1306 || p->bus != KIWI_DISP_BUS_I2C) { fail(p, "I2C an falschem Panel"); return -1; }
    if (addr != p->i2c_addr) { fail(p, "falsche I2C-Adresse"); return -1; }   /* wie NACK */
    if (n < 1) { fail(p, "leere I2C-Uebertragung"); return -1; }
    if (buf[0] == 0x00) ssd_stream(p, 0, buf + 1, n - 1);
    else if (buf[0] == 0x40) ssd_stream(p, 1, buf + 1, n - 1);
    else { fail(p, "SSD1306: unbekanntes Steuerbyte"); return -1; }
    return 0;
}

static int hal_spi(void *ctx, int dc, const uint8_t *buf, size_t n)
{
    sim_panel_t *p = ctx;
    p->spi_writes++;
    if (p->bus != KIWI_DISP_BUS_SPI) { fail(p, "SPI an I2C-Panel"); return -1; }
    if (!p->cs_low) { fail(p, "SPI-Zugriff bei inaktivem CS"); return -1; }
    if (p->kind == SIM_SSD1306) ssd_stream(p, dc, buf, n); else st_stream(p, dc, buf, n);
    return 0;
}

static void hal_cs(void *ctx, int level)
{
    sim_panel_t *p = ctx;
    int was_low = p->cs_low;
    p->cs_low = (level == 0);
    if (was_low && !p->cs_low && p->kind == SIM_ST7735 && p->in_ramwr) {      /* Bild beendet? */
        if (p->wpix != (p->x1 - p->x0 + 1) * (p->y1 - p->y0 + 1)) fail(p, "ST7735: Bild unvollstaendig (CS hoch)");
        else p->frames++;
        p->in_ramwr = 0;
    }
}

static void hal_reset(void *ctx, int level) { sim_panel_t *p = ctx; if (level == 0) p->resets++; }
static void hal_delay(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }          /* Simulation: keine Wartezeit */

void sim_panel_init(sim_panel_t *p, sim_kind_t kind, kiwi_disp_bus_t bus, uint16_t w, uint16_t h)
{
    memset(p, 0, sizeof *p);
    p->kind = kind; p->bus = bus; p->w = w; p->h = h; p->i2c_addr = 0x3C;
    p->hal.i2c_write = hal_i2c; p->hal.spi_write = hal_spi; p->hal.spi_cs = hal_cs;
    p->hal.reset = hal_reset; p->hal.delay_ms = hal_delay; p->hal.ctx = p;
}

const kiwi_disp_hal_t *sim_panel_hal(sim_panel_t *p) { return &p->hal; }

uint32_t sim_panel_rgb(const sim_panel_t *p, unsigned x, unsigned y)
{
    if (x >= p->w || y >= p->h) return 0;
    if (p->kind == SIM_SSD1306) {
        unsigned col = x + p->xoff;
        return (col < 132 && ((p->gdd[y / 8][col] >> (y % 8)) & 1u)) ? 0xFFFFFFu : 0u;
    }
    uint16_t c = p->fb[y * p->w + x];
    uint32_t r = (uint32_t)((c >> 11) & 31u) * 255u / 31u, g = (uint32_t)((c >> 5) & 63u) * 255u / 63u, b = (uint32_t)(c & 31u) * 255u / 31u;
    return r << 16 | g << 8 | b;
}

void sim_panel_print(const sim_panel_t *p, FILE *out)
{
    fputs("\x1b[H", out);
    for (unsigned y = 0; y < p->h; y += 2) {
        for (unsigned x = 0; x < p->w; x++) {
            uint32_t a = sim_panel_rgb(p, x, y), b = sim_panel_rgb(p, x, y + 1);
            fprintf(out, "\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um\xE2\x96\x80", (a >> 16) & 255, (a >> 8) & 255, a & 255,
                    (b >> 16) & 255, (b >> 8) & 255, b & 255);
        }
        fputs("\x1b[0m\n", out);
    }
}

int sim_panel_write_ppm(const sim_panel_t *p, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", p->w, p->h);
    for (unsigned y = 0; y < p->h; y++)
        for (unsigned x = 0; x < p->w; x++) {
            uint32_t c = sim_panel_rgb(p, x, y);
            fputc((int)((c >> 16) & 255), f); fputc((int)((c >> 8) & 255), f); fputc((int)(c & 255), f);
        }
    return fclose(f) == 0 ? 0 : -1;
}
