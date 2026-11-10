#include "kiwi_spec_app.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#define LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define STACK_BYTES(n) (n)
#else
#include "FreeRTOS.h"
#include "task.h"
#include "kiwi_config.h"
#include "klog.h"
#define LOGE(...) KLOGE(TAG, __VA_ARGS__)
#define LOGW(...) KLOGW(TAG, __VA_ARGS__)
#define LOGI(...) KLOGI(TAG, __VA_ARGS__)
#define STACK_BYTES(n) KIWI_STACK_BYTES(n)
#endif

#include "disp_ssd1306.h"
#include "disp_st7735.h"
#include "kiwi_port.h"
#include "kiwi_spec.h"
#include "kiwi_wf.h"

static const char *TAG = "kiwi_spec";

/* ---------------- Konfiguration (Kconfig bzw. kiwi_config.h) ---------------- */
#ifndef CONFIG_KIWI_SPEC_SPAN_KHZ
#define CONFIG_KIWI_SPEC_SPAN_KHZ 50
#endif
#ifndef CONFIG_KIWI_SPEC_FPS
#define CONFIG_KIWI_SPEC_FPS 5
#endif
#ifndef CONFIG_KIWI_SPEC_SMOOTH
#define CONFIG_KIWI_SPEC_SMOOTH 1
#endif
#ifndef CONFIG_KIWI_SPEC_WF_SPEED
#define CONFIG_KIWI_SPEC_WF_SPEED 2
#endif
#ifndef CONFIG_KIWI_SPEC_DBM_LO
#define CONFIG_KIWI_SPEC_DBM_LO (-110)
#endif
#ifndef CONFIG_KIWI_SPEC_DBM_HI
#define CONFIG_KIWI_SPEC_DBM_HI (-20)
#endif
#ifdef CONFIG_KIWI_SPEC_AUTOSCALE
#define SPEC_AUTOSCALE 1
#else
#define SPEC_AUTOSCALE 0
#endif
#ifdef CONFIG_KIWI_SPEC_ROTATE180
#define SPEC_ROTATE 1
#else
#define SPEC_ROTATE 0
#endif

#define STALE_MS 3000u        /* Spektrum aelter als 3 s -> "NO DATA" */
#define STOP_WAIT_MS 1500u    /* so lange wartet session_stop() auf den W/F-Task (>= connect-Zeit je Poll) */

/* ---------------- Zustand (alles statisch: keine Heap-Allokation) ---------------- */
static kiwi_spec_t g_spec;
static kiwi_wf_t   g_wf;
static kiwi_spec_ui_t g_ui;
static kiwi_disp_t *g_disp;
static union {
    disp_ssd1306_t ssd;
    disp_st7735_t  st;
} g_drv;

static kiwi_client_t *volatile g_master;
static atomic_int g_run;          /* 1 = Sitzung aktiv, W/F darf laufen            */
static atomic_int g_busy;         /* 1 = W/F-Task benutzt g_master / g_wf          */
static atomic_int g_ready;        /* init erfolgreich                               */
static volatile int g_giveup;     /* W/F fuer diese Sitzung aufgegeben (nur vom W/F-Task gesetzt, von start geloescht) */

/* ---------------- Display ---------------- */

