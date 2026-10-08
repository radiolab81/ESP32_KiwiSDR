/**
 * @file sink_dac.c
 * @brief ESP32-Audio-Senke ueber den internen 8-Bit-DAC (DMA, "continuous mode").
 *
 * LEHRBUCH-HINTERGRUND
 * ====================
 *
 * 1. Warum ein Zwischenpuffer?
 *    Das WLAN liefert Audio in Schueben (alle ~43 ms ein Frame, mit Jitter);
 *    der DAC verbraucht es gleichmaessig. Zwischen beiden steht ein
 *    Ringpuffer (FreeRTOS StreamBuffer). Vor dem Start wird er zu einem Teil
 *    gefuellt ("Prebuffer"), damit kleine Netzverzoegerungen nicht sofort zu
 *    Aussetzern fuehren.
 *
 * 2. Warum Hochrechnen (Upsampling)?
 *    Der Kiwi liefert 12 kHz. Der DAC-DMA-Modus des ESP32 arbeitet aber nur
 *    ab ca. 19,6 kHz. Wir wiederholen daher jedes Sample "gestreckt" mit
 *    linearer Interpolation: 12 kHz -> 24 kHz (Faktor 2).
 *    Lineare Interpolation erzeugt Spiegelspektren oberhalb 6 kHz; fuer
 *    AM-Sprache/Rundfunk ist das hoerbar kaum relevant und durch einen
 *    einfachen RC-Tiefpass am Ausgang (z. B. 1 kOhm + 10 nF, ca. 16 kHz)
 *    weiter abschwaechbar.
 *
 * 3. Von 16 Bit auf 8 Bit
 *    Der ESP32-DAC hat nur 8 Bit (0..255, Mitte = 128 = ca. 1,65 V).
 *    Wir nehmen die oberen 8 Bit des (verstaerkten) 16-Bit-Samples und
 *    addieren 128 als Gleichspannungsoffset. Ausgang: GPIO25 (DAC1) und
 *    GPIO26 (DAC2) mit identischem Signal. Fuer Kopfhoerer/Verstaerker einen
 *    Koppelkondensator (z. B. 10 uF) in Reihe schalten, um den Offset zu
 *    entfernen.
 *
 * 4. Pufferunter-/ueberlauf
 *    - Ueberlauf (Netz schneller als DAC, Taktdrift): ueberzaehlige Samples
 *      werden verworfen und gezaehlt.
 *    - Unterlauf (Netz zu langsam): Stille ausgeben und neu vorpuffern.
 */
#include "sink_dac.h"

#include <string.h>

#include "driver/dac_continuous.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "sink_dac";

#define DAC_MIN_RATE_HZ  20000   /* untere Grenze des DAC-DMA-Modus (ca. 19,6 kHz) */
#define MAX_UPSAMPLE     6       /* z. B. 4 kHz * 6 = 24 kHz                       */
#define CHUNK_IN         128     /* Eingangs-Samples pro Schleifendurchlauf        */
#define SILENCE_BYTES    256

typedef struct {
    dac_continuous_handle_t dac;
    StreamBufferHandle_t    sb;
    TaskHandle_t            task;
    volatile int            run;
    volatile int            task_done;
    uint32_t                rate, up, prebuf_bytes;
    int                     gain, buffer_ms, prebuffer_ms;
    volatile uint32_t       overruns, underruns;
} dac_sink_t;

static dac_sink_t g;

/* Ein Eingangssample -> 'up' Ausgangsbytes (lineare Interpolation + 8 Bit). */
static inline uint8_t to_u8(int32_t v, int gain)
{
    v *= gain;
    if (v >  32767) v =  32767;      /* hart begrenzen statt ueberlaufen */
    if (v < -32768) v = -32768;
    return (uint8_t)((v >> 8) + 128);
}

static void dac_task(void *arg)
{
    (void)arg;
    int16_t in[CHUNK_IN];
    uint8_t out[CHUNK_IN * MAX_UPSAMPLE];
    uint8_t silence[SILENCE_BYTES];
    memset(silence, 128, sizeof silence);
    int32_t prev = 0;
    int prebuffering = 1;

    while (g.run) {
        if (prebuffering) {
            if (xStreamBufferBytesAvailable(g.sb) < g.prebuf_bytes) {
                /* Stille ausgeben; das Blockieren im DMA-Schreiben taktet die Schleife. */
                dac_continuous_write(g.dac, silence, sizeof silence, NULL, 1000);
                continue;
            }
            prebuffering = 0;
        }

        size_t got = xStreamBufferReceive(g.sb, in, sizeof in, 0);
        size_t ns = got / sizeof(int16_t);
        if (ns == 0) {                       /* Unterlauf */
            g.underruns++;
            prebuffering = 1;
            continue;
        }

        size_t o = 0;
        for (size_t i = 0; i < ns; i++) {
            int32_t x = in[i];
            for (uint32_t k = 0; k < g.up; k++) {
                int32_t v = prev + ((x - prev) * (int32_t)(k + 1)) / (int32_t)g.up;
                out[o++] = to_u8(v, g.gain);
            }
            prev = x;
        }
        /* Blockiert, bis die DMA-Puffer Platz haben -> Wiedergabetakt = DAC-Takt. */
        dac_continuous_write(g.dac, out, o, NULL, 1000);
    }
    g.task_done = 1;
    vTaskDelete(NULL);
}

