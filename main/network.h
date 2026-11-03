#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NETWORK_MAX_APS 20

typedef enum {
    NETWORK_NO_CREDENTIALS,
    NETWORK_CONNECTING,
    NETWORK_CONNECTED,
    NETWORK_FAILED,
} network_connection_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} network_ap_t;

typedef struct {
    network_connection_t connection;
    char ssid[33];
    int disconnect_reason;
    bool scanning;
    uint32_t scan_generation;
    esp_err_t scan_error;
} network_status_t;

esp_err_t network_start(void);
esp_err_t network_scan_start(void);
size_t network_get_scan_results(network_ap_t *results, size_t capacity);
void network_get_status(network_status_t *status);
esp_err_t network_connect(const char *ssid, const char *password);
bool network_has_credentials(void);
bool network_is_connected(void);
bool network_time_ready(void);

