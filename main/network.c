#include "network.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "NETWORK";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool connected;
static bool started;
static bool has_credentials;
static bool switching;
static bool save_on_connect;
static bool retry_after_scan;
static unsigned retries;
static char desired_ssid[33];
static char desired_password[65];
static network_status_t status = {.connection = NETWORK_NO_CREDENTIALS};
static network_ap_t aps[NETWORK_MAX_APS];
static size_t ap_count;

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("mosaico_wifi", NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, "ssid", ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "password", password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static bool load_credentials(char *ssid, char *password)
{
    nvs_handle_t handle;
    if (nvs_open("mosaico_wifi", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t ssid_size = 33;
    size_t password_size = 65;
    bool valid = nvs_get_str(handle, "ssid", ssid, &ssid_size) == ESP_OK &&
                 nvs_get_str(handle, "password", password, &password_size) == ESP_OK &&
                 ssid[0] != '\0' && strlen(password) <= 63;
    nvs_close(handle);
    return valid;
}

static esp_err_t apply_desired_config(void)
{
    wifi_config_t config = {0};
    portENTER_CRITICAL(&lock);
    memcpy(config.sta.ssid, desired_ssid, strlen(desired_ssid));
    memcpy(config.sta.password, desired_password, strlen(desired_password));
    portEXIT_CRITICAL(&lock);
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK) {
        result = esp_wifi_connect();
    }
    portENTER_CRITICAL(&lock);
    switching = false;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "apply Wi-Fi config/connect failed: %s", esp_err_to_name(result));
        status.connection = NETWORK_FAILED;
    }
    portEXIT_CRITICAL(&lock);
    return result;
}

static void scan_done(void)
{
    /* Only the serialized default event loop uses this staging buffer. Keep
     * the AP records off its small stack; the driver copies them to CPU RAM. */
    static EXT_RAM_BSS_ATTR wifi_ap_record_t records[NETWORK_MAX_APS];
    uint16_t count = NETWORK_MAX_APS;
    esp_err_t result = esp_wifi_scan_get_ap_records(&count, records);
    ESP_LOGI(TAG, "Wi-Fi scan done: result=%s records=%u",
             esp_err_to_name(result), (unsigned)count);
    bool reconnect;
    portENTER_CRITICAL(&lock);
    ap_count = 0;
    if (result == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            if (!records[i].ssid[0]) {
                continue;
            }
            network_ap_t *ap = &aps[ap_count++];
            memcpy(ap->ssid, records[i].ssid, sizeof(ap->ssid));
            ap->ssid[32] = '\0';
            ap->rssi = records[i].rssi;
            ap->secure = records[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    status.scanning = false;
    status.scan_error = result;
    status.scan_generation++;
    reconnect = retry_after_scan;
    retry_after_scan = false;
    if (reconnect) {
        status.connection = NETWORK_CONNECTING;
        retries++;
    }
    portEXIT_CRITICAL(&lock);
    if (reconnect) {
        esp_wifi_connect();
    }
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START received; credentials=%d", has_credentials);
        if (network_has_credentials()) {
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Initial Wi-Fi connect failed: %s", esp_err_to_name(result));
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = event;
        atomic_store(&connected, false);
        bool do_switch;
        bool retry;
        portENTER_CRITICAL(&lock);
        do_switch = switching;
        retry = !do_switch && has_credentials && !status.scanning && retries < 3;
        if (!do_switch && has_credentials && status.scanning && retries < 3) {
            retry_after_scan = true;
        }
        if (retry) {
            retries++;
        }
        status.connection = do_switch || retry ? NETWORK_CONNECTING :
                            has_credentials ? NETWORK_FAILED : NETWORK_NO_CREDENTIALS;
        status.disconnect_reason = disconnected->reason;
        portEXIT_CRITICAL(&lock);
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%d retry=%d switching=%d",
                 disconnected->reason, retry, do_switch);
        if (do_switch) {
            esp_err_t result = apply_desired_config();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi configuration failed: %s", esp_err_to_name(result));
            }
        } else if (retry) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        char ssid[33];
        char password[65];
        bool should_save;
        portENTER_CRITICAL(&lock);
        if (switching) {
            portEXIT_CRITICAL(&lock);
            return;
        }
        status.connection = NETWORK_CONNECTED;
        status.disconnect_reason = 0;
        retries = 0;
        should_save = save_on_connect;
        save_on_connect = false;
        memcpy(ssid, desired_ssid, sizeof(ssid));
        memcpy(password, desired_password, sizeof(password));
        portEXIT_CRITICAL(&lock);
        atomic_store(&connected, true);
        if (should_save) {
            esp_err_t result = save_credentials(ssid, password);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Could not save Wi-Fi credentials: %s", esp_err_to_name(result));
            }
        }
        memset(password, 0, sizeof(password));
        ESP_LOGI(TAG, "Wi-Fi has an IP address");
    }
}

