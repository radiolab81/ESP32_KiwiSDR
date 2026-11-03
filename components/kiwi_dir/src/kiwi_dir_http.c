/**
 * @file kiwi_dir_http.c
 * @brief HTTP-GET ueber die Portschicht (kiwi_port.h). Konzept: kiwi_dir_http.h.
 */
#include "kiwi_dir_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiwi_dir_db.h"
#include "kiwi_port.h"

#define DEF_TIMEOUT_MS 120000u
#define DEF_MAX_BODY   (4u * 1024u * 1024u)
#define MAX_REDIRECTS  3
#define BUF_SIZE       1024

/* ================================================================== */
/* URL                                                                */
/* ================================================================== */

int kiwi_dir_url_split(const char *url, kiwi_dir_url_t *o)
{
    memset(o, 0, sizeof *o);
    o->port = 80;
    if (!strncmp(url, "http://", 7)) url += 7;
    else if (!strncmp(url, "https://", 8)) { url += 8; o->tls = 1; o->port = 443; }
    else return -1;

    size_t n = 0;
    while (url[n] && url[n] != ':' && url[n] != '/') n++;
    if (n == 0 || n >= sizeof o->host) return -1;
    memcpy(o->host, url, n);
    url += n;
    if (*url == ':') {
        uint32_t v = 0;
        url++;
        if (*url < '0' || *url > '9') return -1;
        for (; *url >= '0' && *url <= '9'; url++) { v = v * 10 + (uint32_t)(*url - '0'); if (v > 65535) return -1; }
        if (!v) return -1;
        o->port = (uint16_t)v;
    }
    if (*url == '\0') { strcpy(o->path, "/"); return 0; }
    if (*url != '/' || strlen(url) >= sizeof o->path) return -1;
    strcpy(o->path, url);
    return 0;
}

/* ================================================================== */
/* Chunked-Decoder                                                    */
/* ================================================================== */

/* Aufbau eines Chunks:   <hex-Laenge>[;erweiterung] CRLF  <Daten> CRLF   ... 0 CRLF [Trailer] CRLF */
enum { CH_SIZE, CH_EXT, CH_LF, CH_DATA, CH_DATA_END, CH_DONE };

void kiwi_dir_chunk_init(kiwi_dir_chunk_t *c) { memset(c, 0, sizeof *c); c->state = CH_SIZE; }

int kiwi_dir_chunk_push(kiwi_dir_chunk_t *c, const uint8_t *d, size_t n, kiwi_dir_http_body_cb cb, void *user)
{
    size_t i = 0;
    while (i < n) {
        uint8_t ch = d[i];
        switch (c->state) {
        case CH_SIZE: {
            int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 :
                    (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
            if (v >= 0) {
                if (++c->digits > 8) return -1;
                c->left = (c->left << 4) | (uint32_t)v;
                i++;
            } else if (ch == ';' || ch == ' ' || ch == '\t') { c->state = CH_EXT; i++; }
            else if (ch == '\r') { c->state = CH_LF; i++; }
            else if (ch == '\n') c->state = CH_LF;             /* ohne i++: im naechsten Zustand verbrauchen */
            else return -1;
            if (c->digits == 0 && c->state != CH_SIZE) return -1;
            break;
        }
        case CH_EXT:  if (ch == '\r') c->state = CH_LF; i++; break;
        case CH_LF:
            if (ch == '\n') {
                i++;
                c->digits = 0;
                if (c->left == 0) { c->state = CH_DONE; return 1; }     /* Trailer ignorieren */
                c->state = CH_DATA;
            } else if (ch == '\r') i++;
            else return -1;
            break;
        case CH_DATA: {
            size_t k = n - i < c->left ? n - i : c->left;
            if (cb && cb(user, d + i, k) != 0) return -1;
            c->left -= (uint32_t)k;
            i += k;
            if (c->left == 0) c->state = CH_DATA_END;
            break;
        }
        case CH_DATA_END:                                      /* CRLF hinter den Daten */
            if (ch == '\n') { c->state = CH_SIZE; c->left = 0; c->digits = 0; }
            else if (ch != '\r') return -1;
            i++;
            break;
        default:
            return 1;
        }
    }
    return c->state == CH_DONE ? 1 : 0;
}

/* ================================================================== */
/* GET                                                                */
/* ================================================================== */

/** Wert eines Headers (Gross/Klein egal) aus dem Kopfblock holen. 1 = gefunden. */
static int header_get(const char *hdr, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = hdr; *p; ) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > nl && p[nl] == ':') {
            size_t k = 0;
            while (k < nl && ((p[k] | 0x20) == (name[k] | 0x20))) k++;
            if (k == nl) {
                const char *v = p + nl + 1;
                while (v < p + ll && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(p + ll - v);
                if (vl >= cap) vl = cap - 1;
                memcpy(out, v, vl);
                out[vl] = '\0';
                return 1;
            }
        }
        if (!eol) break;
        p = eol + 2;
    }
    return 0;
}

