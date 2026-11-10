/**
 * @file disp_st7735.c
 * @brief ST7735-Treiber (RGB565, SPI) - siehe disp_st7735.h.
 *
 * Init-Folge nach dem bewaehrten "Rcmd1/Rcmd2red/Rcmd3"-Ablauf der Adafruit-Bibliothek
 * (Frame-Rate, Spannungen, Gamma), danach Ausrichtung, 16-Bit-Farbe und Anzeige ein.
 */
#include "disp_st7735.h"

#include <string.h>

enum { SWRESET = 0x01, SLPOUT = 0x11, NORON = 0x13, INVOFF = 0x20, INVON = 0x21, DISPOFF = 0x28, DISPON = 0x29,
       CASET = 0x2A, RASET = 0x2B, RAMWR = 0x2C, MADCTL = 0x36, COLMOD = 0x3A };

/* RGB565 je Palettenindex (Reihenfolge = KIWI_PX_*) */
static const uint16_t k_pal[KIWI_PX_COUNT] = {
    0x0000,  /* BG    schwarz        */
    0x07FF,  /* TRACE cyan           */
    0x0010,  /* FILL  dunkelblau     */
    0x8410,  /* AXIS  grau           */
    0xF800,  /* MARK  rot            */
    0x07E0,  /* PASS  gruen          */
    0xFFFF,  /* TEXT  weiss          */
    0x0821,  /* TINT  fast schwarz   */
};

/* Befehl + Daten, CS wird NICHT bewegt (fuer Folgen innerhalb einer CS-Phase). */
static int cmd_raw(disp_st7735_t *d, uint8_t c, const uint8_t *data, size_t n)
{
    const kiwi_disp_hal_t *h = d->hal;
    int rc = h->spi_write(h->ctx, 0, &c, 1);
    if (!rc && n) rc = h->spi_write(h->ctx, 1, data, n);
    return rc;
}

static int cmd(disp_st7735_t *d, uint8_t c, const uint8_t *data, size_t n)
{
    d->hal->spi_cs(d->hal->ctx, 0);
    int rc = cmd_raw(d, c, data, n);
    d->hal->spi_cs(d->hal->ctx, 1);
    return rc;
}

#define CMD(c, ...) do { static const uint8_t a_[] = { __VA_ARGS__, 0 }; if (cmd(d, (c), a_, sizeof a_ - 1)) return -1; } while (0)

static int st7735_open(kiwi_disp_t *base)
{
    disp_st7735_t *d = (disp_st7735_t *)base;
    const kiwi_disp_hal_t *h = d->hal;
    if (h->reset) {
        h->reset(h->ctx, 1); h->delay_ms(h->ctx, 5);
        h->reset(h->ctx, 0); h->delay_ms(h->ctx, 20);
        h->reset(h->ctx, 1); h->delay_ms(h->ctx, 150);
    }
    if (cmd(d, SWRESET, NULL, 0)) return -1;
    h->delay_ms(h->ctx, 150);
    if (cmd(d, SLPOUT, NULL, 0)) return -1;
    h->delay_ms(h->ctx, 150);
    CMD(0xB1, 0x01, 0x2C, 0x2D);                               /* FRMCTR1 */
    CMD(0xB2, 0x01, 0x2C, 0x2D);                               /* FRMCTR2 */
    CMD(0xB3, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D);             /* FRMCTR3 */
    CMD(0xB4, 0x07);                                           /* INVCTR  */
    CMD(0xC0, 0xA2, 0x02, 0x84);                               /* PWCTR1  */
    CMD(0xC1, 0xC5);                                           /* PWCTR2  */
    CMD(0xC2, 0x0A, 0x00);                                     /* PWCTR3  */
    CMD(0xC3, 0x8A, 0x2A);                                     /* PWCTR4  */
    CMD(0xC4, 0x8A, 0xEE);                                     /* PWCTR5  */
    CMD(0xC5, 0x0E);                                           /* VMCTR1  */
    if (cmd(d, d->cfg.invert ? INVON : INVOFF, NULL, 0)) return -1;
    { uint8_t m = d->cfg.madctl ? d->cfg.madctl : 0xC8; if (cmd(d, MADCTL, &m, 1)) return -1; }
    CMD(COLMOD, 0x05);                                         /* 16 Bit/Pixel */
    CMD(0xE0, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10);  /* Gamma + */
    CMD(0xE1, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10);  /* Gamma - */
    if (cmd(d, NORON, NULL, 0)) return -1;
    h->delay_ms(h->ctx, 10);
    if (cmd(d, DISPON, NULL, 0)) return -1;
    h->delay_ms(h->ctx, 100);
    return 0;
}

