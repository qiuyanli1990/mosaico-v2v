#include "board_audio.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "aec_processor.h"
#include "crash_trace.h"
#include "bsp/esp_mosaico.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcm_ring.h"
#include "pcm_timeline.h"

#define DMA_SAMPLES 240                 /* 5 ms, independent of encoder frames */
#define DMA_EVENTS 32
#define CAPTURE_SAMPLES 4800
#define PLAYBACK_SAMPLES 9600
#define REFERENCE_SAMPLES 12000
#define AEC_MAX_SAMPLES 1536
#define REFERENCE_LOOKAHEAD 480          /* wait 10 ms for completed TX callbacks */

static const char *TAG = "BOARD_AUDIO";
static esp_codec_dev_handle_t codec;
static i2s_chan_handle_t tx_channel, rx_channel;
static SemaphoreHandle_t routing_lock;
static QueueHandle_t dma_events;
static atomic_uint requested_volume;
static atomic_uint input_level;
static atomic_uint dma_event_drops;
static atomic_bool ready;
static bool output_enabled[BOARD_AUDIO_SOURCES];
static int input_source = -1;
static uint32_t route_generation;
static int64_t route_since;
static int64_t last_speech_us;

typedef struct {
    int64_t start;
    bool reference;
    int16_t samples[DMA_SAMPLES];
} dma_frame_t;

typedef struct {
    int64_t next;
    bool reference;
    /* RX and TX each own a scratch frame; do not spend 496 bytes of the
     * shared ISR stack on every DMA callback. The queue copies the frame. */
    dma_frame_t frame;
} dma_clock_t;
static dma_clock_t rx_clock, tx_clock = {.reference = true};

