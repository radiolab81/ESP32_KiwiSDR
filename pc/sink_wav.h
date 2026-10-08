/**
 * @file sink_wav.h
 * @brief PC-Audio-Senke: schreibt PCM in eine WAV-Datei (oder roh nach stdout).
 */
#ifndef SINK_WAV_H
#define SINK_WAV_H
#include "kiwi_audio_sink.h"

/** Legt eine Senke an. path "-" = rohes S16LE nach stdout (z. B. fuer aplay). */
audio_sink_t *sink_wav_create(const char *path);
void sink_wav_destroy(audio_sink_t *s);

#endif
