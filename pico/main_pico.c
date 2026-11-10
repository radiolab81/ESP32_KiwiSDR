/**
 * @file main_pico.c
 * @brief Pico-W/Pico-2-W-Anwendung: WLAN verbinden -> KiwiSDR-Audio empfangen -> PWM-Audio.
 *        Port von main/app_main.c (ESP32); kiwi_task und Konsole sind bewusst nahezu identisch.
 *
 * ABLAUF (FreeRTOS-Tasks, Prioritaeten: hoeher = wichtiger)
 *   pwm_fill (6)  : (in sink_pwm.c) Jitterpuffer -> DMA-Bloecke
 *   tcpip (5), cyw43 async (4): lwIP- und WLAN-Treiber (vom SDK angelegt)
 *   kiwi_net (3)  : verbindet, ruft in einer Schleife kiwi_client_poll() auf, baut bei Fehlern
 *                   mit wachsender Wartezeit neu auf
 *   wifi (2)      : initialisiert den WLAN-Chip, verbindet, ueberwacht den Link, setzt/loescht
 *                   WIFI_CONNECTED_BIT (Ersatz fuer den ESP-IDF-Event-Handler)
 *   dir_upd (2)   : kurzlebig, nur waehrend "d update" (app_dir.c)
 *   console (1)   : liest Zeilen von USB/UART und stimmt um:
 *                     f 1008      Frequenz in kHz
 *                     m usb       Modus (am amn amw sam usb lsb cw nbfm ...)
 *                     s           Status      q  Verbindung beenden
 *                     d ...       Serververzeichnis (siehe app_dir.c; Hilfe: "d")
 *   wdt (1)       : fuettert den Hardware-Watchdog (nur wenn KIWI_WATCHDOG_MS > 0)
 *
 * Hoeflichkeit gegenueber dem Kiwi: nur EINE Verbindung, nach einem Fehler mindestens 5 s
 * (dann 10, 20, 40, 60 s) warten.
 */
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include "../main/app_dir.h"      /* gemeinsame API (unveraendert vom ESP32-Projekt) */
#include "kiwi_client.h"
#include "kiwi_config.h"
#include "kiwi_dir_db.h"
#include "klog.h"
#include "sink_pwm.h"
#ifdef CONFIG_KIWI_SPEC
#include "kiwi_spec_app.h"      /* optionale Spektrumanzeige */
#endif

static const char *TAG = "kiwi_app";

#define WIFI_CONNECTED_BIT (1u << 0)

#define PRIO_WDT      (tskIDLE_PRIORITY + 1)
#define PRIO_CONSOLE  (tskIDLE_PRIORITY + 1)
#define PRIO_WIFI     (tskIDLE_PRIORITY + 2)
#define PRIO_KIWI     (tskIDLE_PRIORITY + 3)

static EventGroupHandle_t g_wifi_events;
static TaskHandle_t       g_h_wifi, g_h_kiwi, g_h_console;   /* fuer den Befehl "h" */
static kiwi_client_t      g_client;        /* ~5,5 KB, bewusst statisch (nicht Stack) */
static volatile int       g_quit_session;  /* "q": aktuelle Sitzung beenden */
static volatile int       g_active;        /* 1 = g_client ist verbunden und darf angesprochen werden */
/* Aktueller Server. Statisch, weil kiwi_config_t nur Zeiger enthaelt (muessen gueltig bleiben).
 * Wird vom kiwi_task geaendert (Wechsel per "d go" oder Failover). */
static char     g_host[KIWI_DIR_HOST_LEN];
static uint16_t g_port;

#ifdef CONFIG_KIWI_ADPCM
#define KIWI_USE_ADPCM 1
#else
#define KIWI_USE_ADPCM 0
#endif

/* ===================================================================== */
/* WLAN (CYW43 + lwIP) - ersetzt den ESP-IDF-Event-Handler                */
/* ===================================================================== */

