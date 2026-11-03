#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "tusb.h"
#include "crash_trace.h"
#include "usb_console.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

#define LOG_CACHE_SIZE 16384
#define LOG_CHUNK_SIZE 512

static vprintf_like_t previous_vprintf;
static portMUX_TYPE log_lock = portMUX_INITIALIZER_UNLOCKED;
/* Software log history; the USB endpoint buffers remain in internal RAM. */
static EXT_RAM_BSS_ATTR char log_cache[LOG_CACHE_SIZE];
static size_t log_head;
static size_t log_tail;
static size_t log_count;
static uint32_t boot_id;
static esp_reset_reason_t boot_reset_reason;
static atomic_bool banner_pending = true;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)dtr;
    (void)rts;
    if (itf == 0) {
        /* idf_monitor releases DTR after opening. Replay boot diagnostics on
         * either line state so this also works with monitor --no-reset. */
        atomic_store(&banner_pending, true);
    }
}

static void cache_log(const char *data, size_t length)
{
    if (length > LOG_CACHE_SIZE) {
        data += length - LOG_CACHE_SIZE;
        length = LOG_CACHE_SIZE;
    }

    portENTER_CRITICAL(&log_lock);
    while (log_count + length > LOG_CACHE_SIZE) {
        log_tail = (log_tail + 1) % LOG_CACHE_SIZE;
        --log_count;
    }
    for (size_t i = 0; i < length; ++i) {
        log_cache[log_head] = data[i];
        log_head = (log_head + 1) % LOG_CACHE_SIZE;
    }
    log_count += length;
    portEXIT_CRITICAL(&log_lock);
}

static int usb_log_vprintf(const char *format, va_list args)
{
    int result = 0;
    va_list uart_args;
    va_copy(uart_args, args);
    if (previous_vprintf) {
        result = previous_vprintf(format, uart_args);
    }
    va_end(uart_args);

    char line[LOG_CHUNK_SIZE];
    va_list log_args;
    va_copy(log_args, args);
    int length = vsnprintf(line, sizeof(line), format, log_args);
    va_end(log_args);
    if (length > 0) {
        size_t cached = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;
        cache_log(line, cached);
    }
    return result;
}

static void usb_log_task(void *arg)
{
    (void)arg;
    char chunk[LOG_CHUNK_SIZE];
    unsigned banner_part = 0;
    int64_t last_diag_us = 0;
    const esp_app_desc_t *app = esp_app_get_description();
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now - last_diag_us >= 5000000) {
            usb_audio_log_diagnostics();
            last_diag_us = now;
        }
        if (atomic_exchange(&banner_pending, false)) banner_part = 0;
        bool ready = tud_cdc_n_ready(0);
        uint32_t available = ready ? tud_cdc_n_write_available(0) : 0;
        if (!ready) {
            banner_part = 0;
        } else if (banner_part < 2) {
            /* Send separately from the ring so overflow cannot lose the
             * reset reason before a terminal opens the runtime CDC port. */
            int length = banner_part == 0 ? snprintf(chunk, sizeof(chunk),
                                  "\r\n--- USB_CONSOLE: boot=%08" PRIx32
                                  " reset=%s(%d) uptime=%" PRId64 "ms"
                                  " version=%s rev=%04x profile=" MOSAICO_USB_AUDIO_PROFILE
                                  " elf=%02x%02x%02x%02x ---\r\n",
                                  boot_id, reset_reason_name(boot_reset_reason),
                                  (int)boot_reset_reason, esp_timer_get_time() / 1000,
                                  app->version, MOSAICO_USB_DEVICE_REVISION,
                                  app->app_elf_sha256[0], app->app_elf_sha256[1],
                                  app->app_elf_sha256[2], app->app_elf_sha256[3]) :
                         (int)crash_trace_format_previous(chunk, sizeof(chunk));
            if (length > 0 && (size_t)length < sizeof(chunk) &&
                available >= (uint32_t)length) {
                if (tud_cdc_n_write(0, chunk, length) == (uint32_t)length) ++banner_part;
                tud_cdc_n_write_flush(0);
            }
        } else if (available) {
            size_t count = available < sizeof(chunk) ? available : sizeof(chunk);
            portENTER_CRITICAL(&log_lock);
            if (count > log_count) {
                count = log_count;
            }
            for (size_t i = 0; i < count; ++i) {
                chunk[i] = log_cache[log_tail];
                log_tail = (log_tail + 1) % LOG_CACHE_SIZE;
            }
            log_count -= count;
            portEXIT_CRITICAL(&log_lock);

            if (count) {
                uint32_t written = tud_cdc_n_write(0, chunk, count);
                if (written < count) {
                    cache_log(chunk + written, count - written);
                }
                tud_cdc_n_write_flush(0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void usb_console_init(void)
{
    boot_id = esp_random();
    boot_reset_reason = esp_reset_reason();
    crash_trace_init(boot_id);
    previous_vprintf = esp_log_set_vprintf(usb_log_vprintf);
    xTaskCreate(usb_log_task, "usb_log", 3072, NULL, 2, NULL);
    ESP_LOGI("USB_CONSOLE", "USB CDC log cache ready");
}
