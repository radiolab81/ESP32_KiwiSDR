/**
 * @file app_main.c
 * @brief ESP32-Anwendung: WLAN verbinden -> KiwiSDR-Audio empfangen -> DAC.
 *
 * ABLAUF (drei FreeRTOS-Tasks)
 *   kiwi_task    : verbindet, ruft in einer Schleife kiwi_client_poll() auf und
 *                  baut bei Fehlern mit wachsender Wartezeit neu auf.
 *   dac_out      : (in sink_dac.c) holt PCM aus dem Ringpuffer -> DAC.
 *   console_task : liest Zeilen von der seriellen Konsole und stimmt um:
 *                    f 1008      Frequenz in kHz
 *                    m usb       Modus (am amn amw sam usb lsb cw nbfm ...)
 *                    s           Status      q  Verbindung beenden
 *                    d ...       Serververzeichnis: Liste laden, nach Land filtern,
 *                                Server waehlen/wechseln (Hilfe: "d"; siehe app_dir.c)
 *
 * Hoeflichkeit gegenueber dem Kiwi: Pro ESP32 immer nur EINE Verbindung, und
 * nach einem Fehler wird mindestens 5 s (dann 10, 20, 40, 60 s) gewartet,
 * damit der Server uns nicht als Dauerverbinder sperrt.
 */
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_dir.h"
#include "kiwi_client.h"
#include "kiwi_dir_db.h"
#include "sink_dac.h"
#ifdef CONFIG_KIWI_SPEC
#include "kiwi_spec_app.h"      /* optionale Spektrumanzeige */
#endif

static const char *TAG = "kiwi_app";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t g_wifi_events;
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
/* WLAN                                                                  */
/* ===================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WLAN getrennt, versuche erneut ...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strlcpy((char *)wc.sta.ssid, CONFIG_KIWI_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_KIWI_WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_KIWI_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* kein Modem-Sleep: weniger Jitter */
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ===================================================================== */
/* KiwiSDR-Task                                                          */
/* ===================================================================== */

static void kiwi_task(void *arg)
{
    (void)arg;
    uint32_t freq_hz = 0;
    char mode[8] = CONFIG_KIWI_MODE;
    if (kiwi_parse_khz(CONFIG_KIWI_FREQ_KHZ, &freq_hz) != 0) {
        ESP_LOGE(TAG, "KIWI_FREQ_KHZ ungueltig: '%s'", CONFIG_KIWI_FREQ_KHZ);
        vTaskDelete(NULL);
    }

    uint32_t backoff_ms = 5000;
    int failovers = 0;                              /* Ersatzserver-Wechsel in Folge */
    app_dir_server_initial(g_host, sizeof g_host, &g_port);   /* zuletzt gewaehlter Server oder Kconfig */

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
            .sink = sink_dac_get(CONFIG_KIWI_DAC_GAIN, CONFIG_KIWI_DAC_BUFFER_MS,
                                 CONFIG_KIWI_DAC_PREBUFFER_MS),
        };

        g_quit_session = 0;
        kiwi_err_t rc = kiwi_client_init(&g_client, &cfg);
        if (rc == KIWI_OK) {
            ESP_LOGI(TAG, "Verbinde %s:%u, %lu.%03lu kHz, Modus %s", g_host, (unsigned)g_port,
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
                    uint32_t ov, un;
                    kiwi_client_command(&g_client, "s", st, sizeof st);
                    sink_dac_stats(&ov, &un);
                    ESP_LOGI(TAG, "%s | DAC-Ueberlauf %u Unterlauf %u", st, (unsigned)ov, (unsigned)un);
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
        ESP_LOGW(TAG, "Sitzung beendet: %s", kiwi_strerror(rc));
        kiwi_client_close(&g_client);              /* IMMER sauber schliessen */

        /* ---- Serverwechsel (Benutzer) ---- */
        if (app_dir_take_switch(g_host, sizeof g_host, &g_port)) {
            ESP_LOGI(TAG, "Wechsel zu %s:%u", g_host, (unsigned)g_port);
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
                strlcpy(g_host, nh, sizeof g_host);
                g_port = np;
                failovers++;
                ESP_LOGW(TAG, "Ersatzserver %s:%u (%d/%d)", g_host, (unsigned)g_port, failovers,
                         CONFIG_KIWI_DIR_FAILOVER_MAX);
                vTaskDelay(pdMS_TO_TICKS(2000));   /* kurze Pause, den naechsten Kiwi nicht bedraengen */
                continue;
            }
        }
#endif
        if (g_client.st.frames > 0) failovers = 0;  /* es lief Audio -> Zaehler zuruecksetzen */

        ESP_LOGI(TAG, "Neuer Versuch in %u s", (unsigned)(backoff_ms / 1000));
        /* In 100-ms-Schritten warten, damit ein "d go" die Pause sofort beendet. */
        for (uint32_t w = 0; w < backoff_ms && !app_dir_switch_pending(); w += 100) vTaskDelay(pdMS_TO_TICKS(100));
        if (app_dir_take_switch(g_host, sizeof g_host, &g_port)) {   /* Wunsch kam waehrend der Pause */
            ESP_LOGI(TAG, "Wechsel zu %s:%u", g_host, (unsigned)g_port);
            failovers = 0;
            backoff_ms = 5000;
            continue;
        }
        backoff_ms = backoff_ms >= 60000 ? 60000 : backoff_ms * 2;
    }
}

/* ===================================================================== */
/* Serielle Konsole                                                      */
/* ===================================================================== */

static void console_task(void *arg)
{
    (void)arg;
    /* Eigener UART-Treiber nur zum Lesen; Logausgaben laufen unveraendert weiter. */
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    char line[80];                                 /* "d go host:port" braucht mehr als die alten 48 Zeichen */
    size_t n = 0;
    printf("\nBefehle: f <kHz> | m <modus> | s (Status) | q | d (Serverliste)\n");
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            if (n) {
                line[n] = '\0';
                char reply[200];
                if (app_dir_command(line)) {
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
        } else if (n < sizeof line - 1) {
            line[n++] = (char)ch;
            putchar(ch);                           /* lokales Echo */
            fflush(stdout);
        }
    }
}

/* ===================================================================== */

void app_main(void)
{
    /* NVS wird vom WLAN-Treiber fuer Kalibrierdaten benoetigt. */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    app_dir_init();                               /* Serverdatenbank aus dem Flash oeffnen (leer ist ok) */
    wifi_start();
#ifdef CONFIG_KIWI_SPEC
    kiwi_spec_app_init(kiwi_spec_hal_create());   /* Display + Tasks; Fehler sind nicht fatal */
#endif
    xTaskCreate(kiwi_task, "kiwi_net", 8192, NULL, 5, NULL);
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);   /* 6 KB: die d-Befehle formatieren Zeilen */
}
