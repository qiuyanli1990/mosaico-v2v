#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define CONFIG_TRANSLATOR_APP_ID "host-test-app"
#define CONFIG_TRANSLATOR_APP_CERT ""
#define CONFIG_TRANSLATOR_WHIP_BASE "https://unused.invalid"
static inline void test_session_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGI(...) test_session_log(__VA_ARGS__)
#define ESP_LOGW(...) test_session_log(__VA_ARGS__)
#define ESP_LOGE(...) test_session_log(__VA_ARGS__)
const char *esp_err_to_name(esp_err_t error);

typedef struct { int unused; } esp_webrtc_media_provider_t;
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
int64_t esp_timer_get_time(void);
void esp_fill_random(void *, size_t);

typedef void *TaskHandle_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(unsigned ticks);
