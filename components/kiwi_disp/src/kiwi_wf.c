/**
 * @file kiwi_wf.c
 * @brief Wasserfall-/Spektrum-Client (/W/F) - siehe kiwi_wf.h.
 *
 * ENTWURFSENTSCHEIDUNGEN
 *  - Eigene, schlanke Verbindung statt Aenderungen an kiwi_client.c: Der Audio-Kern
 *    bleibt bis auf zwei Zeilen unveraendert. Dafuer ist der kleine HTTP-Upgrade-
 *    Handshake hier noch einmal vorhanden (bewusst, ca. 40 Zeilen).
 *  - Gleicher Zeitstempel wie das Audio (master->ts_id): eine Sitzung, kein zweiter Nutzer.
 *  - Jeder Frame beschreibt sein Fenster selbst (x_bin, Zoom). Die Darstellung rechnet
 *    deshalb immer aus dem Frame-Kopf und nicht aus dem, was wir zuletzt angefordert haben.
 *  - ADPCM wird Nibble fuer Nibble dekodiert und sofort in Spalten einsortiert:
 *    kein 1024-Byte-Zeilenpuffer, und nach dem Ende des sichtbaren Bereichs wird abgebrochen.
 *
 * ADPCM-DETAIL (am Mitschnitt belegt): Komprimierte Zeilen haben 517 Byte = 1034 Samples.
 * Die ersten 10 sind Dekoder-Vorlauf (Einschwingen), danach folgen 1024 Bins. Ein Test an
 * einem 225-kHz-Rundfunktraeger zeigt: nur bei "ersten 10 verwerfen" stimmt die Frequenz
 * (224,97 kHz); der Python-Referenzclient verwirft die LETZTEN 10 und liegt damit 10 Bins daneben.
 */
#include "kiwi_wf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiwi_adpcm.h"
#include "kiwi_port.h"

#define WF_HDR_LEN      16u
#define WF_FLAG_COMP    0x00010000u
#define WF_ADPCM_PAD    10u
#define WF_COMP_BYTES   ((KIWI_WF_BINS + WF_ADPCM_PAD) / 2u)   /* 517 */
#define WF_RAW_BYTES    KIWI_WF_BINS
#define WF_DEFAULT_BW   32000000u
#define WF_ZOOM_MAX     14u
#define WF_MIN_VISIBLE  32u

#ifdef KIWI_DEBUG
#include <stdio.h>
#define WF_LOGD(...) do { fprintf(stderr, "[wf] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define WF_LOGD(...) do { } while (0)
#endif

/* ================================================================== */
/* Senden                                                             */
/* ================================================================== */

static int send_frame(kiwi_wf_t *w, uint8_t op, const uint8_t *pl, size_t n)
{
    uint8_t fr[256], mask[4];
    uint32_t r = kiwi_random_u32();
    memcpy(mask, &r, 4);
    size_t len = kiwi_ws_encode_frame(fr, sizeof fr, op, pl, n, mask);
    if (!len) return KIWI_ERR_ARG;
    if (!w->net) return KIWI_OK;                /* Wiedergabe-/Testbetrieb (kiwi_wf_feed): nichts zu senden */
    return kiwi_net_send_all(w->net, fr, len, 2000) == 0 ? KIWI_OK : KIWI_ERR_NET;
}

static int send_cmd(kiwi_wf_t *w, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return KIWI_ERR_ARG;
    WF_LOGD("-> %s", buf);
    return send_frame(w, KIWI_WS_OP_TEXT, (const uint8_t *)buf, (size_t)n);
}

/* ================================================================== */
/* Fenster (Zoom / Start)                                             */
/* ================================================================== */

/** Waehlt den groessten Zoom, dessen Fenster noch >= der gewuenschten Breite ist. */
static void choose_view(kiwi_wf_t *w)
{
    uint32_t want = w->cfg.span_hz;
    uint8_t zmax = WF_ZOOM_MAX;
    if (w->zoom_cap && w->zoom_cap < zmax) zmax = w->zoom_cap;
    uint8_t z = 0;
    while (z < zmax && (w->bw_hz >> (z + 1)) >= want) z++;
    w->zoom = z;
    uint32_t vs = w->bw_hz >> z;
    uint64_t vis = ((uint64_t)want * KIWI_WF_BINS + vs / 2u) / vs;
    if (vis > KIWI_WF_BINS) vis = KIWI_WF_BINS;
    if (vis < WF_MIN_VISIBLE) vis = WF_MIN_VISIBLE;
    w->visible = (uint16_t)vis;
}

