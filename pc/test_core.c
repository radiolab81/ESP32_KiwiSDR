/**
 * @file test_core.c
 * @brief Selbsttests fuer den portablen Kern (laeuft nur auf dem PC, ohne Netz).
 *
 * Getestet wird:
 *   - ADPCM-Dekoder gegen Referenzwerte des Python-Originals (kiwiclient)
 *   - Base64 (RFC-4648-Beispiele)
 *   - WebSocket-Parser: Byte-fuer-Byte-Fuetterung, 16-Bit-Laengen,
 *     Fragmentierung mit Ping dazwischen, zu grosse Nachricht (wird verworfen)
 *   - Frame-Kodierung (Maskierung) im Rundlauf
 */
#include <stdio.h>
#include <string.h>

#include "kiwi_adpcm.h"
#include "kiwi_client.h"
#include "kiwi_ws.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FEHLER Zeile %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/* ---------- ADPCM ---------- */
static void test_adpcm(void)
{
    /* Referenz: erzeugt mit kiwi/client.py ImaAdpcmDecoder (Python) */
    static const uint8_t in[24] = {
        0x82, 0xb7, 0x0e, 0xee, 0x7f, 0x1a, 0x50, 0x39, 0xbe, 0xf0, 0x7e, 0xc2,
        0x34, 0x7f, 0x06, 0x6e, 0xd0, 0x8f, 0x5d, 0xc7, 0x51, 0x24, 0x47, 0xe3 };
    static const int16_t ref[48] = {
        3, 3, 14, 0, -22, -19, -55, -121, -257, 36, -174, -60, -26, 321, 183, 477,
        -21, -497, -436, -1277, -2841, 358, 2645, -1097, 3432, 7692, -610, 17188,
        32767, 32767, -15648, 32767, 32767, -8199, -32768, -32768, -32768, 12285,
        32767, -4095, 8191, 32767, 32767, 32767, 32767, 32767, 32767, -15648 };
    kiwi_adpcm_t s; int16_t out[48];
    kiwi_adpcm_init(&s);
    CHECK(kiwi_adpcm_decode(&s, in, 24, out) == 48);
    CHECK(memcmp(out, ref, sizeof ref) == 0);
    CHECK(s.prev == -15648 && s.index == 88);

    /* Stueckweise Dekodierung muss dasselbe ergeben (Zustand bleibt erhalten). */
    kiwi_adpcm_init(&s);
    int16_t out2[48];
    kiwi_adpcm_decode(&s, in, 7, out2);
    kiwi_adpcm_decode(&s, in + 7, 17, out2 + 14);
    CHECK(memcmp(out2, ref, sizeof ref) == 0);
}

/* ---------- Base64 / Status ---------- */
static void test_base64(void)
{
    char b[32];
    kiwi_ws_base64((const uint8_t *)"foobar", 6, b);  CHECK(!strcmp(b, "Zm9vYmFy"));
    kiwi_ws_base64((const uint8_t *)"fooba", 5, b);   CHECK(!strcmp(b, "Zm9vYmE="));
    kiwi_ws_base64((const uint8_t *)"foob", 4, b);    CHECK(!strcmp(b, "Zm9vYg=="));
    const char *r = "HTTP/1.1 101 Switching Protocols\r\n";
    CHECK(kiwi_ws_parse_status(r, strlen(r)) == 101);
    CHECK(kiwi_ws_parse_status("garbage", 7) == -1);
}

/* ---------- Zahlen ---------- */
static void test_parse(void)
{
    uint32_t hz;
    CHECK(kiwi_parse_khz("225", &hz) == 0 && hz == 225000);
    CHECK(kiwi_parse_khz("7055.5", &hz) == 0 && hz == 7055500);
    CHECK(kiwi_parse_khz("0.123456", &hz) == 0 && hz == 123);
    CHECK(kiwi_parse_khz("abc", &hz) != 0);
    CHECK(kiwi_parse_khz("12x", &hz) != 0);
}

/* ---------- WebSocket-Empfang ---------- */
typedef struct { int count; uint8_t ops[16]; size_t lens[16]; uint8_t first[16]; } rec_t;

static int rec_cb(void *u, uint8_t op, const uint8_t *p, size_t n)
{
    rec_t *r = (rec_t *)u;
    if (r->count < 16) { r->ops[r->count] = op; r->lens[r->count] = n; r->first[r->count] = n ? p[0] : 0; }
    r->count++;
    return 0;
}

/* Hilfsfunktion: unmaskierten Server-Frame bauen */
static size_t srv_frame(uint8_t *o, int fin, uint8_t op, const uint8_t *pl, size_t n)
{
    size_t i = 0;
    o[i++] = (uint8_t)((fin ? 0x80 : 0) | op);
    if (n < 126) o[i++] = (uint8_t)n;
    else { o[i++] = 126; o[i++] = (uint8_t)(n >> 8); o[i++] = (uint8_t)n; }
    memcpy(o + i, pl, n);
    return i + n;
}

