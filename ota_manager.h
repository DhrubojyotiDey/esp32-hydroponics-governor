#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>

// --- Supplied by sensor_manager ---
extern String getSensorJSON();

// --- Config ---
const char* ota_hostname = "hydroponics-esp32";
const char* AP_SSID      = "ESP32_SETUP_AP";

// --- System Objects ---
DNSServer        dnsServer;
AsyncWebServer   server(80);
AsyncWebSocket   ws("/ws");

AsyncClient*     telnetClient = nullptr;
AsyncServer      telnetServer(23);

bool            _apMode     = false;
volatile bool   shouldReboot = false;

// ============================================================
//  WebSocket event handler
// ============================================================
void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
             AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Client #%u connected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    if (len >= 6 && strncmp((char*)data, "reboot", 6) == 0) {
      shouldReboot = true;
    }
  }
}

// ============================================================
//  Telnet
// ============================================================
void handleTelnetClient(void* arg, AsyncClient* client) {
  if (telnetClient && telnetClient->connected()) {
    const char* busy = "Only one Telnet session allowed.\r\n";
    client->add(busy, strlen(busy));
    client->send();
    client->close();
    return;
  }

  telnetClient = client;

  const char* welcome = "=== ESP32 Hydroponics Log ===\r\n";
  client->add(welcome, strlen(welcome));
  client->send();

  client->onDisconnect([](void* arg, AsyncClient* c) {
    telnetClient = nullptr;
  });
}

// ============================================================
//  Captive portal HTML (AP provisioning mode)
// ============================================================
const char _login_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Hydroponics Setup</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{background:#0a0f0a;color:#a8d5a2;font-family:monospace;
         display:flex;align-items:center;justify-content:center;min-height:100vh}
    .card{border:1px solid #2a4a2a;padding:2rem;width:320px;border-radius:4px;
          background:#0d150d}
    h2{color:#5dbb63;font-size:1.1rem;letter-spacing:.15em;
       text-transform:uppercase;margin-bottom:1.5rem}
    label{display:block;font-size:.7rem;letter-spacing:.1em;
          color:#5a7a5a;margin-bottom:.3rem;text-transform:uppercase}
    input{display:block;width:100%;background:#111f11;border:1px solid #2a4a2a;
          color:#a8d5a2;padding:.6rem .8rem;margin-bottom:1rem;
          font-family:monospace;font-size:.9rem;border-radius:2px;outline:none}
    input:focus{border-color:#5dbb63}
    button{width:100%;background:#1a3a1a;border:1px solid #5dbb63;
           color:#5dbb63;padding:.7rem;font-family:monospace;
           letter-spacing:.15em;text-transform:uppercase;cursor:pointer;
           font-size:.8rem;border-radius:2px;margin-top:.5rem}
    button:hover{background:#5dbb63;color:#0a0f0a}
  </style>
</head>
<body>
  <div class="card">
    <h2>WiFi Setup</h2>
    <form action="/save" method="POST">
      <label>Network SSID</label>
      <input name="s" placeholder="Enter SSID" required>
      <label>Password</label>
      <input name="p" type="password" placeholder="Enter password" required>
      <button type="submit">Connect &amp; Save</button>
    </form>
  </div>
</body></html>
)rawliteral";

// ============================================================
//  Dashboard HTML (STA mode) — GitHub WebSocket version
//  Connects to ws://<hostname>/ws, parses {temp, hum, flow}.
// ============================================================
const char _dash_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial;
      text-align: center;
      background: #111;
      color: #00ffcc;
      padding: 30px;
    }
    .box {
      border: 2px solid #00ffcc;
      padding: 20px;
      border-radius: 10px;
      min-width: 280px;
      display: inline-block;
    }
    .data {
      font-size: 26px;
      margin: 15px 0;
    }
    .label {
      color: #888;
      font-size: 14px;
      display: block;
    }
    button {
      margin-top: 20px;
      padding: 10px 20px;
      background: #ff4444;
      color: white;
      border: none;
      border-radius: 5px;
      cursor: pointer;
    }
    #stat {
      font-size: 10px;
      color: #444;
      margin-top: 10px;
    }
  </style>
</head>

<body>
  <div class="box">
    <h1>Hydroponics v1</h1>

    <div class="data">
      <span class="label">TEMPERATURE</span>
      <span id="t">--</span>°C
    </div>

    <div class="data">
      <span class="label">HUMIDITY</span>
      <span id="h">--</span>%
    </div>

    <div class="data">
      <span class="label">WATER FLOW</span>
      <span id="f">--</span> L/min
    </div>

    <button onclick="reboot()">RESTART DEVICE</button>
    <div id="stat">Connecting...</div>
  </div>

  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var ws;

    function init() {
      ws = new WebSocket(gateway);

      ws.onopen = () => {
        document.getElementById('stat').innerHTML = 'LIVE';
      };

      ws.onmessage = (e) => {
        var d = JSON.parse(e.data);
        document.getElementById('t').innerHTML = d.temp;
        document.getElementById('h').innerHTML = d.hum;
        document.getElementById('f').innerHTML = d.flow;
      };

      ws.onclose = () => {
        document.getElementById('stat').innerHTML = 'RECONNECTING...';
        setTimeout(init, 2000);
      };

      ws.onerror = () => {
        ws.close();
      };
    }

    function reboot() {
      if (confirm("Restart?")) ws.send("reboot");
    }

    window.onload = init;
  </script>
</body>
</html>
)rawliteral";
// ============================================================
//  AP provisioning mode
// ============================================================
void _startProvisioning() {
  _apMode = true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);

  IPAddress IP = WiFi.softAPIP();
  dnsServer.start(53, "*", IP);

  Serial.println("[AP] Mode started");
  Serial.println("[AP] SSID: " + String(AP_SSID));
  Serial.println("[AP] URL : http://192.168.4.1");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", _login_html);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    Preferences p;
    p.begin("wifi", false);
    p.putString("s", req->arg("s"));
    p.putString("p", req->arg("p"));
    p.end();

    req->send(200, "text/html",
      "<div style='font-family:monospace;padding:2rem;background:#070d07;color:#39ff6e'>"
      "<h2>Credentials saved.</h2><p>Rebooting...</p></div>");

    delay(1500);
    ESP.restart();
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("/");
  });

  server.begin();
}

