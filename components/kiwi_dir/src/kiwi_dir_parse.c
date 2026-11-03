/**
 * @file kiwi_dir_parse.c
 * @brief Streaming-Parser fuer kiwisdr_com.js. Konzept: kiwi_dir_parse.h.
 *
 * AUFBAU
 *   1. Feldwerte -> Datensatz  (finish_object: URL, GPS, Land, Zahlen ...)
 *   2. Zeichenebene            (Strings mit Escapes \" \\ \uXXXX)
 *   3. Zustandsautomat         (kiwi_dir_parser_push)
 */
#include "kiwi_dir_parse.h"

#include <stdlib.h>
#include <string.h>

#include "kiwi_dir_geo.h"

/* Zustaende des Automaten */
enum { S_PRE, S_SLASH, S_COMMENT, S_ARR, S_OBJ, S_KEY, S_COLON, S_VAL, S_STR, S_LIT, S_NEST, S_DONE };

/* Felder, die uebernommen werden (alle anderen werden ueberlesen). */
enum { F_NAME, F_URL, F_LOC, F_GPS, F_USERS, F_USERS_MAX, F_SNR, F_BANDS, F_OFFLINE, F_STATUS, F_COUNT };

static const struct { const char *key; uint8_t cap; } k_field[F_COUNT] = {
    [F_NAME] = { "name", 240 }, [F_URL] = { "url", 100 }, [F_LOC] = { "loc", 160 },
    [F_GPS] = { "gps", 48 }, [F_USERS] = { "users", 12 }, [F_USERS_MAX] = { "users_max", 12 },
    [F_SNR] = { "snr", 24 }, [F_BANDS] = { "bands", 40 }, [F_OFFLINE] = { "offline", 12 },
    [F_STATUS] = { "status", 16 },
};

/* ================================================================== */
/* 1. Feldwerte -> Datensatz                                          */
/* ================================================================== */

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

void kiwi_dir_copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    /* Folgebytes (10xxxxxx) am Schnitt -> bis zum Anfang des Zeichens zurueck. */
    if (src[n] != '\0') while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    memcpy(dst, src, n);
    dst[n] = '\0';
    /* Auch ein schon vorher abgeschnittenes Zeichen am Ende entfernen. */
    size_t k = n;
    while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0 && (unsigned char)dst[k - 1] >= 0xC0) {
        unsigned char lead = (unsigned char)dst[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (n - (k - 1) < need) dst[k - 1] = '\0';
    }
}

/** "123.456" / "-0.5" -> Tausendstel (ohne Gleitkomma). Schiebt *s weiter. 0 = OK. */
static int parse_milli(const char **s, int32_t *out)
{
    const char *p = *s;
    int neg = 0, any = 0;
    int32_t ip = 0, fr = 0, digits = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    for (; *p >= '0' && *p <= '9'; p++) { any = 1; if (ip < 100000) ip = ip * 10 + (*p - '0'); }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++) {
            any = 1;
            if (digits < 3) { fr = fr * 10 + (*p - '0'); digits++; }
        }
    }
    if (!any) return -1;
    while (digits++ < 3) fr *= 10;
    *out = (ip * 1000 + fr) * (neg ? -1 : 1);
    *s = p;
    return 0;
}

static int16_t milli_to_cdeg(int32_t m) { return (int16_t)(m >= 0 ? (m + 5) / 10 : -((-m + 5) / 10)); }

int kiwi_dir_parse_gps(const char *s, int16_t *lat, int16_t *lon)
{
    int32_t a, b;
    while (*s == ' ' || *s == '(') s++;
    if (parse_milli(&s, &a)) return -1;
    while (*s == ' ' || *s == ',' || *s == ';') s++;
    if (parse_milli(&s, &b)) return -1;
    if (a < -90000 || a > 90000 || b < -180000 || b > 180000) return -1;
    if (a == 0 && b == 0) return -1;            /* "(0.0, 0.0)" = Kiwi kennt seine Position nicht */
    *lat = milli_to_cdeg(a);
    *lon = milli_to_cdeg(b);
    return 0;
}

