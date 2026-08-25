#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi manager for the desktop: scan, connect, disconnect, status.
 * One global station interface; operations are non-blocking from the caller's
 * perspective (connection proceeds in the background). */

#define ARPILE_WIFI_SSID_MAX 33
#define ARPILE_WIFI_AP_MAX   24

typedef enum {
    ARPILE_WIFI_DISCONNECTED = 0,
    ARPILE_WIFI_CONNECTING,
    ARPILE_WIFI_CONNECTED,
    ARPILE_WIFI_ERROR,
} arpile_wifi_state_t;

typedef struct {
    char ssid[ARPILE_WIFI_SSID_MAX];
    int8_t rssi;
    int8_t channel;       /* primary channel */
    int8_t authmode;      /* wifi_auth_mode_t as int */
} arpile_wifi_ap_t;

/* Initialize the WiFi stack (netif/event/wifi). Call once at startup. */
esp_err_t arpile_wifi_init(void);

/* Current connection state. */
arpile_wifi_state_t arpile_wifi_get_state(void);

/* SSID currently connected to ("" if none). */
const char *arpile_wifi_get_connected_ssid(void);

/* IPv4 address of the current connection (e.g. "192.168.1.99"), or "" if none. */
const char *arpile_wifi_get_ip(void);

/* Trigger an async scan. Results are ready shortly after; call
 * arpile_wifi_get_ap_count()/arpile_wifi_get_ap() to read them. */
esp_err_t arpile_wifi_start_scan(void);
bool arpile_wifi_scan_in_progress(void);
int  arpile_wifi_get_ap_count(void);
bool arpile_wifi_get_ap(int index, arpile_wifi_ap_t *out);

/* Call periodically (e.g. from the app poll) to un-wedge scans whose
 * SCAN_DONE was dropped by the remote link. */
void arpile_wifi_poll(void);

/* Raw string describing the current/last scan state (SCAN_DONE seen, and the
 * get_ap_num / get_ap_records result and error). For on-screen debugging. */
const char *arpile_wifi_get_scan_diag(void);

/* Connect to the given network. Returns immediately; state becomes CONNECTING
 * until (dis)association completes. */
esp_err_t arpile_wifi_connect(const char *ssid, const char *password);
esp_err_t arpile_wifi_disconnect(void);

/* Remembered network (persisted in NVS; auto-connects at boot). */
bool arpile_wifi_has_saved(void);
const char *arpile_wifi_get_saved_ssid(void);
esp_err_t arpile_wifi_forget(void);

bool arpile_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif