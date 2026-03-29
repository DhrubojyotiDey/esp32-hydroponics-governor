#include <Arduino.h>
#include "ota_manager.h"
#include "sensor_manager.h"
#include "DHT.h"

// ============================================================
//  Pin definitions
// ============================================================
#define DHTPIN    14
#define DHTTYPE   DHT11
#define FLOW_PIN  27
#define LED_PIN   2

// ============================================================
//  Sensor objects
// ============================================================
DHT dht(DHTPIN, DHTTYPE);

// ============================================================
//  Flow ISR
// ============================================================
portMUX_TYPE      flowMux    = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulseCount = 0;

void IRAM_ATTR flowISR() {
  portENTER_CRITICAL_ISR(&flowMux);
  pulseCount++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

// ============================================================
//  Task handles
//  Stored so tasks can be notified, monitored, or debugged.
//  pushTaskHandle is extern'd in sensor_manager.h so producers
//  can call xTaskNotifyGive() directly after a tray update.
// ============================================================
TaskHandle_t pushTaskHandle       = NULL;
TaskHandle_t dhtTaskHandle        = NULL;
TaskHandle_t flowTaskHandle       = NULL;
TaskHandle_t loggerTaskHandle     = NULL;
TaskHandle_t sensorViewTaskHandle = NULL;
TaskHandle_t ledTaskHandle        = NULL;

// ============================================================
//  Log queue — fixed 128-byte char buffers, no heap alloc.
//  Avoids the heap fragmentation that String* pointers cause
//  over long uptimes.
// ============================================================
#define LOG_MSG_LEN 128
#define LOG_QUEUE_DEPTH 16

QueueHandle_t logQueue;

void enqueueLog(const char* msg) {
  if (!logQueue) return;
  char buf[LOG_MSG_LEN];
  strncpy(buf, msg, LOG_MSG_LEN - 1);
  buf[LOG_MSG_LEN - 1] = '\0';
  xQueueSend(logQueue, buf, 0);  // drop if full, never block
}

// ============================================================
//  TASK: Logger  [Core 1]
//  Blocks on the queue with portMAX_DELAY — zero CPU when idle.
//  Receives fixed-size char buffers, no heap involved.
// ============================================================
void loggerTask(void* pv) {
  char buf[LOG_MSG_LEN];

  while (true) {
    if (xQueueReceive(logQueue, buf, portMAX_DELAY)) {
      Serial.println(buf);

      if (telnetClient && telnetClient->connected()) {
        telnetClient->add(buf, strlen(buf));
        telnetClient->add("\r\n", 2);
        telnetClient->send();
      }
    }
  }
}

// ============================================================
//  TASK: DHT Producer  [Core 0]
//  Reads DHT11 every 1 second.
//  Only calls updateDHT() when reading is valid AND changed.
//  updateDHT() notifies pushTask via TaskNotification.
//  2s stabilisation required after dht.begin().
// ============================================================
void dhtTask(void* pv) {
  dht.begin();
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  float lastT = NAN;
  float lastH = NAN;

  while (true) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      if (t != lastT || h != lastH) {
        updateDHT(t, h);  // notifies pushTask internally
        lastT = t;
        lastH = h;
      }
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  TASK: Flow Producer  [Core 0]
//  Every 500ms: snapshots pulse counter, calculates L/min,
//  calls updateFlow() which notifies pushTask internally.
//  Formula scaled for 500ms window: count / 3.75
// ============================================================
void flowTask(void* pv) {
  while (true) {
    vTaskDelay(500 / portTICK_PERIOD_MS);

    portENTER_CRITICAL(&flowMux);
    uint32_t count = pulseCount;
    pulseCount = 0;
    portEXIT_CRITICAL(&flowMux);

    float lpm = (float)count / 3.75f;
    updateFlow(lpm);  // notifies pushTask internally
  }
}

// ============================================================
//  TASK: Push  [Core 1]
//  Truly event-driven — blocks on ulTaskNotifyTake() with
//  portMAX_DELAY. Woken directly by updateDHT() / updateFlow()
//  via xTaskNotifyGive(). No polling, no dirty flag, no delay.
//  Pushes JSON to all WebSocket clients the moment data lands.
// ============================================================
void pushTask(void* pv) {
  // pushTaskHandle is already set by xTaskCreatePinnedToCore before
  // this task runs — no need to re-assign it here.

  uint32_t notifyCount;

  while (true) {

    // 🔥 Wait for notification(s)
    notifyCount = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // 🔍 Debug: detect burst events
    if (notifyCount > 1) {
      Serial.printf("[PushTask] Burst: %lu events\n", notifyCount);
    }

    // 🔥 Push update (only if clients exist)
    pushSensorUpdate();

    // 🧠 Optional: small yield (helps stability under heavy load)
    taskYIELD();
  }
}

// ============================================================
//  TASK: Sensor View Logger  [Core 1]
//  Periodic: dumps full tray JSON to Serial/Telnet every 1s
//  regardless of whether values changed. For monitoring.
// ============================================================
void sensorViewTask(void* pv) {
  while (true) {
    String json = getSensorJSON();
    enqueueLog(json.c_str());
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  TASK: LED Heartbeat  [Core 1]
// ============================================================
void ledTask(void* pv) {
  pinMode(LED_PIN, OUTPUT);
  while (true) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP32 Hydroponics Governor");

  // Flow ISR
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, FALLING);

  // Fixed-size char buffer queue — no heap fragmentation
  logQueue = xQueueCreate(LOG_QUEUE_DEPTH, LOG_MSG_LEN);

  // Tray + sensor registry — must come before registerSensor()
  initSensorManager();

  // Register only sensors that are physically present and implemented.
  // Adding a sensor here without a corresponding markSensorAlive() call
  // will cause it to appear permanently dead in the JSON.
  registerSensor("dht",  10000);
  registerSensor("flow",  5000);

  // WiFi / OTA / WebSocket / Telnet
  setupWiFiAndOTA();

  // ── Core 0: sensor producers ──────────────────────────────
  xTaskCreatePinnedToCore(dhtTask,        "DHT",    4096, NULL, 2, &dhtTaskHandle,        0);
  xTaskCreatePinnedToCore(flowTask,       "Flow",   2048, NULL, 2, &flowTaskHandle,       0);

  // ── Core 1: network consumers ────────────────────────────
  xTaskCreatePinnedToCore(pushTask,       "Push",   4096, NULL, 2, &pushTaskHandle,       1);
  xTaskCreatePinnedToCore(loggerTask,     "Logger", 4096, NULL, 1, &loggerTaskHandle,     1);
  xTaskCreatePinnedToCore(sensorViewTask, "View",   4096, NULL, 1, &sensorViewTaskHandle, 1);
  xTaskCreatePinnedToCore(ledTask,        "LED",    1024, NULL, 1, &ledTaskHandle,        1);

  Serial.println("[BOOT] All tasks started");
}

// ============================================================
//  LOOP — minimal, just OTA + reboot check
// ============================================================
void loop() {
  handleOTA();

  static unsigned long lastHealthCheck = 0;
  unsigned long now = millis();

  if (now - lastHealthCheck >= 1000) {
    updateSensorHealth();
    lastHealthCheck = now;
  }

  if (shouldReboot) {
    Serial.println("[SYS] Rebooting...");
    delay(500);
    ESP.restart();
  }

  vTaskDelay(10 / portTICK_PERIOD_MS);  // better than delay()
}
