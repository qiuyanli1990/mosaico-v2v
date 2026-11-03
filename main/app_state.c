#include "app_state.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t state = {.phase = APP_IDLE, .message = "Connect USB and Wi-Fi"};

void app_set_status(app_phase_t phase, const char *message)
{
    portENTER_CRITICAL(&state_lock);
    state.phase = phase;
    snprintf(state.message, sizeof(state.message), "%s", message);
    portEXIT_CRITICAL(&state_lock);
}

void app_set_channel(const char *channel)
{
    portENTER_CRITICAL(&state_lock);
    snprintf(state.channel, sizeof(state.channel), "%s", channel);
    portEXIT_CRITICAL(&state_lock);
}

void app_get_snapshot(app_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&state_lock);
    *snapshot = state;
    portEXIT_CRITICAL(&state_lock);
}