// ============================================================
//  STA mode: WiFi + OTA + WebSocket + Dashboard + Telnet
// ============================================================
void setupWiFiAndOTA() {
  Preferences prefs;
  prefs.begin("wifi", true);  // read-only open
  String ssid = prefs.getString("s", "");
  String pass = prefs.getString("p", "");
  prefs.end();

  if (ssid == "") {
    _startProvisioning();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("[WiFi] Connecting");
  int c = 0;
  while (WiFi.status() != WL_CONNECTED && c < 40) {
    delay(500);
    Serial.print(".");
    c++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] Failed — clearing credentials, rebooting");
    Preferences p;
    p.begin("wifi", false);
    p.clear();
    p.end();
    ESP.restart();
  }

  Serial.println("\n[WiFi] Connected!");
  Serial.println("[WiFi] IP: " + WiFi.localIP().toString());

  WiFi.softAPdisconnect(true);
  _apMode = false;

  // mDNS
  MDNS.begin(ota_hostname);

  // WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  // Dashboard at /
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", _dash_html);
  });

  // JSON snapshot at /data (for Pi polling)
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", getSensorJSON());
  });

  // Telnet
  telnetServer.onClient(&handleTelnetClient, NULL);
  telnetServer.begin();

  // OTA
  ArduinoOTA.setHostname(ota_hostname);
  ArduinoOTA.begin();

  server.begin();
  Serial.println("[HTTP] Server started");
}

// ============================================================
//  WebSocket push — called only when tray is dirty
// ============================================================
void pushSensorUpdate() {
  if (ws.count() > 0) {
    ws.textAll(getSensorJSON());
  }
}

// ============================================================
//  Loop handler — call every loop()
// ============================================================
void handleOTA() {
  if (_apMode) {
    dnsServer.processNextRequest();
  } else {
    ArduinoOTA.handle();

    // --- WiFi auto-reconnect (with backoff) ---
    // Without a timer this fires every loop() call (~every 10ms),
    // which actively disrupts the reconnection process.
    static unsigned long lastReconnect = 0;
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastReconnect > 30000) {
        Serial.println("[WiFi] Lost connection, reconnecting...");
        WiFi.reconnect();
        lastReconnect = millis();
      }
    }

    // --- Cleanup ---
    static unsigned long lastClean = 0;
    if (millis() - lastClean > 5000) {
      ws.cleanupClients();
      lastClean = millis();
    }

    // --- Heartbeat ---
    static unsigned long lastPing = 0;
    if (millis() - lastPing > 10000) {
      ws.pingAll();
      lastPing = millis();
    }

    // --- Fallback push ---
    static unsigned long lastPush = 0;
    if (millis() - lastPush > 3000) {
      if (ws.count() > 0) {
        ws.textAll(getSensorJSON());
      }
      lastPush = millis();
    }
  }
}

bool isAPMode() { return _apMode; }

#endif // OTA_MANAGER_H
