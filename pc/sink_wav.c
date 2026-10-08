/**
 * @file sink_wav.c
 * @brief WAV-Schreiber. Format: RIFF/WAVE, PCM, 16 Bit, mono, little endian.
 *
 * LEHRBUCH-HINTERGRUND: WAV-Header (44 Byte)
 *   "RIFF" <Gesamtlaenge-8> "WAVE"
 *   "fmt " 16  <Format=1(PCM)> <Kanaele> <Rate> <Byterate> <Blockgroesse> <Bits>
 *   "data" <Nutzdatenlaenge> <Samples...>
 * Die Laengenfelder sind beim Start unbekannt; wir schreiben Platzhalter und
 * korrigieren sie beim Schliessen ("fseek zurueck, Header neu schreiben").
 */
#include "sink_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    audio_sink_t sink;     /* muss erstes Element sein */
    char    *path;
    FILE    *f;
    int      raw;          /* 1 = ohne Header nach stdout */
    uint32_t rate;
    uint32_t nbytes;
} wav_ctx_t;

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void write_header(wav_ctx_t *w)
{
    uint8_t h[44];
    memcpy(h + 0, "RIFF", 4);  put_le32(h + 4, 36 + w->nbytes);
    memcpy(h + 8, "WAVEfmt ", 8); put_le32(h + 16, 16);
    put_le16(h + 20, 1);               /* PCM            */
    put_le16(h + 22, 1);               /* mono           */
    put_le32(h + 24, w->rate);
    put_le32(h + 28, w->rate * 2);     /* Bytes/Sekunde  */
    put_le16(h + 32, 2);               /* Bytes/Frame    */
    put_le16(h + 34, 16);              /* Bits/Sample    */
    memcpy(h + 36, "data", 4);  put_le32(h + 40, w->nbytes);
    fwrite(h, 1, sizeof h, w->f);
}

static int wav_open(void *ctx, uint32_t rate)
{
    wav_ctx_t *w = (wav_ctx_t *)ctx;
    w->rate = rate;
    w->nbytes = 0;
    if (w->raw) { w->f = stdout; return 0; }
    w->f = fopen(w->path, "wb");
    if (!w->f) return -1;
    write_header(w);                   /* Platzhalter, wird in close() korrigiert */
    return 0;
}

static int wav_write(void *ctx, const int16_t *pcm, size_t n)
{
    wav_ctx_t *w = (wav_ctx_t *)ctx;
    if (!w->f) return -1;
    uint8_t buf[512];                  /* explizit little endian schreiben */
    while (n) {
        size_t k = n > sizeof buf / 2 ? sizeof buf / 2 : n;
        for (size_t i = 0; i < k; i++) put_le16(buf + 2 * i, (uint16_t)pcm[i]);
        if (fwrite(buf, 2, k, w->f) != k) return -1;
        w->nbytes += (uint32_t)(2 * k);
        pcm += k; n -= k;
    }
    return 0;
}

static void wav_close(void *ctx)
{
    wav_ctx_t *w = (wav_ctx_t *)ctx;
    if (!w->f) return;
    if (!w->raw) {
        fseek(w->f, 0, SEEK_SET);
        write_header(w);               /* jetzt mit richtigen Laengen */
        fclose(w->f);
    } else {
        fflush(w->f);
    }
    w->f = NULL;
}

audio_sink_t *sink_wav_create(const char *path)
{
    wav_ctx_t *w = (wav_ctx_t *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->path = strdup(path);
    w->raw = (strcmp(path, "-") == 0);
    w->sink.open = wav_open;
    w->sink.write = wav_write;
    w->sink.close = wav_close;
    w->sink.ctx = w;
    return &w->sink;
}

void sink_wav_destroy(audio_sink_t *s)
{
    if (!s) return;
    wav_ctx_t *w = (wav_ctx_t *)s;
    free(w->path);
    free(w);
}