static kiwi_disp_t *build_display(const kiwi_disp_hal_t *hal)
{
#if defined(CONFIG_KIWI_SPEC_PANEL_SSD1306_I2C) || defined(CONFIG_KIWI_SPEC_PANEL_SSD1306_SPI)
    disp_ssd1306_cfg_t c;
    memset(&c, 0, sizeof c);
#ifdef CONFIG_KIWI_SPEC_PANEL_SSD1306_I2C
    c.bus = KIWI_DISP_BUS_I2C;
    c.i2c_addr = CONFIG_KIWI_SPEC_I2C_ADDR;
#else
    c.bus = KIWI_DISP_BUS_SPI;
#endif
    c.width = CONFIG_KIWI_SPEC_WIDTH;
    c.height = CONFIG_KIWI_SPEC_HEIGHT;
#ifdef CONFIG_KIWI_SPEC_SH1106
    c.col_offset = 2;
#endif
    c.rotate180 = SPEC_ROTATE;
    return disp_ssd1306_init(&g_drv.ssd, hal, &c);
#elif defined(CONFIG_KIWI_SPEC_PANEL_ST7735)
    disp_st7735_cfg_t c;
    memset(&c, 0, sizeof c);
    c.width = CONFIG_KIWI_SPEC_WIDTH;
    c.height = CONFIG_KIWI_SPEC_HEIGHT;
    c.xoff = CONFIG_KIWI_SPEC_XOFF;
    c.yoff = CONFIG_KIWI_SPEC_YOFF;
#ifdef CONFIG_KIWI_SPEC_ST7735_INVERT
    c.invert = 1;
#endif
    (void)SPEC_ROTATE;
    return disp_st7735_init(&g_drv.st, hal, &c);
#else
#error "KIWI_SPEC: kein Panel gewaehlt (CONFIG_KIWI_SPEC_PANEL_*)"
#endif
}

static void disp_task(void *arg)
{
    (void)arg;
    TickType_t period = pdMS_TO_TICKS(1000u / (CONFIG_KIWI_SPEC_FPS > 0 ? CONFIG_KIWI_SPEC_FPS : 1));
    if (period < 1) period = 1;
    TickType_t last = xTaskGetTickCount();
    uint8_t reported = 0;
    for (;;) {
        int rc = kiwi_spec_refresh(&g_spec, g_disp, &g_ui, kiwi_time_ms(), STALE_MS);
        if (rc < 0 && !reported) { LOGW("Display-Fehler (wird zyklisch neu versucht)"); reported = 1; }
        else if (rc >= 0) reported = 0;
        vTaskDelayUntil(&last, period);
    }
}

/* ---------------- W/F ---------------- */

/** Fehler, bei denen ein erneuter Versuch in derselben Sitzung sinnlos oder unhoeflich waere. */
static int is_final(kiwi_err_t e)
{
    return e == KIWI_ERR_BUSY || e == KIWI_ERR_PASSWORD || e == KIWI_ERR_REDIRECT || e == KIWI_ERR_DOWN ||
           e == KIWI_ERR_HANDSHAKE || e == KIWI_ERR_UNSUPPORTED || e == KIWI_ERR_ARG;
}

static void sleep_run(uint32_t ms)       /* schlaeft in kleinen Schritten, bricht bei Sitzungsende ab */
{
    while (ms && atomic_load(&g_run)) {
        uint32_t s = ms > 100u ? 100u : ms;
        vTaskDelay(pdMS_TO_TICKS(s));
        ms -= s;
    }
}

/** Eine Sitzung bearbeiten. Rueckgabe: letzter Fehler; *got = Zeilen empfangen. */
static kiwi_err_t wf_session(kiwi_client_t *m, uint32_t *got)
{
    kiwi_wf_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.master = m;
    cfg.cols = g_disp->width;
    cfg.span_hz = (uint32_t)CONFIG_KIWI_SPEC_SPAN_KHZ * 1000u;
    cfg.speed = CONFIG_KIWI_SPEC_WF_SPEED;
    cfg.sink = &g_spec.sink;
    kiwi_err_t rc = kiwi_wf_init(&g_wf, &cfg);
    if (rc == KIWI_OK) rc = kiwi_wf_connect(&g_wf);
    while (rc == KIWI_OK && atomic_load(&g_run)) {
        rc = kiwi_wf_poll(&g_wf, 100);
        kiwi_spec_set_tune(&g_spec, m->cur_freq_hz, m->cur_mode);   /* einziger Schreiber: dieser Task */
    }
    *got = g_wf.st.lines;
    kiwi_wf_close(&g_wf);
    return rc;
}

