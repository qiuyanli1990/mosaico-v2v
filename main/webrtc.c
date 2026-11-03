#include "webrtc.h"
#include <stdatomic.h>
#include <string.h>
#include "agora_auth.h"
#include "agora_signaling.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_webrtc.h"
#include "media.h"
#include "settings.h"
#include "subtitles.h"

typedef struct {
    media_source_t source;
    esp_webrtc_handle_t rtc;
    atomic_bool connected;
    atomic_bool failed;
    int64_t started_us;
    char session_url[AGORA_DEMO_URL_SIZE];
    char session_jwt[AGORA_DEMO_TOKEN_SIZE];
} rtc_session_t;

static const char *const source_names[MEDIA_SOURCE_COUNT] = {
    "speaker", "mic",
};
static rtc_session_t sessions[MEDIA_SOURCE_COUNT];

static rtc_session_t *session_for(media_source_t source)
{
    if (source < 0 || source >= MEDIA_SOURCE_COUNT) {
        return NULL;
    }
    rtc_session_t *session = &sessions[source];
    session->source = source;
    return session;
}

static int rtc_event(esp_webrtc_event_t *event, void *context)
{
    rtc_session_t *session = context;
    if (!session) {
        return 0;
    }
    switch (event->type) {
    case ESP_WEBRTC_EVENT_CONNECTED:
        atomic_store(&session->connected, true);
        ESP_LOGI("WEBRTC", "%s channel connected", source_names[session->source]);
        break;
    case ESP_WEBRTC_EVENT_CONNECT_FAILED:
    case ESP_WEBRTC_EVENT_DISCONNECTED:
        atomic_store(&session->connected, false);
        atomic_store(&session->failed, true);
        media_set_enabled(session->source, false);
        ESP_LOGE("WEBRTC", "%s channel disconnected", source_names[session->source]);
        break;
    default:
        break;
    }
    return 0;
}

static int rtc_data(esp_peer_data_frame_t *frame, void *context)
{
    rtc_session_t *session = context;
    if (frame && frame->data && frame->size > 0) {
        subtitles_handle_data_for_channel(session && session->source == MEDIA_SOURCE_ONBOARD_MIC,
                                          frame->data, (size_t)frame->size);
    }
    return 0;
}

static int rtc_channel_open(esp_peer_data_channel_info_t *channel, void *context)
{
    rtc_session_t *session = context;
    ESP_LOGI("WEBRTC", "%s data channel opened: %s",
             session ? source_names[session->source] : "unknown",
             channel && channel->label ? channel->label : "unknown");
    return 0;
}

static int rtc_channel_close(esp_peer_data_channel_info_t *channel, void *context)
{
    rtc_session_t *session = context;
    ESP_LOGI("WEBRTC", "%s data channel closed: %s",
             session ? source_names[session->source] : "unknown",
             channel && channel->label ? channel->label : "unknown");
    return 0;
}

esp_err_t translator_webrtc_start(media_source_t source, const char *channel,
                                  const char *device_uid)
{
    rtc_session_t *session = session_for(source);
    if (!session || !channel || !device_uid || session->rtc ||
        agora_auth_create_session(channel, device_uid,
                                  session->session_url, sizeof(session->session_url),
                                  session->session_jwt, sizeof(session->session_jwt)) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&session->connected, false);
    atomic_store(&session->failed, false);
    esp_peer_default_cfg_t peer_config = {
        .rtp_cfg.audio_recv_jitter = {.cache_timeout = 200, .resend_delay = 20},
    };
    agora_signaling_cfg_t signaling = {.bearer_token = session->session_jwt, .source = source};
    esp_webrtc_cfg_t config = {
        .peer_cfg = {
            .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel = 1},
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .video_dir = ESP_PEER_MEDIA_DIR_NONE,
            .enable_data_channel = true,
            .on_data = rtc_data,
            .on_channel_open = rtc_channel_open,
            .on_channel_close = rtc_channel_close,
            .no_auto_reconnect = true,
            .extra_cfg = &peer_config, .extra_size = sizeof(peer_config),
            .ctx = session,
        },
        .signaling_cfg = {
            .signal_url = session->session_url, .extra_cfg = &signaling, .extra_size = sizeof(signaling),
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = agora_signaling_get_impl(),
    };
    esp_webrtc_media_provider_t provider;
    esp_err_t result = media_session_open(source, &provider);
    if (result != ESP_OK) {
        return result;
    }
    if (esp_webrtc_open(&config, &session->rtc) != ESP_PEER_ERR_NONE ||
        esp_webrtc_set_media_provider(session->rtc, &provider) != ESP_PEER_ERR_NONE ||
        esp_webrtc_set_event_handler(session->rtc, rtc_event, session) != ESP_PEER_ERR_NONE) {
        translator_webrtc_stop(source);
        return ESP_FAIL;
    }
    session->started_us = esp_timer_get_time();
    if (esp_webrtc_start(session->rtc) != ESP_PEER_ERR_NONE) {
        translator_webrtc_stop(source);
        return ESP_FAIL;
    }
    ESP_LOGI("WEBRTC", "Started %s channel=%s uid=%s",
             source_names[source], channel, device_uid);
    return ESP_OK;
}

void translator_webrtc_stop(media_source_t source)
{
    rtc_session_t *session = session_for(source);
    if (!session) {
        return;
    }
    media_set_enabled(source, false);
    if (session->rtc) {
        esp_webrtc_close(session->rtc);
        session->rtc = NULL;
    }
    media_session_close(source);
    atomic_store(&session->connected, false);
    atomic_store(&session->failed, false);
    session->started_us = 0;
}

bool translator_webrtc_connected(media_source_t source)
{
    rtc_session_t *session = session_for(source);
    return session && atomic_load(&session->connected);
}

bool translator_webrtc_failed(media_source_t source)
{
    rtc_session_t *session = session_for(source);
    return session && (atomic_load(&session->failed) ||
                       (session->started_us && !atomic_load(&session->connected) &&
                        esp_timer_get_time() - session->started_us > 30000000));
}

void translator_webrtc_query(media_source_t source)
{
    rtc_session_t *session = session_for(source);
    if (session && session->rtc) {
        esp_webrtc_query(session->rtc);
    }
}