/** Abstimmfrequenz im Basisband (Anzeigefrequenz minus Kiwi-Offset). 1 = gueltig. */
static int tune_bb(const kiwi_wf_t *w, uint32_t *bb)
{
    uint32_t f = w->cfg.master->cur_freq_hz, off = w->cfg.master->foff_hz;
    if (!f || f < off) return 0;
    *bb = f - off;
    return *bb < w->bw_hz;
}

/** Fordert Zoom + Fensterbeginn an (Zaehler im 2^24-Raster, wie der Browser). */
static int send_view(kiwi_wf_t *w, uint32_t start_bb)
{
    uint64_t counter = (((uint64_t)start_bb << 24) + w->bw_hz / 2u) / w->bw_hz;
    int rc = send_cmd(w, "SET zoom=%u start=%lu", (unsigned)w->zoom, (unsigned long)counter);
    if (rc) return rc;
    w->req_start_hz = start_bb;
    w->req_valid = 1;
    return 0;
}

/**
 * Fuehrt das Fenster nach, wenn der sichtbare Ausschnitt (span_hz um die Abstimmung)
 * aus dem angeforderten Fenster herauslaufen wuerde. Innerhalb der Reserve passiert
 * NICHTS (nur die Markierung wandert) - das schont den Server. Am Bandrand, wo das Fenster
 * nicht weiter verschiebbar ist, wird nichts erneut gesendet (keine Endlosschleife).
 */
static int retarget(kiwi_wf_t *w)
{
    uint32_t t;
    if (!tune_bb(w, &t)) return 0;
    uint32_t vs = w->bw_hz >> w->zoom, half = w->cfg.span_hz / 2u;
    uint32_t ns = t > vs / 2u ? t - vs / 2u : 0u;
    if (ns > w->bw_hz - vs) ns = w->bw_hz - vs;
    if (w->req_valid) {
        int outside = (t < w->req_start_hz + half) || (t + half > w->req_start_hz + vs);
        if (!outside || ns == w->req_start_hz) return 0;
    }
    return send_view(w, ns);
}

/** Konfiguration nach "wf_setup" - Reihenfolge wie im Browser-Mitschnitt. */
static int do_setup(kiwi_wf_t *w)
{
    int rc;
    if (w->setup_done) return 0;
    choose_view(w);
    if ((rc = send_cmd(w, "SET send_dB=1")) != 0) goto fail;
    w->req_valid = 0;
    if ((rc = retarget(w)) != 0) goto fail;
    if ((rc = send_cmd(w, "SET maxdb=0 mindb=-100")) != 0) goto fail;
    if ((rc = send_cmd(w, "SET interp=13")) != 0) goto fail;
    if ((rc = send_cmd(w, "SET window_func=2")) != 0) goto fail;
    if ((rc = send_cmd(w, "SET wf_speed=%u", (unsigned)w->cfg.speed)) != 0) goto fail;
    if ((rc = send_cmd(w, "SET wf_comp=1")) != 0) goto fail;
    if ((rc = send_cmd(w, "SET keepalive")) != 0) goto fail;
    w->t_last_ka = kiwi_time_ms();
    w->setup_done = 1;
    return 0;
fail:
    w->err = (kiwi_err_t)rc;
    return 1;
}

/* ================================================================== */
/* Zeilen                                                             */
/* ================================================================== */

static inline void put_max(uint8_t *o, unsigned v) { if (v > *o) *o = (uint8_t)v; }

/**
 * Reduziert die Bins [b0, b0+visible) auf ncols Spalten (Max-Hold).
 * Bresenham-artige Verteilung: jede Spalte bekommt floor/ceil(visible/ncols) Bins.
 */
static void reduce(const uint8_t *d, int comp, unsigned b0, unsigned visible,
                   uint8_t *out, unsigned ncols)
{
    memset(out, 0, ncols);
    unsigned col = 0, acc = 0, bin = 0;

    if (!comp) {
        for (bin = b0; bin < b0 + visible && col < ncols; bin++) {
            put_max(&out[col], d[bin]);
            acc += ncols;
            if (acc >= visible) { acc -= visible; col++; }
        }
        return;
    }

    kiwi_adpcm_t st;
    kiwi_adpcm_init(&st);
    int16_t s[8];
    unsigned pos = 0, end = b0 + visible;       /* pos: Sample-Index inkl. Vorlauf */
    for (unsigned b = 0; b < WF_COMP_BYTES && col < ncols; b += 4) {
        size_t nb = WF_COMP_BYTES - b < 4 ? WF_COMP_BYTES - b : 4;
        size_t ns = kiwi_adpcm_decode(&st, d + b, nb, s);
        for (size_t k = 0; k < ns; k++, pos++) {
            if (pos < WF_ADPCM_PAD) continue;
            bin = pos - WF_ADPCM_PAD;
            if (bin < b0) continue;
            if (bin >= end || col >= ncols) return;
            int v = s[k];
            put_max(&out[col], v < 0 ? 0u : v > 255 ? 255u : (unsigned)v);
            acc += ncols;
            if (acc >= visible) { acc -= visible; col++; }
        }
    }
}