static void test_ws_rx(void)
{
    static uint8_t buf[1024], stream[4096], pay[600];
    kiwi_ws_rx_t rx; rec_t rec;
    size_t sl = 0;
    for (size_t i = 0; i < sizeof pay; i++) pay[i] = (uint8_t)i;

    /* 1) Ein kurzer Frame, dann ein 600-Byte-Frame (16-Bit-Laenge), Byte fuer Byte */
    sl += srv_frame(stream + sl, 1, 2, (const uint8_t *)"SND123", 6);
    sl += srv_frame(stream + sl, 1, 2, pay, 600);
    kiwi_ws_rx_init(&rx, buf, sizeof buf); memset(&rec, 0, sizeof rec);
    for (size_t i = 0; i < sl; i++) CHECK(kiwi_ws_rx_push(&rx, stream + i, 1, rec_cb, &rec) == 0);
    CHECK(rec.count == 2 && rec.lens[0] == 6 && rec.lens[1] == 600);
    CHECK(memcmp(buf, pay, 600) == 0);

    /* 2) Alles auf einmal */
    kiwi_ws_rx_init(&rx, buf, sizeof buf); memset(&rec, 0, sizeof rec);
    CHECK(kiwi_ws_rx_push(&rx, stream, sl, rec_cb, &rec) == 0);
    CHECK(rec.count == 2);

    /* 3) Fragmentierung: "AB" (fin=0) + PING + "CD" (fin=1, Fortsetzung) */
    sl = 0;
    sl += srv_frame(stream + sl, 0, 2, (const uint8_t *)"AB", 2);
    sl += srv_frame(stream + sl, 1, 9, (const uint8_t *)"p", 1);
    sl += srv_frame(stream + sl, 1, 0, (const uint8_t *)"CD", 2);
    kiwi_ws_rx_init(&rx, buf, sizeof buf); memset(&rec, 0, sizeof rec);
    CHECK(kiwi_ws_rx_push(&rx, stream, sl, rec_cb, &rec) == 0);
    CHECK(rec.count == 2);
    CHECK(rec.ops[0] == 9 && rec.lens[0] == 1);              /* Ping zuerst  */
    CHECK(rec.ops[1] == 2 && rec.lens[1] == 4 && !memcmp(buf, "ABCD", 4));

    /* 4) Zu grosse Nachricht wird verworfen, danach geht es normal weiter */
    uint8_t small[16];
    kiwi_ws_rx_init(&rx, small, sizeof small); memset(&rec, 0, sizeof rec);
    sl = 0;
    sl += srv_frame(stream + sl, 1, 2, pay, 300);            /* passt nicht */
    sl += srv_frame(stream + sl, 1, 2, (const uint8_t *)"ok", 2);
    CHECK(kiwi_ws_rx_push(&rx, stream, sl, rec_cb, &rec) == 0);
    CHECK(rec.count == 1 && rec.lens[0] == 2 && rx.dropped == 1);

    /* 5) Protokollfehler: RSV-Bit gesetzt */
    const uint8_t bad[2] = { 0xC2, 0x00 };
    kiwi_ws_rx_init(&rx, buf, sizeof buf);
    CHECK(kiwi_ws_rx_push(&rx, bad, 2, rec_cb, &rec) < 0);
}

/* ---------- Senden -> Empfangen im Rundlauf (Maskierung) ---------- */
static void test_ws_tx(void)
{
    uint8_t fr[300], buf[300];
    const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    const char *msg = "SET keepalive";
    size_t n = kiwi_ws_encode_frame(fr, sizeof fr, KIWI_WS_OP_TEXT, (const uint8_t *)msg, strlen(msg), mask);
    CHECK(n == 2 + 4 + strlen(msg));
    CHECK(fr[0] == 0x81 && (fr[1] & 0x80) && (fr[1] & 0x7F) == strlen(msg));
    CHECK(memcmp(fr + 6, msg, strlen(msg)) != 0);            /* wirklich maskiert */

    kiwi_ws_rx_t rx; rec_t rec; memset(&rec, 0, sizeof rec);
    kiwi_ws_rx_init(&rx, buf, sizeof buf);                   /* Parser versteht auch maskiert */
    CHECK(kiwi_ws_rx_push(&rx, fr, n, rec_cb, &rec) == 0);
    CHECK(rec.count == 1 && rec.lens[0] == strlen(msg) && !memcmp(buf, msg, strlen(msg)));
}

int main(void)
{
    test_adpcm(); test_base64(); test_parse(); test_ws_rx(); test_ws_tx();
    if (g_fail) { printf("%d Test(s) fehlgeschlagen\n", g_fail); return 1; }
    printf("Alle Kern-Tests bestanden.\n");
    return 0;
}
