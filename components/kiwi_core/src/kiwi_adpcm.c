/**
 * @file kiwi_adpcm.c
 * @brief IMA-ADPCM-Dekoder, bitgenau nach kiwiclient (kiwi/client.py,
 *        Klasse ImaAdpcmDecoder).
 */
#include "kiwi_adpcm.h"

/* Schrittweitentabelle (89 Eintraege, ungefaehr exponentiell, +10 % pro Stufe).
 * "static const" -> landet beim ESP32 im Flash, nicht im knappen RAM. */
static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767
};

/* Wie aendert sich der Index nach einem 4-Bit-Code? Kleine Codes (|diff|
 * klein) verkleinern die Schrittweite, grosse Codes vergroessern sie. */
static const int8_t k_idx_adj[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

void kiwi_adpcm_init(kiwi_adpcm_t *s)
{
    s->prev = 0;
    s->index = 0;
}

/* Ein 4-Bit-Code -> ein 16-Bit-Sample. */
static int16_t decode_nibble(kiwi_adpcm_t *s, unsigned code)
{
    int step = k_step[s->index];

    /* Neuen Index bestimmen und auf 0..88 begrenzen. */
    int idx = (int)s->index + k_idx_adj[code];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    s->index = (uint8_t)idx;

    /* Differenz = (code_betrag + 0.5) * step / 4, mit Schiebeoperationen:
     * Bit0 -> step/4, Bit1 -> step/2, Bit2 -> step, plus Grundwert step/8.  */
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if (code & 8) diff = -diff;           /* Bit3 = Vorzeichen */

    int v = s->prev + diff;               /* Praediktor + Differenz */
    if (v >  32767) v =  32767;           /* Saettigung statt Ueberlauf */
    if (v < -32768) v = -32768;
    s->prev = (int16_t)v;
    return s->prev;
}

size_t kiwi_adpcm_decode(kiwi_adpcm_t *s, const uint8_t *in, size_t n,
                         int16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        out[o++] = decode_nibble(s, in[i] & 0x0F);  /* zuerst LOW-Nibble  */
        out[o++] = decode_nibble(s, in[i] >> 4);    /* dann HIGH-Nibble   */
    }
    return o;
}
