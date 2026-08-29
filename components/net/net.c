#include "net.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "pcf85063.h"
#include "storage.h"

static const char *TAG = "net";

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1
#define MAX_RETRIES    5

static EventGroupHandle_t s_events;
static net_state_t        s_state = NET_OFF;
static int                s_retries;
static char               s_ip[16] = "0.0.0.0";
static bool               s_time_synced;
static bool               s_initialised;
static esp_netif_t       *s_netif;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            s_state = NET_FAILED;
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));
        s_retries = 0;
        s_state = NET_CONNECTED;
        xEventGroupClearBits(s_events, BIT_FAILED);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
    }
}

esp_err_t net_start(void)
{
    const bd_config_t *cfg = storage_config();
    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no wifi_ssid in config.ini, staying offline");
        s_state = NET_FAILED;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == NET_CONNECTING || s_state == NET_CONNECTED) {
        return ESP_OK;
    }
    if (s_initialised) {
        /* We gave up after MAX_RETRIES; the user asking again resets the
         * counter and kicks off a fresh attempt without re-init'ing Wi-Fi. */
        s_retries = 0;
        s_state = NET_CONNECTING;
        xEventGroupClearBits(s_events, BIT_FAILED);
        return esp_wifi_connect();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "events");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop");
    }
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL), TAG, "ip handler");

    wifi_config_t wifi_cfg = { 0 };
    /* The esp_wifi fields are exactly 32/64 bytes and are not NUL-terminated
     * when full, so copy by length rather than with snprintf. */
    strncpy((char *)wifi_cfg.sta.ssid, cfg->wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, cfg->wifi_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "config");
    /* Modem sleep keeps the radio usable while cutting idle draw to ~20 mA. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "ps");

    s_retries = 0;
    s_state = NET_CONNECTING;
    s_initialised = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "connecting to \"%s\"", cfg->wifi_ssid);
    return ESP_OK;
}

void net_stop(void)
{
    if (!s_initialised || s_state == NET_OFF) {
        return;
    }
    esp_wifi_stop();
    s_state = NET_OFF;
    snprintf(s_ip, sizeof(s_ip), "0.0.0.0");
}

net_state_t net_state(void) { return s_state; }
bool net_is_connected(void) { return s_state == NET_CONNECTED; }
const char *net_ip_str(void) { return s_ip; }

int8_t net_rssi(void)
{
    wifi_ap_record_t ap;
    if (!net_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t net_wait_connected(uint32_t timeout_ms)
{
    if (s_state == NET_CONNECTED) {
        return ESP_OK;
    }
    if (!s_events) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) {
        return ESP_OK;
    }
    return (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

bool net_time_is_synced(void) { return s_time_synced; }

esp_err_t net_time_sync(uint32_t timeout_ms)
{
    const bd_config_t *cfg = storage_config();

    if (cfg->timezone[0]) {
        setenv("TZ", cfg->timezone, 1);
        tzset();
    }
    ESP_RETURN_ON_ERROR(net_wait_connected(timeout_ms), TAG, "offline");

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(
        cfg->ntp_server[0] ? cfg->ntp_server : "pool.ntp.org");
    sntp_cfg.start = true;

    static bool initialised;
    if (initialised) {
        esp_netif_sntp_deinit();
    }
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_cfg), TAG, "sntp init");
    initialised = true;

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP timed out, keeping RTC time");
        return ESP_ERR_TIMEOUT;
    }

    s_time_synced = true;
    /* Push the fresh time down to the RTC: that is what survives the battery
     * being pulled, and what timestamps notes when there is no network. */
    esp_err_t err = pcf85063_from_system_time();

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "time synced: %s (RTC update %s)", buf, esp_err_to_name(err));
    return ESP_OK;
}
