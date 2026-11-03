#include "v2v_client.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include "translation_protocol.h"
#include "cJSON.h"

#define RESPONSE_LIMIT 8192
static const char *TAG = "V2V";

typedef struct {
    char *body;
    size_t used;
    esp_err_t error;
} response_t;

bool v2v_configured(void)
{
    return CONFIG_TRANSLATOR_CUSTOMER_ID[0] && CONFIG_TRANSLATOR_CUSTOMER_SECRET[0];
}

static esp_err_t response_event(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if ((size_t)event->data_len > RESPONSE_LIMIT - response->used) {
            response->error = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
        memcpy(response->body + response->used, event->data, event->data_len);
        response->used += event->data_len;
        response->body[response->used] = '\0';
    }
    return ESP_OK;
}

static char *basic_authorization(void)
{
    const char *id = CONFIG_TRANSLATOR_CUSTOMER_ID;
    const char *secret = CONFIG_TRANSLATOR_CUSTOMER_SECRET;
    size_t raw_size = strlen(id) + strlen(secret) + 1;
    char *raw = malloc(raw_size + 1);
    size_t capacity = ((raw_size + 2) / 3) * 4 + sizeof("Basic ");
    char *header = malloc(capacity);
    if (!raw || !header) {
        free(raw);
        free(header);
        return NULL;
    }
    snprintf(raw, raw_size + 1, "%s:%s", id, secret);
    memcpy(header, "Basic ", 6);
    size_t length = 0;
    int result = mbedtls_base64_encode((unsigned char *)header + 6, capacity - 6,
                                     &length, (const unsigned char *)raw, raw_size);
    memset(raw, 0, raw_size);
    free(raw);
    if (result != 0) {
        free(header);
        return NULL;
    }
    header[6 + length] = '\0';
    return header;
}

static esp_err_t request(esp_http_client_method_t method, const char *path,
                         const char *body, const char *request_id,
                         response_t *response, int *status)
{
    if (!v2v_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[512];
    int length = snprintf(url, sizeof(url), "%s/api/speech-to-speech-translation/v2/projects/%s%s",
                          CONFIG_TRANSLATOR_API_BASE, AGORA_DEMO_APP_ID, path);
    if (length < 0 || length >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *authorization = basic_authorization();
    if (!authorization) {
        return ESP_ERR_NO_MEM;
    }
    response->body = calloc(1, RESPONSE_LIMIT + 1);
    if (!response->body) {
        memset(authorization, 0, strlen(authorization));
        free(authorization);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t config = {
        .url = url, .method = method, .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = response_event, .user_data = response,
        .disable_auto_redirect = true,
        .buffer_size = 1024, .buffer_size_tx = 1536,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = client ? esp_http_client_set_header(client, "Authorization", authorization) : ESP_ERR_NO_MEM;
    if (result == ESP_OK && body) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (result == ESP_OK) {
            result = esp_http_client_set_post_field(client, body, strlen(body));
        }
    }
    if (result == ESP_OK && request_id) {
        result = esp_http_client_set_header(client, "X-Request-Id", request_id);
    }
    if (result == ESP_OK) {
        result = esp_http_client_perform(client);
    }
    if (result == ESP_OK && response->error != ESP_OK) {
        result = response->error;
    }
    *status = client ? esp_http_client_get_status_code(client) : 0;
    ESP_LOGI(TAG, "%s HTTP %d (%s)", path, *status, esp_err_to_name(result));
    if (client) {
        esp_http_client_cleanup(client);
    }
    memset(authorization, 0, strlen(authorization));
    free(authorization);
    return result;
}

esp_err_t v2v_join(const char *channel, const char *source_language,
                   const char *device_uid, const char *target_language,
                   const char *voice_id, char *agent_id, size_t capacity)
{
    if (!channel || !source_language || !device_uid || !target_language ||
        !agent_id || !capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    bool tts_enabled = translation_language_tts_supported(target_language);
    if (tts_enabled && (!voice_id || !*voice_id)) return ESP_ERR_INVALID_ARG;
    agent_id[0] = '\0';
    translation_request_t configuration = {
        .channel = channel, .device_uid = device_uid,
        .agent_uid = TRANSLATOR_AGENT_UID, .voice_id = voice_id, .tts_speed = 1.3f,
        .source_language = source_language, .target_language = target_language,
        .tts_enabled = tts_enabled,
        .idle_timeout = TRANSLATOR_IDLE_TIMEOUT_SECONDS,
    };
    char *body = translation_join_json(&configuration);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    response_t response = {0};
    int status = 0;
    esp_err_t result = request(HTTP_METHOD_POST, "/join", body, channel, &response, &status);
    cJSON_free(body);
    if (result == ESP_OK) {
        if (status < 200 || status >= 300) {
            result = status == 401 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        } else if (!translation_parse_agent(response.body, agent_id, capacity)) {
            result = ESP_ERR_INVALID_RESPONSE;
        } else {
            char state[32];
            if (!translation_parse_status(response.body, state, sizeof(state)) ||
                (strcmp(state, "RUNNING") != 0 && strcmp(state, "STARTING") != 0)) {
                result = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    free(response.body);
    return result;
}

esp_err_t v2v_leave(const char *agent_id)
{
    if (!translation_valid_agent_id(agent_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[176];
    snprintf(path, sizeof(path), "/agents/%s/leave", agent_id);
    response_t response = {0};
    int status = 0;
    esp_err_t result = request(HTTP_METHOD_POST, path, NULL, NULL, &response, &status);
    if (result == ESP_OK) {
        if (status == 404) {
            result = ESP_OK;
        } else if (status >= 200 && status < 300) {
            char state[32];
            result = translation_parse_status(response.body, state, sizeof(state)) &&
                     strcmp(state, "STOPPED") == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        } else {
            result = ESP_FAIL;
        }
    }
    free(response.body);
    return result;
}

esp_err_t v2v_query(const char *agent_id, char *status_text, size_t capacity)
{
    if (!translation_valid_agent_id(agent_id) || !status_text || !capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    snprintf(path, sizeof(path), "/agents/%s", agent_id);
    response_t response = {0};
    int status = 0;
    esp_err_t result = request(HTTP_METHOD_GET, path, NULL, NULL, &response, &status);
    if (result == ESP_OK) {
        if (status == 404) {
            result = ESP_ERR_NOT_FOUND;
        } else if (status != 200 || !translation_parse_status(response.body, status_text, capacity)) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
    }
    free(response.body);
    return result;
}
