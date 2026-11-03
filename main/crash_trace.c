#include "crash_trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "riscv/rvruntime-frames.h"

#define TRACE_MAGIC UINT32_C(0x43545201)
#define PANIC_MAGIC UINT32_C(0x50414e01)

typedef struct {
    uint32_t stage, detail, uptime_ms, check;
} trace_mark_t;

typedef struct {
    uint32_t pc, ra, sp, cause, value;
} trace_frame_t;

typedef struct {
    uint32_t magic, boot_id, image;
    trace_mark_t marks[CRASH_TRACE_SLOTS];
    uint32_t panic_magic, exception, core;
    trace_frame_t frames[SOC_CPU_CORES_NUM];
} trace_record_t;

static RTC_NOINIT_ATTR volatile trace_record_t retained;
static trace_record_t previous;
static unsigned raw_reset[SOC_CPU_CORES_NUM];
static bool previous_valid;

void crash_trace_init(uint32_t boot_id)
{
    esp_reset_reason_t reason = esp_reset_reason();
    previous_valid = reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
                     retained.magic == TRACE_MAGIC;
    if (previous_valid) {
        previous = retained;
    }
    for (unsigned i = 0; i < SOC_CPU_CORES_NUM; ++i) {
        raw_reset[i] = esp_rom_get_reset_reason(i);
    }
    retained.magic = 0;
    for (unsigned i = 0; i < CRASH_TRACE_SLOTS; ++i) {
        retained.marks[i].check = 0;
        retained.marks[i].stage = 0;
    }
    retained.panic_magic = 0;
    retained.boot_id = boot_id;
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t image;
    memcpy(&image, app->app_elf_sha256, sizeof(image));
    retained.image = image;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    retained.magic = TRACE_MAGIC;
    crash_trace_mark(CRASH_TRACE_BOOT, CRASH_STAGE_BOOT, 0);
}

void crash_trace_mark(crash_trace_slot_t slot, crash_trace_stage_t stage, uint32_t detail)
{
    if ((unsigned)slot >= CRASH_TRACE_SLOTS) return;
    volatile trace_mark_t *mark = &retained.marks[slot];
    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000);
    mark->check = 0;
    mark->stage = stage;
    mark->detail = detail;
    mark->uptime_ms = uptime;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    mark->check = TRACE_MAGIC ^ stage ^ detail ^ uptime;
}

void __real_esp_panic_handler(panic_info_t *info);

/* Capture before IDF prints to UART: CDC depends on the scheduler, and even
 * the UART panic handler can fail and fall back to an RTC watchdog reset.
 * Keep this path in IRAM with scalar writes and no libc/RTOS/heap calls. */
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (retained.magic == TRACE_MAGIC && retained.panic_magic != PANIC_MAGIC) {
        retained.exception = g_panic_abort ? PANIC_EXCEPTION_ABORT : info->exception;
        retained.core = info->core;
        for (unsigned i = 0; i < SOC_CPU_CORES_NUM; ++i) {
            const RvExcFrame *frame = i == (unsigned)info->core ? info->frame : g_exc_frames[i];
            retained.frames[i].pc = frame ? frame->mepc : 0;
            retained.frames[i].ra = frame ? frame->ra : 0;
            retained.frames[i].sp = frame ? frame->sp : 0;
            retained.frames[i].cause = frame ? frame->mcause : 0;
            retained.frames[i].value = frame ? frame->mtval : 0;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        retained.panic_magic = PANIC_MAGIC;
    }
    __real_esp_panic_handler(info);
}

size_t crash_trace_format_previous(char *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    int length = snprintf(out, capacity, "\r\n--- RESET_TRACE: raw=%u/%u previous=%s",
                          raw_reset[0], raw_reset[1], previous_valid ? "yes" : "none");
    if (length < 0) return 0;
    size_t used = (size_t)length < capacity ? (size_t)length : capacity - 1;
#define APPEND(...) do { \
    int n = snprintf(out + used, capacity - used, __VA_ARGS__); \
    if (n > 0) used += (size_t)n < capacity - used ? (size_t)n : capacity - used - 1; \
} while (0)
    if (previous_valid) {
        APPEND(" boot=%08" PRIx32 " image=%08" PRIx32,
               previous.boot_id, previous.image);
        for (unsigned i = 0; i < CRASH_TRACE_SLOTS; ++i) {
            const trace_mark_t *mark = &previous.marks[i];
            if (mark->stage && mark->check == (TRACE_MAGIC ^ mark->stage ^ mark->detail ^ mark->uptime_ms)) {
                APPEND(" s%u=%" PRIu32 "/%" PRIu32 "@%" PRIu32 "ms",
                       i, mark->stage, mark->detail, mark->uptime_ms);
            }
        }
        if (previous.panic_magic == PANIC_MAGIC) {
            APPEND(" panic=%" PRIu32 " core=%" PRIu32, previous.exception, previous.core);
            for (unsigned i = 0; i < SOC_CPU_CORES_NUM; ++i) {
                const trace_frame_t *frame = &previous.frames[i];
                APPEND(" c%u:pc=%08" PRIx32 ",ra=%08" PRIx32 ",sp=%08" PRIx32
                       ",cause=%08" PRIx32 ",val=%08" PRIx32,
                       i, frame->pc, frame->ra, frame->sp, frame->cause, frame->value);
            }
        } else {
            APPEND(" panic=not-recorded");
        }
    }
    APPEND(" ---\r\n");
#undef APPEND
    return used;
}
