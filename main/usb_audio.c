#include "usb_audio.h"
#include "usb_descriptors.h"
#include "usb_uac_trace.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcm_ring.h"
#include "usb_device_uac.h"

#define INPUT_BUFFER_SAMPLES 4800
#define MIC_OUTPUT_BUFFER_SAMPLES 9600
#define MIC_OUTPUT_WRITE_CHUNK_SAMPLES 256
#define MIC_PREFILL_SAMPLES 2880
#define MIC_TAIL_WAIT_US 60000
#define MIC_FRAME_HISTORY 64

static StaticSemaphore_t input_mutex_storage;
static SemaphoreHandle_t input_mutex;
static StaticSemaphore_t microphone_space_storage;
static SemaphoreHandle_t microphone_space;
static portMUX_TYPE microphone_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE input_init_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool input_mutex_ready;

/* All callers run in tasks. PCM in PSRAM must not mask USB interrupts.
 * Diagnostics/UI can arrive before usb_audio_init(), so initialize on first use. */
static void lock_input(void)
{
    if (!atomic_load_explicit(&input_mutex_ready, memory_order_acquire)) {
        portENTER_CRITICAL(&input_init_lock);
        if (!atomic_load_explicit(&input_mutex_ready, memory_order_relaxed)) {
            input_mutex = xSemaphoreCreateMutexStatic(&input_mutex_storage);
            microphone_space = xSemaphoreCreateBinaryStatic(&microphone_space_storage);
            configASSERT(input_mutex && microphone_space);
            atomic_store_explicit(&input_mutex_ready, true, memory_order_release);
        }
        portEXIT_CRITICAL(&input_init_lock);
    }
    BaseType_t taken = xSemaphoreTake(input_mutex, portMAX_DELAY);
    configASSERT(taken == pdTRUE);
    (void)taken;
}

static void unlock_input(void)
{
    xSemaphoreGive(input_mutex);
}

/* UAC callbacks copy PCM to/from these rings; USB DMA uses its own buffers. */
static EXT_RAM_BSS_ATTR int16_t input_buffer[INPUT_BUFFER_SAMPLES];
static pcm_ring_t input = {.samples = input_buffer, .capacity = INPUT_BUFFER_SAMPLES};
/* The USB ISR consumes this ring directly; never place it in PSRAM. */
static int16_t microphone_buffer[MIC_OUTPUT_BUFFER_SAMPLES];
static int16_t microphone_write_scratch[MIC_OUTPUT_WRITE_CHUNK_SAMPLES];
static pcm_ring_t microphone_output = {
    .samples = microphone_buffer, .capacity = MIC_OUTPUT_BUFFER_SAMPLES
};
static usb_audio_stats_t state = {.host_volume = 100};
static usb_audio_stats_t stats_snapshot;
static bool speaker_input_enabled;
static bool microphone_output_enabled;
static bool microphone_route_open;
static int64_t last_packet_us;
static uint64_t received_nonzero_samples;
static uint32_t microphone_generation;
static bool microphone_playing;
static int64_t microphone_last_write_us;
static bool microphone_resume_active;
static int64_t microphone_last_callback_us;
typedef struct {
    uint32_t pts;
    uint32_t hash;
    size_t bytes;
} microphone_frame_t;
static microphone_frame_t microphone_frames[MIC_FRAME_HISTORY];
static size_t microphone_frame_count, microphone_frame_next;
static uint32_t microphone_last_pts;

/* Caller holds input_mutex; invalidate writers waiting across a route change. */
static void reset_microphone(void)
{
    portENTER_CRITICAL(&microphone_lock);
    pcm_ring_reset(&microphone_output);
    microphone_route_open = microphone_output_enabled && state.mounted && state.mic_active;
    microphone_playing = false;
    microphone_last_write_us = 0;
    microphone_last_callback_us = 0;
    microphone_frame_count = microphone_frame_next = 0;
    ++microphone_generation;
    portEXIT_CRITICAL(&microphone_lock);
    xSemaphoreGive(microphone_space);
}

void usb_audio_set_microphone_active(bool active)
{
    lock_input();
    state.mic_active = active;
    reset_microphone();
    unlock_input();
}