/* ISR queue storage is internal RAM. Software PCM histories live in PSRAM. */
static EXT_RAM_BSS_ATTR int16_t captured_storage[CAPTURE_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t input_storage[BOARD_AUDIO_SOURCES][CAPTURE_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t output_storage[BOARD_AUDIO_SOURCES][PLAYBACK_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t reference_storage[REFERENCE_SAMPLES];
static pcm_ring_t captured = {.samples = captured_storage, .capacity = CAPTURE_SAMPLES};
static pcm_ring_t inputs[BOARD_AUDIO_SOURCES];
static pcm_ring_t outputs[BOARD_AUDIO_SOURCES];
static pcm_timeline_t reference = {.samples = reference_storage, .capacity = REFERENCE_SAMPLES};
static EXT_RAM_BSS_ATTR int16_t raw[AEC_MAX_SAMPLES], aligned_ref[AEC_MAX_SAMPLES], clean[AEC_MAX_SAMPLES];

static int64_t sample_tick(void)
{
    return (esp_timer_get_time() * BOARD_AUDIO_RATE + 500000) / 1000000;
}

/* Copy completed DMA PCM before the driver clears/reuses it. Neither the
 * network tasks nor the UI ever read/write/reconfigure I2S or the codec. */
static bool dma_completed(i2s_chan_handle_t channel, i2s_event_data_t *event, void *context)
{
    (void)channel;
    dma_clock_t *clock = context;
    if (!event->dma_buf ||
        (event->size != DMA_SAMPLES * sizeof(int16_t) &&
         event->size != DMA_SAMPLES * 2 * sizeof(int16_t))) {
        atomic_fetch_add(&dma_event_drops, 1);
        return false;
    }
    int64_t measured = sample_tick() - DMA_SAMPLES;
    if (!clock->next || measured - clock->next > REFERENCE_LOOKAHEAD ||
        clock->next - measured > REFERENCE_LOOKAHEAD) {
        clock->next = measured;
    }
    dma_frame_t *frame = &clock->frame;
    frame->start = clock->next;
    frame->reference = clock->reference;
    clock->next += DMA_SAMPLES;
    const int16_t *dma = event->dma_buf;
    size_t slots = event->size / (DMA_SAMPLES * sizeof(int16_t));
    for (size_t i = 0; i < DMA_SAMPLES; ++i) {
        /* esp_codec_dev selects the left slot for mono, while the physical
         * full-duplex I2S DMA can still be two-slot. Keep one canonical mono
         * sample for both RX Mic and TX reference. */
        frame->samples[i] = dma[i * slots];
    }
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(dma_events, frame, &wake) != pdTRUE) {
        atomic_fetch_add(&dma_event_drops, 1);
    }
    return wake == pdTRUE;
}

static unsigned peak(const int16_t *samples, size_t count)
{
    unsigned result = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned value = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
        if (value > result) result = value;
    }
    return result;
}

static void input_task(void *argument)
{
    (void)argument;
    dma_frame_t frame;
    int64_t mic_start = 0, mic_end = 0, last_echo = 0;
    int64_t next_metrics_us = 0, reset_retry_us = 0;
    uint64_t mic_samples = 0, processed = 0, sent[2] = {0}, reference_missing = 0;
    uint64_t captured_dropped = 0;
    unsigned mic_gaps = 0, raw_peak = 0, clean_peak = 0;
    unsigned resets = 0, max_dsp_us = 0, max_reset_us = 0;
    unsigned dsp_frames = 0, bypass_frames = 0;
    bool reset_pending = false;
    bool dsp_has_history = false;
    const size_t count = aec_processor_frame_samples();
    for (;;) {
        if (xQueueReceive(dma_events, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (frame.reference) {
            pcm_timeline_write(&reference, frame.start, frame.samples, DMA_SAMPLES);
            if (peak(frame.samples, DMA_SAMPLES)) {
                last_echo = frame.start + DMA_SAMPLES;
                xSemaphoreTake(routing_lock, portMAX_DELAY);
                last_speech_us = esp_timer_get_time();
                xSemaphoreGive(routing_lock);
            }
        } else {
            if (mic_end && frame.start != mic_end) {
                ++mic_gaps;
                pcm_ring_reset(&captured);
                reset_pending = true;
            }
            if (!captured.count) mic_start = frame.start;
            mic_end = frame.start + DMA_SAMPLES;
            pcm_ring_write(&captured, frame.samples, DMA_SAMPLES);
            mic_samples += DMA_SAMPLES;
            if (captured.dropped != captured_dropped) {
                captured_dropped = captured.dropped;
                mic_start = mic_end - (int64_t)captured.count;
                reset_pending = true;
            }
        }
        while (captured.count >= count + REFERENCE_LOOKAHEAD) {
            pcm_ring_read(&captured, raw, count);
            size_t available = pcm_timeline_read(&reference,
                mic_start - CONFIG_TRANSLATOR_AEC_REF_DELAY_MS * 48, aligned_ref, count);
            reference_missing += count - available;
            bool echo_active = last_echo && mic_start <= last_echo + BOARD_AUDIO_RATE / 4;
            /* No DAC playback or echo tail: the output is already raw Mic.
             * Avoid unused DSP work/reallocation during TLS setup or idle.
             * Reset stale adaptive history once playback resumes. */
            if (!echo_active && dsp_has_history) reset_pending = true;
            if (echo_active && reset_pending && esp_timer_get_time() >= reset_retry_us) {
                crash_trace_mark(CRASH_TRACE_AUDIO, CRASH_STAGE_AEC_RESET, mic_gaps);
                int64_t started = esp_timer_get_time();
                if (aec_processor_reset() != ESP_OK) {
                    ESP_LOGE(TAG, "AEC reset failed");
                    reset_retry_us = esp_timer_get_time() + 1000000;
                } else {
                    reset_pending = false;
                    dsp_has_history = false;
                }
                unsigned elapsed = (unsigned)(esp_timer_get_time() - started);
                if (elapsed > max_reset_us) max_reset_us = elapsed;
                ++resets;
            }
            unsigned elapsed = 0;
            esp_err_t result = ESP_ERR_INVALID_STATE;
            if (echo_active) {
                crash_trace_mark(CRASH_TRACE_AUDIO, CRASH_STAGE_AEC_PROCESS, resets);
                int64_t started = esp_timer_get_time();
                result = aec_processor_process_48k(raw, aligned_ref, clean, count);
                elapsed = (unsigned)(esp_timer_get_time() - started);
                if (elapsed > max_dsp_us) max_dsp_us = elapsed;
                if (result == ESP_OK) {
                    dsp_has_history = true;
                    ++dsp_frames;
                }
            }
            if (result != ESP_OK) {
                memcpy(clean, raw, count * sizeof(*clean));
                ++bypass_frames;
            }
            crash_trace_mark(CRASH_TRACE_AUDIO, CRASH_STAGE_AUDIO_READY, elapsed);
            unsigned raw_value = peak(raw, count), clean_value = peak(clean, count);
            if (raw_value > raw_peak) raw_peak = raw_value;
            if (clean_value > clean_peak) clean_peak = clean_value;
            atomic_store(&input_level, raw_value * 100 / 32768);
            xSemaphoreTake(routing_lock, portMAX_DELAY);
            int selected = input_source;
            /* Slice a frame at the exact UI turn boundary. Keep the new
             * speaker's first syllable without forwarding pre-turn samples. */
            if (selected >= 0 && mic_start + (int64_t)count > route_since) {
                size_t offset = route_since > mic_start ? (size_t)(route_since - mic_start) : 0;
                pcm_ring_write(&inputs[selected], clean + offset, count - offset);
                sent[selected] += count - offset;
            }
            xSemaphoreGive(routing_lock);
            mic_start += count;
            processed += count;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_metrics_us) {
            xSemaphoreTake(routing_lock, portMAX_DELAY);
            uint64_t dropped = inputs[0].dropped + inputs[1].dropped;
            int selected = input_source;
            xSemaphoreGive(routing_lock);
            ESP_LOGI(TAG, "Mic=%" PRIu64 " AEC=%" PRIu64 " routed=%" PRIu64 "/%" PRIu64
                     " raw_peak=%u clean_peak=%u route=%d ref_missing=%" PRIu64
                     " dma_drops=%u mic_gaps=%u input_dropped=%" PRIu64
                     " resets=%u dsp_frames=%u bypass_frames=%u dsp_max_us=%u reset_max_us=%u stack_free=%u",
                     mic_samples, processed, sent[0], sent[1], raw_peak, clean_peak, selected,
                     reference_missing, atomic_load(&dma_event_drops), mic_gaps, dropped + captured_dropped,
                     resets, dsp_frames, bypass_frames, max_dsp_us, max_reset_us,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            raw_peak = clean_peak = 0;
            next_metrics_us = now + 10000000;
        }
    }
}

static void output_task(void *argument)
{
    (void)argument;
    int16_t first[DMA_SAMPLES], second[DMA_SAMPLES], mixed[DMA_SAMPLES];
    unsigned applied_volume = UINT_MAX;
    for (;;) {
        unsigned volume = atomic_load(&requested_volume);
        if (volume != applied_volume && esp_codec_dev_set_out_vol(codec, volume) == ESP_CODEC_DEV_OK) {
            applied_volume = volume;
        }
        xSemaphoreTake(routing_lock, portMAX_DELAY);
        size_t a = output_enabled[0] ? pcm_ring_read(&outputs[0], first, DMA_SAMPLES) : 0;
        size_t b = output_enabled[1] ? pcm_ring_read(&outputs[1], second, DMA_SAMPLES) : 0;
        xSemaphoreGive(routing_lock);
        memset(first + a, 0, (DMA_SAMPLES - a) * sizeof(*first));
        memset(second + b, 0, (DMA_SAMPLES - b) * sizeof(*second));
        pcm_mix_s16(mixed, first, second, DMA_SAMPLES);
        if (!volume) memset(mixed, 0, sizeof(mixed));
        int result = esp_codec_dev_write(codec, mixed, sizeof(mixed));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "DAC write failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        /* AEC reference/speaking state come from completed TX, never from a
         * successful queue insertion or a pending write. */
    }
}

esp_err_t board_audio_init(unsigned volume)
{
    if (codec) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(aec_processor_init(), TAG, "AEC init failed");
    routing_lock = xSemaphoreCreateMutex();
    dma_events = xQueueCreate(DMA_EVENTS, sizeof(dma_frame_t));
    if (!routing_lock || !dma_events) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < BOARD_AUDIO_SOURCES; ++i) {
        inputs[i] = (pcm_ring_t){.samples = input_storage[i], .capacity = CAPTURE_SAMPLES};
        outputs[i] = (pcm_ring_t){.samples = output_storage[i], .capacity = PLAYBACK_SAMPLES};
    }
    /* The application's duplex audio owner uses the BSP's verified pins and
     * power/I2C setup. Do not initialize the BSP's separate ADC/DAC handles:
     * ES8311 is one physical codec and BOTH must be configured/opened once. */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "VCC power failed");
    ESP_RETURN_ON_ERROR(bsp_power_set_codec_3v3(true), TAG, "codec power failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = 6;
    channels.dma_frame_num = DMA_SAMPLES;
    channels.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channels, &tx_channel, &rx_channel), TAG, "I2S create failed");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = BSP_AUDIO_I2S_MCLK, .bclk = BSP_AUDIO_I2S_SCLK,
                     .ws = BSP_AUDIO_I2S_LRCLK, .dout = BSP_AUDIO_I2S_SDOUT, .din = BSP_AUDIO_I2S_DSIN},
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_channel, &config), TAG, "TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_channel, &config), TAG, "RX init failed");
    i2s_event_callbacks_t rx_callbacks = {.on_recv = dma_completed};
    i2s_event_callbacks_t tx_callbacks = {.on_sent = dma_completed};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(rx_channel, &rx_callbacks, &rx_clock), TAG, "RX callback failed");
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(tx_channel, &tx_callbacks, &tx_clock), TAG, "TX callback failed");
    audio_codec_i2s_cfg_t data_config = {.port = I2S_NUM_0, .rx_handle = rx_channel, .tx_handle = tx_channel};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_config);
    audio_codec_i2c_cfg_t control_config = {.addr = BSP_AUDIO_CODEC_I2C_ADDR, .bus_handle = bsp_i2c_get_handle()};
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&control_config);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    if (!data || !control || !gpio) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control, .gpio_if = gpio, .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_AUDIO_PA_CTRL, .use_mclk = true, .digital_mic = false,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f}, .no_dac_ref = true,
    };
    const audio_codec_if_t *device = es8311_codec_new(&codec_config);
    if (!device) return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t device_config = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = device, .data_if = data};
    codec = esp_codec_dev_new(&device_config);
    if (!codec) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t format = {.sample_rate = BOARD_AUDIO_RATE, .bits_per_sample = 16, .channel = 1};
    if (esp_codec_dev_open(codec, &format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(codec, 30.0f) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    atomic_store(&requested_volume, volume);
    TaskHandle_t input_worker = NULL, output_worker = NULL;
    if (xTaskCreateWithCaps(input_task, "board_mic", 24 * 1024, NULL, 17, &input_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        esp_codec_dev_close(codec);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(output_task, "board_spk", 4096, NULL, 18, &output_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vTaskDeleteWithCaps(input_worker);
        esp_codec_dev_close(codec);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&ready, true);
    ESP_LOGI(TAG, "ES8311 duplex: fixed 48 kHz/S16/mono left slot; one Mic/AEC worker, one DAC mixer; ref delay=%d ms",
             CONFIG_TRANSLATOR_AEC_REF_DELAY_MS);
    return ESP_OK;
}

bool board_audio_ready(void)
{
    return atomic_load(&ready);
}

void board_audio_set_volume(unsigned volume)
{
    atomic_store(&requested_volume, volume > 100 ? 100 : volume);
}

void board_audio_set_route(int source)
{
    if (!routing_lock) return;
    xSemaphoreTake(routing_lock, portMAX_DELAY);
    if (input_source != source) {
        input_source = source >= 0 && source < BOARD_AUDIO_SOURCES ? source : -1;
        ++route_generation;
        route_since = sample_tick();
        for (unsigned i = 0; i < BOARD_AUDIO_SOURCES; ++i) pcm_ring_reset(&inputs[i]);
    }
    xSemaphoreGive(routing_lock);
}

void board_audio_set_output_enabled(unsigned source, bool enabled)
{
    if (!routing_lock || source >= BOARD_AUDIO_SOURCES) return;
    xSemaphoreTake(routing_lock, portMAX_DELAY);
    if (output_enabled[source] != enabled) {
        output_enabled[source] = enabled;
        pcm_ring_reset(&outputs[source]);
        pcm_ring_reset(&inputs[source]);
        if (input_source == (int)source) route_since = sample_tick();
        ++route_generation;
    }
    xSemaphoreGive(routing_lock);
}

bool board_audio_write(unsigned source, const int16_t *samples, size_t count)
{
    if (!routing_lock || source >= BOARD_AUDIO_SOURCES || !samples) return false;
    while (count) {
        xSemaphoreTake(routing_lock, portMAX_DELAY);
        bool enabled = output_enabled[source];
        size_t space = outputs[source].capacity - outputs[source].count;
        size_t copied = space < count ? space : count;
        if (enabled && copied) pcm_ring_write(&outputs[source], samples, copied);
        xSemaphoreGive(routing_lock);
        if (!enabled) return true;
        samples += copied;
        count -= copied;
        if (count) vTaskDelay(1);
    }
    return true;
}

size_t board_audio_read(unsigned source, int16_t *samples, size_t count, uint32_t timeout_ms)
{
    if (!routing_lock || source >= BOARD_AUDIO_SOURCES || !samples) return 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t read = 0;
    xSemaphoreTake(routing_lock, portMAX_DELAY);
    uint32_t generation = route_generation;
    xSemaphoreGive(routing_lock);
    for (;;) {
        xSemaphoreTake(routing_lock, portMAX_DELAY);
        bool selected = input_source == (int)source &&
                        generation == route_generation;
        if (selected) read += pcm_ring_read(&inputs[source], samples + read, count - read);
        xSemaphoreGive(routing_lock);
        if (!selected) return 0;
        if (read == count || esp_timer_get_time() >= deadline) return read;
        vTaskDelay(1);
    }
}

unsigned board_audio_input_level(void)
{
    return atomic_load(&input_level);
}

bool board_audio_is_speaking(void)
{
    if (!routing_lock) return false;
    xSemaphoreTake(routing_lock, portMAX_DELAY);
    int64_t last = last_speech_us;
    xSemaphoreGive(routing_lock);
    return last && esp_timer_get_time() - last < 200000;
}
