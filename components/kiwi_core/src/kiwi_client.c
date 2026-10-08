/**
 * @file kiwi_client.c
 * @brief KiwiSDR-Audioclient. Protokollbeschreibung: siehe kiwi_client.h.
 *
 * AUFBAU DIESER DATEI
 *   1. Hilfsfunktionen (Zahlen parsen, Modus-Tabelle, Senden)
 *   2. Verarbeitung von "MSG"-Nachrichten  (Steuerkanal, Text)
 *   3. Verarbeitung von "SND"-Frames       (Audiokanal, binaer)
 *   4. Oeffentliche API (init/connect/poll/close/set_tune/command)
 */
#include "kiwi_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KIWI_DEBUG
#define KIWI_LOGD(...) do { printf("[kiwi] " __VA_ARGS__); printf("\n"); } while (0)
#else
#define KIWI_LOGD(...) do { } while (0)
#endif

/* Flags im SND-Frame (Byte 3) */
#define SND_FLAG_ADC_OVFL   0x02
#define SND_FLAG_STEREO     0x08
#define SND_FLAG_COMPRESSED 0x10

/* Audio-Frames werden in kleinen Scheiben dekodiert, damit der Stack klein
 * bleibt: 256 ADPCM-Bytes -> 512 Samples -> 1 KB Stack. */
#define SLICE_BYTES   256
#define SLICE_SAMPLES (2 * SLICE_BYTES)

/* ================================================================== */
/* 1. Hilfsfunktionen                                                 */
/* ================================================================== */

const char *kiwi_strerror(int err)
{
    switch (err) {
    case KIWI_OK:              return "ok";
    case KIWI_ERR_ARG:         return "ungueltiger Parameter";
    case KIWI_ERR_NET:         return "Netzwerkfehler";
    case KIWI_ERR_HANDSHAKE:   return "WebSocket-Handshake abgelehnt";
    case KIWI_ERR_PROTO:       return "Protokollfehler";
    case KIWI_ERR_BUSY:        return "Kiwi ausgelastet oder IP bereits verbunden";
    case KIWI_ERR_PASSWORD:    return "Passwort falsch / Zugang verweigert";
    case KIWI_ERR_DOWN:        return "Kiwi meldet 'down'";
    case KIWI_ERR_REDIRECT:    return "Weiterleitung auf anderen Host";
    case KIWI_ERR_CLOSED:      return "Verbindung vom Kiwi beendet";
    case KIWI_ERR_TIMEOUT:     return "Zeitueberschreitung (keine Daten)";
    case KIWI_ERR_SINK:        return "Audio-Senke meldet Fehler";
    case KIWI_ERR_UNSUPPORTED: return "nicht unterstuetzt (Stereo/IQ)";
    default:                   return "unbekannter Fehler";
    }
}

/**
 * Dezimalzahl "123.456" -> 123456 (Festkomma, 3 Nachkommastellen), ohne
 * Gleitkomma (spart auf dem ESP32 Code und vermeidet printf-Float-Probleme).
 * Weitere Nachkommastellen werden abgeschnitten.
 */
static int parse_fixed3(const char *s, uint32_t *out)
{
    uint32_t ip = 0, frac = 0, digits = 0;
    int have_int = 0;

    while (*s == ' ') s++;
    for (; isdigit((unsigned char)*s); s++) {
        have_int = 1;
        ip = ip * 10 + (uint32_t)(*s - '0');
        if (ip > 4000000u) return -1;           /* Ueberlaufschutz */
    }
    if (*s == '.') {
        s++;
        for (; isdigit((unsigned char)*s); s++) {
            have_int = 1;
            if (digits < 3) { frac = frac * 10 + (uint32_t)(*s - '0'); digits++; }
        }
    }
    if (!have_int) return -1;
    while (digits < 3) { frac *= 10; digits++; }
    while (*s == ' ' || *s == '\r' || *s == '\n') s++;
    if (*s != '\0') return -1;                  /* Muell am Ende */
    *out = ip * 1000u + frac;
    return 0;
}