esp_err_t network_start(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    bool available = load_credentials(ssid, password);
    if (!available && CONFIG_TRANSLATOR_WIFI_SSID[0] &&
        strlen(CONFIG_TRANSLATOR_WIFI_SSID) <= 32 &&
        strlen(CONFIG_TRANSLATOR_WIFI_PASSWORD) <= 63) {
        strcpy(ssid, CONFIG_TRANSLATOR_WIFI_SSID);
        strcpy(password, CONFIG_TRANSLATOR_WIFI_PASSWORD);
        available = true;
    }
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "esp_netif_init completed");
    result = esp_event_loop_create_default();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "default event loop created");
    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "default Wi-Fi STA netif created");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "esp_wifi_init completed");
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL);
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi event handler registration failed: %s", esp_err_to_name(result));
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(result));
        }
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(result));
        }
    }
    if (result == ESP_OK && available) {
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, ssid, strlen(ssid));
        memcpy(config.sta.password, password, strlen(password));
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(result));
        }
    }
    if (result != ESP_OK) {
        return result;
    }
    portENTER_CRITICAL(&lock);
    strcpy(desired_ssid, ssid);
    strcpy(desired_password, password);
    has_credentials = available;
    strcpy(status.ssid, ssid);
    status.connection = available ? NETWORK_CONNECTING : NETWORK_NO_CREDENTIALS;
    portEXIT_CRITICAL(&lock);
    memset(password, 0, sizeof(password));
    result = esp_wifi_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(result));
        return result;
    }
    portENTER_CRITICAL(&lock);
    started = true;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Wi-Fi start completed; scanning is available");

    result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s; Wi-Fi remains available",
                 esp_err_to_name(result));
    }
    {
        esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "time.google.com"));
        esp_err_t sntp_result = esp_netif_sntp_init(&time_config);
        if (sntp_result != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s; Wi-Fi remains available",
                     esp_err_to_name(sntp_result));
        }
    }
    return ESP_OK;
}

esp_err_t network_scan_start(void)
{
    bool wifi_started;
    bool already_scanning;
    portENTER_CRITICAL(&lock);
    wifi_started = started;
    already_scanning = status.scanning;
    if (!started || status.scanning) {
        portEXIT_CRITICAL(&lock);
        ESP_LOGW(TAG, "Wi-Fi scan rejected: started=%d scanning=%d",
                 wifi_started, already_scanning);
        return ESP_ERR_INVALID_STATE;
    }
    status.scanning = true;
    status.scan_error = ESP_OK;
    portEXIT_CRITICAL(&lock);
    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t result = esp_wifi_scan_start(&config, false);
    ESP_LOGI(TAG, "Wi-Fi scan start returned: %s", esp_err_to_name(result));
    if (result != ESP_OK) {
        bool reconnect;
        portENTER_CRITICAL(&lock);
        status.scanning = false;
        status.scan_error = result;
        status.scan_generation++;
        reconnect = retry_after_scan;
        retry_after_scan = false;
        if (reconnect) {
            status.connection = NETWORK_CONNECTING;
            retries++;
        }
        portEXIT_CRITICAL(&lock);
        if (reconnect) {
            esp_wifi_connect();
        }
    }
    return result;
}

size_t network_get_scan_results(network_ap_t *results, size_t capacity)
{
    portENTER_CRITICAL(&lock);
    size_t count = ap_count < capacity ? ap_count : capacity;
    if (count && results) {
        memcpy(results, aps, count * sizeof(*results));
    }
    portEXIT_CRITICAL(&lock);
    return count;
}

void network_get_status(network_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    portEXIT_CRITICAL(&lock);
}

esp_err_t network_connect(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&lock);
    if (!started || switching || status.scanning) {
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_INVALID_STATE;
    }
    strcpy(desired_ssid, ssid);
    strcpy(desired_password, password);
    strcpy(status.ssid, ssid);
    status.connection = NETWORK_CONNECTING;
    status.disconnect_reason = 0;
    status.scan_error = ESP_OK;
    has_credentials = true;
    save_on_connect = true;
    switching = true;
    retry_after_scan = false;
    retries = 0;
    portEXIT_CRITICAL(&lock);
    bool was_connected = atomic_load(&connected);
    atomic_store(&connected, false);
    esp_err_t result = was_connected ? esp_wifi_disconnect() : apply_desired_config();
    if (result != ESP_OK) {
        portENTER_CRITICAL(&lock);
        switching = false;
        status.connection = NETWORK_FAILED;
        portEXIT_CRITICAL(&lock);
    }
    return result;
}

bool network_has_credentials(void)
{
    portENTER_CRITICAL(&lock);
    bool available = has_credentials;
    portEXIT_CRITICAL(&lock);
    return available;
}

bool network_is_connected(void)
{
    return atomic_load(&connected);
}

bool network_time_ready(void)
{
    return time(NULL) >= AGORA_DEMO_MIN_VALID_UNIX_TIME;
}
