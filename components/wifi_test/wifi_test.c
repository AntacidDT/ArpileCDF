#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_test.h"

static const char *TAG = "wifi_mgr";
static const char *NVS_NS = "arpile";
static const char *NVS_KEY_SSID = "wf_ssid";
static const char *NVS_KEY_PASS = "wf_pass";

#define WIFI_PASS_MAX 64

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SCAN_DONE_BIT BIT1

static EventGroupHandle_t s_evg = NULL;
static bool s_initialized = false;
static volatile arpile_wifi_state_t s_state = ARPILE_WIFI_DISCONNECTED;
static volatile bool s_assoc = false;
static char s_connected_ssid[ARPILE_WIFI_SSID_MAX] = "";
static char s_target_ssid[ARPILE_WIFI_SSID_MAX] = "";
static char s_pass[WIFI_PASS_MAX] = "";
static char s_saved_ssid[ARPILE_WIFI_SSID_MAX] = "";
static char s_ip[16] = "";

static bool s_scan_in_progress = false;
static volatile bool s_scan_done_seen = false;
static uint32_t s_scan_start_ms = 0;
static char s_scan_diag[64] = "";
static wifi_ap_record_t s_ap_records[ARPILE_WIFI_AP_MAX];
static int s_ap_count = 0;

static wifi_ap_record_t s_merged[ARPILE_WIFI_AP_MAX];
static int s_merged_count = 0;

