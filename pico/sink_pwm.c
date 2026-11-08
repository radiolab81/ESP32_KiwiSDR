/**
 * @file sink_pwm.c
 * @brief Audio-Senke fuer RP2040/RP2350: PWM als "Digital-Analog-Wandler", gespeist per DMA.
 *
 * LEHRBUCH-HINTERGRUND
 * ====================
 *
 * 1. Warum PWM? Der Pico hat keinen DAC. Eine schnelle Pulsweitenmodulation (hier 48 kHz Traeger,
 *    ca. 11 Bit Aufloesung) plus ein RC-Tiefpass ergibt brauchbares Audio - deutlich besser als
 *    der 8-Bit-DAC des ESP32. (Fuer HiFi-Qualitaet waere I2S ueber PIO die naechste Stufe; die
 *    Schnittstelle audio_sink_t macht das zu einem reinen Austausch dieser Datei.)
 *
 * 2. Takt durch die Hardware, nicht durch Software: Der DMA-Kanal wird vom WRAP-Signal des
 *    PWM-Slices getaktet (DREQ). Pro PWM-Periode schreibt er genau einen neuen Pegel. Damit ist
 *    Abtastrate = PWM-Frequenz = Systemtakt / (TOP+1) - ohne Timer, ohne Jitter.
 *
 * 3. Beide Kanaele eines Slices liegen im selben 32-Bit-Register (CC: A = Bit 0..15, B = Bit 16..31).
 *    Ein 32-Bit-Schreibvorgang setzt also beide Pins gleichzeitig: "dual mono" wie beim
 *    DAC_CHANNEL_MODE_SIMUL des ESP32.
 *
 * 4. Ringpuffer aus vier DMA-Bloecken (Kettenschaltung 0->1->2->3->0):
 *    Ein Kanal pro Block; endet Block k, startet die Hardware sofort Block k+1 (CHAIN_TO) - luckenlos.
 *    Die Unterbrechung (IRQ) des beendeten Kanals bereitet ihn nur fuer die naechste Runde vor
 *    (Leseadresse/Zaehler zuruecksetzen, ohne zu starten) und weckt den Fuell-Task. Der Task hat
 *    dann 3 Blockzeiten (~64 ms) Zeit, den Block neu zu fuellen. Das ueberbrueckt auch das
 *    Loeschen eines Flash-Sektors beim Verzeichnis-Update (typisch 45 ms): DMA und PWM laufen
 *    waehrenddessen ohne CPU weiter.
 *
 * 5. Jitterpuffer: wie beim ESP32 ein StreamBuffer (Netz-Task schreibt nie blockierend, der
 *    Fuell-Task liest). Vorpufferung vor dem Start, bei Unterlauf Stille + erneutes Vorpuffern.
 *
 * 6. Hochrechnung: Kiwi liefert 12 kHz; die Ausgaberate wird auf >= 44,1 kHz angehoben (hier x4 =
 *    48 kHz) mit linearer Interpolation (dieselbe Rechnung wie in sink_dac.c).
 *
 * Hardware: GPIO pin (+ pin+1) -> RC-Tiefpass (z. B. 2,2 kOhm + 10 nF) -> Koppelkondensator
 * (10 uF) -> Verstaerker/Kopfhoerer-Verstaerker. Die GPIO sind kein Kopfhoererausgang!
 */
#include "sink_pwm.h"

#include <string.h>

#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

#include "klog.h"

static const char *TAG = "sink_pwm";

#define MIN_OUT_RATE_HZ  44100u   /* Ausgaberate mindestens (PWM-Traeger weit ueber der Hoergrenze) */
#define MAX_UPSAMPLE     6u
#define NBLK             4        /* DMA-Bloecke im Ring                                            */
#define BLK              1024     /* Ausgabe-Samples pro Block (21 ms bei 48 kHz); Vielfaches von 1 */
#define STAGE            128      /* Eingangs-Samples je Entnahme aus dem Jitterpuffer              */

#define DMA_IRQ_INDEX    1        /* DMA_IRQ_1: DMA_IRQ_0 bleibt anderen (z. B. dem WLAN-Treiber) */

