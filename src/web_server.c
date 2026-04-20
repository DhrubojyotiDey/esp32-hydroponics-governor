#include "web_server.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "sensor_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

static const char *TAG = "WEB";

/* ── Dashboard & Captive Portal HTML ─────────────────────── */
/* These headers are auto-generated from .html files via
 * scripts/embed_html.py on build to bypass buggy PlatformIO
 * assembly linking on Windows for ESP-IDF.                   */
#include "page_setup_html.h"
#include "page_dash_html.h"

/* ── Server handle ───────────────────────────────────────── */
static httpd_handle_t s_server    = NULL;
static bool           s_dash_ready = false;
static char           s_dash_ip[20] = "";
static bool           s_ap_shutdown_scheduled = false;

static esp_err_t captive_portal_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_ADDR "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── Async WebSocket send ────────────────────────────────────
 * httpd_ws_send_frame_async() must be called from within the
 * httpd server task. httpd_queue_work() marshals the call.   */
typedef struct {
    httpd_handle_t hd;
    int            fd;
    uint8_t        data[WS_JSON_BUF_SIZE];
    size_t         len;
} ws_send_arg_t;

static void ws_send_cb(void *arg) {
    ws_send_arg_t *a = (ws_send_arg_t *)arg;
    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = a->data,
        .len     = a->len,
        .final   = true,
    };
    httpd_ws_send_frame_async(a->hd, a->fd, &pkt);
    free(a);
}

void web_server_push_sensor_update(const char *json) {
    if (!s_server) return;

    size_t max_clients = MAX_WS_CLIENTS;
    int    fds[MAX_WS_CLIENTS];

    if (httpd_get_client_list(s_server, &max_clients, fds) != ESP_OK) return;

    size_t jlen = strlen(json);
    if (jlen >= WS_JSON_BUF_SIZE) jlen = WS_JSON_BUF_SIZE - 1;

    for (size_t i = 0; i < max_clients; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;

        ws_send_arg_t *arg = malloc(sizeof(ws_send_arg_t));
        if (!arg) continue;
        arg->hd  = s_server;
        arg->fd  = fds[i];
        arg->len = jlen;
        memcpy(arg->data, json, jlen);
        /* Queue work onto the httpd server task — non-blocking */
        if (httpd_queue_work(s_server, ws_send_cb, arg) != ESP_OK) free(arg);
    }
}

void web_server_set_dash_ready(bool ready, const char *ip) {
    s_dash_ready = ready;
    if (ip) strncpy(s_dash_ip, ip, sizeof(s_dash_ip) - 1);
}

/* ╔══════════════════════════════════════════════════════════╗
 * ║  URI HANDLERS                                           ║
 * ╚══════════════════════════════════════════════════════════╝ */

static void delayed_ap_shutdown_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_ap_shutdown();
    s_ap_shutdown_scheduled = false;
    vTaskDelete(NULL);
}

static void delayed_reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "Executing user-invoked soft restart to drop AP and enter pure STA mode!");
    esp_restart();
}