int kiwi_parse_khz(const char *s, uint32_t *hz)
{
    return parse_fixed3(s, hz);   /* kHz mit 3 Nachkommastellen = Hz */
}

/**
 * Demodulationsmodi mit Standard-Durchlassbereich (Hz relativ zur
 * Traegerfrequenz). Werte aus kiwiclient (_default_passbands).
 * Nur MONO-Modi: Stereo/IQ-Modi (iq, drm, sas, qam) liefern andere
 * Frame-Formate und sind hier bewusst nicht enthalten.
 */
typedef struct { const char *name; int16_t lo, hi; } kiwi_mode_def_t;
static const kiwi_mode_def_t k_modes[] = {
    { "am",   -4900,  4900 }, { "amn",  -2500,  2500 }, { "amw",  -6000,  6000 },
    { "sam",  -4900,  4900 }, { "sal",  -4900,     0 }, { "sau",      0,  4900 },
    { "lsb",  -2700,  -300 }, { "lsn",  -2400,  -300 }, { "usb",    300,  2700 },
    { "usn",    300,  2400 }, { "cw",     300,   700 }, { "cwn",    470,   530 },
    { "nbfm", -6000,  6000 }, { "nnfm", -3000,  3000 },
};

/** Sucht Modus (Gross/Kleinschreibung egal). Schreibt Kleinbuchstaben in @p norm. */
static const kiwi_mode_def_t *find_mode(const char *name, char norm[8])
{
    char tmp[8];
    size_t i = 0;
    if (!name) return NULL;
    for (; name[i] && i < sizeof tmp - 1; i++) tmp[i] = (char)tolower((unsigned char)name[i]);
    if (name[i]) return NULL;                   /* zu lang */
    tmp[i] = '\0';
    for (size_t k = 0; k < sizeof k_modes / sizeof k_modes[0]; k++) {
        if (strcmp(tmp, k_modes[k].name) == 0) {
            if (norm) memcpy(norm, tmp, i + 1);
            return &k_modes[k];
        }
    }
    return NULL;
}

/** Sendet einen maskierten WebSocket-Frame (Client -> Server). */
static int send_frame(kiwi_client_t *c, uint8_t op, const uint8_t *pl, size_t n)
{
    uint8_t fr[256], mask[4];
    uint32_t r = kiwi_random_u32();
    memcpy(mask, &r, 4);
    size_t len = kiwi_ws_encode_frame(fr, sizeof fr, op, pl, n, mask);
    if (!len) return KIWI_ERR_ARG;
    return kiwi_net_send_all(c->net, fr, len, 2000) == 0 ? KIWI_OK : KIWI_ERR_NET;
}

/** printf-artig: sendet einen "SET ..."-Befehl als Text-Frame. */
static int send_cmd(kiwi_client_t *c, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return KIWI_ERR_ARG;
    KIWI_LOGD("-> %s", buf);
    return send_frame(c, KIWI_WS_OP_TEXT, (const uint8_t *)buf, (size_t)n);
}

/** Schickt "SET mod=... freq=..." - das ist das eigentliche "Abstimmen". */
static int send_tune(kiwi_client_t *c)
{
    const kiwi_mode_def_t *m = find_mode(c->cur_mode, NULL);
    if (!m) return KIWI_ERR_ARG;
    /* Hat der Kiwi einen Frequenzversatz (Aufwaertskonverter), erwartet das
     * Protokoll die Basisbandfrequenz = Anzeigefrequenz - Versatz. */
    if (c->cur_freq_hz < c->foff_hz) return KIWI_ERR_ARG;
    uint32_t bb = c->cur_freq_hz - c->foff_hz;
    return send_cmd(c, "SET mod=%s low_cut=%d high_cut=%d freq=%lu.%03lu",
                    m->name, m->lo, m->hi,
                    (unsigned long)(bb / 1000u), (unsigned long)(bb % 1000u));
}

