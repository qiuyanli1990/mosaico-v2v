#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool available;
    bool charging;
    uint8_t percent;
} ui_battery_t;

void ui_device_init(void);
void ui_device_get_battery(ui_battery_t *battery);