int kiwi_dir_parse_hostport(const char *url, char host[KIWI_DIR_HOST_LEN], uint16_t *port, uint8_t *flags)
{
    uint32_t pt = 8073;                          /* KiwiSDR-Standardport, wenn die URL keinen nennt */
    *flags = 0;
    if (!strncmp(url, "http://", 7)) url += 7;
    else if (!strncmp(url, "https://", 8)) { url += 8; *flags |= KIWI_DIR_F_TLS; }
    /* sonst: "host:port" ohne Schema wird wie http behandelt */
    if (*url == '[') return -1;                              /* IPv6: nicht unterstuetzt (Port-Schicht ist IPv4) */
    size_t n = 0;
    while (url[n] && url[n] != ':' && url[n] != '/' && url[n] != '?' && url[n] != ' ') n++;
    if (n == 0 || n >= KIWI_DIR_HOST_LEN) return -1;
    memcpy(host, url, n);
    host[n] = '\0';
    if (url[n] == ':') {
        uint32_t v = 0;
        const char *d = url + n + 1;
        if (*d < '0' || *d > '9') return -1;
        for (; *d >= '0' && *d <= '9'; d++) { v = v * 10 + (uint32_t)(*d - '0'); if (v > 65535) return -1; }
        if (v == 0) return -1;
        pt = v;
    }
    *port = (uint16_t)pt;
    return 0;
}

