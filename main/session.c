#include "session.h"
#include <ctype.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "crash_trace.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "network.h"
#include "nvs.h"
#include "settings.h"
#include "translation_protocol.h"
#include "usb_audio.h"
#include "v2v_client.h"
#include "webrtc.h"

#define SESSION_COUNT 2
#define LEGACY_AGENT_INDEX SESSION_COUNT

static const char *TAG = "SESSION";
static atomic_bool requested;
/* Only the session task owns the journal, cloud calls and WebRTC lifetimes. */
static nvs_handle_t journal;
static char pending_agents[SESSION_COUNT + 1][128];
static const char *const agent_keys[SESSION_COUNT + 1] = {
    "speaker_agent", "mic_agent", "agent_id",
};
static atomic_uint source_language_index;
static atomic_uint target_language_index;
static char selected_voice_id[TRANSLATION_VOICE_ID_SIZE] = "fm_02_male-qn-qingse";
static atomic_uint selected_mode;
static atomic_uint travel_turn;
static bool active;
static bool running;
/* Keep lifecycle checks tied to the workers started for this session. */
static unsigned worker_count;
static session_mode_t active_mode;
static int64_t cleanup_retry_after_us;
static int64_t next_start_attempt_us;

typedef struct {
    media_source_t source;
    const char *name;
    const char *suffix;
    const char *device_uid;
} worker_t;

static const worker_t workers[SESSION_COUNT] = {
    {MEDIA_SOURCE_USB_SPEAKER, "speaker", "_speaker", TRANSLATOR_SPEAKER_DEVICE_UID},
    {MEDIA_SOURCE_ONBOARD_MIC, "mic", "_mic", TRANSLATOR_MIC_DEVICE_UID},
};

bool session_requested(void)
{
    return atomic_load(&requested);
}

void session_set_languages(size_t source_index, size_t target_index)
{
    if (translation_language_at(source_index) && translation_language_at(target_index)) {
        atomic_store(&source_language_index, (unsigned)source_index);
        atomic_store(&target_language_index, (unsigned)target_index);
    }
}

void session_set_voice(const char *voice_id)
{
    char formatted[TRANSLATION_VOICE_ID_SIZE];
    if (translation_format_voice_id(voice_id, formatted, sizeof(formatted)))
        memcpy(selected_voice_id, formatted, strlen(formatted) + 1);
}

void session_set_mode(session_mode_t mode)
{
    if (mode < 0 || mode >= SESSION_MODE_COUNT) return;
    session_set_travel_turn(SESSION_TRAVEL_NONE);
    atomic_store(&selected_mode, mode);
    media_set_portable_mode(mode == SESSION_MODE_PORTABLE);
}

session_mode_t session_get_mode(void)
{
    unsigned mode = atomic_load(&selected_mode);
    return mode < SESSION_MODE_COUNT ? (session_mode_t)mode : SESSION_MODE_LIVE_MEETING;
}

void session_set_travel_turn(session_travel_turn_t turn)
{
    if (turn < SESSION_TRAVEL_NONE || turn > SESSION_TRAVEL_TARGET_LANGUAGE) {
        turn = SESSION_TRAVEL_NONE;
    }
    atomic_store(&travel_turn, (unsigned)turn);
    media_set_portable_input(turn == SESSION_TRAVEL_SOURCE_LANGUAGE ?
                             MEDIA_SOURCE_USB_SPEAKER :
                             turn == SESSION_TRAVEL_TARGET_LANGUAGE ?
                             MEDIA_SOURCE_ONBOARD_MIC : MEDIA_SOURCE_COUNT);
}

session_travel_turn_t session_get_travel_turn(void)
{
    return (session_travel_turn_t)atomic_load(&travel_turn);
}

void session_request(bool start)
{
    atomic_store(&requested, start);
    if (!start) {
        session_set_travel_turn(SESSION_TRAVEL_NONE);
        media_set_enabled(MEDIA_SOURCE_USB_SPEAKER, false);
        media_set_enabled(MEDIA_SOURCE_ONBOARD_MIC, false);
    }
}

static void apply_media_routing(void)
{
    bool allow = active && running && session_requested();
    /* Keep both TTS downlink paths available while local capture is paused. */
    bool allow_speaker = allow;
    bool allow_mic = allow && worker_count > 1;
    media_set_enabled(MEDIA_SOURCE_USB_SPEAKER, allow_speaker);
    media_set_enabled(MEDIA_SOURCE_ONBOARD_MIC, allow_mic);
}

