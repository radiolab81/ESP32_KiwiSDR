/**
 * @file app_dir.c
 * @brief ESP32-Anbindung der Serverdatenbank. Ueberblick: app_dir.h.
 *
 * THREADS
 *   console_task : ruft app_dir_command() auf (Filter, Liste, "d go", "d update")
 *   dir_upd      : kurzlebiger Task, nur waehrend eines Updates
 *   kiwi_task    : fragt app_dir_switch_pending()/take_switch()/failover()
 * Gemeinsamer Zustand ist klein: der Wechselwunsch (Host/Port + atomares Flag) und die
 * Datenbank selbst, die lesend aus allen Tasks benutzt werden darf (kiwi_dir_db.h).
 * Der Filter (s_cmd.q) wird nur vom Konsolen-Task geschrieben; der kiwi_task liest eine
 * Kopie fuer das Failover - ein seltenes "halb aktualisiert" waere harmlos (anderer Treffer).
 */
#include "app_dir.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "dir_store_flash.h"
#include "kiwi_client.h"
#include "kiwi_dir_cmd.h"
#include "kiwi_dir_update.h"

static const char *TAG = "app_dir";

static const kiwi_dir_store_t *s_store;
static kiwi_dir_db_t  s_db;
static kiwi_dir_cmd_t s_cmd;

/* ---- Wechselwunsch (Konsole -> kiwi_task) ---- */
static char       s_pend_host[KIWI_DIR_HOST_LEN];
static uint16_t   s_pend_port;
static atomic_int s_pend;                  /* 1 = Host/Port oben gueltig; Felder erst schreiben, dann Flag */

/* ---- Update ---- */
static atomic_int s_upd_running;
static uint32_t   s_last_attempt_ms;
static int        s_have_attempt;

static void out(void *user, const char *line) { (void)user; printf("%s\n", line); }

/* ================================================================== */
/* Update (eigener Task: das Herunterladen dauert einige Sekunden)    */
/* ================================================================== */

static uint32_t s_next_report;

static void on_progress(void *user, uint32_t bytes, uint32_t servers)
{
    (void)user;
    if (bytes >= s_next_report) {                      /* nicht jede 32 KB eine Zeile */
        printf("  Verzeichnis: %u KB geladen, %u Server\n", (unsigned)(bytes / 1024), (unsigned)servers);
        s_next_report = bytes + 256u * 1024u;
    }
}

static void update_task(void *arg)
{
    (void)arg;
    /* ~9 KB: auf dem Heap, nur fuer die Dauer des Updates (kein dauerhafter RAM-Verbrauch). */
    kiwi_dir_update_t *u = malloc(sizeof *u);
    if (!u) {
        printf("Update: zu wenig freier Speicher\n");
    } else {
        kiwi_dir_result_t r;
        s_next_report = 0;
        printf("Update: lade %s ...\n", CONFIG_KIWI_DIR_URL);
        int rc = kiwi_dir_update_http(u, s_store, &s_db, CONFIG_KIWI_DIR_URL, on_progress, NULL, &r);
        if (rc == KIWI_DIR_OK) {
            printf("Update ok: %u Server (vorher %u), %u Laender, %u uebersprungen, %u ueberzaehlig, Generation %u\n",
                   (unsigned)r.entries, (unsigned)r.old_entries, (unsigned)r.countries, (unsigned)r.skipped,
                   (unsigned)r.overflow, (unsigned)r.generation);
        } else {
            printf("Update fehlgeschlagen: %s (Datenbank unveraendert)\n", kiwi_dir_strerror(rc));
        }
        free(u);
    }
    atomic_store(&s_upd_running, 0);
    vTaskDelete(NULL);
}

