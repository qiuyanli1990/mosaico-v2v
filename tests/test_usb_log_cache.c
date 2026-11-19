#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXT_RAM_BSS_ATTR
#define portMAX_DELAY UINT32_MAX
typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static _Thread_local unsigned mutex_depth;
static unsigned critical_entries;
#define portENTER_CRITICAL(lock) ((void)(lock), ++critical_entries)
#define portEXIT_CRITICAL(lock) ((void)(lock))

static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    assert(pthread_mutex_init(storage, NULL) == 0);
    return storage;
}

static void xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{
    assert(ticks == portMAX_DELAY && !mutex_depth);
    assert(pthread_mutex_lock(mutex) == 0);
    ++mutex_depth;
}

static void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex_depth == 1);
    --mutex_depth;
    assert(pthread_mutex_unlock(mutex) == 0);
}

#include "usb_log_cache_functions.inc"

static char input[LOG_CACHE_SIZE * 2], output[LOG_CACHE_SIZE * 2];

static void fixture(void)
{
    log_head = log_tail = log_count = 0;
    memset(output, 0, sizeof(output));
}

static void check_wrap_and_overflow(void)
{
    fixture();
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (char)(i % 251);
    cache_log(input, LOG_CACHE_SIZE - 100);
    assert(read_cached_log(output, LOG_CACHE_SIZE - 200) == LOG_CACHE_SIZE - 200);
    assert(memcmp(output, input, LOG_CACHE_SIZE - 200) == 0);
    cache_log(input, 512);
    assert(read_cached_log(output, sizeof(output)) == 612);
    assert(memcmp(output, input + LOG_CACHE_SIZE - 200, 100) == 0);
    assert(memcmp(output + 100, input, 512) == 0);

    fixture();
    cache_log(input, LOG_CACHE_SIZE);
    cache_log(input + LOG_CACHE_SIZE, 512);
    assert(log_count == LOG_CACHE_SIZE);
    assert(read_cached_log(output, sizeof(output)) == LOG_CACHE_SIZE);
    assert(memcmp(output, input + 512, LOG_CACHE_SIZE) == 0);

    fixture();
    cache_log(input, sizeof(input));
    assert(read_cached_log(output, sizeof(output)) == LOG_CACHE_SIZE);
    assert(memcmp(output, input + LOG_CACHE_SIZE, LOG_CACHE_SIZE) == 0);
    assert(read_cached_log(output, sizeof(output)) == 0);
    cache_log(input, 100);
    assert(read_cached_log(output, 0) == 0 && log_count == 100);
}

static void *write_logs(void *arg)
{
    unsigned tag = (uintptr_t)arg;
    for (unsigned i = 0; i < 64; ++i) {
        unsigned char line[32];
        memset(line, tag + i, sizeof(line));
        cache_log((const char *)line, sizeof(line));
    }
    assert(!mutex_depth);
    return NULL;
}

static void check_concurrent_writers(void)
{
    fixture();
    pthread_t writers[2];
    assert(pthread_create(&writers[0], NULL, write_logs, (void *)(uintptr_t)1) == 0);
    assert(pthread_create(&writers[1], NULL, write_logs, (void *)(uintptr_t)129) == 0);
    for (unsigned i = 0; i < 2; ++i) assert(pthread_join(writers[i], NULL) == 0);
    assert(read_cached_log(output, sizeof(output)) == 4096);
    unsigned next[2] = {1, 129};
    for (unsigned offset = 0; offset < 4096; offset += 32) {
        unsigned char first = output[offset];
        unsigned writer = first >= 129;
        assert(first == next[writer]++);
        for (unsigned i = 1; i < 32; ++i) assert((unsigned char)output[offset + i] == first);
    }
    assert(next[0] == 65 && next[1] == 193);
}

int main(void)
{
    log_lock = xSemaphoreCreateMutexStatic(&log_lock_storage);
    check_wrap_and_overflow();
    check_concurrent_writers();
    assert(critical_entries == 0 && !mutex_depth);
    assert(pthread_mutex_destroy(log_lock) == 0);
    puts("PASS: real USB log cache preserves wrap/overflow and concurrent writes without masking interrupts");
}