static void wifi_task(void *arg)
{
    (void)arg;

    /* Muss NACH dem Start des Schedulers geschehen (legt FreeRTOS-Tasks des Treibers an). */
    if (cyw43_arch_init_with_country(CONFIG_KIWI_WIFI_COUNTRY)) {
        KLOGE(TAG, "WLAN-Chip (CYW43) laesst sich nicht initialisieren");
        vTaskDelete(NULL);
    }
    cyw43_arch_enable_sta_mode();
#ifdef CONFIG_KIWI_WIFI_NO_POWERSAVE
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);   /* kein Modem-Sleep: weniger Jitter */
#endif

    const uint32_t auth = strlen(CONFIG_KIWI_WIFI_PASSWORD) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;

    for (;;) {
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
            xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(1000));                /* Link steht: nur beobachten */
            continue;
        }
        if (xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED_BIT) {
            xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
            KLOGW(TAG, "WLAN getrennt, versuche erneut ...");
        }
        int rc = cyw43_arch_wifi_connect_timeout_ms(CONFIG_KIWI_WIFI_SSID, CONFIG_KIWI_WIFI_PASSWORD, auth, 20000);
        if (rc == 0) {
            KLOGI(TAG, "WLAN verbunden, IP %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
        } else {
            KLOGW(TAG, "WLAN-Verbindung fehlgeschlagen (%d), neuer Versuch in 5 s", rc);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

/* ===================================================================== */
/* KiwiSDR-Task (inhaltlich identisch zu main/app_main.c)                 */
/* ===================================================================== */

static void kiwi_task(void *arg)
{
    (void)arg;
    uint32_t freq_hz = 0;
    char mode[8] = CONFIG_KIWI_MODE;
    if (kiwi_parse_khz(CONFIG_KIWI_FREQ_KHZ, &freq_hz) != 0) {
        KLOGE(TAG, "KIWI_FREQ_KHZ ungueltig: '%s'", CONFIG_KIWI_FREQ_KHZ);
        vTaskDelete(NULL);
    }

    uint32_t backoff_ms = 5000;
    int failovers = 0;                              /* Ersatzserver-Wechsel in Folge */
    (void)failovers;                                /* ohne KIWI_DIR_FAILOVER ungenutzt */
    app_dir_server_initial(g_host, sizeof g_host, &g_port);   /* zuletzt gewaehlter Server oder CMake-Vorgabe */

    for (;;) {
        /* Erst weitermachen, wenn das WLAN eine IP hat. */
        xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        kiwi_config_t cfg = {
            .host = g_host,
            .port = g_port,
            .password = CONFIG_KIWI_PASSWORD,
            .user_name = CONFIG_KIWI_USER_NAME,
            .freq_hz = freq_hz,
            .mode = mode,
            .compression = KIWI_USE_ADPCM,
            .sink = sink_pwm_get(CONFIG_KIWI_AUDIO_PIN, CONFIG_KIWI_AUDIO_GAIN, CONFIG_KIWI_AUDIO_BUFFER_MS,
                                 CONFIG_KIWI_AUDIO_PREBUFFER_MS),
        };

        g_quit_session = 0;
        kiwi_err_t rc = kiwi_client_init(&g_client, &cfg);
        if (rc == KIWI_OK) {
            KLOGI(TAG, "Verbinde %s:%u, %lu.%03lu kHz, Modus %s", g_host, (unsigned)g_port,
                  (unsigned long)(freq_hz / 1000), (unsigned long)(freq_hz % 1000), mode);
            rc = kiwi_client_connect(&g_client);
        }
        if (rc == KIWI_OK) {
            backoff_ms = 5000;                      /* Verbindung steht -> Wartezeit zuruecksetzen */
            g_active = 1;                           /* ab hier darf die Konsole umstimmen */
#ifdef CONFIG_KIWI_SPEC
            kiwi_spec_app_session_start(&g_client);
#endif
            /* app_dir_switch_pending(): Benutzer hat "d go ..." eingegeben -> Sitzung beenden. */
            while (!g_quit_session && !app_dir_switch_pending() &&
                   (rc = kiwi_client_poll(&g_client, 200)) == KIWI_OK) {
#ifdef CONFIG_KIWI_STATUS_LOG
                static uint32_t t_log;
                uint32_t now = kiwi_time_ms();
                if (now - t_log >= 10000u) {
                    char st[160];
                    uint32_t ov, un, late;
                    kiwi_client_command(&g_client, "s", st, sizeof st);
                    sink_pwm_stats(&ov, &un, &late);
                    KLOGI(TAG, "%s | Audio-Ueberlauf %u Unterlauf %u verspaetet %u | Heap min %u", st, (unsigned)ov,
                          (unsigned)un, (unsigned)late, (unsigned)xPortGetMinimumEverFreeHeapSize());
                    t_log = now;
                }
#endif
            }
            g_active = 0;
#ifdef CONFIG_KIWI_SPEC
            kiwi_spec_app_session_stop();           /* vor kiwi_client_close(): W/F haengt am Master */
#endif
            /* Zuletzt gewaehlte Abstimmung fuer eine Neuverbindung merken. */
            freq_hz = g_client.cur_freq_hz;
            memcpy(mode, g_client.cur_mode, sizeof mode);
            if (g_quit_session) rc = KIWI_ERR_CLOSED;
        }
        KLOGW(TAG, "Sitzung beendet: %s", kiwi_strerror(rc));
        kiwi_client_close(&g_client);              /* IMMER sauber schliessen */

        /* ---- Serverwechsel (Benutzer) ---- */
        if (app_dir_take_switch(g_host, sizeof g_host, &g_port)) {
            KLOGI(TAG, "Wechsel zu %s:%u", g_host, (unsigned)g_port);
            failovers = 0;
            backoff_ms = 5000;
            continue;                              /* sofort verbinden, keine Wartezeit */
        }

#ifdef CONFIG_KIWI_DIR_FAILOVER
        /* ---- Ausweichen auf anderen Server aus der Datenbank ----
         * Nur wenn der Server uns gar nicht erst Audio geliefert hat und der Fehler zu
         * "dieser Server geht gerade nicht" passt (belegt, down, Passwort, kein Handshake ...). */
        if (!g_quit_session && g_client.st.frames == 0 && failovers < CONFIG_KIWI_DIR_FAILOVER_MAX &&
            (xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED_BIT) &&      /* WLAN-Ausfall ist kein Kiwi-Problem */
            (rc == KIWI_ERR_BUSY || rc == KIWI_ERR_DOWN || rc == KIWI_ERR_PASSWORD || rc == KIWI_ERR_HANDSHAKE ||
             rc == KIWI_ERR_REDIRECT || rc == KIWI_ERR_TIMEOUT || rc == KIWI_ERR_NET || rc == KIWI_ERR_PROTO ||
             rc == KIWI_ERR_CLOSED)) {
            char nh[KIWI_DIR_HOST_LEN];
            uint16_t np;
            if (app_dir_failover(g_host, g_port, nh, sizeof nh, &np)) {
                snprintf(g_host, sizeof g_host, "%s", nh);
                g_port = np;
                failovers++;
                KLOGW(TAG, "Ersatzserver %s:%u (%d/%d)", g_host, (unsigned)g_port, failovers,
                      CONFIG_KIWI_DIR_FAILOVER_MAX);
                vTaskDelay(pdMS_TO_TICKS(2000));   /* kurze Pause, den naechsten Kiwi nicht bedraengen */
                continue;
            }
        }
#endif
        if (g_client.st.frames > 0) failovers = 0;  /* es lief Audio -> Zaehler zuruecksetzen */

        KLOGI(TAG, "Neuer Versuch in %u s", (unsigned)(backoff_ms / 1000));
        /* In 100-ms-Schritten warten, damit ein "d go" die Pause sofort beendet. */
        for (uint32_t w = 0; w < backoff_ms && !app_dir_switch_pending(); w += 100) vTaskDelay(pdMS_TO_TICKS(100));
        if (app_dir_take_switch(g_host, sizeof g_host, &g_port)) {   /* Wunsch kam waehrend der Pause */
            KLOGI(TAG, "Wechsel zu %s:%u", g_host, (unsigned)g_port);
            failovers = 0;
            backoff_ms = 5000;
            continue;
        }
        backoff_ms = backoff_ms >= 60000 ? 60000 : backoff_ms * 2;
    }
}

/* ===================================================================== */
/* Konsole (USB-CDC und/oder UART ueber das SDK-stdio)                    */
/* ===================================================================== */

/** Diagnose "h": freier Heap und Stack-Reserve (kleinster je erreichter Wert) der eigenen Tasks. */
static void print_health(void)
{
    printf("\nHeap frei %u B, Minimum seit Start %u B\n", (unsigned)xPortGetFreeHeapSize(),
           (unsigned)xPortGetMinimumEverFreeHeapSize());
    printf("Stack-Reserve (B): wifi %u, kiwi_net %u, console %u\n",
           (unsigned)(uxTaskGetStackHighWaterMark(g_h_wifi) * sizeof(StackType_t)),
           (unsigned)(uxTaskGetStackHighWaterMark(g_h_kiwi) * sizeof(StackType_t)),
           (unsigned)(uxTaskGetStackHighWaterMark(g_h_console) * sizeof(StackType_t)));
}

static void console_task(void *arg)
{
    (void)arg;
    char line[80];                                 /* "d go host:port" braucht mehr als 48 Zeichen */
    size_t n = 0;

    vTaskDelay(pdMS_TO_TICKS(2000));               /* Zeit, ein USB-Terminal zu oeffnen */
    printf("\nKiwiSDR-Client (%s)\n", PICO_BOARD);
    printf("Befehle: f <kHz> | m <modus> | s (Status) | q | d (Serverliste) | h (Heap/Stack)\n");
    for (;;) {
        int c = getchar_timeout_us(0);             /* nicht blockierend */
        if (c == PICO_ERROR_TIMEOUT) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        char ch = (char)c;
        if (ch == '\r' || ch == '\n') {
            if (n) {
                line[n] = '\0';
                char reply[200];
                if (!strcmp(line, "h")) {
                    print_health();
                } else if (app_dir_command(line)) {
                    printf("\n");                  /* "d ..." funktioniert auch ohne laufende Verbindung */
                } else if (!g_active) {
                    printf("\nnicht verbunden\n");
                } else {
                    int r = kiwi_client_command(&g_client, line, reply, sizeof reply);
                    printf("\n%s\n", reply);
                    if (r == 1) g_quit_session = 1;
                }
                n = 0;
            }
        } else if ((ch == '\b' || ch == 0x7f) && n) {
            n--;
            printf("\b \b");                       /* Backspace (im ESP32-Original nicht vorhanden) */
            fflush(stdout);
        } else if (ch >= ' ' && n < sizeof line - 1) {
            line[n++] = ch;
            putchar(ch);                           /* lokales Echo */
            fflush(stdout);
        }
    }
}

/* ===================================================================== */
/* Watchdog und FreeRTOS-Hooks                                            */
/* ===================================================================== */

#if CONFIG_KIWI_WATCHDOG_MS > 0
static void wdt_task(void *arg)
{
    (void)arg;
    for (;;) {
        watchdog_update();                         /* niedrigste Prioritaet: bleibt sie aus, steht das System */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_KIWI_WATCHDOG_MS / 4));
    }
}
#endif

