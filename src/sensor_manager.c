#include "sensor_manager.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>

#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "SENSOR";

/* ── Sensor metadata ─────────────────────────────────────── */
typedef struct {
    char     name[16];
    uint32_t timeout_ms;
    uint32_t last_seen_ms; /* millis of last markAlive call */
    bool     active;       /* has ever been seen            */
    bool     alive;        /* currently within timeout      */
    bool     prev_alive;   /* previous cycle (for RECOVERED)*/
} sensor_meta_t;

/* ── Tray — protected by tray_mutex ─────────────────────── */
typedef struct {
    float temp;
    float hum;
    float flow;
} sensor_tray_t;

/* ── Module state ────────────────────────────────────────── */
static sensor_meta_t     s_sensors[MAX_SENSORS];
static uint8_t           s_count = 0;
static sensor_tray_t     s_tray;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t     s_log_queue;
static volatile bool     s_ap_mode = false;


/* ── Helpers ─────────────────────────────────────────────── */

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ── Public API ──────────────────────────────────────────── */

void sensor_manager_init(void) {
    s_mutex     = xSemaphoreCreateMutex();
    s_log_queue = xQueueCreate(LOG_QUEUE_DEPTH, LOG_MSG_LEN);
    s_count     = 0;
    memset(&s_tray, 0, sizeof(s_tray));
    ESP_LOGI(TAG, "Initialised");
}

void sensor_register(const char *name, uint32_t timeout_ms) {
    if (s_count >= MAX_SENSORS) return;
    sensor_meta_t *s = &s_sensors[s_count++];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->timeout_ms   = timeout_ms;
    s->last_seen_ms = 0;
    s->active       = false;
    s->alive        = false;
    s->prev_alive   = false;
    ESP_LOGI(TAG, "Registered sensor '%s' timeout=%lums", name, (unsigned long)timeout_ms);
}

void sensor_mark_alive(const char *name) {
    uint32_t t = now_ms();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_sensors[i].name, name) == 0) {
            s_sensors[i].last_seen_ms = t;
            s_sensors[i].active       = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

void sensor_update_dht(float temp, float hum) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_tray.temp = temp;
    s_tray.hum  = hum;
    xSemaphoreGive(s_mutex);

    sensor_mark_alive("dht");
    if (push_task_handle) xTaskNotifyGive(push_task_handle);
}

void sensor_update_flow(float lpm) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_tray.flow = lpm;
    xSemaphoreGive(s_mutex);

    sensor_mark_alive("flow");
    if (push_task_handle) xTaskNotifyGive(push_task_handle);
}

/* ── Health engine ───────────────────────────────────────── */

void sensor_update_health(void) {
    /* Capture events outside critical section to avoid
     * priority inversion on enqueue_log / xTaskNotifyGive. */
    typedef struct { bool dead; bool transition; char name[16]; } evt_t;
    evt_t  events[MAX_SENSORS];
    int    evt_count = 0;
    uint32_t t = now_ms();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        sensor_meta_t *s = &s_sensors[i];
        s->prev_alive = s->alive;
        s->alive      = s->active && ((t - s->last_seen_ms) < s->timeout_ms);
        bool tx = (s->prev_alive != s->alive);

        /* Emit event on: DEAD (every cycle) or ALIVE←DEAD (once) */
        if (!s->alive || (s->alive && tx)) {
            if (evt_count < MAX_SENSORS) {
                events[evt_count].dead       = !s->alive;
                events[evt_count].transition = tx;
                strncpy(events[evt_count].name, s->name, 15);
                events[evt_count].name[15]   = '\0';
                evt_count++;
            }
        }
    }
    xSemaphoreGive(s_mutex);

    bool notify = false;
    for (int i = 0; i < evt_count; i++) {
        char msg[64];
        if (events[i].dead) {
            snprintf(msg, sizeof(msg), "[ALERT] Sensor '%s' DEAD", events[i].name);
            ESP_LOGW(TAG, "Sensor '%s' DEAD", events[i].name);
        } else {
            snprintf(msg, sizeof(msg), "[INFO]  Sensor '%s' RECOVERED", events[i].name);
            ESP_LOGI(TAG, "Sensor '%s' RECOVERED", events[i].name);
            notify = true;
        }
        enqueue_log(msg);
    }
    if (notify && push_task_handle) xTaskNotifyGive(push_task_handle);
}

/* ── JSON builder ────────────────────────────────────────── */

esp_err_t sensor_get_json(char *buf, size_t len) {
    /* sensor sub-object: {"dht":true,"flow":false} */
    char sp[256] = "{";
    int  sp_len  = 1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < s_count; i++) {
        char entry[32];
        int wrote = snprintf(entry, sizeof(entry), "\"%s\":%s%s",
            s_sensors[i].name,
            s_sensors[i].alive ? "true" : "false",
            (i < s_count - 1) ? "," : "");
        if (sp_len + wrote < (int)sizeof(sp) - 2) {
            memcpy(sp + sp_len, entry, wrote);
            sp_len += wrote;
        }
    }
    sp[sp_len++] = '}';
    sp[sp_len]   = '\0';

    int wrote = snprintf(buf, len,
        "{\"temp\":%.1f,\"hum\":%.1f,\"flow\":%.2f,\"sensors\":%s}",
        s_tray.temp, s_tray.hum, s_tray.flow, sp);

    xSemaphoreGive(s_mutex);

    return (wrote > 0 && wrote < (int)len) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── AP mode ─────────────────────────────────────────────── */
void sensor_set_ap_mode(bool ap) { s_ap_mode = ap; }
bool sensor_is_ap_mode(void)     { return s_ap_mode; }

/* ── Log queue ───────────────────────────────────────────── */

void enqueue_log(const char *msg) {
    if (!s_log_queue) return;
    char buf[LOG_MSG_LEN];
    strncpy(buf, msg, LOG_MSG_LEN - 1);
    buf[LOG_MSG_LEN - 1] = '\0';
    xQueueSend(s_log_queue, buf, 0);  /* non-blocking, drop on full */
}

void *sensor_get_log_queue(void) { return (void *)s_log_queue; }