typedef struct {                 /* Zustand des Koerper-Empfangs */
    kiwi_dir_http_body_cb cb;
    void *user;
    uint32_t total, max;
    int err;
} body_t;

static int body_out(void *user, const uint8_t *d, size_t n)
{
    body_t *b = (body_t *)user;
    if (b->total + n > b->max) { b->err = KIWI_DIR_ERR_TOO_BIG; return 1; }
    b->total += (uint32_t)n;
    if (b->cb && b->cb(b->user, d, n) != 0) { b->err = KIWI_DIR_ERR_PARSE; return 1; }
    return 0;
}

int kiwi_dir_http_get(const kiwi_dir_http_cfg_t *cfg, kiwi_dir_http_body_cb cb, void *user,
                      kiwi_dir_http_info_t *info)
{
    if (!cfg || !cfg->url) return KIWI_DIR_ERR_ARG;
    kiwi_dir_http_info_t dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof *info);

    uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : DEF_TIMEOUT_MS;
    uint32_t t0 = kiwi_time_ms();
    body_t body = { cb, user, 0, cfg->max_body ? cfg->max_body : DEF_MAX_BODY, 0 };
    char cur[192];
    if (strlen(cfg->url) >= sizeof cur) return KIWI_DIR_ERR_ARG;
    strcpy(cur, cfg->url);

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        kiwi_dir_url_t u;
        if (kiwi_dir_url_split(cur, &u) != 0) return KIWI_DIR_ERR_ARG;
        if (u.tls) return KIWI_DIR_ERR_TLS;

        kiwi_net_t *net = NULL;
        if (kiwi_net_connect(&net, u.host, u.port, 10000) != 0) return KIWI_DIR_ERR_NET;

        /* ---- Anfrage ---- */
        char req[400], hostport[80];
        if (u.port == 80) snprintf(hostport, sizeof hostport, "%s", u.host);
        else              snprintf(hostport, sizeof hostport, "%s:%u", u.host, (unsigned)u.port);
        int rl = snprintf(req, sizeof req,
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
                          "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                          u.path, hostport, cfg->user_agent ? cfg->user_agent : "kiwi_esp32");
        int rc = KIWI_DIR_ERR_NET;
        if (rl <= 0 || (size_t)rl >= sizeof req) { rc = KIWI_DIR_ERR_ARG; goto out; }
        if (kiwi_net_send_all(net, (const uint8_t *)req, (size_t)rl, 5000) != 0) goto out;

        /* ---- Kopf lesen (bis Leerzeile) ---- */
        uint8_t buf[BUF_SIZE + 1];
        size_t have = 0, hend = 0;
        while (!hend) {
            if (kiwi_time_ms() - t0 > timeout) { rc = KIWI_DIR_ERR_TIMEOUT; goto out; }
            if (have >= BUF_SIZE) { rc = KIWI_DIR_ERR_HTTP; goto out; }          /* Kopf zu gross */
            int r = kiwi_net_recv(net, buf + have, BUF_SIZE - have, 500);
            if (r > 0) {
                have += (size_t)r;
                for (size_t i = 3; i < have && !hend; i++)
                    if (!memcmp(buf + i - 3, "\r\n\r\n", 4)) hend = i + 1;
            } else if (r != KIWI_NET_TIMEOUT) goto out;                         /* Fehler/Ende vor Kopfende */
        }
        /* Kopf als C-String auswerten: Endnull NACH dem Kopf setzen. Das dort liegende Byte
         * gehoert schon zum Koerper und wird vor dessen Verarbeitung wiederhergestellt. */
        uint8_t saved = buf[hend];
        buf[hend] = '\0';
        int status = 0;
        if (sscanf((const char *)buf, "HTTP/%*d.%*d %d", &status) != 1) { rc = KIWI_DIR_ERR_HTTP; goto out; }
        info->status = status;

        /* ---- Umleitung ---- */
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc[192];
            if (!header_get((const char *)buf, "Location", loc, sizeof loc)) { rc = KIWI_DIR_ERR_HTTP; goto out; }
            if (loc[0] == '/') {                                   /* relativ: gleicher Host */
                char abs[192];
                if (snprintf(abs, sizeof abs, "http://%s:%u%s", u.host, (unsigned)u.port, loc) >= (int)sizeof abs)
                    { rc = KIWI_DIR_ERR_HTTP; goto out; }
                strcpy(cur, abs);
            } else if (strlen(loc) < sizeof cur) strcpy(cur, loc);
            else { rc = KIWI_DIR_ERR_HTTP; goto out; }
            info->redirects++;
            kiwi_net_close(net);
            continue;                                              /* naechster Versuch mit neuer URL */
        }
        if (status != 200) { rc = KIWI_DIR_ERR_HTTP; goto out; }

        char v[64];
        if (header_get((const char *)buf, "Content-Encoding", v, sizeof v) &&
            strcmp(v, "identity") != 0) { rc = KIWI_DIR_ERR_HTTP; goto out; }   /* gzip o. Ae. koennen wir nicht */
        int chunked = header_get((const char *)buf, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked");
        int have_len = header_get((const char *)buf, "Content-Length", v, sizeof v);
        uint32_t clen = have_len ? (uint32_t)strtoul(v, NULL, 10) : 0;
        if (have_len && !chunked && clen > body.max) { rc = KIWI_DIR_ERR_TOO_BIG; goto out; }

        buf[hend] = saved;

        /* ---- Koerper ---- */
        kiwi_dir_chunk_t ck;
        kiwi_dir_chunk_init(&ck);
        uint32_t got = 0;
        int done = 0;
        const uint8_t *p = buf + hend;
        size_t pn = have - hend;
        for (;;) {
            if (pn) {
                if (chunked) {
                    int cr = kiwi_dir_chunk_push(&ck, p, pn, body_out, &body);
                    if (cr < 0) { rc = body.err ? body.err : KIWI_DIR_ERR_HTTP; goto out; }
                    if (cr == 1) done = 1;
                } else {
                    if (have_len && pn > clen - got) pn = clen - got;     /* nichts hinter dem Koerper lesen */
                    if (body_out(&body, p, pn) != 0) { rc = body.err; goto out; }
                    got += (uint32_t)pn;
                    if (have_len && got >= clen) done = 1;
                }
            }
            if (done) break;
            if (kiwi_time_ms() - t0 > timeout) { rc = KIWI_DIR_ERR_TIMEOUT; goto out; }
            int r = kiwi_net_recv(net, buf, BUF_SIZE, 500);
            if (r > 0) { p = buf; pn = (size_t)r; }
            else if (r == KIWI_NET_TIMEOUT) { p = buf; pn = 0; }
            else if (r == KIWI_NET_CLOSED) {
                /* Ende der Verbindung: nur ohne Laengenangabe ein regulaeres Ende. */
                rc = (!chunked && !have_len) ? KIWI_DIR_OK : KIWI_DIR_ERR_NET;
                info->body_bytes = body.total;
                kiwi_net_close(net);
                return rc;
            } else goto out;
        }
        info->body_bytes = body.total;
        kiwi_net_close(net);
        return KIWI_DIR_OK;

    out:
        kiwi_net_close(net);
        return rc;
    }
    return KIWI_DIR_ERR_HTTP;            /* zu viele Umleitungen */
}
