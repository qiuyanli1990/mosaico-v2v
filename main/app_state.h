#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_IDLE, APP_STARTING, APP_CONNECTING, APP_RUNNING, APP_STOPPING, APP_ERROR
} app_phase_t;

typedef struct {
    app_phase_t phase;
    char message[128];
    char channel[64];
} app_snapshot_t;

void app_set_status(app_phase_t phase, const char *message);
void app_set_channel(const char *channel);
void app_get_snapshot(app_snapshot_t *snapshot);

