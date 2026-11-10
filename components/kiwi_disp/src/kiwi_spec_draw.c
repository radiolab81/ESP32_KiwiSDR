/**
 * @file kiwi_spec_draw.c
 * @brief Renderer und 5x7-Schrift - siehe kiwi_spec_draw.h.
 */
#include "kiwi_spec_draw.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 5x7-Schrift (Spalten, Bit 0 = oben). Zeichen: ' ' + - . 0-9 A-Z.      */
/* ------------------------------------------------------------------ */
static const uint8_t k_digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E}};
static const uint8_t k_alpha[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}};
static const uint8_t k_minus[5] = {0x08,0x08,0x08,0x08,0x08};
static const uint8_t k_plus[5]  = {0x08,0x08,0x3E,0x08,0x08};
static const uint8_t k_dot[5]   = {0x00,0x60,0x60,0x00,0x00};
static const uint8_t k_blank[5] = {0,0,0,0,0};

static const uint8_t *glyph(char c)
{
    if (c >= '0' && c <= '9') return k_digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return k_alpha[c - 'A'];
    if (c >= 'a' && c <= 'z') return k_alpha[c - 'a'];
    if (c == '-') return k_minus;
    if (c == '+') return k_plus;
    if (c == '.') return k_dot;
    return k_blank;
}

#define CHAR_W 6u   /* 5 Spalten + 1 Luecke */

static void text_row(uint8_t *px, uint16_t w, int x0, const char *s, unsigned grow)
{
    for (; *s; s++, x0 += (int)CHAR_W) {
        const uint8_t *g = glyph(*s);
        for (unsigned c = 0; c < 5; c++) {
            int x = x0 + (int)c;
            if (x >= 0 && x < (int)w && ((g[c] >> grow) & 1u)) px[x] = KIWI_PX_TEXT;
        }
    }
}

static int text_w(const char *s) { return (int)(strlen(s) * CHAR_W) - 1; }

/* ------------------------------------------------------------------ */

static void fmt_khz3(char *o, size_t n, uint32_t hz)
{
    snprintf(o, n, "%lu.%03lu", (unsigned long)(hz / 1000u), (unsigned long)(hz % 1000u));
}
static void fmt_khz1(char *o, size_t n, uint32_t hz)
{
    uint32_t t = (hz + 50u) / 100u;                         /* auf 0,1 kHz gerundet */
    snprintf(o, n, "%lu.%lu", (unsigned long)(t / 10u), (unsigned long)(t % 10u));
}

static int col_of(uint32_t f, uint32_t f0, uint32_t span, unsigned w, int *ok)
{
    int64_t d = (int64_t)f - (int64_t)f0;
    int64_t c = d * (int64_t)w / (int64_t)span;
    *ok = d >= 0 && c < (int64_t)w;
    return (int)c;
}