static int cb_update(void *user, int force)
{
    (void)user;
    if (!s_store) { printf("Keine Flash-Partition 'kiwidb' - Update nicht moeglich.\n"); return -1; }
    uint32_t now = kiwi_time_ms();
    uint32_t min_ms = (uint32_t)CONFIG_KIWI_DIR_MIN_INTERVAL_S * 1000u;
    if (!force && s_have_attempt && now - s_last_attempt_ms < min_ms) {
        printf("Zu frueh: letzter Versuch vor %u s, Mindestabstand %d s ('d update force' erzwingt es).\n",
               (unsigned)((now - s_last_attempt_ms) / 1000u), CONFIG_KIWI_DIR_MIN_INTERVAL_S);
        return -1;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_upd_running, &expected, 1)) {
        printf("Es laeuft bereits ein Update.\n");
        return -1;
    }
    s_have_attempt = 1;
    s_last_attempt_ms = now;
    /* Prioritaet 3: unter Netzwerk-Task (5) und DAC-Task, damit das Audio Vorrang behaelt. */
    if (xTaskCreate(update_task, "dir_upd", 8192, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_upd_running, 0);
        printf("Update: Task konnte nicht gestartet werden.\n");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/* Serverwechsel                                                      */
/* ================================================================== */

static int cb_select(void *user, const kiwi_dir_rec_t *rec)
{
    (void)user;
    snprintf(s_pend_host, sizeof s_pend_host, "%s", rec->host);
    s_pend_port = rec->port;
    atomic_store(&s_pend, 1);              /* ab jetzt darf der kiwi_task es lesen */
    return 0;
}

int app_dir_switch_pending(void) { return atomic_load(&s_pend); }

/* ---- Letzten gewaehlten Server in NVS merken (ueberlebt Neustart) ---- */
static void nvs_save_server(const char *host, uint16_t port)
{
    nvs_handle_t h;
    if (nvs_open("kiwi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "host", host);
    nvs_set_u16(h, "port", port);
    nvs_commit(h);
    nvs_close(h);
}

int app_dir_take_switch(char *host, size_t cap, uint16_t *port)
{
    if (!atomic_exchange(&s_pend, 0)) return 0;
    snprintf(host, cap, "%s", s_pend_host);
    *port = s_pend_port;
    nvs_save_server(host, *port);
    return 1;
}

void app_dir_server_initial(char *host, size_t cap, uint16_t *port)
{
    snprintf(host, cap, "%s", CONFIG_KIWI_HOST);
    *port = CONFIG_KIWI_PORT;
    nvs_handle_t h;
    if (nvs_open("kiwi", NVS_READONLY, &h) != ESP_OK) return;
    char tmp[KIWI_DIR_HOST_LEN];
    size_t len = sizeof tmp;
    uint16_t p;
    if (nvs_get_str(h, "host", tmp, &len) == ESP_OK && nvs_get_u16(h, "port", &p) == ESP_OK && tmp[0] && p) {
        snprintf(host, cap, "%s", tmp);
        *port = p;
        ESP_LOGI(TAG, "Gespeicherter Server: %s:%u", host, (unsigned)p);
    }
    nvs_close(h);
}

int app_dir_failover(const char *cur_host, uint16_t cur_port, char *host, size_t cap, uint16_t *port)
{
    if (!kiwi_dir_db_valid(&s_db)) return 0;
    kiwi_dir_query_t q = s_cmd.q;           /* Filter des Benutzers (z. B. Land) bleibt wirksam */
    q.only_free = 1;                        /* Ersatz nur mit freiem Kanal */
    uint32_t start = 0, idx;
    if (kiwi_dir_db_lookup(&s_db, cur_host, cur_port, &idx) == 0) start = idx + 1;

    kiwi_dir_rec_t r;
    uint32_t cur = start;
    for (int pass = 0; pass < 2; pass++) {  /* erst ab dem aktuellen Eintrag, dann von vorn (reihum) */
        while (kiwi_dir_db_find(&s_db, &q, &cur, &r, &idx) == 1) {
            if (pass == 1 && idx >= start) break;
            if (r.port == cur_port && !strcmp(r.host, cur_host)) continue;
            snprintf(host, cap, "%s", r.host);
            *port = r.port;
            ESP_LOGW(TAG, "Failover -> %s (%s:%u)", r.name, r.host, (unsigned)r.port);
            return 1;
        }
        cur = 0;
    }
    return 0;
}

/* ================================================================== */
/* Start und Konsole                                                  */
/* ================================================================== */

void app_dir_init(void)
{
    s_store = dir_store_flash_get();
    kiwi_dir_cmd_init(&s_cmd, &s_db, out, cb_update, cb_select, NULL);
    if (!s_store) return;
    int rc = kiwi_dir_db_open(&s_db, s_store, 1);        /* mit Pruefsumme: erkennt halbe/kaputte Daten */
    if (rc == KIWI_DIR_OK)
        ESP_LOGI(TAG, "Serverdatenbank: %u Server, Generation %u",
                 (unsigned)kiwi_dir_db_count(&s_db), (unsigned)kiwi_dir_db_generation(&s_db));
    else
        ESP_LOGI(TAG, "Noch keine Serverdatenbank - 'd update' laedt die Liste (%s)", CONFIG_KIWI_DIR_URL);
}

int app_dir_command(const char *line)
{
    return kiwi_dir_command(&s_cmd, line);
}