static int st7735_present(kiwi_disp_t *base, const kiwi_spec_view_t *v)
{
    disp_st7735_t *d = (disp_st7735_t *)base;
    const kiwi_disp_hal_t *h = d->hal;
    const unsigned W = d->cfg.width, H = d->cfg.height;
    kiwi_spec_rctx_t rc;
    if (kiwi_spec_rctx_init(&rc, v, (uint16_t)W, (uint16_t)H) != 0) return -1;

    const uint8_t x0 = d->cfg.xoff, y0 = d->cfg.yoff;
    const uint8_t ca[4] = { 0, x0, 0, (uint8_t)(x0 + W - 1u) };
    const uint8_t ra[4] = { 0, y0, 0, (uint8_t)(y0 + H - 1u) };
    int r = 0;
    h->spi_cs(h->ctx, 0);                                      /* CS bleibt fuer das ganze Bild aktiv */
    if ((r = cmd_raw(d, CASET, ca, 4)) == 0 && (r = cmd_raw(d, RASET, ra, 4)) == 0 && (r = cmd_raw(d, RAMWR, NULL, 0)) == 0) {
        for (unsigned y = 0; y < H && r == 0; y += DISP_ST7735_BAND_ROWS) {
            unsigned rows = H - y < DISP_ST7735_BAND_ROWS ? H - y : DISP_ST7735_BAND_ROWS;
            uint8_t *o = d->band;
            for (unsigned k = 0; k < rows; k++) {
                kiwi_spec_render_row(&rc, (uint16_t)(y + k), d->px);
                for (unsigned x = 0; x < W; x++) {
                    uint16_t c = d->pal[d->px[x]];
                    *o++ = (uint8_t)(c >> 8);                  /* RGB565, High-Byte zuerst */
                    *o++ = (uint8_t)c;
                }
            }
            r = h->spi_write(h->ctx, 1, d->band, (size_t)(o - d->band));
        }
    }
    h->spi_cs(h->ctx, 1);
    return r;
}

static void st7735_close(kiwi_disp_t *base)
{
    cmd((disp_st7735_t *)base, DISPOFF, NULL, 0);
}

kiwi_disp_t *disp_st7735_init(disp_st7735_t *d, const kiwi_disp_hal_t *hal, const disp_st7735_cfg_t *cfg)
{
    if (!d || !hal || !cfg || !hal->spi_write || !hal->spi_cs || !hal->delay_ms) return NULL;
    if (cfg->width < 16 || cfg->width > KIWI_WF_COLS_MAX || cfg->height < KIWI_DRAW_MIN_H || cfg->height > 255) return NULL;
    if ((unsigned)cfg->xoff + cfg->width > 255u || (unsigned)cfg->yoff + cfg->height > 255u) return NULL;
    memset(d, 0, sizeof *d);
    d->hal = hal;
    d->cfg = *cfg;
    memcpy(d->pal, k_pal, sizeof d->pal);
    d->base.width = cfg->width;
    d->base.height = cfg->height;
    d->base.open = st7735_open;
    d->base.present = st7735_present;
    d->base.close = st7735_close;
    return &d->base;
}
