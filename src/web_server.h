#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

/** Start the HTTP server and register all URI + WebSocket handlers. */
esp_err_t web_server_start(void);

/** Stop the HTTP server. */
void web_server_stop(void);

/**
 * @brief Broadcast a JSON string to all connected WebSocket clients.
 *
 * Uses httpd_queue_work() for each client fd — safe to call from any
 * FreeRTOS task (including push_task on Core 1).
 */
void web_server_push_sensor_update(const char *json);

/** Signal that sensors have warmed up and the dashboard is ready.
 *  Called ~4s after STA connects; enables /dashready to return true. */
void web_server_set_dash_ready(bool ready, const char *ip);

#endif /* WEB_SERVER_H */
