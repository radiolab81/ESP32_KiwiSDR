/**
 * @file app_dir.h
 * @brief ESP32-Anbindung der Serverdatenbank: Update-Task, Konsolenbefehle, Serverwechsel.
 *
 * Aufgabenteilung (nichts davon steckt in kiwi_core):
 *   kiwi_dir (portabel)   Datenbank, Parser, HTTP, Befehle        -> components/kiwi_dir
 *   app_dir  (nur ESP32)  FreeRTOS-Task fuers Update, NVS, Wechsel -> diese Datei
 *   app_main.c            fragt app_dir, welcher Server gerade gilt
 */
#ifndef APP_DIR_H
#define APP_DIR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Einmal beim Start (nach nvs_flash_init): Flash-Store und Datenbank oeffnen. */
void app_dir_init(void);

/**
 * Konsolenzeile pruefen. 1 = war ein "d ..."-Befehl und ist erledigt, 0 = anderer Befehl.
 * Darf aus dem Konsolen-Task aufgerufen werden (Stack >= 6 KB).
 */
int app_dir_command(const char *line);

/** Startserver: der zuletzt mit "d go" gewaehlte (aus NVS), sonst CONFIG_KIWI_HOST/PORT. */
void app_dir_server_initial(char *host, size_t cap, uint16_t *port);

/** 1, wenn der Benutzer einen Serverwechsel angefordert hat (billig, jede Schleife aufrufbar). */
int app_dir_switch_pending(void);

/** Uebernimmt den angeforderten Server (und merkt ihn in NVS). 1 = neuer Server steht in host/port. */
int app_dir_take_switch(char *host, size_t cap, uint16_t *port);

/**
 * Sucht einen Ersatzserver nach einem Fehlschlag von cur_host:cur_port: der naechste
 * Treffer des aktuellen Filters mit freiem Kanal (reihum, ohne den aktuellen).
 * @return 1 = gefunden (host/port gesetzt), 0 = nichts passendes / keine Datenbank
 */
int app_dir_failover(const char *cur_host, uint16_t cur_port, char *host, size_t cap, uint16_t *port);

#ifdef __cplusplus
}
#endif
#endif
