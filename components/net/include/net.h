#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Wi-Fi station + SNTP. Credentials come from /sdcard/config.ini. */

typedef enum {
    NET_OFF,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_FAILED,        /* no credentials, or the AP keeps refusing us */
} net_state_t;

/* Brings up the netif/Wi-Fi stack and starts connecting in the background.
 * Returns ESP_ERR_INVALID_STATE when config.ini has no SSID. */
esp_err_t net_start(void);
void      net_stop(void);

net_state_t net_state(void);
bool        net_is_connected(void);
int8_t      net_rssi(void);
const char *net_ip_str(void);

esp_err_t net_wait_connected(uint32_t timeout_ms);

/* Applies the configured TZ, then kicks off SNTP and pushes the result into
 * the PCF85063 so the time survives the next power cut. */
esp_err_t net_time_sync(uint32_t timeout_ms);
bool      net_time_is_synced(void);