/* ── GET / — captive portal or dashboard ─────────────────── */
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    /* Prevent caching so dashboard always reflects live state */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (wifi_manager_get_state() == WIFI_MGR_CONNECTED) {
        /* If the visitor arrives from home WiFi, AP is no longer needed */
        if (wifi_manager_ap_active() && !s_ap_shutdown_scheduled) {
            /* Schedule AP shutdown — leave 3s grace for the response to land */
            ESP_LOGI(TAG, "Dashboard hit from home WiFi — scheduling AP shutdown");
            s_ap_shutdown_scheduled = true;
            if (xTaskCreate(delayed_ap_shutdown_task, "ap_shutdown",
                            2048, NULL, 4, NULL) != pdPASS) {
                s_ap_shutdown_scheduled = false;
                ESP_LOGW(TAG, "Failed to schedule AP shutdown task");
            }
        }
        const size_t dash_len = page_dash_html_len;
        httpd_resp_send(req, (const char *)page_dash_html_start, dash_len);
    } else {
        const size_t setup_len = page_setup_html_len;
        httpd_resp_send(req, (const char *)page_setup_html_start, setup_len);
    }
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
    ESP_LOGI(TAG, "Reboot explicitly requested by Captive Portal UI. Restarting in 1s...");
    xTaskCreate(delayed_reboot_task, "delayed_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t scan_handler(httpd_req_t *req) {
    /* Start a new scan explicitly only if not already running */
    esp_err_t ret = ESP_OK;
    if (!wifi_manager_scan_running()) {
        ret = wifi_manager_scan_start();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "/scan requested");
        httpd_resp_sendstr(req, "{\"status\":\"started\"}");
    } else {
        ESP_LOGW(TAG, "/scan failed: %s", esp_err_to_name(ret));
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
    }
    return ESP_OK;
}

/* ── GET /scanresults — return cached scan JSON ─────────── */
static esp_err_t scanresults_handler(httpd_req_t *req) {
    char buf[2048];
    wifi_manager_get_scan_json(buf, sizeof(buf));
    ESP_LOGI(TAG, "/scanresults -> %s", buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}



/* ── GET /scanstatus ────────────────────────────────────── */
static esp_err_t scanstatus_handler(httpd_req_t *req) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"scanning\":%s}",
             wifi_manager_scan_running() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t keepalive_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── URL decoder ─────────────────────────────────────────────
 *
 * BUG FIX: The original parsed URL-encoded body values raw, without
 * percent-decoding. Any SSID or password containing spaces (%20),
 * @ (%40), + (encoded as +), or other special characters arrived
 * corrupted, causing silent connection failure.
 *
 * This decoder handles both %XX hex sequences and + → space.
 * dst is always NUL-terminated; overflow is truncated safely.
 * Returns number of bytes written to dst (excluding NUL).
 * ──────────────────────────────────────────────────────────── */
static int url_decode(const char *src, size_t src_len,
                      char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; si < src_len && di < dst_size - 1; si++) {
        if (src[si] == '%' && si + 2 < src_len) {
            /* Parse hex pair */
            char hex[3] = { src[si+1], src[si+2], '\0' };
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                dst[di++] = (char)val;
                si += 2;
            } else {
                dst[di++] = src[si]; /* not valid hex — copy literally */
            }
        } else if (src[si] == '+') {
            dst[di++] = ' ';        /* HTML form encodes spaces as + */
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return (int)di;
}

/* Extract a named field from a URL-encoded string: field=VALUE&...
 * Returns true if the field was found and decoded into dst.         */
static bool extract_field(const char *body, const char *field,
                           char *dst, size_t dst_size) {
    /* Build "field=" to search for, avoiding partial name matches */
    char key[32];
    snprintf(key, sizeof(key), "%s=", field);

    const char *p = strstr(body, key);
    if (!p) {
        /* Also check if it's at the very start of body */
        if (strncmp(body, key, strlen(key)) != 0) return false;
        p = body;
    }
    p += strlen(key);

    const char *amp = strchr(p, '&');
    size_t raw_len  = amp ? (size_t)(amp - p) : strlen(p);

    url_decode(p, raw_len, dst, dst_size);
    return true;
}

/* ── POST /save — receive credentials, start STA connect ── */
static esp_err_t save_handler(httpd_req_t *req) {
    wifi_mgr_state_t st = wifi_manager_get_state();
    if (st == WIFI_MGR_CONNECTING || st == WIFI_MGR_CONNECTED) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"busy\"}");
        return ESP_OK;
    }

    /* Read URL-encoded body: s=SSID&p=PASS&o=1 */
    char body[384] = {0};
    int  recv_len  = httpd_req_recv(req, body,
                                    sizeof(body) - 1 < req->content_len
                                    ? sizeof(body) - 1
                                    : req->content_len);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    char ssid[64] = {0}, pass[64] = {0}, open_str[4] = {0};

    /* extract_field + url_decode handles %XX and + → space */
    extract_field(body, "s", ssid, sizeof(ssid));
    extract_field(body, "p", pass, sizeof(pass));
    extract_field(body, "o", open_str, sizeof(open_str));

    /* JS sends o=1 for open networks, o=0 (or absent) for secured */
    bool open = (open_str[0] == '1');

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"msg\":\"SSID required\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "/save — SSID='%s' | user's given password is %s | open=%d",
             ssid, pass, (int)open);

    esp_err_t ret = wifi_manager_connect(ssid, pass, open);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        ret == ESP_OK ? "{\"status\":\"connecting\"}" : "{\"status\":\"error\"}");
    return ESP_OK;
}

/* ── GET /status — polled by captive portal JS ──────────── */
static esp_err_t status_handler(httpd_req_t *req) {
    char buf[96];
    wifi_mgr_state_t st = wifi_manager_get_state();

    if (st == WIFI_MGR_CONNECTED) {
        snprintf(buf, sizeof(buf),
            "{\"status\":\"connected\",\"ip\":\"%s\"}", wifi_manager_get_ip());
    } else if (st == WIFI_MGR_FAILED) {
        snprintf(buf, sizeof(buf), "{\"status\":\"failed\"}");
    } else if (st == WIFI_MGR_CONNECTING) {
        snprintf(buf, sizeof(buf), "{\"status\":\"connecting\"}");
    } else {
        snprintf(buf, sizeof(buf), "{\"status\":\"idle\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── POST /ap_shutdown — shut down AP on demand ─────────── */
static esp_err_t ap_shutdown_handler(httpd_req_t *req) {
    if (wifi_manager_ap_active() && !s_ap_shutdown_scheduled) {
        ESP_LOGI(TAG, "AP shutdown requested via /ap_shutdown endpoint");
        s_ap_shutdown_scheduled = true;
        if (xTaskCreate(delayed_ap_shutdown_task, "ap_shutdown",
                        2048, NULL, 4, NULL) != pdPASS) {
            s_ap_shutdown_scheduled = false;
            ESP_LOGW(TAG, "Failed to schedule AP shutdown task");
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"scheduled\"}");
    return ESP_OK;
}

/* ── GET /dashready — gateway ping readiness gate ───────── */
static esp_err_t dashready_handler(httpd_req_t *req) {
    bool connected = (wifi_manager_get_state() == WIFI_MGR_CONNECTED);

    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"ready\":%s,\"ip\":\"%s\",\"local\":\"http://" MDNS_HOSTNAME ".local\"}",
        connected ? "true" : "false", wifi_manager_get_ip());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /data — snapshot sensor JSON (REST fallback) ────── */
static esp_err_t data_handler(httpd_req_t *req) {
    if (wifi_manager_get_state() != WIFI_MGR_CONNECTED) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not connected");
        return ESP_OK;
    }
    char buf[WS_JSON_BUF_SIZE];
    if (sensor_get_json(buf, sizeof(buf)) == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, buf);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    }
    return ESP_OK;
}

/* ── GET /ws — WebSocket endpoint ──────────────────────── */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        /* Upgrade handshake — httpd handles the 101 Switching Protocols.
         * Nothing to do here; just log the new client.         */
        ESP_LOGI(TAG, "WS client connected, fd=%d",
                 httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* ── Receive a WebSocket frame ──────────────────────── */
    httpd_ws_frame_t pkt = {.type = HTTPD_WS_TYPE_TEXT};

    /* First call with len=0 to get the frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt.len == 0 || pkt.len > 32) {
        return ESP_OK;
    }

    uint8_t buf[33] = {0};
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) return ret;

    if (strncmp((char *)buf, "reboot", 6) == 0) {
        ESP_LOGI(TAG, "WS reboot command received");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return ESP_OK;
}

/* ── POST /ota — OTA firmware upload ───────────────────── */
static esp_err_t ota_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "OTA start — content_len=%d", req->content_len);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle;
    esp_err_t ret = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    int  remaining = req->content_len;

    while (remaining > 0) {
        int to_recv = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive failed");
            return ESP_FAIL;
        }
        ret = esp_ota_write(handle, buf, received);
        if (ret != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "OTA write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    ret = esp_ota_end(handle);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA validation failed");
        return ESP_FAIL;
    }

    esp_ota_set_boot_partition(part);
    ESP_LOGI(TAG, "OTA complete — rebooting");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"msg\":\"OTA complete. Rebooting...\"}");

    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