static void fatal(const char *what)
{
    printf("\nFATAL: %s - Neustart\n", what);
    watchdog_reboot(0, 0, 50);
    for (;;) { }
}

/* Nur melden: die Aufrufer (sink_open, update_task, kiwi_net_connect) behandeln NULL selbst. */
void vApplicationMallocFailedHook(void) { printf("\nWARNUNG: FreeRTOS-Heap erschoepft (Befehl h zeigt die Reserve)\n"); }

void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    (void)t;
    printf("\nStack-Ueberlauf in Task '%s'\n", name);
    fatal("Stack-Ueberlauf");
}

void vAssertCalled(const char *file, int line)
{
    printf("\nASSERT %s:%d\n", file, line);
    fatal("configASSERT");
}

/* ===================================================================== */

int main(void)
{
    stdio_init_all();

    if (watchdog_caused_reboot()) KLOGW(TAG, "Start nach Watchdog-/Software-Reset");
    KLOGI(TAG, "KiwiSDR-Client fuer %s (Systemtakt %u MHz)", PICO_BOARD, (unsigned)(clock_get_hz(clk_sys) / 1000000u));

    app_dir_init();                               /* Serverdatenbank aus dem Flash oeffnen (leer ist ok) */
#ifdef CONFIG_KIWI_SPEC
    kiwi_spec_app_init(kiwi_spec_hal_create());   /* Display + Tasks (vor dem Scheduler-Start); Fehler sind nicht fatal */
#endif

    g_wifi_events = xEventGroupCreate();
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(wifi_task,    "wifi",     KIWI_STACK_BYTES(4096), NULL, PRIO_WIFI,    &g_h_wifi);
    ok &= xTaskCreate(kiwi_task,    "kiwi_net", KIWI_STACK_BYTES(8192), NULL, PRIO_KIWI,    &g_h_kiwi);
    ok &= xTaskCreate(console_task, "console",  KIWI_STACK_BYTES(6144), NULL, PRIO_CONSOLE, &g_h_console);   /* die d-Befehle formatieren Zeilen */
#if CONFIG_KIWI_WATCHDOG_MS > 0
    ok &= xTaskCreate(wdt_task,     "wdt",      KIWI_STACK_BYTES(1024), NULL, PRIO_WDT,     NULL);
#endif
    if (ok != pdPASS) fatal("Task konnte nicht angelegt werden (Heap zu klein?)");
#if CONFIG_KIWI_WATCHDOG_MS > 0
    watchdog_enable(CONFIG_KIWI_WATCHDOG_MS, true /* im Debugger anhalten */);
#endif
    vTaskStartScheduler();
    for (;;) { }                                  /* nie erreicht */
}
