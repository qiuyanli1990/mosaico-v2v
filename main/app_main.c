#include "app_state.h"
#include "crash_trace.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media.h"
#include "network.h"
#include "nvs_flash.h"
#include "session.h"
#include "ui.h"
#include "usb_audio.h"
#include "usb_console.h"

static void log_heap(const char *stage)
{
    ESP_LOGI("APP", "%s: internal free=%u largest=%u, DMA free=%u largest=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void app_main(void)
{
    usb_console_init();
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        ESP_LOGE("APP", "NVS init failed (%s); existing NVS was not erased", esp_err_to_name(result));
        return;
    }

    log_heap("Startup heap");
    bool degraded = false;
    result = ui_init();
    if (result != ESP_OK) {
        ESP_LOGE("APP", "Display initialization failed: %s", esp_err_to_name(result));
        degraded = true;
    }

    result = media_init();
    if (result != ESP_OK) {
        ESP_LOGE("APP", "Media/AEC initialization failed: %s", esp_err_to_name(result));
        degraded = true;
    }
    result = usb_audio_init();
    if (result != ESP_OK) {
        ESP_LOGE("APP", "USB audio initialization failed: %s", esp_err_to_name(result));
        degraded = true;
    }

    /* Hardware DMA resources are initialized before Wi-Fi's RX/TX buffers.
     * A module failure still allows Wi-Fi provisioning to start. */
    log_heap("Pre-network heap");
    ESP_LOGI("APP", "Starting Wi-Fi after display, media, and USB initialization");
    result = network_start();
    bool network_ready = result == ESP_OK;
    if (!network_ready) {
        ESP_LOGE("APP", "Wi-Fi initialization failed: %s", esp_err_to_name(result));
        degraded = true;
    } else if (!network_has_credentials()) {
        ESP_LOGI("APP", "No saved Wi-Fi credentials; open Wi-Fi from the Home screen");
    }

    result = session_init();
    if (result != ESP_OK) {
        ESP_LOGE("APP", "Session journal initialization failed: %s", esp_err_to_name(result));
        degraded = true;
    }
    log_heap("Startup complete heap");
    crash_trace_mark(CRASH_TRACE_BOOT, CRASH_STAGE_READY, degraded);

    if (!network_ready) {
        app_set_status(APP_ERROR, "Wi-Fi initialization failed; check UART log");
    } else if (degraded) {
        app_set_status(APP_ERROR, "Wi-Fi is ready; check UART for a module error");
    } else {
        app_set_status(APP_IDLE, "Choose a mode to begin translating");
    }
}
