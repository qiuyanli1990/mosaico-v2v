#include "usb_capture.h"
#include <stdatomic.h>
#include <string.h>
#include "board_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_audio.h"

static atomic_bool started[2];
static uint64_t samples_read[2];
static int64_t next_frame_us[2];
static atomic_bool portable_mode;
static atomic_int portable_input = -1;
static atomic_bool enabled[2];
static atomic_uint routing_revision;
static uint64_t valid_samples[2], silent_samples[2];
static int64_t next_metrics_us[2];

static void update_route(void)
{
    unsigned revision;
    do {
        revision = atomic_load(&routing_revision);
        int selected = atomic_load(&portable_mode) ? atomic_load(&portable_input) : 1;
        if (selected >= 0 && !atomic_load(&enabled[selected])) selected = -1;
        board_audio_set_route(selected);
        /* UI turn changes and session enable/disable run on different tasks.
         * Reapply if a concurrent setter overtook this snapshot. */
    } while (revision != atomic_load(&routing_revision));
}

void usb_capture_set_portable_mode(bool portable)
{
    atomic_store(&portable_mode, portable);
    atomic_store(&portable_input, -1);
    atomic_fetch_add(&routing_revision, 1);
    update_route();
}

void usb_capture_set_portable_input(int source)
{
    if (source < 0 || source > 1) source = -1;
    atomic_store(&portable_input, source);
    atomic_fetch_add(&routing_revision, 1);
    update_route();
    ESP_LOGI("USB_CAPTURE", "Travel send route=%d (ADC/AEC continue running)", source);
}

void usb_capture_set_enabled(unsigned source, bool allow)
{
    if (source > 1) return;
    atomic_store(&enabled[source], allow);
    atomic_fetch_add(&routing_revision, 1);
    update_route();
}

unsigned usb_capture_get_portable_level(void)
{
    return board_audio_input_level();
}

static esp_capture_err_t source_open(esp_capture_audio_src_if_t *src)
{
    (void)src;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_codecs(esp_capture_audio_src_if_t *src,
                                      const esp_capture_format_id_t **codecs, uint8_t *count)
{
    (void)src;
    static const esp_capture_format_id_t pcm = ESP_CAPTURE_FMT_ID_PCM;
    *codecs = &pcm;
    *count = 1;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_caps(esp_capture_audio_src_if_t *src,
                                    esp_capture_audio_info_t *requested,
                                    esp_capture_audio_info_t *actual)
{
    (void)src;
    if (requested->format_id != ESP_CAPTURE_FMT_ID_PCM) return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    *actual = (esp_capture_audio_info_t) {
        .format_id = ESP_CAPTURE_FMT_ID_PCM, .sample_rate = BOARD_AUDIO_RATE,
        .channel = 1, .bits_per_sample = 16,
    };
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_fixed(esp_capture_audio_src_if_t *src,
                                     const esp_capture_audio_info_t *caps)
{
    (void)src;
    return caps->format_id == ESP_CAPTURE_FMT_ID_PCM && caps->sample_rate == BOARD_AUDIO_RATE &&
           caps->channel == 1 && caps->bits_per_sample == 16 ?
           ESP_CAPTURE_ERR_OK : ESP_CAPTURE_ERR_NOT_SUPPORTED;
}

static unsigned source_index(esp_capture_audio_src_if_t *src)
{
    return src == usb_capture_source(USB_CAPTURE_SOURCE_MICROPHONE) ? 1 : 0;
}

static esp_capture_err_t source_start(esp_capture_audio_src_if_t *src)
{
    unsigned index = source_index(src);
    samples_read[index] = 0;
    valid_samples[index] = silent_samples[index] = 0;
    next_metrics_us[index] = esp_timer_get_time() + 10000000;
    next_frame_us[index] = esp_timer_get_time();
    atomic_store(&started[index], true);
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_read(esp_capture_audio_src_if_t *src,
                                    esp_capture_stream_frame_t *frame)
{
    if (!frame || !frame->data || frame->size <= 0 || frame->size % sizeof(int16_t)) {
        return ESP_CAPTURE_ERR_INVALID_ARG;
    }
    unsigned index = source_index(src);
    size_t samples = frame->size / sizeof(int16_t);
    if (samples > 4800) return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    int64_t duration_us = samples * 1000000ULL / BOARD_AUDIO_RATE;
    next_frame_us[index] += duration_us;
    /* One absolute sample clock for both live PCM and intentional silence.
     * Do not add a second notification wait or reset the clock after every read. */
    while (atomic_load(&started[index])) {
        int64_t remaining = next_frame_us[index] - esp_timer_get_time();
        if (remaining <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(remaining > 10000 ? 10 : 1));
    }
    if (!atomic_load(&started[index])) return ESP_CAPTURE_ERR_INVALID_STATE;

    size_t read = 0;
    bool portable = atomic_load(&portable_mode);
    if (atomic_load(&enabled[index])) {
        if (portable || index == 1) {
            /* Only this copy is direction gated. Hardware capture and AEC
             * never wait for an encoder, network connection or UI selection. */
            read = board_audio_read(index, (int16_t *)frame->data, samples, 40);
        } else if (usb_audio_is_mounted()) {
            read = usb_audio_read((int16_t *)frame->data, samples);
        }
    }
    memset(frame->data + read * sizeof(int16_t), 0, (samples - read) * sizeof(int16_t));
    valid_samples[index] += read;
    silent_samples[index] += samples - read;
    int64_t now = esp_timer_get_time();
    if (now >= next_metrics_us[index]) {
        ESP_LOGI("USB_CAPTURE", "Encoder source=%u PCM samples=%llu silence=%llu selected=%d enabled=%d",
                 index, (unsigned long long)valid_samples[index], (unsigned long long)silent_samples[index],
                 portable ? atomic_load(&portable_input) : 1, atomic_load(&enabled[index]));
        next_metrics_us[index] = now + 10000000;
    }
    if (esp_timer_get_time() - next_frame_us[index] > duration_us * 2) {
        next_frame_us[index] = esp_timer_get_time();
    }
    frame->pts = samples_read[index] * 1000 / BOARD_AUDIO_RATE;
    samples_read[index] += samples;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_stop(esp_capture_audio_src_if_t *src)
{
    atomic_store(&started[source_index(src)], false);
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_audio_src_if_t *usb_capture_source(usb_capture_source_id_t id)
{
    static esp_capture_audio_src_if_t sources[] = {
        {.open = source_open, .close = source_stop, .start = source_start, .stop = source_stop,
         .get_support_codecs = source_codecs, .set_fixed_caps = source_fixed,
         .negotiate_caps = source_caps, .read_frame = source_read},
        {.open = source_open, .close = source_stop, .start = source_start, .stop = source_stop,
         .get_support_codecs = source_codecs, .set_fixed_caps = source_fixed,
         .negotiate_caps = source_caps, .read_frame = source_read},
    };
    return id <= USB_CAPTURE_SOURCE_MICROPHONE ? &sources[id] : NULL;
}
