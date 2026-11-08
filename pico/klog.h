/**
 * @file klog.h
 * @brief Minimaler Ersatz fuer ESP_LOGx (ESP-IDF) - haelt app_dir.c/main_pico.c nahe am Original.
 */
#ifndef KLOG_H
#define KLOG_H

#include <stdio.h>

#define KLOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define KLOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define KLOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)

/** FreeRTOS-xTaskCreate() erwartet die Stackgroesse in WORTEN, ESP-IDF in BYTES. */
#define KIWI_STACK_BYTES(n) ((configSTACK_DEPTH_TYPE)((n) / sizeof(StackType_t)))

#endif
