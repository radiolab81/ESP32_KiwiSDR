/**
 * @file dir_store_flash.h
 * @brief ESP32-Store der Serverdatenbank: Flash-Partition "kiwidb" (zwei Slots A/B).
 */
#ifndef DIR_STORE_FLASH_H
#define DIR_STORE_FLASH_H

#include "kiwi_dir_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Findet die Partition "kiwidb" (siehe partitions.csv). NULL, wenn sie fehlt. */
const kiwi_dir_store_t *dir_store_flash_get(void);

#ifdef __cplusplus
}
#endif
#endif
