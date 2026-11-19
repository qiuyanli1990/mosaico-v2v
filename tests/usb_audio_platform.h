#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include "sdkconfig.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define EXT_RAM_BSS_ATTR
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern unsigned critical_depth, critical_entries, mutex_depth, mutex_creations, signal_creations;
extern unsigned isr_depth, isr_entries, isr_gives;
#define portENTER_CRITICAL(lock) ((void)(lock), ++critical_depth, ++critical_entries)
#define portEXIT_CRITICAL(lock) ((void)(lock), --critical_depth)
#define portENTER_CRITICAL_ISR(lock) (assert(isr_depth), ++isr_entries, portENTER_CRITICAL(lock))
#define portEXIT_CRITICAL_ISR(lock) portEXIT_CRITICAL(lock)
#define portYIELD_FROM_ISR(wake) ((void)(wake))
typedef int BaseType_t;
typedef struct { bool initialized, binary, available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define configASSERT(condition) assert(condition)
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    assert(critical_depth == 1 && !storage->initialized);
    storage->initialized = true;
    ++mutex_creations;
    return storage;
}
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    assert(critical_depth == 1 && !storage->initialized);
    storage->initialized = storage->binary = true;
    ++signal_creations;
    return storage;
}
void test_usb_wait_for_signal(void);
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{
    assert(mutex && mutex->initialized);
    if (mutex->binary) {
        if (!ticks) {
            assert(critical_depth == 1 && mutex_depth == 1 && !isr_depth);
            bool available = mutex->available;
            mutex->available = false;
            return available ? pdTRUE : pdFALSE;
        }
        assert(ticks == portMAX_DELAY && !critical_depth && !mutex_depth);
        while (!mutex->available) test_usb_wait_for_signal();
        mutex->available = false;
        return pdTRUE;
    }
    assert(ticks == portMAX_DELAY);
    assert(!isr_depth && critical_depth == 0 && mutex_depth++ == 0);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && mutex->initialized && !critical_depth && !isr_depth);
    if (mutex->binary) {
        bool available = mutex->available;
        mutex->available = true;
        return available ? pdFALSE : pdTRUE;
    }
    assert(mutex && mutex->initialized && mutex_depth-- == 1);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *wake)
{
    assert(isr_depth && !critical_depth && semaphore && semaphore->initialized && semaphore->binary);
    bool available = semaphore->available;
    semaphore->available = true;
    ++isr_gives;
    *wake = pdTRUE;
    return available ? pdFALSE : pdTRUE;
}
static inline void test_usb_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGI(...) test_usb_log(__VA_ARGS__)
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned ticks);