static esp_err_t cleanup_agent(unsigned index)
{
    char *agent = pending_agents[index];
    if (!agent[0]) {
        return ESP_OK;
    }
    if (!network_is_connected() || !network_time_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    cleanup_retry_after_us = esp_timer_get_time() + 15000000;
    esp_err_t result = v2v_leave(agent);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s Agent cleanup is pending (%s)",
                 index < SESSION_COUNT ? workers[index].name : "legacy",
                 esp_err_to_name(result));
        return result;
    }
    result = nvs_erase_key(journal, agent_keys[index]);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(journal);
    }
    if (result == ESP_OK) {
        agent[0] = '\0';
    }
    return result;
}

static esp_err_t cleanup_all_agents(void)
{
    esp_err_t overall = ESP_OK;
    for (unsigned i = 0; i < SESSION_COUNT + 1; ++i) {
        esp_err_t result = cleanup_agent(i);
        if (result != ESP_OK) {
            overall = result;
        }
    }
    return overall;
}

static esp_err_t stop_workers(void)
{
    media_set_enabled(MEDIA_SOURCE_USB_SPEAKER, false);
    media_set_enabled(MEDIA_SOURCE_ONBOARD_MIC, false);
    translator_webrtc_stop(MEDIA_SOURCE_USB_SPEAKER);
    translator_webrtc_stop(MEDIA_SOURCE_ONBOARD_MIC);
    active = false;
    running = false;
    worker_count = 0;
    return cleanup_all_agents();
}

static void fail_session(const char *message)
{
    session_request(false);
    app_set_status(APP_STOPPING, "Closing translation");
    stop_workers();
    app_set_status(APP_ERROR, message);
    ESP_LOGE(TAG, "%s", message);
}

static void channel_language_code(const translation_language_t *language, char code[3])
{
    if (!language || !language->code || strlen(language->code) < 2) {
        memcpy(code, "xx", 3);
        return;
    }
    if (strncmp(language->code, "zh", 2) == 0) {
        memcpy(code, "cn", 3);
        return;
    }
    code[0] = (char)tolower((unsigned char)language->code[0]);
    code[1] = (char)tolower((unsigned char)language->code[1]);
    code[2] = '\0';
}

static bool make_channels(char *first_channel, size_t first_capacity,
                          char *second_channel, size_t second_capacity,
                          const translation_language_t *source_language,
                          const translation_language_t *target_language)
{
    unsigned char random[16];
    char base[AGORA_DEMO_CHANNEL_SIZE];
    char suffixes[SESSION_COUNT][8];
    const char *first_suffix = workers[0].suffix;
    const char *second_suffix = workers[1].suffix;
    if (session_get_mode() == SESSION_MODE_PORTABLE) {
        char source_code[3];
        char target_code[3];
        channel_language_code(source_language, source_code);
        channel_language_code(target_language, target_code);
        snprintf(suffixes[0], sizeof(suffixes[0]), "_%s-%s", source_code, target_code);
        snprintf(suffixes[1], sizeof(suffixes[1]), "_%s-%s", target_code, source_code);
        first_suffix = suffixes[0];
        second_suffix = suffixes[1];
    }
    esp_fill_random(random, sizeof(random));
    if (!translation_channel_from_random(random, base, sizeof(base))) {
        return false;
    }
    int first_length = snprintf(first_channel, first_capacity, "%s%s", base, first_suffix);
    int second_length = snprintf(second_channel, second_capacity, "%s%s", base, second_suffix);
    return first_length > 0 && (size_t)first_length < first_capacity &&
           second_length > 0 && (size_t)second_length < second_capacity;
}

