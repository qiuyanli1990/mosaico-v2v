#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CRASH_TRACE_BOOT,
    CRASH_TRACE_SESSION,
    CRASH_TRACE_SIGNAL_SPEAKER,
    CRASH_TRACE_SIGNAL_MIC,
    CRASH_TRACE_AUDIO,
    CRASH_TRACE_SLOTS,
} crash_trace_slot_t;

typedef enum {
    CRASH_STAGE_BOOT = 1,
    CRASH_STAGE_READY,
    CRASH_STAGE_JOIN,
    CRASH_STAGE_JOURNAL,
    CRASH_STAGE_WEBRTC,
    CRASH_STAGE_HTTP_BEGIN,
    CRASH_STAGE_HTTP_END,
    CRASH_STAGE_AEC_RESET,
    CRASH_STAGE_AEC_PROCESS,
    CRASH_STAGE_AUDIO_READY,
} crash_trace_stage_t;

void crash_trace_init(uint32_t boot_id);
/* Each slot has one task writer. This records numeric stages without logging,
 * heap allocation, or application locks. Call only from task context. */
void crash_trace_mark(crash_trace_slot_t slot, crash_trace_stage_t stage, uint32_t detail);
size_t crash_trace_format_previous(char *out, size_t capacity);
