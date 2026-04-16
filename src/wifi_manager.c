#include "wifi_manager.h"
#include "app_config.h"
#include "sensor_manager.h"   /* sensor_set_ap_mode, enqueue_log */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"

static const char *TAG = "WIFI";

/* ── Event group bits ────────────────────────────────────── */
#define BIT_STA_CONNECTED  BIT0
#define BIT_STA_FAILED     BIT1

static EventGroupHandle_t s_eg;

/* ── Module state ────────────────────────────────────────── */
static volatile wifi_mgr_state_t  s_state          = WIFI_MGR_IDLE;
static char                       s_ip[20]         = "";
static volatile bool              s_ap_active      = false;
static volatile bool              s_scanning       = false;
static int64_t                    s_conn_start_us  = 0;
static bool                       s_sta_netif_done = false; /* created once only */
static wifi_mgr_ap_shutdown_cb_t  s_shutdown_cb    = NULL;
static esp_err_t                  s_last_scan_err  = ESP_OK;

/* ── NVS helpers ─────────────────────────────────────────── */

static esp_err_t nvs_write(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static void nvs_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_read(char *ssid, size_t ssid_len,
                     char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK && ssid[0]) &&
              (nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len) == ESP_OK);
    nvs_close(h);
    return ok;
}

/* ── mDNS — started once STA gets an IP ─────────────────── */

static void start_mdns(void) {
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("Hydroponics Governor");
    /*
     * addService is mandatory for Android .local discovery.
     * Without it, .local resolves on iOS/macOS but silently
     * fails on Android because it relies on the service record,
     * not just the hostname record, to trigger mDNS lookup.
     */
    mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://" MDNS_HOSTNAME ".local");
}

/* ── WiFi event handler ──────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data) {
    if (base == WIFI_EVENT) {
        switch ((wifi_event_t)id) {

        case WIFI_EVENT_STA_START:
            /*
             * Fired when WiFi driver starts STA.
             * Only connect if we are expecting to (direct STA mode).
             * On APSTA provisioning, don't connect until save_handler.
             */
            if (s_state != WIFI_MGR_AP_ONLY) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA associated with AP");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)event_data;
            int64_t elapsed_ms =
                (esp_timer_get_time() - s_conn_start_us) / 1000LL;

            ESP_LOGW(TAG, "STA disconnected — reason=%d elapsed=%lldms",
                     ev->reason, (long long)elapsed_ms);

            if (s_state == WIFI_MGR_CONNECTING) {
                bool hard_fail =
                    (ev->reason == WIFI_REASON_NO_AP_FOUND)            ||
                    (ev->reason == WIFI_REASON_AUTH_FAIL)               ||
                    (ev->reason == WIFI_REASON_ASSOC_FAIL)              ||
                    (ev->reason == WIFI_REASON_HANDSHAKE_TIMEOUT)       ||
                    (ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)  ||
                    (ev->reason == WIFI_REASON_MIC_FAILURE)             ||
                    (elapsed_ms  > WIFI_CONNECT_TIMEOUT_MS);

                if (hard_fail) {
                    s_state = WIFI_MGR_FAILED;
                    xEventGroupSetBits(s_eg, BIT_STA_FAILED);
                    ESP_LOGW(TAG, "Hard fail — stopping retry");
                } else {
                    /* AUTH_EXPIRE or transient glitch — retry once */
                    ESP_LOGI(TAG, "Transient disconnect — retrying");
                    esp_wifi_connect();
                }
            } else if (s_state == WIFI_MGR_CONNECTED) {
                /* Runtime drop — reconnect handled by main loop with backoff */
                s_state = WIFI_MGR_FAILED;
                ESP_LOGW(TAG, "Runtime WiFi drop — main loop will reconnect");
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            s_scanning = false;
            ESP_LOGD(TAG, "Scan complete");
            break;

        default:
            break;
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);

        s_state = WIFI_MGR_CONNECTED;
        xEventGroupSetBits(s_eg, BIT_STA_CONNECTED);

        sensor_set_ap_mode(false);
        start_mdns();

        char msg[64];
        snprintf(msg, sizeof(msg), "[STA] Connected — IP: %s", s_ip);
        enqueue_log(msg);
    }
}