typedef struct {
    /* Hardware */
    unsigned  pin, slice;
    uint32_t  top;                          /* PWM-Wrap-Wert (Aufloesung = top+1 Stufen) */
    uint32_t  silence_word;                 /* Mittelwert fuer beide Kanaele             */
    int       ch[NBLK];                     /* DMA-Kanaele                               */
    uint32_t  blk[NBLK][BLK];               /* Ausgabebloecke (A | B<<16)                */
    int       irq_installed;

    /* Software */
    StreamBufferHandle_t sb;
    TaskHandle_t         task;
    volatile int         run, task_done;
    uint32_t             rate, up, prebuf_bytes;
    int                  gain, buffer_ms, prebuffer_ms;
    volatile uint32_t    overruns, underruns, late_blocks;
    volatile uint32_t    irq_done;          /* vom ISR gezaehlte beendete Bloecke        */
    uint32_t             filled;            /* vom Task gefuellte Bloecke                */
    uint32_t             fill_idx;

    /* Interpolatorzustand (nur Fuell-Task) */
    int16_t  stg[STAGE];
    size_t   stg_len, stg_pos;
    int32_t  prev, cur;
    uint32_t phase;
    int      prebuffering;
} pwm_sink_t;

static pwm_sink_t g;

/* ------------------------------------------------------------------------- */
/* DMA-Unterbrechung: Kanal neu laden (ohne Start) und Fuell-Task wecken     */
/* ------------------------------------------------------------------------- */
static void __not_in_flash_func(dma_isr)(void)
{
    BaseType_t woken = pdFALSE;
    uint32_t status = dma_hw->ints1;
    for (int i = 0; i < NBLK; i++) {
        if (!(status & (1u << g.ch[i]))) continue;
        dma_hw->ints1 = 1u << g.ch[i];                              /* Flag quittieren */
        dma_channel_set_read_addr(g.ch[i], g.blk[i], false);        /* fuer die naechste Runde bereit, */
        dma_channel_set_trans_count(g.ch[i], BLK, false);           /* aber NICHT starten (Kette macht das) */
        g.irq_done++;
        if (g.task) vTaskNotifyGiveFromISR(g.task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------------- */
/* Fuell-Task: Jitterpuffer -> interpolieren -> PWM-Pegel                    */
/* ------------------------------------------------------------------------- */

/** int16-Sample (mit Verstaerkung) -> PWM-Pegel 0..top. */
static inline uint32_t to_level(int32_t v)
{
    v *= g.gain;
    if (v >  32767) v =  32767;      /* hart begrenzen statt ueberlaufen */
    if (v < -32768) v = -32768;
    return (uint32_t)(((uint32_t)(v + 32768) * (g.top + 1u)) >> 16);   /* 0 .. top */
}

static int pull_sample(int32_t *x)
{
    if (g.stg_pos >= g.stg_len) {
        size_t got = xStreamBufferReceive(g.sb, g.stg, sizeof g.stg, 0);
        g.stg_len = got / sizeof(int16_t);
        g.stg_pos = 0;
        if (!g.stg_len) return 0;
    }
    *x = g.stg[g.stg_pos++];
    return 1;
}

static inline size_t buffered_bytes(void)
{
    return xStreamBufferBytesAvailable(g.sb) + (g.stg_len - g.stg_pos) * sizeof(int16_t);
}

static void fill_block(uint32_t *dst)
{
    uint32_t i = 0;

    if (g.prebuffering) {
        if (buffered_bytes() < g.prebuf_bytes) goto silence;
        g.prebuffering = 0;
    }
    for (; i < BLK; i++) {
        if (g.phase == 0) {                       /* naechstes Eingangssample holen */
            int32_t nx;
            if (!pull_sample(&nx)) {              /* Unterlauf: Rest des Blocks Stille, neu vorpuffern */
                g.underruns++;
                g.prebuffering = 1;
                g.prev = g.cur = 0;
                goto silence;
            }
            g.prev = g.cur;
            g.cur = nx;
        }
        int32_t v = g.prev + ((g.cur - g.prev) * (int32_t)(g.phase + 1u)) / (int32_t)g.up;
        uint32_t lv = to_level(v);
        dst[i] = lv | (lv << 16);
        if (++g.phase == g.up) g.phase = 0;
    }
    return;

silence:
    for (; i < BLK; i++) dst[i] = g.silence_word;
}

static void fill_task(void *arg)
{
    (void)arg;
    while (g.run) {
        if (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(200)) == 0) continue;
        if (!g.run) break;

        /* Wie weit sind wir dem DMA hinterher? Ab NBLK-1 Rueckstand wird alter Inhalt erneut gespielt. */
        uint32_t lag = g.irq_done - g.filled;
        if (lag >= NBLK - 1) g.late_blocks++;

        fill_block(g.blk[g.fill_idx]);
        g.fill_idx = (g.fill_idx + 1) % NBLK;
        g.filled++;
    }
    g.task_done = 1;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* Hardware ein-/ausrichten                                                  */
/* ------------------------------------------------------------------------- */

static void hw_teardown(void)
{
    if (g.irq_installed) {
        for (int i = 0; i < NBLK; i++) if (g.ch[i] >= 0) dma_channel_set_irq1_enabled(g.ch[i], false);
        irq_set_enabled(DMA_IRQ_1, false);
        irq_remove_handler(DMA_IRQ_1, dma_isr);
        g.irq_installed = 0;
    }
    for (int i = 0; i < NBLK; i++) {
        if (g.ch[i] < 0) continue;
        /* Kette zuerst lösen (Kanal "verkettet" sich nur mit sich selbst = keine Kette), dann stoppen. */
        dma_channel_config c = dma_get_channel_config((uint)g.ch[i]);
        channel_config_set_chain_to(&c, (uint)g.ch[i]);
        dma_channel_set_config((uint)g.ch[i], &c, false);
    }
    for (int i = 0; i < NBLK; i++) {
        if (g.ch[i] < 0) continue;
        dma_channel_abort((uint)g.ch[i]);
        dma_channel_unclaim((uint)g.ch[i]);
        g.ch[i] = -1;
    }
    pwm_set_enabled(g.slice, false);
    gpio_deinit(g.pin);
    gpio_deinit(g.pin + 1);
}

static int hw_setup(uint32_t out_rate)
{
    g.slice = pwm_gpio_to_slice_num(g.pin);
    if (pwm_gpio_to_channel(g.pin) != PWM_CHAN_A) return -1;       /* Pin muss ein A-Kanal sein */

    /* PWM: Periode = 1 / Abtastrate. TOP = f_sys / f_s - 1 (auf ganzzahlig gerundet). */
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t period = (sys + out_rate / 2u) / out_rate;
    if (period < 256u || period > 65536u) return -1;               /* < 8 Bit oder > 16 Bit: unsinnig */
    g.top = period - 1u;
    uint32_t mid = period / 2u;
    g.silence_word = mid | (mid << 16);
    KLOGI(TAG, "PWM: GPIO%u/%u, Ausgaberate %u Hz (Soll %u), %u Stufen (> %u Bit)",
          g.pin, g.pin + 1u, (unsigned)(sys / period), (unsigned)out_rate, (unsigned)period,
          (unsigned)(31 - __builtin_clz(period)));

    pwm_config pc = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pc, 1);
    pwm_config_set_wrap(&pc, (uint16_t)g.top);
    pwm_init(g.slice, &pc, false);
    pwm_set_both_levels(g.slice, (uint16_t)mid, (uint16_t)mid);
    gpio_set_function(g.pin, GPIO_FUNC_PWM);
    gpio_set_function(g.pin + 1, GPIO_FUNC_PWM);

    /* Bloecke mit Stille fuellen, bevor DMA startet. */
    for (int i = 0; i < NBLK; i++)
        for (int j = 0; j < BLK; j++) g.blk[i][j] = g.silence_word;

    /* Vier verkettete DMA-Kanaele. */
    for (int i = 0; i < NBLK; i++) g.ch[i] = -1;
    for (int i = 0; i < NBLK; i++) {
        int c = dma_claim_unused_channel(false);
        if (c < 0) return -1;
        g.ch[i] = c;
    }
    for (int i = 0; i < NBLK; i++) {
        dma_channel_config c = dma_channel_get_default_config((uint)g.ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pwm_get_dreq(g.slice));          /* getaktet vom PWM-Wrap */
        channel_config_set_chain_to(&c, (uint)g.ch[(i + 1) % NBLK]);
        dma_channel_configure((uint)g.ch[i], &c, &pwm_hw->slice[g.slice].cc, g.blk[i], BLK, false);
    }

    /* Unterbrechung (geteilt, damit andere Nutzer von DMA_IRQ_1 nicht verdraengt werden). */
    dma_hw->ints1 = 0xFFFFu;
    for (int i = 0; i < NBLK; i++) dma_channel_set_irq1_enabled((uint)g.ch[i], true);
    irq_add_shared_handler(DMA_IRQ_1, dma_isr, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_priority(DMA_IRQ_1, PICO_LOWEST_IRQ_PRIORITY);           /* FromISR-Aufrufe jederzeit zulaessig */
    irq_set_enabled(DMA_IRQ_1, true);
    g.irq_installed = 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* audio_sink_t-Schnittstelle                                                */
/* ------------------------------------------------------------------------- */

static int sink_open(void *ctx, uint32_t rate)
{
    (void)ctx;
    if (rate == 0) return -1;

    g.rate = rate;
    g.up = (rate >= MIN_OUT_RATE_HZ) ? 1u : (MIN_OUT_RATE_HZ + rate - 1u) / rate;
    if (g.up > MAX_UPSAMPLE) return -1;

    size_t sb_bytes = (size_t)rate * 2u * (uint32_t)g.buffer_ms / 1000u;
    g.prebuf_bytes = (uint32_t)((size_t)rate * 2u * (uint32_t)g.prebuffer_ms / 1000u);
    if (g.prebuf_bytes >= sb_bytes) g.prebuf_bytes = (uint32_t)(sb_bytes / 2);
    g.sb = xStreamBufferCreate(sb_bytes, 1);
    if (!g.sb) { KLOGE(TAG, "Jitterpuffer (%u Byte): zu wenig Heap", (unsigned)sb_bytes); return -1; }

    g.overruns = g.underruns = g.late_blocks = 0;
    g.irq_done = g.filled = g.fill_idx = 0;
    g.stg_len = g.stg_pos = 0;
    g.prev = g.cur = 0;
    g.phase = 0;
    g.prebuffering = 1;
    g.run = 1;
    g.task_done = 0;
    g.task = NULL;

    if (xTaskCreate(fill_task, "pwm_fill", KIWI_STACK_BYTES(2048), NULL, configMAX_PRIORITIES - 2, &g.task) != pdPASS)
        goto fail_sb;
    if (hw_setup(rate * g.up) != 0) goto fail_task;

    /* Start: erst PWM (liefert DREQ), dann Kanal 0 - die Kette uebernimmt den Rest. */
    pwm_set_enabled(g.slice, true);
    dma_channel_start((uint)g.ch[0]);

    KLOGI(TAG, "Audio: Quellrate %u Hz x%u, Puffer %d ms, Vorpuffer %d ms, 4 x %u Samples DMA-Ring",
          (unsigned)rate, (unsigned)g.up, g.buffer_ms, g.prebuffer_ms, (unsigned)BLK);
    return 0;

fail_task:
    hw_teardown();
    g.run = 0;
    xTaskNotifyGive(g.task);
    for (int i = 0; i < 100 && !g.task_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
fail_sb:
    vStreamBufferDelete(g.sb);
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
    hw_teardown();                      /* zuerst: danach kommt kein DMA-IRQ mehr und der Task ist ungefaehrdet */
    g.run = 0;
    xTaskNotifyGive(g.task);
    for (int i = 0; i < 100 && !g.task_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    vStreamBufferDelete(g.sb);
    g.sb = NULL;
    KLOGI(TAG, "Audio geschlossen (Ueberlaeufe %u, Unterlaeufe %u, verspaetete Bloecke %u)",
          (unsigned)g.overruns, (unsigned)g.underruns, (unsigned)g.late_blocks);
}

static audio_sink_t s_sink = { sink_open, sink_write, sink_close, NULL };

const audio_sink_t *sink_pwm_get(unsigned pin, int gain, int buffer_ms, int prebuffer_ms)
{
    g.pin = pin;
    g.gain = gain < 1 ? 1 : (gain > 8 ? 8 : gain);
    g.buffer_ms = buffer_ms;
    g.prebuffer_ms = prebuffer_ms;
    for (int i = 0; i < NBLK; i++) g.ch[i] = -1;
    return &s_sink;
}

void sink_pwm_stats(uint32_t *overruns, uint32_t *underruns, uint32_t *late_blocks)
{
    if (overruns) *overruns = g.overruns;
    if (underruns) *underruns = g.underruns;
    if (late_blocks) *late_blocks = g.late_blocks;
}