static esp_err_t start_worker(const worker_t *worker, const char *channel,
                              const translation_language_t *source_language,
                              const translation_language_t *target_language)
{
    char *agent = pending_agents[worker->source];
    const translation_voice_t *voice = NULL;
    size_t voice_count = translation_voice_count_for_language(target_language->code);
    for (size_t i = 0; i < voice_count; ++i) {
        const translation_voice_t *candidate =
            translation_voice_for_language(target_language->code, i);
        if (candidate && strcmp(selected_voice_id, candidate->id) == 0) {
            voice = candidate;
            break;
        }
    }
    if (!voice && voice_count) {
        voice = translation_voice_for_language(target_language->code, 0);
    }
    ESP_LOGI(TAG, "New %s channel=%s device_uid=%s agent_uid=%s", worker->name,
             channel, worker->device_uid, TRANSLATOR_AGENT_UID);
    crash_trace_mark(CRASH_TRACE_SESSION, CRASH_STAGE_JOIN, worker->source);
    esp_err_t result = v2v_join(channel, source_language->code, worker->device_uid,
                                target_language->code, voice ? voice->id : NULL,
                                agent, sizeof(pending_agents[0]));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s V2V /join failed: %s", worker->name, esp_err_to_name(result));
        return result;
    }
    crash_trace_mark(CRASH_TRACE_SESSION, CRASH_STAGE_JOURNAL, worker->source);
    result = nvs_set_str(journal, agent_keys[worker->source], agent);
    if (result == ESP_OK) {
        result = nvs_commit(journal);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s Agent journal write failed: %s", worker->name,
                 esp_err_to_name(result));
        return result;
    }
    crash_trace_mark(CRASH_TRACE_SESSION, CRASH_STAGE_WEBRTC, worker->source);
    result = translator_webrtc_start(worker->source, channel, worker->device_uid);
    crash_trace_mark(CRASH_TRACE_SESSION, CRASH_STAGE_READY, worker->source);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s WebRTC start failed: %s", worker->name, esp_err_to_name(result));
    }
    return result;
}

static void start_session(void)
{
    if (!network_is_connected()) {
        app_set_status(APP_CONNECTING, "Waiting for Wi-Fi connection");
        return;
    }
    if (!network_time_ready()) {
        app_set_status(APP_CONNECTING, "Syncing network time");
        return;
    }
    if (!CONFIG_TRANSLATOR_APP_ID[0]) {
        fail_session("Set Agora App ID in sdkconfig.local, then rebuild");
        return;
    }
    if (CONFIG_TRANSLATOR_APP_CERT[0]) {
        fail_session("App Certificate requires RTC Token support; leave it empty for this demo");
        return;
    }
    if (!v2v_configured()) {
        fail_session("Set Agora customer ID and secret, then rebuild");
        return;
    }
    if (cleanup_all_agents() != ESP_OK) {
        session_request(false);
        app_set_status(APP_IDLE, "Ready");
        return;
    }

    const translation_language_t *source_language =
        translation_language_at(atomic_load(&source_language_index));
    const translation_language_t *target_language =
        translation_language_at(atomic_load(&target_language_index));
    char speaker_channel[AGORA_DEMO_CHANNEL_SIZE];
    char mic_channel[AGORA_DEMO_CHANNEL_SIZE];
    if (!source_language || !target_language ||
        !make_channels(speaker_channel, sizeof(speaker_channel),
                       mic_channel, sizeof(mic_channel), source_language, target_language)) {
        fail_session("Could not create unique translation channels");
        return;
    }
    app_set_channel(speaker_channel);
    char message[128];
    active_mode = session_get_mode();
    worker_count = active_mode == SESSION_MODE_MEDIA ? 1 : SESSION_COUNT;
    snprintf(message, sizeof(message), "%s: %s to %s",
             active_mode == SESSION_MODE_MEDIA ? "Starting media translation" :
             active_mode == SESSION_MODE_PORTABLE ? "Starting travel translation" : "Starting live meeting",
             source_language->english_name, target_language->english_name);
    app_set_status(APP_STARTING, message);
    ESP_LOGI(TAG, "Starting mode=%u V2V rooms=%u", (unsigned)active_mode, worker_count);

    esp_err_t result = ESP_OK;
    for (unsigned i = 0; i < worker_count; ++i) {
        if (!session_requested() || !network_is_connected()) break;
        result = start_worker(&workers[i], i ? mic_channel : speaker_channel,
                              i ? target_language : source_language,
                              i ? source_language : target_language);
        if (result != ESP_OK) break;
    }
    if (result != ESP_OK) {
        fail_session("Could not start translation; check the log");
        return;
    }
    if (!session_requested() || !network_is_connected()) {
        session_request(false);
        result = stop_workers();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Stopped; Agent cleanup will retry when online");
        }
        app_set_status(APP_IDLE, "Stopped");
        return;
    }
    app_set_status(APP_CONNECTING, active_mode == SESSION_MODE_MEDIA ?
                   "Connecting media translation" : active_mode == SESSION_MODE_PORTABLE ?
                   "Connecting travel translation" : "Connecting two-way meeting translation");
    active = true;
}