int kiwi_spec_rctx_init(kiwi_spec_rctx_t *r, const kiwi_spec_view_t *v, uint16_t w, uint16_t h)
{
    if (!r || !v || w < 16 || w > KIWI_WF_COLS_MAX || h < KIWI_DRAW_MIN_H) return -1;
    memset(r, 0, sizeof *r);
    r->w = w; r->h = h;
    r->y_axis = (uint16_t)(h - 11);
    r->plot_top = 9;
    r->plot_bot = (uint16_t)(r->y_axis - 1);
    r->y_label = (uint16_t)(h - 8);
    r->x_mark = -1; r->x_lo = r->x_hi = -1;
    r->have = v->valid && v->cols > 0 && v->span_hz > 0;

    /* Kopfzeile: Abstimmfrequenz links, Modus rechts. */
    if (v->f_tune_hz) {
        char f[12];
        fmt_khz3(f, sizeof f, v->f_tune_hz);
        snprintf(r->hdr_l, sizeof r->hdr_l, "%s KHZ", f);
    }
    snprintf(r->hdr_r, sizeof r->hdr_r, "%s", v->mode);

    if (!r->have) return 0;

    /* Kurve: Pegel -> y (0 = unten). */
    unsigned plot_h = (unsigned)(r->plot_bot - r->plot_top) + 1u;
    for (unsigned x = 0; x < w; x++) {
        unsigned c = (unsigned)((uint32_t)x * v->cols / w);
        r->yt[x] = (uint8_t)(r->plot_bot - (v->lvl[c] * (plot_h - 1u)) / 255u);
    }

    /* Markierung und Durchlassbereich. */
    int ok;
    if (v->f_tune_hz) {
        int c = col_of(v->f_tune_hz, v->f_start_hz, v->span_hz, w, &ok);
        if (ok) r->x_mark = (int16_t)c;
        int64_t flo = (int64_t)v->f_tune_hz + v->pb_lo_hz, fhi = (int64_t)v->f_tune_hz + v->pb_hi_hz;
        if (v->pb_hi_hz > v->pb_lo_hz && flo >= 0) {
            int cl = col_of((uint32_t)flo, v->f_start_hz, v->span_hz, w, &ok);
            if (!ok && flo < (int64_t)v->f_start_hz) cl = 0, ok = 1;
            int ch = col_of((uint32_t)fhi, v->f_start_hz, v->span_hz, w, &ok);
            if (fhi < (int64_t)v->f_start_hz) ch = -1;
            if (ch >= (int)w) ch = (int)w - 1;
            if (cl < 0) cl = 0;
            if (ch >= cl) { r->x_lo = (int16_t)cl; r->x_hi = (int16_t)ch; }
        }
    }

    /* Teilstriche auf "runden" absoluten Frequenzen (Schritt so, dass 4..10 Striche entstehen). */
    static const uint32_t steps[] = {1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000};
    uint32_t step = steps[(sizeof steps / sizeof steps[0]) - 1];
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (v->span_hz / steps[i] <= 10u) { step = steps[i]; break; }
    uint32_t f = ((v->f_start_hz + step - 1u) / step) * step;
    for (; f < v->f_start_hz + v->span_hz && r->nticks < KIWI_DRAW_MAX_TICKS; f += step) {
        int c = col_of(f, v->f_start_hz, v->span_hz, w, &ok);
        if (ok) r->tick_x[r->nticks++] = (int16_t)c;
    }

    fmt_khz1(r->ftr_l, sizeof r->ftr_l, v->f_start_hz);
    fmt_khz1(r->ftr_r, sizeof r->ftr_r, v->f_start_hz + v->span_hz);
    return 0;
}

void kiwi_spec_render_row(const kiwi_spec_rctx_t *r, uint16_t y, uint8_t *px)
{
    const unsigned w = r->w;
    memset(px, KIWI_PX_BG, w);

    if (y < 8u) {                                            /* ---- Kopfzeile ---- */
        if (y < 7u) {
            text_row(px, r->w, 0, r->hdr_l, y);
            text_row(px, r->w, (int)w - text_w(r->hdr_r), r->hdr_r, y);
        }
        return;
    }
    if (y >= r->y_label) {                                   /* ---- Beschriftung ---- */
        unsigned g = (unsigned)(y - r->y_label);
        if (g < 7u && r->have) {
            text_row(px, r->w, 0, r->ftr_l, g);
            text_row(px, r->w, (int)w - text_w(r->ftr_r), r->ftr_r, g);
        }
        return;
    }
    if (y == r->y_axis) { memset(px, KIWI_PX_AXIS, w); return; }
    if (y > r->y_axis) {                                     /* ---- Durchlassbereich ---- */
        if (r->have && r->x_lo >= 0)
            for (int x = r->x_lo; x <= r->x_hi; x++) px[x] = KIWI_PX_PASS;
        return;
    }
    if (y < r->plot_top) return;                             /* Luecke unter der Kopfzeile */

    /* ---- Kurvenbereich ---- */
    if (!r->have) {
        const char *msg = "NO DATA";
        unsigned mid = (r->plot_top + r->plot_bot) / 2u;
        if (y >= mid - 3u && y <= mid + 3u) text_row(px, r->w, ((int)w - text_w(msg)) / 2, msg, y - (mid - 3u));
        return;
    }
    if (r->x_lo >= 0)
        for (int x = r->x_lo; x <= r->x_hi; x++) px[x] = KIWI_PX_TINT;
    for (unsigned x = 0; x < w; x++) {
        unsigned a = r->yt[x ? x - 1u : 0u], b = r->yt[x];
        unsigned top = a < b ? a : b, bot = a < b ? b : a;
        if (y >= top && y <= bot) px[x] = KIWI_PX_TRACE;
        else if (y > bot) px[x] = KIWI_PX_FILL;
    }
    if (y + 2u >= r->y_axis)                                 /* Teilstriche: 2 Pixel ueber der Achse */
        for (unsigned i = 0; i < r->nticks; i++) px[r->tick_x[i]] = KIWI_PX_AXIS;
    if (r->x_mark >= 0 && (((unsigned)y - r->plot_top) & 1u) == 0u) px[r->x_mark] = KIWI_PX_MARK;
}