/* ── Captive portal detection endpoints ──────────────────── */

/* Android: GET /generate_204 → 302 to portal */
static esp_err_t captive_android_handler(httpd_req_t *req) {
    return captive_portal_redirect(req);
}

/* Windows: GET /connecttest.txt -> redirect to keep captive portal open */
static esp_err_t captive_windows_handler(httpd_req_t *req) {
    return captive_portal_redirect(req);
}

/* Apple: GET /hotspot-detect.html -> redirect to keep captive portal open */
static esp_err_t captive_apple_handler(httpd_req_t *req) {
    return captive_portal_redirect(req);
}

/* ── 404 / catchall → redirect to portal ─────────────────── */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    if (wifi_manager_get_state() != WIFI_MGR_CONNECTED) {
        /* In provisioning mode, all unknown URLs → captive portal */
        captive_portal_redirect(req);
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    return ESP_OK;
}

/* ── Server start / stop ─────────────────────────────────── */

esp_err_t web_server_start(void) {
    httpd_config_t cfg        = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers      = 24;
    cfg.stack_size            = 8192;
    cfg.lru_purge_enable      = true;  /* drop oldest client on slot exhaustion */

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── URI table ─────────────────────────────────────────
     * Order matters for is_websocket — WS handler must have
     * is_websocket = true so httpd handles the upgrade.      */
    static const httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = root_handler          },
        { .uri = "/scan",               .method = HTTP_GET,  .handler = scan_handler          },
        { .uri = "/scanresults",        .method = HTTP_GET,  .handler = scanresults_handler   },
        { .uri = "/scanstatus",         .method = HTTP_GET,  .handler = scanstatus_handler    },
        { .uri = "/keepalive",          .method = HTTP_GET,  .handler = keepalive_handler     },
        { .uri = "/favicon.ico",        .method = HTTP_GET,  .handler = favicon_handler       },
        { .uri = "/save",               .method = HTTP_POST, .handler = save_handler          },
        { .uri = "/status",             .method = HTTP_GET,  .handler = status_handler        },
        { .uri = "/ap_shutdown",        .method = HTTP_POST, .handler = ap_shutdown_handler   },
        { .uri = "/reboot",             .method = HTTP_POST, .handler = reboot_handler        },
        { .uri = "/dashready",          .method = HTTP_GET,  .handler = dashready_handler     },
        { .uri = "/data",               .method = HTTP_GET,  .handler = data_handler          },
        { .uri = "/ota",                .method = HTTP_POST, .handler = ota_handler           },
        { .uri = "/generate_204",       .method = HTTP_GET,  .handler = captive_android_handler},
        { .uri = "/connecttest.txt",    .method = HTTP_GET,  .handler = captive_windows_handler},
        { .uri = "/fwlink",             .method = HTTP_GET,  .handler = captive_windows_handler},
        { .uri = "/ncsi.txt",           .method = HTTP_GET,  .handler = captive_windows_handler},
        { .uri = "/hotspot-detect.html",.method = HTTP_GET,  .handler = captive_apple_handler },
        { .uri = "/success.txt",        .method = HTTP_GET,  .handler = captive_apple_handler },
        { .uri = "/ws",                 .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true                                                                 },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_handler);

    ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
