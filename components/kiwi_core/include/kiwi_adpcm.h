/**
 * @file kiwi_adpcm.h
 * @brief IMA-ADPCM-Dekoder (4 Bit pro Sample -> 16-Bit-PCM).
 *
 * LEHRBUCH-HINTERGRUND: Differenz-PCM
 * -----------------------------------
 * Statt jedes Sample mit 16 Bit zu uebertragen, sendet ADPCM nur eine
 * *Aenderung* gegenueber dem vorigen Sample, grob quantisiert auf 4 Bit.
 * Eine adaptive Schrittweite ("step") passt die Quantisierung an die
 * Signaldynamik an. Ergebnis: Datenrate -75 % bei brauchbarer Sprachqualitaet.
 *
 * Wichtig: Der Dekoder hat ein GEDAECHTNIS (letztes Sample + Schrittindex).
 * Er muss ueber alle Audio-Frames einer Verbindung hinweg weiterlaufen und
 * darf nur beim Verbindungsaufbau zurueckgesetzt werden.
 */
#ifndef KIWI_ADPCM_H
#define KIWI_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t prev;   /**< zuletzt dekodiertes Sample (Praediktor) */
    uint8_t index;  /**< Index in die Schrittweitentabelle (0..88) */
} kiwi_adpcm_t;

void kiwi_adpcm_init(kiwi_adpcm_t *s);

/**
 * Dekodiert @p n Eingangsbytes zu 2*n Samples.
 * Reihenfolge laut KiwiSDR: erst das LOW-Nibble, dann das HIGH-Nibble.
 * @return Anzahl geschriebener Samples (= 2*n)
 */
size_t kiwi_adpcm_decode(kiwi_adpcm_t *s, const uint8_t *in, size_t n,
                         int16_t *out);

#ifdef __cplusplus
}
#endif
#endif /* KIWI_ADPCM_H */
