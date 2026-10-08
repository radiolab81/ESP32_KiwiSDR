/**
 * @file sink_dac.h
 * @brief ESP32-Audio-Senke: 16-Bit-PCM -> 8-Bit-DAC (GPIO25 + GPIO26).
 */
#ifndef SINK_DAC_H
#define SINK_DAC_H

#include "kiwi_audio_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Liefert die (einzige) DAC-Senke. gain: 1..8, buffer_ms/prebuffer_ms: Jitterpuffer. */
const audio_sink_t *sink_dac_get(int gain, int buffer_ms, int prebuffer_ms);

/** Zaehler fuer Diagnose. */
void sink_dac_stats(uint32_t *overruns, uint32_t *underruns);

#ifdef __cplusplus
}
#endif
#endif