static void session_task(void *argument)
{
    (void)argument;
    bool auto_started = false;
    int64_t next_query_us = 0;
    int64_t next_metrics_us = 0;
    unsigned query_failures[SESSION_COUNT] = {0};
    while (true) {
        usb_audio_stats_t usb;
        usb_audio_get_stats(&usb);
        int64_t now = esp_timer_get_time();
        if (CONFIG_TRANSLATOR_AUTO_START && !auto_started &&
            network_is_connected() && network_time_ready() && v2v_configured()) {
            auto_started = true;
            session_request(true);
        }
        if (active && !network_is_connected()) {
            fail_session("Connection lost. Reconnect, then resume translation");
        }
        if (active && !session_requested()) {
            app_set_status(APP_STOPPING, "Stopping translation");
            esp_err_t result = stop_workers();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Stopped; Agent cleanup will retry when online");
            }
            app_set_status(APP_IDLE, "Stopped");
        } else if (!active && !session_requested()) {
            app_snapshot_t state;
            app_get_snapshot(&state);
            if (state.phase == APP_CONNECTING || state.phase == APP_STARTING ||
                state.phase == APP_STOPPING) {
                app_set_status(APP_IDLE, "Stopped");
            }
        }
        if (!active && session_requested() && now >= next_start_attempt_us) {
            start_session();
            next_start_attempt_us = esp_timer_get_time() + 2500000;
            next_query_us = esp_timer_get_time() + 15000000;
            query_failures[0] = query_failures[1] = 0;
        }
        bool failed = false;
        bool all_connected = true;
        for (unsigned i = 0; i < worker_count; ++i) {
            failed |= translator_webrtc_failed(workers[i].source);
            all_connected &= translator_webrtc_connected(workers[i].source);
        }
        if (active && session_requested() && failed) {
            fail_session("Translation disconnected. Please resume to reconnect");
        }
        if (active && session_requested() && !running && all_connected) {
            running = true;
            apply_media_routing();
            app_set_status(APP_RUNNING, active_mode == SESSION_MODE_MEDIA ?
                           "Translating media from your computer or phone." :
                           active_mode == SESSION_MODE_PORTABLE ?
                           "Tap a language card and speak." :
                           "Two-way translation for your live meeting.");
        }
        if (active && session_requested() && running) {
            apply_media_routing();
        }
        if (active && session_requested() && now >= next_query_us) {
            bool query_failed = false;
            for (unsigned i = 0; i < worker_count; ++i) {
                char status[32];
                esp_err_t result = v2v_query(pending_agents[i], status, sizeof(status));
                if (result == ESP_ERR_NOT_FOUND || (result == ESP_OK &&
                    strcmp(status, "RUNNING") != 0 && strcmp(status, "STARTING") != 0)) {
                    fail_session("Translation stopped. Please resume to reconnect");
                    query_failed = true;
                    break;
                }
                if (result != ESP_OK && ++query_failures[i] >= 3) {
                    fail_session("Could not verify translation. Please retry");
                    query_failed = true;
                    break;
                }
                if (result == ESP_OK) {
                    query_failures[i] = 0;
                }
            }
            (void)query_failed;
            next_query_us = esp_timer_get_time() + 15000000;
        }
        if (active && session_requested() && running && now >= next_metrics_us) {
            usb_audio_get_stats(&usb);
            ESP_LOGI(TAG, "USB input bytes=%" PRIu64 " queued=%u dropped=%" PRIu64
                     " TTS frames=%" PRIu32, usb.bytes_received,
                     (unsigned)usb.buffered_samples, usb.dropped_samples,
                     media_rendered_frames());
            for (unsigned i = 0; i < worker_count; ++i) {
                translator_webrtc_query(workers[i].source);
            }
            next_metrics_us = esp_timer_get_time() + 10000000;
        }
        if (!active && !session_requested() && now >= cleanup_retry_after_us &&
            network_is_connected() && network_time_ready() && v2v_configured()) {
            cleanup_all_agents();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t session_init(void)
{
    esp_err_t result = nvs_open("translator", NVS_READWRITE, &journal);
    if (result != ESP_OK) {
        return result;
    }
    for (unsigned i = 0; i < SESSION_COUNT + 1; ++i) {
        size_t capacity = sizeof(pending_agents[i]);
        result = nvs_get_str(journal, agent_keys[i], pending_agents[i], &capacity);
        if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(journal);
            return result;
        }
        if (pending_agents[i][0] && !translation_valid_agent_id(pending_agents[i])) {
            nvs_close(journal);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (xTaskCreate(session_task, "translation", 12288, NULL, 4, NULL) != pdPASS) {
        nvs_close(journal);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