/* Namespaced by main/CMakeLists.txt when the UAC component owns USB setup. */
#if !CONFIG_USB_DEVICE_UAC_AS_PART
void uac_device_mount_cb(void);
void uac_device_umount_cb(void);
void uac_device_suspend_cb(bool remote_wakeup_en);
void uac_device_resume_cb(void);
#endif

static esp_err_t receive_audio(uint8_t *data, size_t length, void *ctx)
{
    (void)ctx;
    int16_t mono[256];
    unsigned peak = 0;
    size_t nonzero = 0;
    size_t frames = length / (2 * sizeof(int16_t));
    for (size_t offset = 0; offset < frames;) {
        size_t count = frames - offset;
        if (count > 256) {
            count = 256;
        }
        for (size_t i = 0; i < count; ++i) {
            size_t at = (offset + i) * 4;
            int16_t left = (int16_t)((uint16_t)data[at] | ((uint16_t)data[at + 1] << 8));
            int16_t right = (int16_t)((uint16_t)data[at + 2] | ((uint16_t)data[at + 3] << 8));
            mono[i] = ((int32_t)left + right) / 2;
            nonzero += mono[i] != 0;
            unsigned magnitude = mono[i] < 0 ? -(int32_t)mono[i] : mono[i];
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        lock_input();
        if (speaker_input_enabled && !state.host_muted) {
            pcm_ring_write(&input, mono, count);
        }
        unlock_input();
        offset += count;
    }
    int64_t now = esp_timer_get_time();
    lock_input();
    state.bytes_received += length;
    received_nonzero_samples += nonzero;
    state.level = peak * 100 / 32768;
    last_packet_us = now;
    unlock_input();
    return ESP_OK;
}

static void host_mute(uint32_t mute, void *ctx)
{
    (void)ctx;
    lock_input();
    state.host_muted = mute != 0;
    if (mute) {
        pcm_ring_reset(&input);
    }
    unlock_input();
}

static void host_volume(uint32_t volume, void *ctx)
{
    (void)ctx;
    /* Keep host volume separate from the TTS speaker volume. */
    lock_input();
    state.host_volume = volume > 100 ? 100 : volume;
    unlock_input();
}

/* Caller holds microphone_lock. All touched PCM and metadata are internal RAM. */
static size_t read_microphone_locked(int16_t *data, size_t samples, int64_t now)
{
    if (microphone_route_open) {
        if (microphone_last_callback_us) {
            uint32_t gap = now - microphone_last_callback_us;
            if (gap > state.mic_callback_max_gap_us) state.mic_callback_max_gap_us = gap;
            if (gap > 15000) ++state.mic_callback_late;
        }
        microphone_last_callback_us = now;
    }
    size_t read = 0;
    if (microphone_route_open) {
        /* Accumulate jitter headroom, but release short final utterances too. */
        if (!microphone_playing && microphone_output.count &&
            (microphone_output.count >= MIC_PREFILL_SAMPLES ||
             now - microphone_last_write_us >= MIC_TAIL_WAIT_US)) {
            microphone_playing = true;
        }
        if (microphone_playing) {
            read = pcm_ring_read(&microphone_output, data, samples);
            if (read < samples) {
                state.mic_underrun_samples += samples - read;
                ++state.mic_underruns;
                microphone_playing = false;
            }
        }
    }
    return read;
}

size_t usb_audio_read_microphone_isr(int16_t *data, size_t samples)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&microphone_lock);
    size_t read = read_microphone_locked(data, samples, now);
    portEXIT_CRITICAL_ISR(&microphone_lock);
    if (read < samples) {
        memset(data + read, 0, (samples - read) * sizeof(*data));
    }
    if (read) {
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(microphone_space, &wake);
        portYIELD_FROM_ISR(wake);
    }
    return read;
}