/* ---- audio_sink_t-Schnittstelle ---- */

static int sink_open(void *ctx, uint32_t rate)
{
    (void)ctx;
    if (rate == 0) return -1;

    /* Hochrechnungsfaktor so waehlen, dass DAC-Rate >= 20 kHz. */
    g.rate = rate;
    g.up = (rate >= DAC_MIN_RATE_HZ) ? 1u : (DAC_MIN_RATE_HZ + rate - 1) / rate;
    if (g.up > MAX_UPSAMPLE) return -1;

    size_t sb_bytes = (size_t)rate * 2u * (uint32_t)g.buffer_ms / 1000u;
    g.prebuf_bytes = (uint32_t)((size_t)rate * 2u * (uint32_t)g.prebuffer_ms / 1000u);
    if (g.prebuf_bytes >= sb_bytes) g.prebuf_bytes = (uint32_t)(sb_bytes / 2);
    g.sb = xStreamBufferCreate(sb_bytes, 1);
    if (!g.sb) return -1;

    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,        /* GPIO25 + GPIO26 */
        .desc_num  = 4,                           /* 4 DMA-Deskriptoren ...        */
        .buf_size  = 1024,                        /* ... mit je 1024 Byte (~43 ms) */
        .freq_hz   = rate * g.up,
        .offset    = 0,
        .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,    /* alternativ: DAC_DIGI_CLK_SRC_APLL */
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,      /* beide Kanaele gleiches Signal */
    };
    if (dac_continuous_new_channels(&cfg, &g.dac) != ESP_OK) goto fail_sb;
    if (dac_continuous_enable(g.dac) != ESP_OK) goto fail_ch;

    g.run = 1;
    g.task_done = 0;
    g.overruns = g.underruns = 0;
    if (xTaskCreate(dac_task, "dac_out", 4096, NULL, configMAX_PRIORITIES - 4, &g.task) != pdPASS)
        goto fail_en;
    ESP_LOGI(TAG, "DAC: Quellrate %u Hz x%u = %u Hz, Puffer %d ms, Vorpuffer %d ms",
             (unsigned)rate, (unsigned)g.up, (unsigned)(rate * g.up), g.buffer_ms, g.prebuffer_ms);
    return 0;

fail_en: dac_continuous_disable(g.dac);
fail_ch: dac_continuous_del_channels(g.dac);
fail_sb: vStreamBufferDelete(g.sb);
    g.sb = NULL;
    return -1;
}

static int sink_write(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    if (!g.sb) return -1;
    size_t want = n * sizeof(int16_t);
    size_t room = xStreamBufferSpacesAvailable(g.sb) & ~(size_t)1;   /* nur ganze Samples */
    size_t w = want <= room ? want : room;
    if (w) xStreamBufferSend(g.sb, pcm, w, 0);     /* nie blockieren: Netz-Task muss weiterlaufen */
    if (w < want) g.overruns++;
    return 0;
}

static void sink_close(void *ctx)
{
    (void)ctx;
    if (!g.sb) return;
    g.run = 0;
    for (int i = 0; i < 100 && !g.task_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    dac_continuous_disable(g.dac);
    dac_continuous_del_channels(g.dac);
    vStreamBufferDelete(g.sb);
    g.sb = NULL;
    ESP_LOGI(TAG, "DAC geschlossen (Ueberlaeufe %u, Unterlaeufe %u)",
             (unsigned)g.overruns, (unsigned)g.underruns);
}

static audio_sink_t s_sink = { sink_open, sink_write, sink_close, NULL };

const audio_sink_t *sink_dac_get(int gain, int buffer_ms, int prebuffer_ms)
{
    g.gain = gain < 1 ? 1 : (gain > 8 ? 8 : gain);
    g.buffer_ms = buffer_ms;
    g.prebuffer_ms = prebuffer_ms;
    return &s_sink;
}

void sink_dac_stats(uint32_t *overruns, uint32_t *underruns)
{
    if (overruns) *overruns = g.overruns;
    if (underruns) *underruns = g.underruns;
}
