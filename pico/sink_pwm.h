/**
 * @file sink_pwm.h
 * @brief Pico-Audio-Senke: 16-Bit-PCM -> PWM-Audio per DMA (Gegenstueck zu main/sink_dac.h).
 */
#ifndef SINK_PWM_H
#define SINK_PWM_H

#include "kiwi_audio_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Liefert die (einzige) PWM-Senke.
 * @param pin          GPIO des A-Kanals (GERADE); GPIO pin+1 gibt dasselbe Signal aus
 * @param gain         Software-Verstaerkung 1..8
 * @param buffer_ms    Jitterpuffer (Netz -> Audio) in Millisekunden
 * @param prebuffer_ms Vorpufferung vor Wiedergabestart in Millisekunden
 */
const audio_sink_t *sink_pwm_get(unsigned pin, int gain, int buffer_ms, int prebuffer_ms);

/** Zaehler fuer Diagnose: Jitterpuffer-Ueberlaeufe, -Unterlaeufe und verspaetete DMA-Bloecke. */
void sink_pwm_stats(uint32_t *overruns, uint32_t *underruns, uint32_t *late_blocks);

#ifdef __cplusplus
}
#endif
#endif
