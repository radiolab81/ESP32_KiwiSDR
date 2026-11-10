/**
 * @file kiwi_spec.c
 * @brief Glaettung, Autoskalierung, Seqlock - siehe kiwi_spec.h.
 *
 * Alle Pegel sind "dBm + 255" (ein Byte = ein dB). Intern wird mit 4 Nachkommabits
 * gerechnet (x16), damit die Glaettung ohne Gleitkomma nicht an Rundung haengen bleibt.
 */
#include "kiwi_spec.h"

#include <string.h>

#define AUTO_FLOOR_PCT   25      /* Rauschteppich = 25-%-Perzentil der Zeile            */
#define AUTO_HEADROOM_LO 5       /* dB unter dem Teppich als Anzeigeuntergrenze          */
#define AUTO_HEADROOM_HI 3       /* dB ueber der Spitze                                  */
#define AUTO_MIN_RANGE   25      /* kleinste angezeigte Spanne in dB (sonst Rauschen aufgeblasen) */
#define AUTO_PEAK_DECAY  3       /* dB pro Sekunde, mit denen die Spitzenreferenz sinkt  */

static void seq_begin(kiwi_spec_t *s)
{
    atomic_store_explicit(&s->guard, atomic_load_explicit(&s->guard, memory_order_relaxed) + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}
static void seq_end(kiwi_spec_t *s)
{
    atomic_store_explicit(&s->guard, atomic_load_explicit(&s->guard, memory_order_relaxed) + 1u, memory_order_release);
}

static void sink_line(void *ctx, const kiwi_wf_line_t *ln) { kiwi_spec_push((kiwi_spec_t *)ctx, ln); }

void kiwi_spec_init(kiwi_spec_t *s, uint8_t smooth, uint8_t autoscale, int lo_dbm, int hi_dbm)
{
    memset(s, 0, sizeof *s);
    s->smooth = smooth > 3 ? 3 : smooth;
    s->autoscale = autoscale ? 1 : 0;
    s->lo_dbm = (int16_t)lo_dbm;
    s->hi_dbm = (int16_t)hi_dbm;
    atomic_init(&s->guard, 0u);
    s->sink.line = sink_line;
    s->sink.ctx = s;
}

void kiwi_spec_reset(kiwi_spec_t *s)
{
    seq_begin(s);
    s->have = 0;
    s->v.valid = 0;
    s->v.ver++;
    seq_end(s);
}

/** Wert am p-ten Perzentil (Zaehlhistogramm, O(n + 256)). */
static unsigned percentile(const uint8_t *v, unsigned n, unsigned pct)
{
    uint8_t cnt[256];
    memset(cnt, 0, sizeof cnt);
    for (unsigned i = 0; i < n; i++) if (cnt[v[i]] < 255) cnt[v[i]]++;
    unsigned want = (n * pct) / 100u, acc = 0;
    for (unsigned b = 0; b < 256; b++) {
        acc += cnt[b];
        if (acc > want) return b;
    }
    return 255;
}

void kiwi_spec_push(kiwi_spec_t *s, const kiwi_wf_line_t *ln)
{
    unsigned n = ln->ncols;
    if (n == 0 || n > KIWI_WF_COLS_MAX) return;

    /* ---- Glaettung (nicht ueber eine verschobene Frequenzachse hinweg) ---- */
    int moved = !s->have || n != s->v.cols;
    if (!moved) {
        uint32_t d = ln->f_start_hz > s->f_start_prev ? ln->f_start_hz - s->f_start_prev : s->f_start_prev - ln->f_start_hz;
        moved = d > ln->span_hz / (2u * n);                  /* mehr als eine halbe Spalte */
    }
    unsigned sh = s->smooth;
    for (unsigned i = 0; i < n; i++) {
        uint16_t r16 = (uint16_t)(ln->cols[i] << 4);
        if (moved || sh == 0) s->sm[i] = r16;
        else s->sm[i] = (uint16_t)((int)s->sm[i] + (((int)r16 - (int)s->sm[i]) / (1 << sh)));
    }
    s->f_start_prev = ln->f_start_hz;

    /* ---- Skalierung ---- */
    int lo16, hi16;
    if (s->autoscale) {
        unsigned fl = percentile(ln->cols, n, AUTO_FLOOR_PCT);
        unsigned mx = 0;
        for (unsigned i = 0; i < n; i++) if (ln->cols[i] > mx) mx = ln->cols[i];
        uint32_t dt = s->have ? ln->t_ms - s->t_line_ms : 0;
        if (dt > 5000u) dt = 5000u;
        if (!s->have) { s->floor16 = (uint16_t)(fl << 4); s->peak16 = (uint16_t)(mx << 4); }
        else {
            s->floor16 = (uint16_t)((int)s->floor16 + (((int)(fl << 4) - (int)s->floor16) / 8));
            int pk = (int)s->peak16 - (int)((AUTO_PEAK_DECAY * 16u * dt) / 1000u);
            s->peak16 = (uint16_t)((int)(mx << 4) > pk ? (int)(mx << 4) : pk);
        }
        lo16 = (int)s->floor16 - AUTO_HEADROOM_LO * 16;
        hi16 = (int)s->peak16 + AUTO_HEADROOM_HI * 16;
        if (hi16 - lo16 < AUTO_MIN_RANGE * 16) hi16 = lo16 + AUTO_MIN_RANGE * 16;
    } else {
        lo16 = ((int)s->lo_dbm + 255) * 16;
        hi16 = ((int)s->hi_dbm + 255) * 16;
        if (hi16 <= lo16) hi16 = lo16 + 16;
    }

    /* ---- Veroeffentlichen ---- */
    seq_begin(s);
    for (unsigned i = 0; i < n; i++) {
        int l = ((int)s->sm[i] - lo16) * 255 / (hi16 - lo16);
        s->v.lvl[i] = (uint8_t)(l < 0 ? 0 : l > 255 ? 255 : l);
    }
    s->v.cols = (uint16_t)n;
    s->v.f_start_hz = ln->f_start_hz;
    s->v.span_hz = ln->span_hz;
    s->v.valid = 1;
    s->v.ver++;
    s->t_line_ms = ln->t_ms;
    s->have = 1;
    seq_end(s);
}

void kiwi_spec_set_tune(kiwi_spec_t *s, uint32_t f_hz, const char *mode)
{
    char m[8] = {0};
    if (mode) strncpy(m, mode, sizeof m - 1);
    if (s->v.f_tune_hz == f_hz && memcmp(s->v.mode, m, sizeof m) == 0) return;
    int lo = 0, hi = 0;
    if (kiwi_mode_passband(m, &lo, &hi) != 0) { lo = 0; hi = 0; }
    seq_begin(s);
    s->v.f_tune_hz = f_hz;
    memcpy(s->v.mode, m, sizeof m);
    s->v.pb_lo_hz = (int16_t)lo;
    s->v.pb_hi_hz = (int16_t)hi;
    s->v.ver++;
    seq_end(s);
}

int kiwi_spec_snapshot(kiwi_spec_t *s, kiwi_spec_view_t *out, uint32_t now_ms, uint32_t stale_ms)
{
    for (int tries = 0; tries < 8; tries++) {
        unsigned g1 = atomic_load_explicit(&s->guard, memory_order_acquire);
        if (g1 & 1u) continue;                       /* Schreiber aktiv */
        memcpy(out, &s->v, sizeof *out);
        uint32_t t = s->t_line_ms;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->guard, memory_order_relaxed) != g1) continue;
        if (out->valid && now_ms - t > stale_ms) { out->valid = 0; out->ver++; }   /* veraltet */
        return 1;
    }
    return 0;
}