static esp_err_t provide_microphone(uint8_t *data, size_t length,
                                    size_t *bytes_read, void *ctx)
{
    (void)ctx;
    if (!data || !bytes_read || length % sizeof(int16_t)) return ESP_ERR_INVALID_ARG;
    size_t samples = length / sizeof(int16_t);
    int64_t now = esp_timer_get_time();
    lock_input();
    portENTER_CRITICAL(&microphone_lock);
    size_t read = read_microphone_locked((int16_t *)data, samples, now);
    portEXIT_CRITICAL(&microphone_lock);
    unlock_input();
    if (read) xSemaphoreGive(microphone_space);
    if (read < samples) memset(data + read * sizeof(int16_t), 0, (samples - read) * sizeof(int16_t));
    *bytes_read = length;
    return ESP_OK;
}

esp_err_t usb_audio_init(void)
{
    lock_input();
    unlock_input();
    _Static_assert(CONFIG_UAC_SAMPLE_RATE == 48000, "USB input must be 48 kHz");
    _Static_assert(CONFIG_UAC_SPEAKER_CHANNEL_NUM == 2, "USB input must be stereo");
    _Static_assert(CONFIG_UAC_MIC_CHANNEL_NUM == 1, "USB microphone must be mono");
    _Static_assert(CONFIG_UAC_BYTES_PER_SAMPLE == 2, "USB input must be 16-bit PCM");
    uac_device_config_t config = {
        .output_cb = receive_audio,
        .input_cb = provide_microphone,
        .set_mute_cb = host_mute,
        .set_volume_cb = host_volume,
        .spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK,
        .mic_itf_num = ITF_NUM_AUDIO_STREAMING_MIC,
    };
    return uac_device_init(&config);
}

bool usb_audio_is_mounted(void)
{
    bool mounted;
    lock_input();
    mounted = state.mounted;
    unlock_input();
    return mounted;
}

void usb_audio_set_forwarding(bool speaker_input, bool mic_output_enabled)
{
    lock_input();
    if (speaker_input_enabled != speaker_input) {
        pcm_ring_reset(&input);
        speaker_input_enabled = speaker_input;
    }
    if (microphone_output_enabled != mic_output_enabled) {
        microphone_output_enabled = mic_output_enabled;
        reset_microphone();
    }
    unlock_input();
}

void usb_audio_clear(void)
{
    lock_input();
    pcm_ring_reset(&input);
    reset_microphone();
    unlock_input();
}

size_t usb_audio_read(int16_t *samples, size_t count)
{
    lock_input();
    size_t read = pcm_ring_read(&input, samples, count);
    unlock_input();
    return read;
}

void usb_audio_write_microphone(const int16_t *samples, size_t count)
{
    if (!samples || !count) {
        return;
    }
    lock_input();
    uint32_t generation = microphone_generation;
    unlock_input();
    for (size_t offset = 0; offset < count;) {
        size_t chunk = count - offset;
        if (chunk > MIC_OUTPUT_WRITE_CHUNK_SAMPLES) {
            chunk = MIC_OUTPUT_WRITE_CHUNK_SAMPLES;
        }
        int64_t now = esp_timer_get_time();
        lock_input();
        /* Render frames/stacks may be in PSRAM; stage before masking interrupts. */
        memcpy(microphone_write_scratch, samples + offset, chunk * sizeof(*samples));
        portENTER_CRITICAL(&microphone_lock);
        if (!microphone_output_enabled || !state.mounted || !state.mic_active ||
            generation != microphone_generation) {
            portEXIT_CRITICAL(&microphone_lock);
            unlock_input();
            return;
        }
        size_t space = microphone_output.capacity - microphone_output.count;
        if (chunk > space) chunk = space;
        if (chunk) {
            pcm_ring_write(&microphone_output, microphone_write_scratch, chunk);
            microphone_last_write_us = now;
            state.mic_written_samples += chunk;
        } else {
            ++state.mic_write_waits;
            /* Discard old signals while the ring is locked. A later read or
             * route reset then wakes the writer without one-tick polling. */
            xSemaphoreTake(microphone_space, 0);
        }
        portEXIT_CRITICAL(&microphone_lock);
        unlock_input();
        offset += chunk;
        if (!chunk) {
            BaseType_t signaled = xSemaphoreTake(microphone_space, portMAX_DELAY);
            configASSERT(signaled == pdTRUE);
            (void)signaled;
        }
    }
}