/* ── Internal: start AP+STA for provisioning ────────────────
 *
 * BUG FIX: The original start_ap() created only the AP netif.
 * When connect_sta() was later called, the STA interface had no
 * netif so the DHCP client had nowhere to store the IP lease —
 * IP_EVENT_STA_GOT_IP never fired and provisioning silently hung.
 *
 * Fix: create BOTH netifs here before calling esp_wifi_start().
 * The s_sta_netif_done guard prevents double-creation on retry.
 * ──────────────────────────────────────────────────────────── */
static void start_ap(void) {
    esp_netif_create_default_wifi_ap();

    /* Create STA netif now — DHCP client needs it when IP arrives */
    if (!s_sta_netif_done) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_done = true;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = sizeof(AP_SSID) - 1,
            .channel        = 6,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_active = true;
    s_state     = WIFI_MGR_AP_ONLY;
    sensor_set_ap_mode(true);

    ESP_LOGI(TAG, "AP started — SSID: '%s'  IP: %s", AP_SSID, AP_IP_ADDR);
}

/* ── Internal: initiate STA connection ──────────────────────
 *
 * BUG FIX: The original set threshold.authmode = WIFI_AUTH_WPA2_PSK
 * unconditionally. The WiFi driver rejects open-network associations
 * when a WPA2 minimum is set — they never reach WL_CONNECTED.
 *
 * Fix: caller passes `open`; we only set the threshold for secured nets.
 * ──────────────────────────────────────────────────────────── */
static void connect_sta(const char *ssid, const char *pass, bool open) {
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);

    if (!open) {
        sta.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }
    /* For open nets, threshold stays at WIFI_AUTH_OPEN (== 0, zero-init) */

    s_state         = WIFI_MGR_CONNECTING;
    s_conn_start_us = esp_timer_get_time();
    xEventGroupClearBits(s_eg, BIT_STA_CONNECTED | BIT_STA_FAILED);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    /*
     * In APSTA mode the WiFi driver is already started by start_ap(),
     * so WIFI_EVENT_STA_START will NOT fire again — we call
     * esp_wifi_connect() directly here instead.
     */
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
        s_state = WIFI_MGR_FAILED;
        xEventGroupSetBits(s_eg, BIT_STA_FAILED);
    }
}

/* ── Public API ──────────────────────────────────────────── */

esp_err_t wifi_manager_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_eg = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    return ESP_OK;
}

void wifi_manager_set_ap_shutdown_cb(wifi_mgr_ap_shutdown_cb_t cb) {
    s_shutdown_cb = cb;
}

