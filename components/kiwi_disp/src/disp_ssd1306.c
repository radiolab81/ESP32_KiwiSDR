/**
 * @file disp_ssd1306.c
 * @brief SSD1306/SH1106-Treiber - siehe disp_ssd1306.h.
 *
 * Bus-Protokoll: I2C: 1. Byte = Steuerbyte (0x00 Befehle, 0x40 Daten), danach die Nutzbytes.
 *                SPI: Befehl/Daten ueber die DC-Leitung (HAL), CS vom Treiber gehalten.
 */
#include "disp_ssd1306.h"

#include <string.h>

static int write_cmd(disp_ssd1306_t *d, const uint8_t *cmd, size_t n)
{
    const kiwi_disp_hal_t *h = d->hal;
    if (d->cfg.bus == KIWI_DISP_BUS_I2C) {
        uint8_t b[1 + 40];
        if (n > 40) return -1;
        b[0] = 0x00;
        memcpy(b + 1, cmd, n);
        return h->i2c_write(h->ctx, d->cfg.i2c_addr, b, n + 1);
    }
    h->spi_cs(h->ctx, 0);
    int rc = h->spi_write(h->ctx, 0, cmd, n);
    h->spi_cs(h->ctx, 1);
    return rc;
}

static int ssd1306_open(kiwi_disp_t *base)
{
    disp_ssd1306_t *d = (disp_ssd1306_t *)base;
    const kiwi_disp_hal_t *h = d->hal;
    if (h->reset) {                                  /* Hardware-Reset: 1 - 0 - 1 */
        h->reset(h->ctx, 1); h->delay_ms(h->ctx, 1);
        h->reset(h->ctx, 0); h->delay_ms(h->ctx, 10);
        h->reset(h->ctx, 1); h->delay_ms(h->ctx, 10);
    }
    const uint8_t c = d->cfg.contrast ? d->cfg.contrast : 0xCF;
    const uint8_t init[] = {
        0xAE,                                        /* Anzeige aus                               */
        0xD5, 0x80,                                  /* Takt                                      */
        0xA8, (uint8_t)(d->cfg.height - 1u),         /* Multiplex-Verhaeltnis                     */
        0xD3, 0x00,                                  /* Display-Offset                            */
        0x40,                                        /* Startzeile 0                              */
        0x8D, 0x14,                                  /* interne Ladungspumpe an                   */
        0x20, 0x02,                                  /* Page-Adressierung (SSD1306 und SH1106)    */
        (uint8_t)(d->cfg.rotate180 ? 0xA0 : 0xA1),   /* Segment-Remap                             */
        (uint8_t)(d->cfg.rotate180 ? 0xC0 : 0xC8),   /* COM-Scan-Richtung                         */
        0xDA, (uint8_t)(d->cfg.height == 32 ? 0x02 : 0x12),   /* COM-Pins                         */
        0x81, c,                                     /* Kontrast                                  */
        0xD9, 0xF1,                                  /* Pre-Charge                                */
        0xDB, 0x40,                                  /* VCOMH                                     */
        0xA4,                                        /* RAM-Inhalt anzeigen                       */
        0xA6,                                        /* normal (nicht invertiert)                 */
        0xAF                                         /* Anzeige ein                               */
    };
    return write_cmd(d, init, sizeof init);
}

static int ssd1306_present(kiwi_disp_t *base, const kiwi_spec_view_t *v)
{
    disp_ssd1306_t *d = (disp_ssd1306_t *)base;
    const kiwi_disp_hal_t *h = d->hal;
    kiwi_spec_rctx_t rc;
    if (kiwi_spec_rctx_init(&rc, v, d->cfg.width, d->cfg.height) != 0) return -1;

    uint8_t *page = d->buf + 1;
    for (unsigned p = 0; p < d->cfg.height / 8u; p++) {
        memset(page, 0, d->cfg.width);
        for (unsigned r = 0; r < 8; r++) {
            kiwi_spec_render_row(&rc, (uint16_t)(p * 8u + r), d->px);
            for (unsigned x = 0; x < d->cfg.width; x++) {
                uint8_t k = d->px[x];                /* "an": alles ausser Hintergrund, Flaeche, Toenung */
                if (k != KIWI_PX_BG && k != KIWI_PX_FILL && k != KIWI_PX_TINT) page[x] |= (uint8_t)(1u << r);
            }
        }
        const uint8_t pos[3] = { (uint8_t)(0xB0u | p), (uint8_t)(d->cfg.col_offset & 0x0Fu),
                                 (uint8_t)(0x10u | (d->cfg.col_offset >> 4)) };
        if (write_cmd(d, pos, sizeof pos) != 0) return -1;
        int r;
        if (d->cfg.bus == KIWI_DISP_BUS_I2C) {
            d->buf[0] = 0x40;
            r = h->i2c_write(h->ctx, d->cfg.i2c_addr, d->buf, 1u + d->cfg.width);
        } else {
            h->spi_cs(h->ctx, 0);
            r = h->spi_write(h->ctx, 1, page, d->cfg.width);
            h->spi_cs(h->ctx, 1);
        }
        if (r != 0) return -1;
    }
    return 0;
}

static void ssd1306_close(kiwi_disp_t *base)
{
    const uint8_t off = 0xAE;                        /* Anzeige aus (schont das OLED) */
    write_cmd((disp_ssd1306_t *)base, &off, 1);
}

kiwi_disp_t *disp_ssd1306_init(disp_ssd1306_t *d, const kiwi_disp_hal_t *hal, const disp_ssd1306_cfg_t *cfg)
{
    if (!d || !hal || !cfg || !hal->delay_ms) return NULL;
    if (cfg->width < 16 || cfg->width > 132 || cfg->width > KIWI_WF_COLS_MAX + 4u) return NULL;
    if (cfg->height != 32 && cfg->height != 64) return NULL;
    if (cfg->bus == KIWI_DISP_BUS_I2C ? !hal->i2c_write : (!hal->spi_write || !hal->spi_cs)) return NULL;
    memset(d, 0, sizeof *d);
    d->hal = hal;
    d->cfg = *cfg;
    if (!d->cfg.i2c_addr) d->cfg.i2c_addr = 0x3C;
    d->base.width = cfg->width;
    d->base.height = cfg->height;
    d->base.open = ssd1306_open;
    d->base.present = ssd1306_present;
    d->base.close = ssd1306_close;
    return &d->base;
}
