/**
 * @file flash_util.h
 * @brief Sicheres Loeschen/Programmieren des QSPI-Flashs (gemeinsam fuer Datenbank und Einstellungen).
 *
 * Beim Schreiben ist der Flash fuer das Ausfuehren von Code NICHT lesbar (XIP aus). Das SDK
 * kuemmert sich mit flash_safe_execute() darum (Interrupts aus, ggf. anderen Kern anhalten).
 * Waehrend eines Sektor-Loeschens (typ. 45 ms, max. 400 ms) steht die CPU still; das Audio laeuft
 * dank DMA-Ring (sink_pwm.c) weiter.
 */
#ifndef FLASH_UTIL_H
#define FLASH_UTIL_H

#include <stddef.h>
#include <stdint.h>

/** Loescht @p len Bytes ab Flash-Offset @p off (beide Vielfache von 4096). 0 = OK. */
int kiwi_flash_erase(uint32_t off, size_t len);

/** Beschreibt (nur auf geloeschtem Bereich!) @p len Bytes ab beliebigem Offset. 0 = OK.
 *  Intern seitenweise (256 B); nicht betroffene Bytes bleiben unveraendert (0xFF wird nur "programmiert"). */
int kiwi_flash_write(uint32_t off, const void *data, size_t len);

/** Liest per XIP (memcpy aus dem Flash-Adressraum). */
void kiwi_flash_read(uint32_t off, void *buf, size_t len);

#endif
