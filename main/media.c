#include "media.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "av_render.h"
#include "board_audio.h"
#include "audio_render.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_log.h"
#include "session.h"
#include "network.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "usb_audio.h"
#include "usb_capture.h"

static esp_capture_handle_t captures[MEDIA_SOURCE_COUNT];
static av_render_handle_t players[MEDIA_SOURCE_COUNT];
static audio_render_handle_t gated_renderers[MEDIA_SOURCE_COUNT];
static atomic_bool enabled[MEDIA_SOURCE_COUNT];
static atomic_uint volume = MEDIA_DEFAULT_SPEAKER_VOLUME;
static atomic_uint rendered_frames[MEDIA_SOURCE_COUNT];
static atomic_bool portable_mode;
typedef struct {
    media_source_t source;
    av_render_audio_frame_info_t info;
    bool opened;
} gated_render_t;

/* Opus and DTLS need larger stacks than the library's generic defaults. */
static void media_thread_schedule(const char *name, media_lib_thread_cfg_t *config)
{
    if (strcmp(name, "aenc_0") == 0) {
        config->stack_size = 40 * 1024;
        config->priority = 10;
        config->core_id = 1;
    } else if (strcmp(name, "AUD_SRC") == 0 || strcmp(name, "Adec") == 0) {
        config->stack_size = 40 * 1024;
        config->priority = 15;
    } else if (strcmp(name, "pc_task") == 0) {
        config->stack_size = 25 * 1024;
        /* TLS and signaling must yield CPU time to Mic/AEC (17) and DAC (18). */
        config->priority = 12;
        config->core_id = 1;
    }
}

static void capture_thread_schedule(const char *name, esp_capture_thread_schedule_cfg_t *config)
{
    media_lib_thread_cfg_t schedule = {
        .stack_size = config->stack_size, .priority = config->priority,
        .core_id = config->core_id,
    };
    media_thread_schedule(name, &schedule);
    config->stack_size = schedule.stack_size;
    config->priority = schedule.priority;
    config->core_id = schedule.core_id;
    config->stack_in_ext = true;
}

static audio_render_handle_t gate_init(void *config, int size)
{
    if (size != sizeof(gated_render_t)) {
        return NULL;
    }
    gated_render_t *gate = calloc(1, sizeof(*gate));
    if (gate) {
        *gate = *(gated_render_t *)config;
    }
    return gate;
}

static int gate_open(audio_render_handle_t handle, av_render_audio_frame_info_t *info)
{
    gated_render_t *gate = handle;
    if (!info || info->sample_rate != BOARD_AUDIO_RATE || info->bits_per_sample != 16 ||
        (info->channel != 1 && info->channel != 2)) return ESP_FAIL;
    gate->info = *info;
    gate->opened = true;
    return ESP_OK;
}

static int gate_write(audio_render_handle_t handle, av_render_audio_frame_t *frame)
{
    gated_render_t *gate = handle;
    if (!atomic_load(&enabled[gate->source]) || !session_requested() || !network_is_connected()) {
        return ESP_OK;
    }
    if (!gate->opened || !frame || frame->size < 0 ||
        (frame->size && !frame->data) || frame->size % (gate->info.channel * sizeof(int16_t))) {
        return ESP_FAIL;
    }
    /* Each renderer owns its conversion scratch. Both speaker directions feed
     * bounded mixer inputs; only board_spk touches the physical DAC. */
    int16_t mono[240];
    const int16_t *input = (const int16_t *)frame->data;
    size_t frames = frame->size / (gate->info.channel * sizeof(int16_t));
    for (size_t offset = 0; offset < frames;) {
        size_t count = frames - offset;
        if (count > 240) count = 240;
        for (size_t i = 0; i < count; ++i) {
            int32_t value = input[(offset + i) * gate->info.channel];
            if (gate->info.channel == 2) value = (value + input[(offset + i) * 2 + 1]) / 2;
            mono[i] = (int16_t)value;
        }
        if (gate->source == MEDIA_SOURCE_ONBOARD_MIC && !atomic_load(&portable_mode)) {
            usb_audio_write_microphone(mono, count);
        } else if (!board_audio_write(gate->source, mono, count)) {
            return ESP_FAIL;
        }
        offset += count;
    }
    if (frames) atomic_fetch_add(&rendered_frames[gate->source], 1);
    return ESP_OK;
}

static int gate_latency(audio_render_handle_t handle, uint32_t *latency)
{
    (void)handle;
    *latency = 30; /* physical TX DMA; per-source buffering is bounded to 200 ms */
    return ESP_OK;
}

static int gate_info(audio_render_handle_t handle, av_render_audio_frame_info_t *info)
{
    gated_render_t *gate = handle;
    if (!gate->opened || !info) return ESP_FAIL;
    *info = gate->info;
    return ESP_OK;
}

