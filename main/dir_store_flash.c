/**
 * @file dir_store_flash.c
 * @brief Flash-Partition als kiwi_dir_store_t (Konzept A/B-Slots: kiwi_dir_store.h).
 *
 * Besonderheit Loeschen: Ein 4-KB-Sektor zu loeschen dauert ca. 30-50 ms, und
 * waehrenddessen steht der Zugriff auf den Flash (und damit Code im Flash!) still.
 * Der Audiopfad hat DMA-Puffer (4 x ~43 ms) und den Jitterpuffer als Reserve; wir
 * loeschen deshalb sektorweise und geben dazwischen die CPU frei. Ein einzelnes
 * Update loescht einmal 192 KB = 48 Sektoren (ca. 2-3 s); leichte Aussetzer
 * waehrend dieser Zeit sind moeglich, bleiben aber meist vom Puffer verdeckt.
 */
#include "dir_store_flash.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dir_flash";
static const esp_partition_t *s_part;
static size_t s_slot;

static int fl_read(void *ctx, int slot, size_t off, void *buf, size_t len)
{
    (void)ctx;
    if (off + len > s_slot) return -1;
    return esp_partition_read(s_part, (size_t)slot * s_slot + off, buf, len) == ESP_OK ? 0 : -1;
}

static int fl_erase(void *ctx, int slot)
{
    (void)ctx;
    for (size_t o = 0; o < s_slot; o += 4096) {
        if (esp_partition_erase_range(s_part, (size_t)slot * s_slot + o, 4096) != ESP_OK) return -1;
        vTaskDelay(1);                       /* CPU freigeben: Audio-/WLAN-Tasks duerfen dazwischen laufen */
    }
    return 0;
}

static int fl_write(void *ctx, int slot, size_t off, const void *buf, size_t len)
{
    (void)ctx;
    if (off + len > s_slot) return -1;
    return esp_partition_write(s_part, (size_t)slot * s_slot + off, buf, len) == ESP_OK ? 0 : -1;
}

static kiwi_dir_store_t s_store = { 0, 4096, fl_read, fl_erase, fl_write, NULL };

const kiwi_dir_store_t *dir_store_flash_get(void)
{
    if (s_part) return &s_store;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "kiwidb");
    if (!s_part) {
        ESP_LOGE(TAG, "Partition 'kiwidb' fehlt - partitions.csv aktiv? (idf.py fullclean, sdkconfig loeschen)");
        return NULL;
    }
    s_slot = (s_part->size / KIWI_DIR_SLOTS) & ~(size_t)4095;
    s_store.slot_size = s_slot;
    ESP_LOGI(TAG, "Partition kiwidb: %u KB, 2 Slots zu %u KB", (unsigned)(s_part->size / 1024), (unsigned)(s_slot / 1024));
    return &s_store;
}
