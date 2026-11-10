/**
 * @file kiwi_spec_app.h
 * @brief Gemeinsame FreeRTOS-Anbindung des Spektrums (ESP32 und Pico, identischer Code).
 *
 * Nur drei Aufrufe aus der Anwendung:
 *   kiwi_spec_app_init(hal)        einmal beim Start (legt Display- und W/F-Task an)
 *   kiwi_spec_app_session_start()  nach erfolgreichem kiwi_client_connect()
 *   kiwi_spec_app_session_stop()   VOR kiwi_client_close()
 *
 * Alles ist optional und nicht-fatal: Ohne Display oder bei abgelehntem /W/F laeuft das
 * Audio unveraendert weiter. Die Plattform liefert nur kiwi_spec_hal_create().
 */
#ifndef KIWI_SPEC_APP_H
#define KIWI_SPEC_APP_H

#include "kiwi_client.h"
#include "kiwi_disp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Von der Plattform bereitgestellt (main/spec_hal_esp32.c, pico/spec_hal_pico.c):
 * Busse/Pins gemaess Konfiguration einrichten. NULL = Hardware nicht verfuegbar.
 * Wird genau einmal aufgerufen - vom ANWENDUNGSCODE, dessen Ergebnis an kiwi_spec_app_init()
 * uebergeben wird (keine Rueckabhaengigkeit der Komponente kiwi_disp von main: sonst scheitert
 * das Linken in ESP-IDF an der Archivreihenfolge).
 */
const kiwi_disp_hal_t *kiwi_spec_hal_create(void);

/** Display anlegen und Tasks starten. @p hal = Ergebnis von kiwi_spec_hal_create() (NULL erlaubt: Spektrum aus).
 *  Fehler werden nur geloggt (Audio ist wichtiger). */
void kiwi_spec_app_init(const kiwi_disp_hal_t *hal);

/** Master ist verbunden: W/F darf sich (sobald dessen Einrichtung fertig ist) anhaengen. */
void kiwi_spec_app_session_start(kiwi_client_t *master);

/** Beendet W/F und wartet (begrenzt) darauf. Danach darf der Master geschlossen werden. */
void kiwi_spec_app_session_stop(void);

#ifdef __cplusplus
}
#endif
#endif