static void on_line(kiwi_wf_t *w, const uint8_t *p, size_t n)
{
    if (n < WF_HDR_LEN) { w->st.bad++; return; }
    uint32_t x_bin = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    uint32_t fz    = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    uint32_t seq   = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    unsigned zoom = fz & 0xFFFFu;
    int comp = (fz & WF_FLAG_COMP) != 0;

    if (n - WF_HDR_LEN != (comp ? WF_COMP_BYTES : WF_RAW_BYTES)) { w->st.bad++; return; }
    /* Frames mit anderem Zoom sind noch die Voreinstellung des Servers (vor unserem "SET zoom"). */
    if (!w->setup_done || zoom != w->zoom) { w->st.stale++; return; }

    uint32_t t;
    if (!tune_bb(w, &t)) { w->st.stale++; return; }

    uint32_t vs = w->bw_hz >> zoom;
    uint32_t start = (uint32_t)(((uint64_t)x_bin * w->bw_hz) >> 24);
    int64_t tb = ((int64_t)t - (int64_t)start) * (int64_t)KIWI_WF_BINS / (int64_t)vs;
    if (tb < 0 || tb >= (int64_t)KIWI_WF_BINS) { w->st.stale++; return; }   /* Abstimmung nicht im Fenster */

    /* Sichtbaren Ausschnitt um die Abstimmung legen, im Fenster festgehalten. */
    int64_t b0 = tb - (int64_t)(w->visible / 2u);
    if (b0 < 0) b0 = 0;
    if (b0 > (int64_t)(KIWI_WF_BINS - w->visible)) b0 = (int64_t)(KIWI_WF_BINS - w->visible);

    kiwi_wf_line_t *ln = &w->line;
    ln->ncols = w->cfg.cols;
    reduce(p + WF_HDR_LEN, comp, (unsigned)b0, w->visible, ln->cols, w->cfg.cols);
    ln->f_start_hz = start + (uint32_t)(((uint64_t)b0 * vs) / KIWI_WF_BINS) + w->cfg.master->foff_hz;
    ln->span_hz = (uint32_t)(((uint64_t)w->visible * vs) / KIWI_WF_BINS);
    ln->seq = seq;
    ln->t_ms = kiwi_time_ms();
    w->st.lines++;
    if (w->cfg.sink && w->cfg.sink->line) w->cfg.sink->line(w->cfg.sink->ctx, ln);
}

/* ================================================================== */
/* Nachrichten                                                        */
/* ================================================================== */

static int on_param(kiwi_wf_t *w, const char *name, const char *val)
{
    WF_LOGD("MSG %s=%s", name, val ? val : "");
    if (!strcmp(name, "too_busy")) { w->err = KIWI_ERR_BUSY; return 1; }
    if (!strcmp(name, "down"))     { w->err = KIWI_ERR_DOWN; return 1; }
    if (!strcmp(name, "redirect")) { w->err = KIWI_ERR_REDIRECT; return 1; }
    if (!strcmp(name, "badp") && val && strcmp(val, "0") != 0) {
        w->err = (val[0] >= '5' && val[0] <= '7') ? KIWI_ERR_BUSY : KIWI_ERR_PASSWORD;
        return 1;
    }
    if (!strcmp(name, "bandwidth") && val) {
        unsigned long b = strtoul(val, NULL, 10);
        if (b >= 1000000ul && b <= 400000000ul) w->bw_hz = (uint32_t)b;
    } else if (!strcmp(name, "wf_fft_size") && val) {
        if (atoi(val) != (int)KIWI_WF_BINS) { w->err = KIWI_ERR_UNSUPPORTED; return 1; }
    } else if (!strcmp(name, "zoom_cap") && val) {
        int z = atoi(val);
        if (z > 0 && z <= (int)WF_ZOOM_MAX) w->zoom_cap = (uint8_t)z;
    } else if (!strcmp(name, "wf_setup")) {
        if (do_setup(w)) return 1;
    }
    return 0;
}

