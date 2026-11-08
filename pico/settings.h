/**
 * @file settings.h
 * @brief Kleiner Einstellungsspeicher im Flash - ersetzt NVS des ESP32 (nur: zuletzt gewaehlter Server).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stddef.h>
#include <stdint.h>

/** 1 = gespeicherter Server gelesen (host/port gesetzt), 0 = keiner gespeichert/gueltig. */
int settings_load_server(char *host, size_t cap, uint16_t *port);

/** Speichert Server (Sektor wird neu geschrieben; selten aufgerufen: nur bei "d go"). 0 = OK. */
int settings_save_server(const char *host, uint16_t port);

#endif