static int gate_speed(audio_render_handle_t handle, float speed)
{
    (void)handle;
    return speed == 1.0f ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static int gate_close(audio_render_handle_t handle)
{
    gated_render_t *gate = handle;
    gate->opened = false;
    return ESP_OK;
}

static void gate_free(audio_render_handle_t handle)
{
    free(handle);
}

esp_err_t media_init(void)
{
    esp_err_t result = media_lib_add_default_adapter();
    if (result != ESP_OK) {
        return result;
    }
    media_lib_thread_set_schedule_cb(media_thread_schedule);
    if (esp_capture_set_thread_scheduler(capture_thread_schedule) != ESP_CAPTURE_ERR_OK) {
        return ESP_FAIL;
    }
    if (esp_audio_enc_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    return board_audio_init(atomic_load(&volume));
}

esp_err_t media_session_open(media_source_t source, esp_webrtc_media_provider_t *provider)
{
    if (source < 0 || source >= MEDIA_SOURCE_COUNT || !provider ||
        !board_audio_ready() || captures[source] || players[source]) {
        return ESP_ERR_INVALID_STATE;
    }
    media_set_enabled(source, false);
    atomic_store(&rendered_frames[source], 0);
    esp_capture_cfg_t capture_config = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = usb_capture_source(source == MEDIA_SOURCE_USB_SPEAKER ?
                                        USB_CAPTURE_SOURCE_SPEAKER :
                                        USB_CAPTURE_SOURCE_MICROPHONE),
    };
    if (esp_capture_open(&capture_config, &captures[source]) != ESP_CAPTURE_ERR_OK) {
        captures[source] = NULL;
        return ESP_FAIL;
    }
    gated_render_t gate_context = {.source = source};
    audio_render_cfg_t gate_config = {
        .ops = {.init = gate_init, .open = gate_open, .write = gate_write,
                .get_latency = gate_latency, .get_frame_info = gate_info,
                .set_speed = gate_speed, .close = gate_close, .deinit = gate_free},
        .cfg = &gate_context, .cfg_size = sizeof(gate_context),
    };
    gated_renderers[source] = audio_render_alloc_handle(&gate_config);
    if (!gated_renderers[source]) {
        media_session_close(source);
        return ESP_ERR_NO_MEM;
    }
    av_render_cfg_t config = {
        .audio_render = gated_renderers[source],
        .audio_raw_fifo_size = 4 * 1024,
        .audio_render_fifo_size = 16 * 1024,
        .allow_drop_data = false,
    };
    players[source] = av_render_open(&config);
    uint32_t threshold = source == MEDIA_SOURCE_USB_SPEAKER ?
                         4800 * sizeof(int16_t) : 960 * sizeof(int16_t);
    if (!players[source] || av_render_set_audio_threshold(players[source], threshold) != 0) {
        media_session_close(source);
        return ESP_FAIL;
    }
    *provider = (esp_webrtc_media_provider_t) {
        .capture = captures[source], .player = players[source]
    };
    ESP_LOGI("MEDIA", "%s capture -> 48 kHz mono Opus; downlink=%s",
             atomic_load(&portable_mode) ? "AEC onboard microphone (selected direction)" :
             source == MEDIA_SOURCE_USB_SPEAKER ? "USB speaker" : "AEC onboard microphone",
             atomic_load(&portable_mode) || source == MEDIA_SOURCE_USB_SPEAKER ?
             "ES8311 mono mixer" : "USB microphone");
    return ESP_OK;
}

void media_session_close(media_source_t source)
{
    if (source < 0 || source >= MEDIA_SOURCE_COUNT) {
        return;
    }
    media_set_enabled(source, false);
    if (captures[source]) {
        esp_capture_close(captures[source]);
        captures[source] = NULL;
    }
    if (players[source]) {
        av_render_close(players[source]);
        players[source] = NULL;
    }
    if (gated_renderers[source]) {
        audio_render_free_handle(gated_renderers[source]);
        gated_renderers[source] = NULL;
    }
}

void media_set_enabled(media_source_t source, bool allow)
{
    if (source < 0 || source >= MEDIA_SOURCE_COUNT) {
        return;
    }
    atomic_store(&enabled[source], allow);
    /* Live meetings return the reverse translation to USB. Only the speaker-side
     * translation is local DAC output; travel mode mixes both directions. */
    board_audio_set_output_enabled(source, allow &&
                                   (atomic_load(&portable_mode) ||
                                    source == MEDIA_SOURCE_USB_SPEAKER));
    usb_capture_set_enabled(source, allow);
    bool usb_mode = !atomic_load(&portable_mode);
    usb_audio_set_forwarding(usb_mode && atomic_load(&enabled[MEDIA_SOURCE_USB_SPEAKER]),
                             usb_mode && atomic_load(&enabled[MEDIA_SOURCE_ONBOARD_MIC]));
}

void media_set_portable_mode(bool portable)
{
    atomic_store(&portable_mode, portable);
    usb_capture_set_portable_mode(portable);
    usb_capture_set_portable_input(-1);
    for (media_source_t source = MEDIA_SOURCE_USB_SPEAKER;
         source < MEDIA_SOURCE_COUNT; ++source) {
        board_audio_set_output_enabled(source, atomic_load(&enabled[source]) &&
                                       (portable || source == MEDIA_SOURCE_USB_SPEAKER));
    }
    usb_audio_set_forwarding(!portable && atomic_load(&enabled[MEDIA_SOURCE_USB_SPEAKER]),
                             !portable && atomic_load(&enabled[MEDIA_SOURCE_ONBOARD_MIC]));
}

void media_set_portable_input(media_source_t source)
{
    int selected = source >= 0 && source < MEDIA_SOURCE_COUNT ? (int)source : -1;
    usb_capture_set_portable_input(selected);
}

unsigned media_get_portable_input_level(void)
{
    return usb_capture_get_portable_level();
}

bool media_is_portable_mode(void)
{
    return atomic_load(&portable_mode);
}

void media_set_volume(unsigned value)
{
    atomic_store(&volume, value > 100 ? 100 : value);
    board_audio_set_volume(value);
}

unsigned media_get_volume(void)
{
    return atomic_load(&volume);
}

uint32_t media_rendered_frames(void)
{
    return atomic_load(&rendered_frames[MEDIA_SOURCE_USB_SPEAKER]) +
           atomic_load(&rendered_frames[MEDIA_SOURCE_ONBOARD_MIC]);
}

bool media_is_speaking(void)
{
    return session_requested() && board_audio_is_speaking();
}
