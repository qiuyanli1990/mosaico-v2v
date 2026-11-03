#include "usb_audio.h"
#include "usb_descriptors.h"
#include "usb_uac_trace.h"
#include <inttypes.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "pcm_ring.h"
#include "usb_device_uac.h"

#define INPUT_BUFFER_SAMPLES 4800
#define MIC_OUTPUT_BUFFER_SAMPLES 9600
#define MIC_OUTPUT_WRITE_CHUNK_SAMPLES 256

static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
/* UAC callbacks copy PCM to/from these rings; USB DMA uses its own buffers. */
static EXT_RAM_BSS_ATTR int16_t input_buffer[INPUT_BUFFER_SAMPLES];
static pcm_ring_t input = {.samples = input_buffer, .capacity = INPUT_BUFFER_SAMPLES};
static EXT_RAM_BSS_ATTR int16_t microphone_buffer[MIC_OUTPUT_BUFFER_SAMPLES];
static pcm_ring_t microphone_output = {
    .samples = microphone_buffer, .capacity = MIC_OUTPUT_BUFFER_SAMPLES
};
static usb_audio_stats_t state = {.host_volume = 100};
static bool speaker_input_enabled;
static bool microphone_output_enabled;
static int64_t last_packet_us;
static uint64_t received_nonzero_samples;

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
        portENTER_CRITICAL(&input_lock);
        if (speaker_input_enabled && !state.host_muted) {
            pcm_ring_write(&input, mono, count);
        }
        portEXIT_CRITICAL(&input_lock);
        offset += count;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&input_lock);
    state.bytes_received += length;
    received_nonzero_samples += nonzero;
    state.level = peak * 100 / 32768;
    last_packet_us = now;
    portEXIT_CRITICAL(&input_lock);
    return ESP_OK;
}

static void host_mute(uint32_t mute, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&input_lock);
    state.host_muted = mute != 0;
    if (mute) {
        pcm_ring_reset(&input);
    }
    portEXIT_CRITICAL(&input_lock);
}

static void host_volume(uint32_t volume, void *ctx)
{
    (void)ctx;
    /* Keep host volume separate from the TTS speaker volume. */
    portENTER_CRITICAL(&input_lock);
    state.host_volume = volume > 100 ? 100 : volume;
    portEXIT_CRITICAL(&input_lock);
}

static esp_err_t provide_microphone(uint8_t *data, size_t length,
                                    size_t *bytes_read, void *ctx)
{
    (void)ctx;
    if (!data || !bytes_read || length % sizeof(int16_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t samples = length / sizeof(int16_t);
    portENTER_CRITICAL(&input_lock);
    size_t read = microphone_output_enabled ?
                  pcm_ring_read(&microphone_output, (int16_t *)data, samples) : 0;
    portEXIT_CRITICAL(&input_lock);
    if (read < samples) {
        memset(data + read * sizeof(int16_t), 0,
               (samples - read) * sizeof(int16_t));
    }
    *bytes_read = length;
    return ESP_OK;
}

esp_err_t usb_audio_init(void)
{
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
    portENTER_CRITICAL(&input_lock);
    mounted = state.mounted;
    portEXIT_CRITICAL(&input_lock);
    return mounted;
}

void usb_audio_set_forwarding(bool speaker_input, bool mic_output_enabled)
{
    portENTER_CRITICAL(&input_lock);
    if (speaker_input_enabled != speaker_input) {
        pcm_ring_reset(&input);
        speaker_input_enabled = speaker_input;
    }
    if (microphone_output_enabled != mic_output_enabled) {
        pcm_ring_reset(&microphone_output);
        microphone_output_enabled = mic_output_enabled;
    }
    portEXIT_CRITICAL(&input_lock);
}

void usb_audio_clear(void)
{
    portENTER_CRITICAL(&input_lock);
    pcm_ring_reset(&input);
    pcm_ring_reset(&microphone_output);
    portEXIT_CRITICAL(&input_lock);
}

size_t usb_audio_read(int16_t *samples, size_t count)
{
    portENTER_CRITICAL(&input_lock);
    size_t read = pcm_ring_read(&input, samples, count);
    portEXIT_CRITICAL(&input_lock);
    return read;
}

void usb_audio_write_microphone(const int16_t *samples, size_t count)
{
    if (!samples || !count) {
        return;
    }
    for (size_t offset = 0; offset < count;) {
        size_t chunk = count - offset;
        if (chunk > MIC_OUTPUT_WRITE_CHUNK_SAMPLES) {
            chunk = MIC_OUTPUT_WRITE_CHUNK_SAMPLES;
        }
        portENTER_CRITICAL(&input_lock);
        if (microphone_output_enabled) {
            pcm_ring_write(&microphone_output, samples + offset, chunk);
        }
        portEXIT_CRITICAL(&input_lock);
        offset += chunk;
    }
}

void usb_audio_get_stats(usb_audio_stats_t *stats)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&input_lock);
    *stats = state;
    stats->streaming = state.mounted && last_packet_us && now - last_packet_us < 500000;
    stats->dropped_samples = input.dropped;
    stats->buffered_samples = input.count;
    stats->mic_dropped_samples = microphone_output.dropped;
    if (!stats->streaming || stats->host_muted) {
        stats->level = 0;
    }
    portEXIT_CRITICAL(&input_lock);
}

void usb_audio_log_diagnostics(void)
{
    static uint32_t previous_out_bytes;
    static uint64_t previous_app_bytes;
    static int64_t previous_us;
    usb_uac_trace_t trace;
    usb_uac_trace_get(&trace);
    usb_audio_stats_t stats;
    usb_audio_get_stats(&stats);
    portENTER_CRITICAL(&input_lock);
    bool forwarding = speaker_input_enabled;
    uint64_t nonzero = received_nonzero_samples;
    portEXIT_CRITICAL(&input_lock);
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
    previous_out_bytes = trace.out_bytes;
    previous_app_bytes = stats.bytes_received;
    previous_us = now;
}

void tud_mount_cb(void)
{
    usb_uac_trace_reset_interfaces();
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_mount_cb();
#endif
    portENTER_CRITICAL(&input_lock);
    state.mounted = true;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    pcm_ring_reset(&microphone_output);
    portEXIT_CRITICAL(&input_lock);
    ESP_LOGI("USB_AUDIO", "Mounted as " MOSAICO_USB_DEVICE_NAME
             " (48 kHz stereo speaker + mono microphone)");
}

void tud_umount_cb(void)
{
    usb_uac_trace_reset_interfaces();
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_umount_cb();
#endif
    portENTER_CRITICAL(&input_lock);
    state.mounted = false;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    pcm_ring_reset(&microphone_output);
    portEXIT_CRITICAL(&input_lock);
}

void tud_suspend_cb(bool remote_wakeup_en)
{
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_suspend_cb(remote_wakeup_en);
#else
    (void)remote_wakeup_en;
#endif
    tud_umount_cb();
}

void tud_resume_cb(void)
{
#if !CONFIG_USB_DEVICE_UAC_AS_PART
    uac_device_resume_cb();
#endif
    portENTER_CRITICAL(&input_lock);
    state.mounted = true;
    last_packet_us = 0;
    pcm_ring_reset(&input);
    pcm_ring_reset(&microphone_output);
    portEXIT_CRITICAL(&input_lock);
}