static void wf_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = 5000;
    unsigned fails = 0;
    for (;;) {
        kiwi_client_t *m = g_master;
        if (!atomic_load(&g_run) || !m || g_giveup || !m->setup_done) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* Dekker-artig: erst busy setzen, dann run erneut pruefen (stop setzt run=0, wartet auf busy=0). */
        atomic_store(&g_busy, 1);
        if (!atomic_load(&g_run)) { atomic_store(&g_busy, 0); continue; }

        uint32_t got = 0;
        kiwi_err_t rc = wf_session(m, &got);
        kiwi_spec_reset(&g_spec);                       /* Anzeige: "NO DATA" (Schreiber = dieser Task) */
        atomic_store(&g_busy, 0);

        if (!atomic_load(&g_run)) { backoff_ms = 5000; fails = 0; continue; }   /* normales Sitzungsende */
        if (got) { backoff_ms = 5000; fails = 0; }
        if (is_final(rc)) {
            LOGW("Spektrum abgelehnt/nicht moeglich (%s) - Audio laeuft weiter, kein erneuter Versuch in dieser Sitzung",
                 kiwi_strerror(rc));
            g_giveup = 1;
        } else if (++fails >= 4) {
            LOGW("Spektrum: wiederholt Fehler (%s) - aufgegeben fuer diese Sitzung", kiwi_strerror(rc));
            g_giveup = 1;
        } else {
            LOGW("Spektrum getrennt (%s), neuer Versuch in %lu s", kiwi_strerror(rc), (unsigned long)(backoff_ms / 1000u));
            sleep_run(backoff_ms);
            if (backoff_ms < 60000u) backoff_ms *= 2u;
        }
    }
}

/* ---------------- Oeffentliche API ---------------- */

void kiwi_spec_app_init(const kiwi_disp_hal_t *hal)
{
    if (atomic_load(&g_ready)) return;
    if (!hal) { LOGE("Display-HAL nicht verfuegbar - Spektrum aus"); return; }
    g_disp = build_display(hal);
    if (!g_disp) { LOGE("Display-Konfiguration ungueltig - Spektrum aus"); return; }
    kiwi_spec_init(&g_spec, CONFIG_KIWI_SPEC_SMOOTH, SPEC_AUTOSCALE, CONFIG_KIWI_SPEC_DBM_LO, CONFIG_KIWI_SPEC_DBM_HI);
    memset(&g_ui, 0, sizeof g_ui);
    /* Prioritaeten bewusst UNTER dem Audio-/Netz-Task (ESP32: kiwi_net=5; Pico: kiwi_net=3): Anzeige und Spektrum
     * duerfen Audio nie verdraengen. Display = 1 (wie Konsole/Watchdog), W/F = 2. */
    if (xTaskCreate(disp_task, "spec_disp", STACK_BYTES(3072), NULL, 1, NULL) != pdPASS ||
        xTaskCreate(wf_task, "spec_wf", STACK_BYTES(4096), NULL, 2, NULL) != pdPASS) {
        LOGE("Task-Anlage fehlgeschlagen - Spektrum aus");
        return;
    }
    atomic_store(&g_ready, 1);
    LOGI("Spektrum aktiv: %ux%u, %d kHz Breite, %d Bilder/s", (unsigned)g_disp->width, (unsigned)g_disp->height,
         CONFIG_KIWI_SPEC_SPAN_KHZ, CONFIG_KIWI_SPEC_FPS);
}

void kiwi_spec_app_session_start(kiwi_client_t *master)
{
    if (!atomic_load(&g_ready)) return;
    g_giveup = 0;
    g_master = master;
    atomic_store(&g_run, 1);
}

void kiwi_spec_app_session_stop(void)
{
    if (!atomic_load(&g_ready)) return;
    atomic_store(&g_run, 0);
    for (uint32_t w = 0; atomic_load(&g_busy) && w < STOP_WAIT_MS; w += 10u) vTaskDelay(pdMS_TO_TICKS(10));
    if (atomic_load(&g_busy)) LOGW("W/F-Task reagiert nicht rechtzeitig (Netzwerk haengt?)");
    g_master = NULL;
}
