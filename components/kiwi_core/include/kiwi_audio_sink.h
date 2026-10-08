/**
 * @file kiwi_audio_sink.h
 * @brief Abstrakte Audio-Senke ("Sink") - das Gegenstueck zur Netzwerk-Quelle.
 *
 * Der Client liefert dekodierte PCM-Samples (16 Bit, signed, mono) an eine
 * Senke. Was die Senke damit tut, ist ihr ueberlassen:
 *   - PC:    in eine WAV-Datei schreiben (pc/sink_wav.c)
 *   - ESP32: ueber den 8-Bit-DAC ausgeben (main/sink_dac.c)
 *
 * Das ist das klassische "Strategy"-Entwurfsmuster in C: eine Struktur mit
 * Funktionszeigern plus einem Kontextzeiger (statt C++-Vererbung).
 */
#ifndef KIWI_AUDIO_SINK_H
#define KIWI_AUDIO_SINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_sink {
    /** Wird einmal aufgerufen, sobald die Abtastrate bekannt ist. 0 = OK. */
    int (*open)(void *ctx, uint32_t sample_rate_hz);
    /** Liefert @p n Samples. Darf NICHT lange blockieren. <0 = Fehler.       */
    int (*write)(void *ctx, const int16_t *pcm, size_t n);
    /** Wird beim Beenden der Verbindung aufgerufen (Aufraeumen). */
    void (*close)(void *ctx);
    void *ctx; /**< Beliebige Nutzdaten der Senke. */
} audio_sink_t;

#ifdef __cplusplus
}
#endif
#endif /* KIWI_AUDIO_SINK_H */
