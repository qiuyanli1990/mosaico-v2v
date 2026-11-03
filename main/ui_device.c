#include "ui_device.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE battery_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_battery_t latest;

static void battery_task(void *argument)
{
    (void)argument;
    bool initialized = false;
    /* Keep gauge initialization and its I2C transactions off the startup and
     * LVGL paths. Audio owns the high-priority work throughout polling. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        if (!initialized) initialized = bsp_battery_init() == ESP_OK;
        bsp_battery_status_t status = {0};
        ui_battery_t next = {0};
        if (initialized && bsp_battery_read(&status) == ESP_OK &&
            status.voltage_mv > 2000 && status.voltage_mv < 5000 &&
            status.state_of_charge <= 100) {
            next.available = true;
            next.percent = status.state_of_charge;
            next.charging = status.current_ma > 0;
        }
        portENTER_CRITICAL(&battery_lock);
        latest = next;
        portEXIT_CRITICAL(&battery_lock);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void ui_device_init(void)
{
    if (xTaskCreateWithCaps(battery_task, "ui_battery", 4096, NULL, 1, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW("UI", "Battery status is unavailable: worker allocation failed");
    }
}

void ui_device_get_battery(ui_battery_t *battery)
{
    portENTER_CRITICAL(&battery_lock);
    *battery = latest;
    portEXIT_CRITICAL(&battery_lock);
}