void usb_audio_get_stats(usb_audio_stats_t *stats)
{
    int64_t now = esp_timer_get_time();
    lock_input();
    portENTER_CRITICAL(&microphone_lock);
    stats_snapshot = state;
    stats_snapshot.mic_dropped_samples = microphone_output.dropped;
    stats_snapshot.mic_buffered_samples = microphone_output.count;
    portEXIT_CRITICAL(&microphone_lock);
    /* A caller's stack may be in PSRAM; copy there with interrupts enabled. */
    *stats = stats_snapshot;
    stats->streaming = state.mounted && last_packet_us && now - last_packet_us < 500000;
    stats->dropped_samples = input.dropped;
    stats->buffered_samples = input.count;
    if (!stats->streaming || stats->host_muted) {
        stats->level = 0;
    }
    unlock_input();
}

void usb_audio_trace_microphone_frame(uint32_t pts, const uint8_t *data, size_t bytes)
{
    if (!data || !bytes) return;
    uint32_t hash = 2166136261u;
    bool nonzero = false;
    for (size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ data[i]) * 16777619u;
        nonzero |= data[i] != 0;
    }
    lock_input();
    ++state.mic_render_frames;
    if (microphone_frame_count) {
        if (pts == microphone_last_pts) ++state.mic_pts_same;
        else if ((int32_t)(pts - microphone_last_pts) < 0) ++state.mic_pts_back;
    }
    /* A hint only: decoders can reuse PTS. Never suppress speech using a hash. */
    for (size_t i = 0; nonzero && i < microphone_frame_count; ++i) {
        const microphone_frame_t *previous = &microphone_frames[i];
        if (previous->pts == pts && previous->bytes == bytes && previous->hash == hash) {
            ++state.mic_repeat_hints;
            break;
        }
    }
    microphone_frames[microphone_frame_next] = (microphone_frame_t){pts, hash, bytes};
    microphone_frame_next = (microphone_frame_next + 1) % MIC_FRAME_HISTORY;
    if (microphone_frame_count < MIC_FRAME_HISTORY) ++microphone_frame_count;
    microphone_last_pts = pts;
    unlock_input();
}