static bool ap_known(const wifi_ap_record_t *ap)
{
    for (int i = 0; i < s_merged_count; i++) {
        if (memcmp(s_merged[i].bssid, ap->bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void merge_records(const wifi_ap_record_t *recs, int n)
{
    for (int i = 0; i < n && s_merged_count < ARPILE_WIFI_AP_MAX; i++) {
        if (!ap_known(&recs[i])) {
            s_merged[s_merged_count++] = recs[i];
        }
    }
}

static void set_state(arpile_wifi_state_t st)
{
    s_state = st;
    if (st != ARPILE_WIFI_CONNECTED) {
        s_connected_ssid[0] = 0;
    }
}

static void store_saved(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
}

static bool load_saved(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_len, pl = pass_len;
    esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, ssid, &sl);
    esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, pass, &pl);
    nvs_close(h);
    if (e1 != ESP_OK || ssid[0] == 0) {
        return false;
    }
    if (e2 != ESP_OK) {
        pass[0] = 0;
    }
    return true;
}

static void harvest_results(const char *via)
{
    uint16_t n = ARPILE_WIFI_AP_MAX;
    esp_err_t er = esp_wifi_scan_get_ap_records(&n, s_ap_records);
    if (er == ESP_OK) {
        merge_records(s_ap_records, (int)n);
        s_ap_count = s_merged_count;
        snprintf(s_scan_diag, sizeof(s_scan_diag), "done via %s, total=%d",
                 via, s_ap_count);
        ESP_LOGI(TAG, "scan done (%s): +%d this sweep, %d APs total",
                 via, (int)n, s_ap_count);
    } else {
        snprintf(s_scan_diag, sizeof(s_scan_diag), "%s: get_records=%s",
                 via, esp_err_to_name(er));
        ESP_LOGW(TAG, "scan harvest via %s failed: %s", via, esp_err_to_name(er));
    }
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_target_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_assoc = true;
        wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
        if (e->ssid_len && e->ssid_len < ARPILE_WIFI_SSID_MAX) {
            memcpy(s_connected_ssid, e->ssid, e->ssid_len);
            s_connected_ssid[e->ssid_len] = 0;
        }
        set_state(ARPILE_WIFI_CONNECTING);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_assoc = false;
        s_ip[0] = 0;
        ESP_LOGW(TAG, "disconnected (%d)", ((wifi_event_sta_disconnected_t *)data)->reason);
        if (s_state == ARPILE_WIFI_CONNECTED) {
            set_state(ARPILE_WIFI_ERROR);
        } else {
            set_state(ARPILE_WIFI_DISCONNECTED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        set_state(ARPILE_WIFI_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_scan_done_seen = true;
        s_scan_in_progress = false;
        harvest_results("event");
        if (s_evg) {
            xEventGroupSetBits(s_evg, WIFI_SCAN_DONE_BIT);
        }
    }
}

esp_err_t arpile_wifi_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_evg = xEventGroupCreate();
    if (!s_evg) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    char ssid[ARPILE_WIFI_SSID_MAX] = "";
    char pass[WIFI_PASS_MAX] = "";
    if (load_saved(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t wcfg = { 0 };
        strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
        strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WEP;
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", pass);
        snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
        ESP_LOGI(TAG, "saved network '%s' found; auto-connecting", ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    return ESP_OK;
}

arpile_wifi_state_t arpile_wifi_get_state(void)
{
    return s_state;
}

const char *arpile_wifi_get_connected_ssid(void)
{
    return s_connected_ssid;
}

const char *arpile_wifi_get_ip(void)
{
    return s_ip;
}

esp_err_t arpile_wifi_start_scan(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_in_progress) {
        return ESP_OK;
    }
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_ap_count = s_merged_count;
    s_scan_in_progress = true;
    s_scan_done_seen = false;
    s_scan_start_ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
    esp_err_t ret = esp_wifi_scan_start(&sc, false);
    if (ret != ESP_OK) {
        s_scan_in_progress = false;
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "scan started");
    return ESP_OK;
}

#define WIFI_SCAN_TIMEOUT_MS 6000

void arpile_wifi_poll(void)
{
    if (!s_scan_in_progress) {
        return;
    }
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
    bool timed_out = (now_ms - s_scan_start_ms) >= WIFI_SCAN_TIMEOUT_MS;
    if (!s_scan_done_seen && !timed_out) {
        return;
    }

    s_scan_in_progress = false;
    harvest_results(s_scan_done_seen ? "event+poll" : "timeout");
}

const char *arpile_wifi_get_scan_diag(void)
{
    return s_scan_diag;
}

bool arpile_wifi_scan_in_progress(void)
{
    return s_scan_in_progress;
}

int arpile_wifi_get_ap_count(void)
{
    return s_ap_count;
}

bool arpile_wifi_get_ap(int index, arpile_wifi_ap_t *out)
{
    if (index < 0 || index >= s_ap_count || !out) {
        return false;
    }
    const wifi_ap_record_t *r = &s_merged[index];
    memcpy(out->ssid, r->ssid, ARPILE_WIFI_SSID_MAX);
    out->ssid[ARPILE_WIFI_SSID_MAX - 1] = 0;
    out->rssi = r->rssi;
    out->channel = (int8_t)r->primary;
    out->authmode = (int8_t)r->authmode;
    return true;
}

esp_err_t arpile_wifi_connect(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(ssid) >= ARPILE_WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_WEP;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_assoc) {
        esp_wifi_disconnect();
    }
    strncpy(s_target_ssid, ssid, ARPILE_WIFI_SSID_MAX - 1);
    s_target_ssid[ARPILE_WIFI_SSID_MAX - 1] = 0;
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    set_state(ARPILE_WIFI_CONNECTING);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(ret));
        set_state(ARPILE_WIFI_DISCONNECTED);
        return ret;
    }
    return ESP_OK;
}

esp_err_t arpile_wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_wifi_disconnect();
    s_target_ssid[0] = 0;
    s_pass[0] = 0;
    set_state(ARPILE_WIFI_DISCONNECTED);
    return ret;
}

bool arpile_wifi_has_saved(void)
{
    return s_saved_ssid[0] != 0;
}

const char *arpile_wifi_get_saved_ssid(void)
{
    return s_saved_ssid;
}

esp_err_t arpile_wifi_forget(void)
{
    nvs_handle_t h;
    esp_err_t ret = ESP_OK;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        ret = nvs_commit(h);
        nvs_close(h);
    } else {
        return ESP_FAIL;
    }
    s_saved_ssid[0] = 0;
    if (s_state == ARPILE_WIFI_CONNECTED || s_state == ARPILE_WIFI_CONNECTING) {
        arpile_wifi_disconnect();
    }
    return ret;
}

esp_err_t arpile_wifi_test(void)
{
    return arpile_wifi_init();
}

bool arpile_wifi_is_connected(void)
{
    return s_state == ARPILE_WIFI_CONNECTED && s_ip[0] != 0;
}