static uint8_t clamp_u8(const char *s)
{
    long v = strtol(s, NULL, 10);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/** Entfernt den Ortsnamen am Ende des Namens ("0-30 MHz SDR | Dortmund" -> "0-30 MHz SDR"):
 *  Der Ort steht ohnehin im Feld "place"; so bleibt im knappen Namensfeld mehr Nutzbares. */
static void strip_loc_suffix(char *name, const char *loc)
{
    size_t nl = strlen(name), ll = strlen(loc);
    if (!ll || ll >= nl || !ci_eq(name + nl - ll, loc)) return;
    size_t k = nl - ll;
    while (k > 0 && (name[k - 1] == ' ' || name[k - 1] == '|' || name[k - 1] == ',' ||
                     name[k - 1] == '-' || name[k - 1] == ':')) k--;
    if (k > 0) name[k] = '\0';            /* nur kuerzen, wenn etwas uebrig bleibt */
}

static void finish_object(kiwi_dir_parser_t *p)
{
    kiwi_dir_item_t it;
    kiwi_dir_rec_t *r = &it.rec;
    memset(&it, 0, sizeof it);
    p->stats.objects++;

    if (kiwi_dir_parse_hostport(p->val[F_URL], r->host, &r->port, &r->flags) != 0) { p->stats.skipped++; return; }

    if (ci_eq(p->val[F_OFFLINE], "yes") || ci_eq(p->val[F_OFFLINE], "true") || ci_eq(p->val[F_STATUS], "offline"))
        r->flags |= KIWI_DIR_F_OFFLINE;
    if (ci_eq(p->val[F_STATUS], "private")) r->flags |= KIWI_DIR_F_PRIVATE;

    r->users = clamp_u8(p->val[F_USERS]);
    r->users_max = clamp_u8(p->val[F_USERS_MAX]);

    /* snr: "40,41" = SNR(0-30 MHz), SNR(1,8-30 MHz) */
    const char *sn = p->val[F_SNR];
    r->snr_all = clamp_u8(sn);
    const char *comma = strchr(sn, ',');
    r->snr_hf = comma ? clamp_u8(comma + 1) : 0;

    /* bands: "0-30000000" (Hz) */
    char *end;
    unsigned long lo = strtoul(p->val[F_BANDS], &end, 10);
    if (*end == '-') {
        unsigned long hi = strtoul(end + 1, NULL, 10);
        if (hi >= lo && hi <= 4000000000ul) { r->band_lo_hz = (uint32_t)lo; r->band_hi_hz = (uint32_t)hi; }
    }

    int has_geo = kiwi_dir_parse_gps(p->val[F_GPS], &r->lat_cdeg, &r->lon_cdeg) == 0;
    if (!has_geo) r->lat_cdeg = r->lon_cdeg = KIWI_DIR_NO_GEO;

    const char *c = kiwi_dir_geo_resolve(p->val[F_LOC], p->val[F_NAME], has_geo, r->lat_cdeg, r->lon_cdeg);
    if (c) kiwi_dir_copy_utf8(it.country, sizeof it.country, c);

    kiwi_dir_copy_utf8(r->place, sizeof r->place, p->val[F_LOC]);
    strip_loc_suffix(p->val[F_NAME], p->val[F_LOC]);
    kiwi_dir_copy_utf8(r->name, sizeof r->name, p->val[F_NAME][0] ? p->val[F_NAME] : r->host);

    p->stats.accepted++;
    if (p->cb && p->cb(p->user, &it) != 0) p->state = S_DONE;     /* Abbruch */
}

/* ================================================================== */
/* 2. Zeichenebene                                                    */
/* ================================================================== */

/** Haengt ein Byte an das aktuelle Ziel (Schluessel oder Feldwert); zu Langes wird gekuerzt. */
static void put(kiwi_dir_parser_t *p, char c)
{
    if (p->in_key) {
        if (p->keylen < sizeof p->key - 1) p->key[p->keylen++] = c;
    } else if (p->field >= 0) {
        if (p->vlen < k_field[p->field].cap - 1u) {
            p->val[p->field][p->vlen++] = c;
            p->val[p->field][p->vlen] = '\0';
        }
    }
}

static void put_cp(kiwi_dir_parser_t *p, uint32_t cp)
{
    if (cp < 0x80) put(p, (char)cp);
    else if (cp < 0x800) { put(p, (char)(0xC0 | (cp >> 6))); put(p, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        put(p, (char)(0xE0 | (cp >> 12))); put(p, (char)(0x80 | ((cp >> 6) & 0x3F))); put(p, (char)(0x80 | (cp & 0x3F)));
    } else {
        put(p, (char)(0xF0 | (cp >> 18))); put(p, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put(p, (char)(0x80 | ((cp >> 6) & 0x3F))); put(p, (char)(0x80 | (cp & 0x3F)));
    }
}

/** \uXXXX fertig gelesen: Ersatzpaare (Surrogate) zu einem Zeichen verbinden. */
static void unicode_done(kiwi_dir_parser_t *p)
{
    uint32_t u = p->uni;
    if (u >= 0xD800 && u < 0xDC00) {                         /* erste Haelfte */
        if (p->hi) put(p, '?');
        p->hi = (uint16_t)u;
        return;
    }
    if (u >= 0xDC00 && u < 0xE000) {                         /* zweite Haelfte */
        if (p->hi) put_cp(p, 0x10000u + (((uint32_t)p->hi - 0xD800u) << 10) + (u - 0xDC00u));
        else put(p, '?');
        p->hi = 0;
        return;
    }
    if (p->hi) { put(p, '?'); p->hi = 0; }
    put_cp(p, u);
}

/** Verarbeitet ein Byte INNERHALB eines Strings. @return 1 = schliessendes Anfuehrungszeichen. */
static int string_byte(kiwi_dir_parser_t *p, unsigned char c)
{
    if (p->esc == 2) {                                       /* hex-Ziffern von \uXXXX */
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) { p->esc = 0; put(p, '?'); return 0; }
        p->uni = (uint16_t)((p->uni << 4) | (unsigned)v);
        if (++p->hexn == 4) { p->esc = 0; unicode_done(p); }
        return 0;
    }
    if (p->esc == 1) {                                       /* Zeichen nach Backslash */
        p->esc = 0;
        switch (c) {
        case 'n': put(p, '\n'); break;
        case 't': put(p, '\t'); break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': p->esc = 2; p->uni = 0; p->hexn = 0; break;
        default:  put(p, (char)c); break;                    /* \" \\ \/ */
        }
        return 0;
    }
    if (c == '"') return 1;
    if (c == '\\') { p->esc = 1; return 0; }
    if (p->hi) { put(p, '?'); p->hi = 0; }
    put(p, (char)c);
    return 0;
}

/* ================================================================== */
/* 3. Zustandsautomat                                                 */
/* ================================================================== */

void kiwi_dir_parser_init(kiwi_dir_parser_t *p, kiwi_dir_item_cb cb, void *user)
{
    memset(p, 0, sizeof *p);
    p->cb = cb;
    p->user = user;
    p->field = -1;
    p->state = S_PRE;
}

int kiwi_dir_parser_finished(const kiwi_dir_parser_t *p) { return p->closed; }

static int is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void end_value(kiwi_dir_parser_t *p) { p->state = S_OBJ; p->field = -1; }

int kiwi_dir_parser_push(kiwi_dir_parser_t *p, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = data[i];
        switch (p->state) {
        case S_PRE:                                          /* JS-Vorspann bis zum "[" ueberlesen */
            if (c == '[') p->state = S_ARR;
            else if (c == '/') p->state = S_SLASH;
            break;
        case S_SLASH:                                        /* "//" beginnt einen Zeilenkommentar - dessen Inhalt */
            p->state = (c == '/') ? S_COMMENT : S_PRE;       /* (z. B. "[" oder URLs) darf nichts ausloesen */
            break;
        case S_COMMENT:
            if (c == '\n') p->state = S_PRE;
            break;
        case S_ARR:
            if (c == '{') {
                for (int f = 0; f < F_COUNT; f++) p->val[f][0] = '\0';
                p->field = -1;
                p->state = S_OBJ;
            } else if (c == ']') { p->state = S_DONE; p->closed = 1; }
            break;
        case S_OBJ:
            if (c == '"') { p->state = S_KEY; p->in_key = 1; p->keylen = 0; p->esc = 0; p->hi = 0; }
            else if (c == '}') {
                p->state = S_ARR;
                finish_object(p);
                if (p->state == S_DONE) return 1;            /* Callback wollte abbrechen */
            }
            break;
        case S_KEY:
            if (string_byte(p, c)) {
                p->in_key = 0;
                p->key[p->keylen] = '\0';
                p->field = -1;
                for (int f = 0; f < F_COUNT; f++)
                    if (!strcmp(p->key, k_field[f].key)) { p->field = (int8_t)f; break; }
                p->state = S_COLON;
            }
            break;
        case S_COLON:
            if (c == ':') p->state = S_VAL;
            break;
        case S_VAL:
            if (is_ws(c)) break;
            p->vlen = 0;
            if (p->field >= 0) p->val[p->field][0] = '\0';
            if (c == '"') { p->state = S_STR; p->esc = 0; p->hi = 0; }
            else if (c == '{' || c == '[') { p->state = S_NEST; p->nest = 1; p->nest_str = 0; p->nest_esc = 0; }
            else if (c == ',') end_value(p);
            else if (c == '}') { end_value(p); i--; }          /* Wert fehlt: "}" erneut behandeln */
            else { p->state = S_LIT; put(p, (char)c); }
            break;
        case S_STR:
            if (string_byte(p, c)) end_value(p);
            break;
        case S_LIT:                                          /* Zahl, true, false, null */
            if (c == ',' || is_ws(c)) end_value(p);
            else if (c == '}') { end_value(p); i--; }
            else put(p, (char)c);
            break;
        case S_NEST:                                         /* unbekannter verschachtelter Wert */
            if (p->nest_str) {
                if (p->nest_esc) p->nest_esc = 0;
                else if (c == '\\') p->nest_esc = 1;
                else if (c == '"') p->nest_str = 0;
            } else if (c == '"') p->nest_str = 1;
            else if (c == '{' || c == '[') p->nest++;
            else if ((c == '}' || c == ']') && --p->nest == 0) end_value(p);
            break;
        case S_DONE:                                         /* Rest (";" usw.) ignorieren */
        default:
            return 0;
        }
    }
    return 0;
}