/** Uebernimmt einen aus anderem Task angeforderten Abstimmwunsch. */
static int apply_pending(kiwi_client_t *c)
{
    if (!atomic_exchange(&c->pend, 0)) return KIWI_OK;
    c->cur_freq_hz = c->pend_freq_hz;
    memcpy(c->cur_mode, c->pend_mode, sizeof c->cur_mode);
    return c->setup_done ? send_tune(c) : KIWI_OK;
}

/* ================================================================== */
/* 2. MSG-Nachrichten (Textparameter "name=wert")                     */
/* ================================================================== */

/** Wird aufgerufen, sobald Abtastrate bekannt ist: sendet die Grundkonfiguration. */
static int do_setup(kiwi_client_t *c)
{
    int rc;

    /* Reihenfolge wie im Referenzclient (kiwirecorder.py). */
    if ((rc = send_cmd(c, "SET squelch=0 max=0")) != 0) goto fail;
    if ((rc = send_cmd(c, "SET genattn=0")) != 0) goto fail;
    if ((rc = send_cmd(c, "SET gen=0 mix=-1")) != 0) goto fail;
    if ((rc = send_cmd(c, "SET ident_user=%s", c->cfg.user_name ? c->cfg.user_name : "kiwi_esp32")) != 0) goto fail;

    c->setup_done = 1;
    apply_pending(c);                            /* evtl. schon neuer Wunsch */
    if ((rc = send_tune(c)) != 0) goto fail;

    /* AGC an (automatische Verstaerkungsregelung) mit Standardwerten. */
    if ((rc = send_cmd(c, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50")) != 0) goto fail;
    if (!c->cfg.compression &&
        (rc = send_cmd(c, "SET compression=0")) != 0) goto fail;
    if ((rc = send_cmd(c, "SET keepalive")) != 0) goto fail;
    c->t_last_ka = kiwi_time_ms();
    return 0;
fail:
    c->err = rc;
    return 1;
}

/** Behandelt einen einzelnen Parameter einer MSG-Nachricht. 0 = weiter. */
static int on_param(kiwi_client_t *c, const char *name, const char *val)
{
    KIWI_LOGD("MSG %s=%s", name, val ? val : "");

    /* ---- Fehlermeldungen des Servers ---- */
    if (!strcmp(name, "too_busy")) { c->err = KIWI_ERR_BUSY; return 1; }
    if (!strcmp(name, "down"))     { c->err = KIWI_ERR_DOWN; return 1; }
    if (!strcmp(name, "redirect")) { c->err = KIWI_ERR_REDIRECT; return 1; }
    if (!strcmp(name, "badp") && val && strcmp(val, "0") != 0) {
        /* badp=0 heisst "Anmeldung ok"; 1..4 Passwortprobleme, 5..7 belegt */
        c->err = (val[0] >= '5' && val[0] <= '7') ? KIWI_ERR_BUSY : KIWI_ERR_PASSWORD;
        return 1;
    }

    /* ---- Nutzdaten ---- */
    if (!strcmp(name, "version_maj") && val) { c->ver_maj = (uint32_t)atoi(val); }
    else if (!strcmp(name, "version_min") && val) { c->ver_min = (uint32_t)atoi(val); }
    else if (!strcmp(name, "freq_offset") && val) {
        uint32_t hz;                             /* Wert in kHz */
        if (parse_fixed3(val, &hz) == 0) c->foff_hz = hz;
    }
    else if (!strcmp(name, "audio_rate") && val) {
        /* Der Server nennt seine Audio-Abtastrate (12000 Hz). Der Client muss
         * mit "AR OK" quittieren, sonst beginnt der Audiostrom nicht. */
        c->audio_rate = (uint32_t)atoi(val);
        c->st.audio_rate = c->audio_rate;
        if (c->audio_rate < 4000 || c->audio_rate > 48000) { c->err = KIWI_ERR_PROTO; return 1; }
        int rc = send_cmd(c, "SET AR OK in=%lu out=44100", (unsigned long)c->audio_rate);
        if (rc) { c->err = rc; return 1; }
    }
    else if (!strcmp(name, "sample_rate") && val) {
        /* Kommt beim KiwiSDR v1.9xx als ERSTE Nachricht (z. B. 11998.895009 =
         * tatsaechliche ADC-Taktrate/Dezimation, nicht exakt 12000). Zeigt an,
         * dass der Kanal bereit ist -> jetzt konfigurieren. Die nominelle
         * Audiorate kommt spaeter ueber "audio_rate=". */
        uint32_t mhz;
        if (parse_fixed3(val, &mhz) == 0) c->sample_rate_mhz = mhz;
        if (do_setup(c)) return 1;
    }
    return 0;
}

/** Zerlegt "a=1 b=2 c" in Parameter. Verändert den Puffer (NUL-Terminierung). */
static int on_msg(kiwi_client_t *c, char *s)
{
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        char *name = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = '\0';
        char *val = strchr(name, '=');
        if (val) *val++ = '\0';
        if (on_param(c, name, val)) return 1;
    }
    return 0;
}

/* ================================================================== */
/* 3. SND-Frames (Audio)                                              */
/* ================================================================== */

static int sink_write(kiwi_client_t *c, const int16_t *pcm, size_t n)
{
    c->st.samples += (uint32_t)n;
    if (c->cfg.sink && c->cfg.sink->write &&
        c->cfg.sink->write(c->cfg.sink->ctx, pcm, n) < 0) {
        c->err = KIWI_ERR_SINK;
        return 1;
    }
    return 0;
}

/**
 * @param d  Frame-Inhalt NACH dem 3-Byte-Tag "SND"
 * @param n  Laenge von d
 * Aufbau: [0]=flags [1..4]=seq LE [5..6]=smeter BE [7..]=Audio
 */
static int on_snd(kiwi_client_t *c, const uint8_t *d, size_t n)
{
    if (n < 7) return 0;
    uint8_t  flags = d[0];
    uint32_t seq   = (uint32_t)d[1] | ((uint32_t)d[2] << 8) |
                     ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
    unsigned smeter = ((unsigned)d[5] << 8) | d[6];
    const uint8_t *audio = d + 7;
    size_t alen = n - 7;

    /* RSSI[dBm] = 0.1*smeter - 127  ->  in Zehntel-dBm: smeter - 1270 */
    c->st.rssi_tenths_dbm = (int)smeter - 1270;

    /* Senke erst beim ersten Audioframe oeffnen: dann ist "audio_rate=" sicher
     * bekannt. Fehlt sie, wird sample_rate auf ganze kHz gerundet (11998.9 -> 12000). */
    if (!c->sink_open) {
        if (!c->audio_rate)
            c->audio_rate = ((c->sample_rate_mhz / 1000u + 500u) / 1000u) * 1000u;
        c->st.audio_rate = c->audio_rate;
        if (c->audio_rate < 4000 || c->audio_rate > 48000) { c->err = KIWI_ERR_PROTO; return 1; }
        if (c->cfg.sink && c->cfg.sink->open &&
            c->cfg.sink->open(c->cfg.sink->ctx, c->audio_rate) != 0) {
            c->err = KIWI_ERR_SINK;
            return 1;
        }
        c->sink_open = 1;
    }

    /* Der allererste Frame enthaelt evtl. noch Audio des vorigen Nutzers dieses
     * Kanals (siehe kiwirecorder: "first audio buffer is leftover ..."). Er wird
     * deshalb NICHT ausgegeben. Er wird aber trotzdem DEKODIERT, damit der
     * ADPCM-Dekoder im gleichen Zustand bleibt wie der Kodierer im Server -
     * sonst bliebe ein dauerhafter Gleichspannungsversatz im Signal. */
    int emit = c->first_dropped;
    c->first_dropped = 1;

    /* Luecken in der Sequenznummer = verlorene Frames (TCP verliert nichts,
     * aber der Server verwirft bei Ueberlast selbst Pakete). */
    if (c->have_seq && seq != c->expect_seq) c->st.lost_frames += seq - c->expect_seq;
    c->expect_seq = seq + 1;
    c->have_seq = 1;
    if (emit) c->st.frames++;

    if (flags & SND_FLAG_STEREO) return 0;      /* IQ/DRM: hier nicht unterstuetzt */

    int16_t pcm[SLICE_SAMPLES];

    if (flags & SND_FLAG_COMPRESSED) {
        /* ADPCM: 1 Byte = 2 Samples. Dekoderzustand laeuft frameuebergreifend. */
        while (alen) {
            size_t k = alen > SLICE_BYTES ? SLICE_BYTES : alen;
            size_t ns = kiwi_adpcm_decode(&c->adpcm, audio, k, pcm);
            if (emit && sink_write(c, pcm, ns)) return 1;
            audio += k;
            alen -= k;
        }
    } else {
        /* Rohes PCM: int16, BIG endian (Netzwerkbyteordnung). Wir setzen es
         * bytegenau zusammen -> unabhaengig von der Prozessor-Endianness. */
        size_t total = alen / 2;
        while (total) {
            size_t k = total > SLICE_SAMPLES ? SLICE_SAMPLES : total;
            for (size_t i = 0; i < k; i++)
                pcm[i] = (int16_t)(((uint16_t)audio[2 * i] << 8) | audio[2 * i + 1]);
            if (emit && sink_write(c, pcm, k)) return 1;
            audio += 2 * k;
            total -= k;
        }
    }
    return 0;
}

/** Callback des WebSocket-Parsers: eine komplette Nachricht ist da. */
static int on_ws_message(void *user, uint8_t op, const uint8_t *p, size_t n)
{
    kiwi_client_t *c = (kiwi_client_t *)user;

    switch (op) {
    case KIWI_WS_OP_CLOSE: c->err = KIWI_ERR_CLOSED; return 1;
    case KIWI_WS_OP_PING:                       /* Pflicht: mit Pong antworten */
        if (send_frame(c, KIWI_WS_OP_PONG, p, n) != 0) { c->err = KIWI_ERR_NET; return 1; }
        return 0;
    case KIWI_WS_OP_TEXT:
    case KIWI_WS_OP_BINARY: break;
    default: return 0;
    }

    if (n < 3) return 0;
    if (!memcmp(p, "SND", 3)) return on_snd(c, p + 3, n - 3);
    if (!memcmp(p, "MSG", 3)) {
        if (n < 4) return 0;
        /* Hinter dem Tag folgt ein Trennbyte, dann die Parameter. Zum Parsen
         * wird NUL-terminiert (rxbuf hat dafuer ein Byte Reserve). */
        uint8_t *w = (uint8_t *)p;
        w[n] = '\0';
        return on_msg(c, (char *)(w + 4));
    }
    return 0;           /* "W/F" (Wasserfall), "EXT" ... interessieren uns nicht */
}

/* ================================================================== */
/* 4. Oeffentliche API                                                */
/* ================================================================== */

kiwi_err_t kiwi_client_init(kiwi_client_t *c, const kiwi_config_t *cfg)
{
    if (!c || !cfg || !cfg->host) return KIWI_ERR_ARG;
    char norm[8];
    const char *mode = cfg->mode ? cfg->mode : "am";
    if (!find_mode(mode, norm)) return KIWI_ERR_ARG;
    if (cfg->freq_hz == 0 || cfg->freq_hz > 32000000u) return KIWI_ERR_ARG;

    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (!c->cfg.port) c->cfg.port = 8073;
    if (!c->cfg.password) c->cfg.password = "";
    if (!c->cfg.connect_timeout_ms) c->cfg.connect_timeout_ms = 10000;
    if (!c->cfg.idle_timeout_ms) c->cfg.idle_timeout_ms = 10000;
    c->cur_freq_hz = cfg->freq_hz;
    memcpy(c->cur_mode, norm, sizeof c->cur_mode);
    atomic_init(&c->pend, 0);
    return KIWI_OK;
}

kiwi_err_t kiwi_client_connect(kiwi_client_t *c)
{
    if (!c || c->net) return KIWI_ERR_ARG;

    /* Protokollzustand fuer eine frische Verbindung zuruecksetzen. */
    c->err = 0; c->setup_done = 0; c->first_dropped = 0; c->have_seq = 0;
    c->audio_rate = 0; c->sample_rate_mhz = 0; c->foff_hz = 0;
    memset(&c->st, 0, sizeof c->st);
    kiwi_adpcm_init(&c->adpcm);
    kiwi_ws_rx_init(&c->ws, c->rxbuf, KIWI_RX_BUF_SIZE - 1);

    uint32_t to = c->cfg.connect_timeout_ms;
    if (kiwi_net_connect(&c->net, c->cfg.host, c->cfg.port, to) != 0) {
        c->net = NULL;
        return KIWI_ERR_NET;
    }

    /* ---- Schritt 1: HTTP-Upgrade-Anfrage ---- */
    uint8_t key[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = kiwi_random_u32();
        memcpy(key + i, &r, 4);
    }
    /* Pfad: "/ws/kiwi/<ts>/SND" (KiwiSDR v1.9xx) bzw. "/<ts>/SND" (alt). Auf
     * einem neuen Kiwi wird der alte Pfad zwar per "101" akzeptiert, danach
     * aber still ignoriert - genau das Verhalten, das wir beobachtet haben. */
    const char *prefix = c->cfg.path_prefix ? c->cfg.path_prefix : "/ws/kiwi";
    char path[80], origin[112];
    snprintf(path, sizeof path, "%s/%lu/SND", prefix,
             (unsigned long)(1000000000u + kiwi_random_u32() % 900000000u));
    snprintf(origin, sizeof origin, "http://%s:%u", c->cfg.host, (unsigned)c->cfg.port);
    /* rxbuf dient hier als Scratch-Speicher; der WS-Parser nutzt ihn erst spaeter. */
    size_t rl = kiwi_ws_build_request((char *)c->rxbuf, KIWI_RX_BUF_SIZE,
                                      c->cfg.host, c->cfg.port, path, origin, key);
    kiwi_err_t rc = KIWI_ERR_ARG;
    if (!rl) goto fail;
    if (kiwi_net_send_all(c->net, c->rxbuf, rl, 3000) != 0) { rc = KIWI_ERR_NET; goto fail; }

    /* ---- Schritt 2: Antwort bis zur Leerzeile lesen ---- */
    size_t have = 0, end = 0;
    uint32_t t0 = kiwi_time_ms();
    for (;;) {
        int r = kiwi_net_recv(c->net, c->tmp + have, sizeof c->tmp - have, 500);
        if (r > 0) {
            have += (size_t)r;
            for (size_t i = 3; i < have && !end; i++)
                if (!memcmp(c->tmp + i - 3, "\r\n\r\n", 4)) end = i + 1;
            if (end) break;
            if (have == sizeof c->tmp) { rc = KIWI_ERR_HANDSHAKE; goto fail; }
        } else if (r == KIWI_NET_TIMEOUT) {
            if (kiwi_time_ms() - t0 > to) { rc = KIWI_ERR_TIMEOUT; goto fail; }
        } else {
            rc = KIWI_ERR_NET; goto fail;
        }
    }
    int status = kiwi_ws_parse_status((const char *)c->tmp, have);
    KIWI_LOGD("HTTP-Status %d", status);
    if (status != 101) {
        rc = (status >= 300 && status < 400) ? KIWI_ERR_REDIRECT : KIWI_ERR_HANDSHAKE;
        goto fail;
    }
    /* Hinweis: Wir pruefen Sec-WebSocket-Accept nicht (wie der Python-Client);
     * das waere eine SHA-1-Berechnung, schuetzt hier aber vor nichts. */

    /* ---- Schritt 3: Anmeldung ---- */
    c->t_last_rx = c->t_last_ka = kiwi_time_ms();
    /* Ohne Passwort sendet der Browser "p=#" ("#" wird vom Server ignoriert). */
    if ((rc = (kiwi_err_t)send_cmd(c, "SET auth t=kiwi p=%s",
                                   c->cfg.password[0] ? c->cfg.password : "#")) != 0) goto fail;
    /* Selbstauskunft wie der Browser ("openwebrx.js"): Client-Typ und Kanal. */
    if ((rc = (kiwi_err_t)send_cmd(c, "SERVER DE CLIENT kiwi_esp32 SND")) != 0) goto fail;

    /* Bytes, die direkt hinter den HTTP-Headern kamen, gehoeren schon zum WebSocket. */
    if (have > end) {
        int pr = kiwi_ws_rx_push(&c->ws, c->tmp + end, have - end, on_ws_message, c);
        if (pr < 0) { rc = KIWI_ERR_PROTO; goto fail; }
        if (pr > 0) { rc = (kiwi_err_t)(c->err ? c->err : KIWI_ERR_CLOSED); goto fail; }
    }
    return KIWI_OK;

fail:
    kiwi_client_close(c);
    return rc;
}

kiwi_err_t kiwi_client_poll(kiwi_client_t *c, uint32_t timeout_ms)
{
    if (!c || !c->net) return KIWI_ERR_CLOSED;

    int rc = apply_pending(c);
    if (rc) return (kiwi_err_t)rc;

    int r = kiwi_net_recv(c->net, c->tmp, sizeof c->tmp, timeout_ms);
    uint32_t now = kiwi_time_ms();

    if (r > 0) {
        c->t_last_rx = now;
        int pr = kiwi_ws_rx_push(&c->ws, c->tmp, (size_t)r, on_ws_message, c);
        if (pr < 0) return KIWI_ERR_PROTO;
        if (pr > 0) return (kiwi_err_t)(c->err ? c->err : KIWI_ERR_CLOSED);
    } else if (r == KIWI_NET_TIMEOUT) {
        if (now - c->t_last_rx > c->cfg.idle_timeout_ms) return KIWI_ERR_TIMEOUT;
    } else if (r == KIWI_NET_CLOSED) {
        return KIWI_ERR_CLOSED;
    } else {
        return KIWI_ERR_NET;
    }

    /* Keepalive: der Server wirft stumme Clients nach kurzer Zeit raus. */
    if (now - c->t_last_ka >= 1000u) {
        c->t_last_ka = now;
        if (send_cmd(c, "SET keepalive") != 0) return KIWI_ERR_NET;
    }
    c->st.dropped_msgs = c->ws.dropped;
    return KIWI_OK;
}

kiwi_err_t kiwi_client_set_tune(kiwi_client_t *c, uint32_t freq_hz, const char *mode)
{
    if (!c) return KIWI_ERR_ARG;
    /* Basis = noch nicht abgearbeiteter Wunsch, sonst aktueller Zustand. */
    uint32_t f; char m[8];
    if (atomic_load(&c->pend)) { f = c->pend_freq_hz; memcpy(m, c->pend_mode, sizeof m); }
    else                       { f = c->cur_freq_hz;  memcpy(m, c->cur_mode, sizeof m); }

    if (freq_hz) {
        if (freq_hz > 32000000u) return KIWI_ERR_ARG;
        f = freq_hz;
    }
    if (mode) {
        if (!find_mode(mode, m)) return KIWI_ERR_ARG;
    }
    c->pend_freq_hz = f;
    memcpy(c->pend_mode, m, sizeof m);
    atomic_store(&c->pend, 1);            /* Freigabe: ab hier liest der Netz-Task */
    return KIWI_OK;
}

void kiwi_client_close(kiwi_client_t *c)
{
    if (!c) return;
    if (c->net) {
        /* Sauberes Beenden: Close-Frame mit Statuscode 1001 ("going away"),
         * dann TCP schliessen. Wichtig, damit der Kiwi den Kanal sofort frei
         * gibt und unsere IP nicht wegen "haengender" Verbindungen auffaellt. */
        const uint8_t code[2] = { 0x03, 0xE9 };
        if (send_frame(c, KIWI_WS_OP_CLOSE, code, 2) == KIWI_OK) {
            /* "Auslaufen lassen": Schliesst man einen TCP-Socket, in dem noch
             * ungelesene Daten liegen (hier: weitere Audio-Frames), sendet das
             * Betriebssystem ein RST statt eines ordentlichen FIN - der Server
             * kann dann unser Close-Frame verlieren. Deshalb lesen und
             * verwerfen wir noch kurz, bis der Server seinerseits schliesst
             * (hoechstens ca. 300 ms). */
            uint32_t t0 = kiwi_time_ms();
            while (kiwi_time_ms() - t0 < 300u) {
                int r = kiwi_net_recv(c->net, c->tmp, sizeof c->tmp, 50);
                if (r == KIWI_NET_CLOSED || r == KIWI_NET_ERR) break;
            }
        }
        kiwi_net_close(c->net);
        c->net = NULL;
    }
    if (c->sink_open) {
        if (c->cfg.sink && c->cfg.sink->close) c->cfg.sink->close(c->cfg.sink->ctx);
        c->sink_open = 0;
    }
}

int kiwi_client_command(kiwi_client_t *c, const char *line, char *reply, size_t cap)
{
    char buf[48];
    size_t n = 0;
    while (*line == ' ') line++;
    while (line[n] && n < sizeof buf - 1) { buf[n] = line[n]; n++; }
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = '\0';
    if (!n) { if (cap) reply[0] = '\0'; return 0; }

    const char *arg = buf + 1;
    while (*arg == ' ') arg++;

    switch (tolower((unsigned char)buf[0])) {
    case 'f': {
        uint32_t hz;
        if (kiwi_parse_khz(arg, &hz) || kiwi_client_set_tune(c, hz, NULL)) {
            snprintf(reply, cap, "Fehler: Frequenz in kHz (z. B. 'f 225' oder 'f 7055.5')");
            return -1;
        }
        snprintf(reply, cap, "Frequenz -> %lu.%03lu kHz", (unsigned long)(hz / 1000), (unsigned long)(hz % 1000));
        return 0;
    }
    case 'm':
        if (kiwi_client_set_tune(c, 0, arg)) {
            snprintf(reply, cap, "Fehler: Modus unbekannt (am amn amw sam sal sau lsb lsn usb usn cw cwn nbfm nnfm)");
            return -1;
        }
        snprintf(reply, cap, "Modus -> %s", arg);
        return 0;
    case 's': {
        int t = c->st.rssi_tenths_dbm;
        snprintf(reply, cap, "f=%lu.%03lu kHz mode=%s rate=%lu Hz frames=%lu lost=%lu samples=%lu RSSI=%s%d.%d dBm",
                 (unsigned long)(c->cur_freq_hz / 1000), (unsigned long)(c->cur_freq_hz % 1000),
                 c->cur_mode, (unsigned long)c->st.audio_rate, (unsigned long)c->st.frames,
                 (unsigned long)c->st.lost_frames, (unsigned long)c->st.samples,
                 t < 0 ? "-" : "", (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
        return 0;
    }
    case 'q':
        snprintf(reply, cap, "Beende ...");
        return 1;
    default:
        snprintf(reply, cap, "Befehle: f <kHz> | m <modus> | s | q");
        return -1;
    }
}
