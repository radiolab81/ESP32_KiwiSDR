/**
 * @file kiwi_ws.c
 * @brief Implementierung des minimalen WebSocket-Clients (siehe kiwi_ws.h).
 */
#include "kiwi_ws.h"

#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* Base64                                                             */
/* ================================================================== */

/* Je 3 Eingabebytes (24 Bit) werden zu 4 Zeichen (4 x 6 Bit). */
size_t kiwi_ws_base64(const uint8_t *in, size_t n, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';  /* Auffuellzeichen */
        out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ================================================================== */
/* Opening Handshake                                                  */
/* ================================================================== */

size_t kiwi_ws_build_request(char *out, size_t cap, const char *host,
                             uint16_t port, const char *path,
                             const char *origin, const uint8_t key[16])
{
    char key_b64[KIWI_WS_KEY_B64_LEN];
    kiwi_ws_base64(key, 16, key_b64);

    /* Host-Header: Port nur anhaengen, wenn er nicht der Standard (80) ist -
     * genau wie der Python-Referenzclient (kiwi/wsclient.py). */
    char hostport[96];
    if (port == 80) snprintf(hostport, sizeof hostport, "%s", host);
    else            snprintf(hostport, sizeof hostport, "%s:%u", host, (unsigned)port);

    /* Origin: Browser senden ihn immer; manche Server werten ihn aus. */
    char origin_line[128] = "";
    if (origin) snprintf(origin_line, sizeof origin_line, "Origin: %s\r\n", origin);

    int n = snprintf(out, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "%s"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path, hostport, origin_line, key_b64);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

int kiwi_ws_parse_status(const char *resp, size_t len)
{
    /* Erwartet: "HTTP/1.x SSS ..." */
    if (len < 12 || strncmp(resp, "HTTP/", 5) != 0) return -1;
    const char *p = memchr(resp, ' ', len);
    if (!p || (size_t)(p - resp) + 4 > len) return -1;
    int code = 0;
    for (int i = 1; i <= 3; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        code = code * 10 + (p[i] - '0');
    }
    return code;
}

/* ================================================================== */
/* Frames senden                                                      */
/* ================================================================== */

size_t kiwi_ws_encode_frame(uint8_t *out, size_t cap, uint8_t opcode,
                            const uint8_t *payload, size_t n,
                            const uint8_t mask[4])
{
    size_t hdr = 2 + (n < 126 ? 0 : (n <= 0xFFFF ? 2 : 8)) + 4;
    if (cap < hdr + n) return 0;

    size_t i = 0;
    out[i++] = (uint8_t)(0x80 | (opcode & 0x0F));       /* FIN=1 + Opcode        */
    if (n < 126) {
        out[i++] = (uint8_t)(0x80 | n);                 /* MASK=1 + LEN7         */
    } else if (n <= 0xFFFF) {
        out[i++] = 0x80 | 126;
        out[i++] = (uint8_t)(n >> 8);
        out[i++] = (uint8_t)n;
    } else {
        out[i++] = 0x80 | 127;
        for (int s = 56; s >= 0; s -= 8) out[i++] = (uint8_t)((uint64_t)n >> s);
    }
    memcpy(out + i, mask, 4);
    i += 4;
    for (size_t k = 0; k < n; k++) out[i + k] = payload[k] ^ mask[k & 3];
    return i + n;
}

/* ================================================================== */
/* Frames empfangen (Zustandsautomat)                                 */
/* ================================================================== */

void kiwi_ws_rx_init(kiwi_ws_rx_t *rx, uint8_t *buf, size_t cap)
{
    memset(rx, 0, sizeof *rx);
    rx->buf = buf;
    rx->cap = cap;
    rx->hdr_need = 2;
}

/* Ende eines Frames: ggf. Nachricht an den Callback uebergeben. */
static int finish_frame(kiwi_ws_rx_t *rx, kiwi_ws_msg_cb cb, void *user)
{
    int rc = 0;
    if (rx->opcode >= 0x8) {
        /* Steuer-Frames duerfen zwischen Fragmenten einer Daten-Nachricht
         * auftauchen - deshalb eigener Puffer (ctl), nicht buf. */
        rc = cb(user, rx->opcode, rx->ctl, rx->ctl_len);
        rx->ctl_len = 0;
    } else if (rx->fin) {
        if (rx->overflow) rx->dropped++;
        else              rc = cb(user, rx->msg_opcode, rx->buf, rx->msg_len);
        rx->msg_len = 0;
        rx->overflow = 0;
    }
    rx->in_payload = 0;
    rx->hdr_have = 0;
    rx->hdr_need = 2;
    return rc;
}

/* Header ist komplett: Felder auswerten. <0 bei Protokollverstoss. */
static int parse_header(kiwi_ws_rx_t *rx)
{
    const uint8_t *h = rx->hdr;
    rx->fin    = h[0] >> 7;
    uint8_t rsv = (h[0] >> 4) & 7;
    rx->opcode = h[0] & 0x0F;
    rx->masked = h[1] >> 7;
    uint8_t l7 = h[1] & 0x7F;
    if (rsv) return -1;               /* wir haben keine Erweiterungen ausgehandelt */

    size_t idx = 2;
    if (l7 == 126) {
        rx->plen = ((uint64_t)h[2] << 8) | h[3];
        idx = 4;
    } else if (l7 == 127) {
        rx->plen = 0;
        for (int i = 0; i < 8; i++) rx->plen = (rx->plen << 8) | h[2 + i];
        if (rx->plen >> 31) return -1; /* > 2 GiB: unsinnig */
        idx = 10;
    } else {
        rx->plen = l7;
    }
    if (rx->masked) memcpy(rx->mask, h + idx, 4);

    if (rx->opcode >= 0x8 && (rx->plen > KIWI_WS_MAX_CTL || !rx->fin))
        return -1;                    /* Steuer-Frames: <=125 Byte, nie fragmentiert */

    rx->pgot = 0;
    if (rx->opcode < 0x8) {
        if (rx->opcode != KIWI_WS_OP_CONT) {   /* neue Nachricht beginnt */
            rx->msg_opcode = rx->opcode;
            rx->msg_len = 0;
            rx->overflow = 0;
        }
        /* Passt die Nachricht nicht in den Puffer, wird sie komplett verworfen
         * (z. B. riesige "load_cfg"-JSON-Nachrichten des KiwiSDR). */
        if (rx->plen > rx->cap - rx->msg_len) rx->overflow = 1;
    }
    return 0;
}

int kiwi_ws_rx_push(kiwi_ws_rx_t *rx, const uint8_t *data, size_t n,
                    kiwi_ws_msg_cb cb, void *user)
{
    while (n > 0) {
        if (!rx->in_payload) {
            /* ---- Zustand 1: Header Byte fuer Byte sammeln ---- */
            rx->hdr[rx->hdr_have++] = *data++;
            n--;
            if (rx->hdr_have == 2) {
                uint8_t l7 = rx->hdr[1] & 0x7F;
                rx->hdr_need = (uint8_t)(2 + (l7 == 126 ? 2 : (l7 == 127 ? 8 : 0))
                                           + ((rx->hdr[1] & 0x80) ? 4 : 0));
            }
            if (rx->hdr_have == rx->hdr_need) {
                if (parse_header(rx) < 0) return -1;
                if (rx->plen == 0) {
                    int rc = finish_frame(rx, cb, user);
                    if (rc) return rc;
                } else {
                    rx->in_payload = 1;
                }
            }
        } else {
            /* ---- Zustand 2: Nutzdaten (blockweise kopieren) ---- */
            uint64_t left = rx->plen - rx->pgot;
            size_t take = (n < left) ? n : (size_t)left;

            uint8_t *dst = NULL;
            if (rx->opcode >= 0x8)      dst = rx->ctl + rx->ctl_len;
            else if (!rx->overflow)     dst = rx->buf + rx->msg_len;

            if (dst) {
                if (rx->masked) {
                    for (size_t i = 0; i < take; i++)
                        dst[i] = data[i] ^ rx->mask[(rx->pgot + i) & 3];
                } else {
                    memcpy(dst, data, take);
                }
                if (rx->opcode >= 0x8) rx->ctl_len += take;
                else                   rx->msg_len += take;
            }
            rx->pgot += take;
            data += take;
            n -= take;

            if (rx->pgot == rx->plen) {
                int rc = finish_frame(rx, cb, user);
                if (rc) return rc;
            }
        }
    }
    return 0;
}
