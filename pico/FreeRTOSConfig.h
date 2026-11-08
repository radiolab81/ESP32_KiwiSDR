/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS-Konfiguration fuer Pico W (RP2040) und Pico 2 W (RP2350, ARM).
 *
 * Bewusst EIN Kern (configNUMBER_OF_CORES 1): Der KiwiSDR-Client braucht nur ca. 24 KB/s
 * Audio; ein Kern reicht weit. Das spart die SMP-Fallstricke (Spinlocks, Core-Affinitaet),
 * und der zweite Kern bleibt frei (z. B. fuer spaeter Display/Bedienelemente).
 * Wer SMP will: configNUMBER_OF_CORES 2 und configUSE_CORE_AFFINITY 1 setzen.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "rp2040_config.h"          /* vom Port: PICO-spezifische Voreinstellungen */

/* ---- Scheduler ---------------------------------------------------------- */
#define configNUMBER_OF_CORES                   1
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_TICKLESS_IDLE                 0
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    8      /* 0 = Idle ... 7 */
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)512)   /* in WORTEN (4 Byte) */
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configENABLE_BACKWARD_COMPATIBILITY     1      /* lwIP-FreeRTOS-Port (sys_arch.c) nutzt portTICK_RATE_MS */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5

/* ---- Speicher ----------------------------------------------------------- */
/* Heap-4 im SRAM. Bedarf: Jitterpuffer (24 KB bei 1 s), Task-Stacks (~35 KB),
 * lwIP-Tasks, kiwi_dir-Update (9 KB). Der RP2040 hat 264 KB SRAM insgesamt. */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   (112 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ---- Hooks / Diagnose --------------------------------------------------- */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1      /* vApplicationMallocFailedHook in main_pico.c */
#define configCHECK_FOR_STACK_OVERFLOW          2      /* vApplicationStackOverflowHook in main_pico.c */
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_APPLICATION_TASK_TAG          0
extern void vAssertCalled(const char *file, int line);
#define configASSERT(x)                         do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

/* ---- Synchronisation ---------------------------------------------------- */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    1      /* lwIP-FreeRTOS-Port (sys_arch.c) */
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_NEWLIB_REENTRANT              0      /* der Kiwi-Code nutzt errno nicht (siehe port_lwip.c) */
#define configUSE_C_RUNTIME_TLS_SUPPORT         0

/* ---- Software-Timer (u. a. fuer pico_async_context_freertos) ------------- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            ((configSTACK_DEPTH_TYPE)512)

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ---- Pico-SDK-Anbindung ------------------------------------------------- */
#define configSUPPORT_PICO_SYNC_INTEROP         1      /* SDK-Mutexe/Semaphoren arbeiten mit FreeRTOS zusammen */
#define configSUPPORT_PICO_TIME_INTEROP         1      /* SDK sleep_ms() & Co. geben die CPU frei              */

/* ---- RP2350 (Cortex-M33, ARM, kein TrustZone) --------------------------- */
#if PICO_RP2350
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16     /* einzig getesteter Wert dieses Ports */
#endif

/* ---- Optionale API-Funktionen ------------------------------------------- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xSemaphoreGetMutexHolder        1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1

#endif /* FREERTOS_CONFIG_H */
