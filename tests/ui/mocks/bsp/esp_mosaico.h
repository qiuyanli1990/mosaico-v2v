#pragma once
#include <stdbool.h>
#include "esp_err.h"
void *bsp_display_start(void);
bool bsp_display_lock(int timeout);
void bsp_display_unlock(void);
esp_err_t bsp_display_brightness_set(int percent);