esp_err_t wifi_manager_start(void) {
    char ssid[64] = {0}, pass[64] = {0};

    if (!nvs_read(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No credentials — starting provisioning AP");
        start_ap();
        return ESP_OK;
    }

    /* ── Direct STA path ─────────────────────────────────── */
    ESP_LOGI(TAG, "Credentials found — connecting to '%s'", ssid);

    if (!s_sta_netif_done) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_done = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    /* Direct STA path: assume secured. If open, user re-provisions. */
    sta.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    s_state         = WIFI_MGR_CONNECTING;
    s_conn_start_us = esp_timer_get_time();

    ESP_ERROR_CHECK(esp_wifi_start());   /* fires STA_START → connect */

    EventBits_t bits = xEventGroupWaitBits(
        s_eg,
        BIT_STA_CONNECTED | BIT_STA_FAILED,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & BIT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Direct STA connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Direct STA failed — clearing credentials, rebooting");
    nvs_clear();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_FAIL; /* unreachable */
}

esp_err_t wifi_manager_connect(const char *ssid, const char *pass, bool open) {
    if (s_state != WIFI_MGR_AP_ONLY && s_state != WIFI_MGR_FAILED) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Write before connecting — survives power cycle mid-handshake */
    nvs_write(ssid, pass);
    connect_sta(ssid, pass, open);
    return ESP_OK;
}

esp_err_t wifi_manager_reconnect(void) {
    /*
     * BUG FIX: the original main loop called wifi_manager_start()
     * on reconnect which re-runs esp_netif_init() and
     * esp_event_loop_create_default() — both abort if called twice.
     *
     * This function only re-reads NVS credentials and calls
     * esp_wifi_connect(). The driver, netif and event loop are
     * already initialised — we must not touch them.
     */
    char ssid[64] = {0}, pass[64] = {0};
    if (!nvs_read(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "reconnect: no credentials in NVS");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    s_state         = WIFI_MGR_CONNECTING;
    s_conn_start_us = esp_timer_get_time();
    xEventGroupClearBits(s_eg, BIT_STA_CONNECTED | BIT_STA_FAILED);

    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
        s_state = WIFI_MGR_FAILED;
    }
    return ret;
}

esp_err_t wifi_manager_scan_start(void) {
    if (s_scanning) {
        ESP_LOGI(TAG, "Scan request ignored: already running");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t sc = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_scanning = true;
    s_last_scan_err = ESP_OK;

    esp_err_t ret = esp_wifi_scan_start(&sc, false);
    if (ret != ESP_OK) {
        s_scanning = false;
        s_last_scan_err = ret;
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi scan started");
    return ESP_OK;
}

void wifi_manager_get_scan_json(char *buf, size_t len) {
    if (s_scanning) { strncpy(buf, "[]", len); return; }

    if (s_last_scan_err != ESP_OK) {
        ESP_LOGW(TAG, "Returning empty scan list after scan error: %s",
                 esp_err_to_name(s_last_scan_err));
        strncpy(buf, "[]", len);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    ESP_LOGI(TAG, "Scan complete, APs found: %u", (unsigned)ap_num);
    if (ap_num == 0) { strncpy(buf, "[]", len); return; }

    uint16_t fetch = ap_num > 20 ? 20 : ap_num;
    wifi_ap_record_t *aps = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!aps) { strncpy(buf, "[]", len); return; }
    esp_wifi_scan_get_ap_records(&fetch, aps);

    int  pos   = 0;
    bool first = true;
    pos += snprintf(buf + pos, len - pos, "[");

    for (int i = 0; i < fetch && pos < (int)len - 96; i++) {
        if (strlen((char *)aps[i].ssid) == 0) continue; /* hidden */

        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)aps[j].ssid, (char *)aps[i].ssid) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;

        bool open = (aps[i].authmode == WIFI_AUTH_OPEN);
        char ssid_json[96];
        size_t out = 0;

        for (size_t k = 0; aps[i].ssid[k] != '\0' && out < sizeof(ssid_json) - 3; k++) {
            char ch = (char)aps[i].ssid[k];
            if (ch == '\\' || ch == '"') {
                ssid_json[out++] = '\\';
                ssid_json[out++] = ch;
            } else if ((unsigned char)ch >= 0x20) {
                ssid_json[out++] = ch;
            }
        }
        ssid_json[out] = '\0';

        if (!first) pos += snprintf(buf + pos, len - pos, ",");
        first = false;
        pos += snprintf(buf + pos, len - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
            ssid_json, aps[i].rssi, open ? "true" : "false");
    }
    snprintf(buf + pos, len - pos, "]");
    free(aps);
}

wifi_mgr_state_t wifi_manager_get_state(void)  { return s_state;     }
const char      *wifi_manager_get_ip(void)      { return s_ip;        }
bool             wifi_manager_ap_active(void)   { return s_ap_active; }
bool             wifi_manager_scan_running(void){ return s_scanning;  }

void wifi_manager_ap_shutdown(void) {
    if (!s_ap_active) return;
    s_ap_active = false;

    /*
     * BUG FIX: the original called esp_wifi_set_mode(WIFI_MODE_STA)
     * without notifying the DNS server. The DNS socket was bound to
     * INADDR_ANY and kept running after the AP netif disappeared,
     * leaking a FreeRTOS task and a socket slot.
     *
     * Fix: fire the registered callback (set to dns_server_stop in
     * main.c) BEFORE changing WiFi mode. The DNS task then marks
     * itself stopped and self-deletes on the next 1s recvfrom timeout.
     */
    if (s_shutdown_cb) s_shutdown_cb();

    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "AP shut down — home WiFi client reached dashboard");
}