static int on_msg(kiwi_wf_t *w, char *s)
{
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        char *name = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = '\0';
        char *val = strchr(name, '=');
        if (val) *val++ = '\0';
        if (on_param(w, name, val)) return 1;
    }
    return 0;
}

static int on_ws_message(void *user, uint8_t op, const uint8_t *p, size_t n)
{
    kiwi_wf_t *w = (kiwi_wf_t *)user;
    switch (op) {
    case KIWI_WS_OP_CLOSE: w->err = KIWI_ERR_CLOSED; return 1;
    case KIWI_WS_OP_PING:
        if (w->net && send_frame(w, KIWI_WS_OP_PONG, p, n) != 0) { w->err = KIWI_ERR_NET; return 1; }
        return 0;
    case KIWI_WS_OP_TEXT:
    case KIWI_WS_OP_BINARY: break;
    default: return 0;
    }
    if (n < 3) return 0;
    if (!memcmp(p, "W/F", 3)) { on_line(w, p, n); return 0; }
    if (!memcmp(p, "MSG", 3)) {
        if (n < 4) return 0;
        uint8_t *m = (uint8_t *)p;
        m[n] = '\0';                              /* rxbuf hat dafuer ein Byte Reserve */
        return on_msg(w, (char *)(m + 4));
    }
    return 0;
}

/* ================================================================== */
/* Oeffentliche API                                                   */
/* ================================================================== */

kiwi_err_t kiwi_wf_init(kiwi_wf_t *w, const kiwi_wf_config_t *cfg)
{
    if (!w || !cfg || !cfg->master) return KIWI_ERR_ARG;
    if (cfg->cols < 16 || cfg->cols > KIWI_WF_COLS_MAX) return KIWI_ERR_ARG;
    if (cfg->span_hz < 1000u) return KIWI_ERR_ARG;
    memset(w, 0, sizeof *w);
    w->cfg = *cfg;
    if (!w->cfg.speed) w->cfg.speed = 2;
    if (w->cfg.speed > 4) w->cfg.speed = 4;
    if (!w->cfg.connect_timeout_ms) w->cfg.connect_timeout_ms = 10000;
    if (!w->cfg.idle_timeout_ms) w->cfg.idle_timeout_ms = 10000;
    w->bw_hz = WF_DEFAULT_BW;
    choose_view(w);
    kiwi_ws_rx_init(&w->ws, w->rxbuf, sizeof w->rxbuf - 1);
    return KIWI_OK;
}

kiwi_err_t kiwi_wf_connect(kiwi_wf_t *w)
{
    if (!w || w->net) return KIWI_ERR_ARG;
    const kiwi_client_t *m = w->cfg.master;
    if (!m->ts_id) return KIWI_ERR_ARG;           /* Audio-Sitzung muss schon verbunden sein */

    w->err = 0; w->setup_done = 0; w->req_valid = 0;
    memset(&w->st, 0, sizeof w->st);
    kiwi_ws_rx_init(&w->ws, w->rxbuf, sizeof w->rxbuf - 1);
    w->bw_hz = WF_DEFAULT_BW;
    w->zoom_cap = 0;
    choose_view(w);

    uint32_t to = w->cfg.connect_timeout_ms;
    if (kiwi_net_connect(&w->net, m->cfg.host, m->cfg.port, to) != 0) { w->net = NULL; return KIWI_ERR_NET; }

    uint8_t key[16];
    for (int i = 0; i < 16; i += 4) { uint32_t r = kiwi_random_u32(); memcpy(key + i, &r, 4); }
    const char *prefix = m->cfg.path_prefix ? m->cfg.path_prefix : "/ws/kiwi";
    char path[80], origin[112];
    snprintf(path, sizeof path, "%s/%lu/W/F", prefix, (unsigned long)m->ts_id);   /* GLEICHER Zeitstempel */
    snprintf(origin, sizeof origin, "http://%s:%u", m->cfg.host, (unsigned)m->cfg.port);

    kiwi_err_t rc = KIWI_ERR_ARG;
    size_t rl = kiwi_ws_build_request((char *)w->rxbuf, sizeof w->rxbuf, m->cfg.host, m->cfg.port, path, origin, key);
    if (!rl) goto fail;
    if (kiwi_net_send_all(w->net, w->rxbuf, rl, 3000) != 0) { rc = KIWI_ERR_NET; goto fail; }

    size_t have = 0, end = 0;
    uint32_t t0 = kiwi_time_ms();
    for (;;) {
        int r = kiwi_net_recv(w->net, w->tmp + have, sizeof w->tmp - have, 500);
        if (r > 0) {
            have += (size_t)r;
            for (size_t i = 3; i < have && !end; i++)
                if (!memcmp(w->tmp + i - 3, "\r\n\r\n", 4)) end = i + 1;
            if (end) break;
            if (have == sizeof w->tmp) { rc = KIWI_ERR_HANDSHAKE; goto fail; }
        } else if (r == KIWI_NET_TIMEOUT) {
            if (kiwi_time_ms() - t0 > to) { rc = KIWI_ERR_TIMEOUT; goto fail; }
        } else {
            rc = KIWI_ERR_NET; goto fail;
        }
    }
    int status = kiwi_ws_parse_status((const char *)w->tmp, have);
    if (status != 101) { rc = (status >= 300 && status < 400) ? KIWI_ERR_REDIRECT : KIWI_ERR_HANDSHAKE; goto fail; }

    w->t_last_rx = w->t_last_ka = kiwi_time_ms();
    if ((rc = (kiwi_err_t)send_cmd(w, "SET auth t=kiwi p=%s", m->cfg.password[0] ? m->cfg.password : "#")) != 0) goto fail;
    if ((rc = (kiwi_err_t)send_cmd(w, "SERVER DE CLIENT kiwi_esp32 W/F")) != 0) goto fail;

    if (have > end) {
        int pr = kiwi_ws_rx_push(&w->ws, w->tmp + end, have - end, on_ws_message, w);
        if (pr < 0) { rc = KIWI_ERR_PROTO; goto fail; }
        if (pr > 0) { rc = w->err ? w->err : KIWI_ERR_CLOSED; goto fail; }
    }
    return KIWI_OK;
fail:
    kiwi_wf_close(w);
    return rc;
}