void usb_audio_log_diagnostics(void)
{
    static uint32_t previous_out_bytes;
    static uint64_t previous_app_bytes;
    static uint32_t previous_in_bytes;
    static int64_t previous_us;
    usb_uac_trace_t trace;
    usb_uac_trace_get(&trace);
    usb_audio_stats_t stats;
    usb_audio_get_stats(&stats);
    lock_input();
    bool forwarding = speaker_input_enabled;
    uint64_t nonzero = received_nonzero_samples;
    unlock_input();
    int64_t now = esp_timer_get_time();
    int64_t age_ms = trace.out_packets ?
        (int64_t)((uint32_t)((uint32_t)now - trace.out_last_us) / 1000) : -1;
    const char *speed = tud_speed_get() == TUSB_SPEED_HIGH ? "HS" :
                        tud_speed_get() == TUSB_SPEED_FULL ? "FS" : "none";
    ESP_LOGI("USB_DIAG", "rev=%04x speed=%s mounted=%d alt=%" PRIu32 "/%" PRIu32
             " changes=%" PRIu32 " mute=%d forward=%d age_ms=%" PRId64
             " level=%u queued=%u",
             MOSAICO_USB_DEVICE_REVISION, speed, stats.mounted, trace.speaker_alt,
             trace.mic_alt, trace.changes, stats.host_muted, forwarding, age_ms,
             stats.level, (unsigned)stats.buffered_samples);
    ESP_LOGI("USB_DIAG", "window_ms=%" PRId64 " OUT packets=%" PRIu32
             " bytes=%" PRIu32 " delta=%" PRIu32 " zero=%" PRIu32
             " err=%" PRIu32 " last=%" PRIu32 " submit_fail=%" PRIu32
             " FB packets=%" PRIu32 " err=%" PRIu32 " submit_fail=%" PRIu32
             " value=%08" PRIx32 " len=%" PRIu32 " APP bytes=%" PRIu64
             " delta=%" PRIu64 " nonzero=%" PRIu64,
             (now - previous_us) / 1000, trace.out_packets, trace.out_bytes,
             trace.out_bytes - previous_out_bytes, trace.out_zero, trace.out_errors,
             trace.out_last_size, trace.out_submit_fail, trace.fb_packets,
             trace.fb_errors, trace.fb_submit_fail, trace.fb_value, trace.fb_size,
             stats.bytes_received, stats.bytes_received - previous_app_bytes, nonzero);
    ESP_LOGI("USB_MIC", "profile=" MOSAICO_USB_AUDIO_PROFILE
             " active=%d queued=%u written=%" PRIu64 " dropped=%" PRIu64
             " waits=%" PRIu32 " starves=%" PRIu32 " missing=%" PRIu64
             " IN packets=%" PRIu32 " bytes=%" PRIu32 " delta=%" PRIu32
             " zero=%" PRIu32 " err=%" PRIu32 " submit_fail=%" PRIu32,
             stats.mic_active, (unsigned)stats.mic_buffered_samples,
             stats.mic_written_samples, stats.mic_dropped_samples,
             stats.mic_write_waits, stats.mic_underruns, stats.mic_underrun_samples,
             trace.in_packets, trace.in_bytes, trace.in_bytes - previous_in_bytes,
             trace.in_zero, trace.in_errors, trace.in_submit_fail);
    ESP_LOGI("USB_MIC_PCM", "frames=%" PRIu32 " pts_same=%" PRIu32
             " pts_back=%" PRIu32 " repeat_hint=%" PRIu32
             " callback_late=%" PRIu32 " callback_max_us=%" PRIu32
             " IN_gap_over_2ms=%" PRIu32 " IN_gap_max_us=%" PRIu32,
             stats.mic_render_frames, stats.mic_pts_same, stats.mic_pts_back,
             stats.mic_repeat_hints, stats.mic_callback_late, stats.mic_callback_max_gap_us,
             trace.in_gap_over_2ms, trace.in_gap_max_us);
    ESP_LOGI("USB_MIC_IN", "tx_pcm=%" PRIu32 " tx_zlp=%" PRIu32
             " fail_pcm=%" PRIu32 " fail_zlp=%" PRIu32 " fail_unknown=%" PRIu32
             " fail_pcm_bytes=%" PRIu32 " fifo_empty=%" PRIu32
             " fifo_short=%" PRIu32 " feed_max_us=%" PRIu32,
             trace.in_tx_pcm, trace.in_tx_zlp, trace.in_fail_pcm, trace.in_fail_zlp,
             trace.in_fail_unknown, trace.in_fail_pcm_bytes, trace.in_fifo_empty,
             trace.in_fifo_short, trace.in_feed_max_us);
    previous_out_bytes = trace.out_bytes;
    previous_app_bytes = stats.bytes_received;
    previous_in_bytes = trace.in_bytes;
    previous_us = now;
}

void tud_mount_cb(void)
{
    usb_uac_trace_reset_interfaces();
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_mount_cb();
#endif
    lock_input();
    state.mounted = true;
    state.mic_active = false;
    microphone_resume_active = false;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    reset_microphone();
    unlock_input();
    ESP_LOGI("USB_AUDIO", "Mounted as " MOSAICO_USB_DEVICE_NAME
             " (48 kHz stereo speaker + mono microphone)");
}

void tud_umount_cb(void)
{
    usb_uac_trace_reset_interfaces();
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_umount_cb();
#endif
    lock_input();
    state.mounted = false;
    state.mic_active = false;
    microphone_resume_active = false;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    reset_microphone();
    unlock_input();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    lock_input();
    bool mic_active = state.mic_active;
    unlock_input();
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_suspend_cb(remote_wakeup_en);
#else
    (void)remote_wakeup_en;
#endif
    tud_umount_cb();
    lock_input();
    microphone_resume_active = mic_active;
    unlock_input();
}

void tud_resume_cb(void)
{
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_resume_cb();
#endif
    lock_input();
    state.mounted = true;
    state.mic_active = microphone_resume_active;
    microphone_resume_active = false;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    reset_microphone();
    unlock_input();
}

