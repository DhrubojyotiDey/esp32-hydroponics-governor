#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <Arduino.h>

extern TaskHandle_t pushTaskHandle;

// Defined in .ino — routes to Serial + Telnet via log queue
extern void enqueueLog(const char* msg);

// ============================================================
// CONFIG
// ============================================================
#define MAX_SENSORS 10

// ============================================================
// SENSOR META (HEALTH TRACKING)
// ============================================================
struct SensorMeta {
  const char* name;
  uint32_t    timeout;
  uint32_t    lastSeen;

  bool        active;
  bool        alive;
  bool        prevAlive;
};

// ============================================================
// SENSOR DATA (TRAY)
// ============================================================
struct SensorState {
  float temp;
  float hum;
  float flow;
};

// ============================================================
// GLOBALS
// ============================================================
SensorMeta        sensors[MAX_SENSORS];
uint8_t           sensorCount = 0;

SensorState       tray;
SemaphoreHandle_t trayMutex;

// ============================================================
// INIT
// ============================================================
void initSensorManager() {
  trayMutex = xSemaphoreCreateMutex();
  sensorCount = 0;

  tray.temp = 0;
  tray.hum  = 0;
  tray.flow = 0;
}

// ============================================================
// REGISTER SENSOR
// ============================================================
void registerSensor(const char* name, uint32_t timeout) {
  if (sensorCount >= MAX_SENSORS) return;

  sensors[sensorCount++] = {
    name,
    timeout,
    0,
    false,
    false,
    false
  };
}

// ============================================================
// MARK SENSOR ALIVE
// ============================================================
void markSensorAlive(const char* name) {
  uint32_t now = millis();

  xSemaphoreTake(trayMutex, portMAX_DELAY);
  for (int i = 0; i < sensorCount; i++) {
    if (strcmp(sensors[i].name, name) == 0) {
      sensors[i].lastSeen = now;
      sensors[i].active   = true;
      sensors[i].alive    = true;
      break;
    }
  }
  xSemaphoreGive(trayMutex);
}

// ============================================================
// HEALTH ENGINE (CORE LOGIC)
// ============================================================
void updateSensorHealth() {
  uint32_t now = millis();

  // Collect transitions inside mutex, act on them after release.
  // This avoids calling xTaskNotifyGive() or enqueueLog() while
  // holding the mutex, which could cause priority inversion.
  struct Transition { bool dead; char name[16]; };
  Transition transitions[MAX_SENSORS];
  int transCount = 0;

  xSemaphoreTake(trayMutex, portMAX_DELAY);

  for (int i = 0; i < sensorCount; i++) {
    SensorMeta &s = sensors[i];

    s.prevAlive = s.alive;

    if (!s.active) {
      s.alive = false;
    } else {
      s.alive = (now - s.lastSeen) < s.timeout;
    }

    if (s.prevAlive != s.alive && transCount < MAX_SENSORS) {
      transitions[transCount].dead = !s.alive;
      strncpy(transitions[transCount].name, s.name, 15);
      transitions[transCount].name[15] = '\0';
      transCount++;
    }
  }

  xSemaphoreGive(trayMutex);

  // Act on transitions outside the mutex
  bool shouldNotify = false;
  for (int i = 0; i < transCount; i++) {
    char msg[64];
    if (transitions[i].dead) {
      snprintf(msg, sizeof(msg), "[ALERT] Sensor %s DEAD", transitions[i].name);
    } else {
      snprintf(msg, sizeof(msg), "[INFO] Sensor %s RECOVERED", transitions[i].name);
    }
    enqueueLog(msg);
    shouldNotify = true;
  }

  if (shouldNotify && pushTaskHandle) {
    xTaskNotifyGive(pushTaskHandle);
  }
}

// ============================================================
// PRODUCERS
// ============================================================
void updateDHT(float t, float h) {
  if (isnan(t) || isnan(h)) return;

  xSemaphoreTake(trayMutex, portMAX_DELAY);
  tray.temp = t;
  tray.hum  = h;
  xSemaphoreGive(trayMutex);

  markSensorAlive("dht");

  if (pushTaskHandle) xTaskNotifyGive(pushTaskHandle);
}

void updateFlow(float lpm) {
  if (isnan(lpm)) return;

  xSemaphoreTake(trayMutex, portMAX_DELAY);
  tray.flow = lpm;
  xSemaphoreGive(trayMutex);

  markSensorAlive("flow");

  if (pushTaskHandle) xTaskNotifyGive(pushTaskHandle);
}

// ============================================================
// JSON BUILDER
// ============================================================
String getSensorJSON() {
  char buf[256];
  // MAX_SENSORS * ~20 chars per entry + wrapping = safe at 256
  char sensorBuf[256] = "{";
  int  sensorBufLen   = 1;  // tracks current length, avoids unsafe strcat

  xSemaphoreTake(trayMutex, portMAX_DELAY);

  for (int i = 0; i < sensorCount; i++) {
    char entry[32];
    int wrote = snprintf(entry, sizeof(entry),
      "\"%s\":%s%s",
      sensors[i].name,
      sensors[i].alive ? "true" : "false",
      (i < sensorCount - 1) ? "," : ""
    );
    // Only append if it fits — silently skip if buffer would overflow
    if (sensorBufLen + wrote < (int)sizeof(sensorBuf) - 2) {
      memcpy(sensorBuf + sensorBufLen, entry, wrote);
      sensorBufLen += wrote;
    }
  }
  sensorBuf[sensorBufLen++] = '}';
  sensorBuf[sensorBufLen]   = '\0';

  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"hum\":%.1f,\"flow\":%.2f,\"sensors\":%s}",
    tray.temp,
    tray.hum,
    tray.flow,
    sensorBuf
  );

  xSemaphoreGive(trayMutex);
  return String(buf);
}

#endif