kiwi_err_t kiwi_wf_poll(kiwi_wf_t *w, uint32_t timeout_ms)
{
    if (!w || !w->net) return KIWI_ERR_CLOSED;

    /* Bis zu 8 Lesevorgaenge je Aufruf: ein Wasserfall-Frame (1040 B) braucht mehrere. */
    for (int i = 0; i < 8; i++) {
        int r = kiwi_net_recv(w->net, w->tmp, sizeof w->tmp, i == 0 ? timeout_ms : 0);
        uint32_t now = kiwi_time_ms();
        if (r > 0) {
            w->t_last_rx = now;
            int pr = kiwi_ws_rx_push(&w->ws, w->tmp, (size_t)r, on_ws_message, w);
            if (pr < 0) return KIWI_ERR_PROTO;
            if (pr > 0) return w->err ? w->err : KIWI_ERR_CLOSED;
        } else if (r == KIWI_NET_TIMEOUT) {
            if (now - w->t_last_rx > w->cfg.idle_timeout_ms) return KIWI_ERR_TIMEOUT;
            break;
        } else if (r == KIWI_NET_CLOSED) {
            return KIWI_ERR_CLOSED;
        } else {
            return KIWI_ERR_NET;
        }
    }

    if (w->setup_done && retarget(w) != 0) return KIWI_ERR_NET;

    uint32_t now = kiwi_time_ms();
    if (now - w->t_last_ka >= 1000u) {
        w->t_last_ka = now;
        if (send_cmd(w, "SET keepalive") != 0) return KIWI_ERR_NET;
    }
    return KIWI_OK;
}

void kiwi_wf_close(kiwi_wf_t *w)
{
    if (!w || !w->net) return;
    const uint8_t code[2] = { 0x03, 0xE9 };       /* 1001 "going away" */
    if (send_frame(w, KIWI_WS_OP_CLOSE, code, 2) == KIWI_OK) {
        /* Auslaufen lassen (kein RST statt FIN), hoechstens ca. 300 ms. */
        uint32_t t0 = kiwi_time_ms();
        while (kiwi_time_ms() - t0 < 300u) {
            int r = kiwi_net_recv(w->net, w->tmp, sizeof w->tmp, 50);
            if (r == KIWI_NET_CLOSED || r == KIWI_NET_ERR) break;
        }
    }
    kiwi_net_close(w->net);
    w->net = NULL;
}

kiwi_err_t kiwi_wf_feed(kiwi_wf_t *w, const uint8_t *data, size_t n)
{
    if (!w || !data) return KIWI_ERR_ARG;
    int pr = kiwi_ws_rx_push(&w->ws, data, n, on_ws_message, w);
    if (pr < 0) return KIWI_ERR_PROTO;
    if (pr > 0) return w->err ? w->err : KIWI_ERR_CLOSED;
    return KIWI_OK;
}
