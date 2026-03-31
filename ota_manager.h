#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>

// Provided by sensor_manager.h
extern String getSensorJSON();

// ============================================================
// CONFIG
// ============================================================
const char* ota_hostname = "hydroponics-esp32";
const char* AP_SSID      = "ESP32_SETUP_AP";

// Static IP — skips DHCP on every boot, saves 5-15s
IPAddress STATIC_IP (192, 168, 0, 150);
IPAddress GATEWAY   (192, 168, 0,   1);
IPAddress SUBNET    (255, 255, 255,  0);
IPAddress DNS_SRV   (192, 168, 0,   1);

// ============================================================
// SYSTEM OBJECTS
// ============================================================
DNSServer      dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

AsyncClient* telnetClient = nullptr;
AsyncServer  telnetServer(23);

// ============================================================
// STATE
// _apMode        — true while AP is serving the captive portal
// _connecting    — true after /save fires WiFi.begin()
// _staReady      — true once STA connected + services started
// _connFailed    — true if connection timed out / explicitly failed
// _staIP         — IP string set when STA connects
// _connStart     — millis() when WiFi.begin() was called (timeout)
// shouldReboot   — set by WS "reboot" message, checked in loop()
// ============================================================
bool           _apMode     = false;
bool           _connecting = false;
bool           _staReady   = false;
bool           _connFailed = false;
String         _staIP      = "";
unsigned long  _connStart  = 0;
volatile bool  shouldReboot = false;

// ============================================================
// WebSocket event handler
// ============================================================
void onEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
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
// Telnet handler
// ============================================================
void handleTelnetClient(void* arg, AsyncClient* client) {
  if (telnetClient && telnetClient->connected()) {
    const char* busy = "Only one session allowed.\r\n";
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
// WiFi SCAN → JSON
// Returns a JSON array of visible networks.
// Empty array if scan is still in progress.
// ============================================================
String getScanJSON() {
  int n = WiFi.scanComplete();
  if (n < 0) return "[]";   // WIFI_SCAN_RUNNING or not started

  String json = "[";
  bool first = true;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // skip hidden

    // Deduplicate
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == ssid) { dup = true; break; }
    }
    if (dup) continue;

    if (!first) json += ",";
    first = false;

    // Escape SSID for valid JSON
    String esc = "";
    for (int k = 0; k < (int)ssid.length(); k++) {
      char c = ssid[k];
      if (c == '"')       esc += "\\\"";
      else if (c == '\\') esc += "\\\\";
      else                esc += c;
    }

    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    json += "{\"ssid\":\"" + esc + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"open\":" + (open ? "true" : "false") + "}";
  }

  json += "]";
  return json;
}

// ============================================================
// _setupSTAServices
// Called exactly once when STA connects during provisioning.
// Server is already running — just starts OTA, Telnet, mDNS.
// AP is stopped so the phone must reconnect to home WiFi.
// ============================================================
void _setupSTAServices() {
  _staIP      = WiFi.localIP().toString();
  _connecting = false;
  _connFailed = false;
  _staReady   = true;
  _apMode     = false;
  _connStart  = 0;

  Serial.println("[STA] Connected! IP: " + _staIP);

  WiFi.softAPdisconnect(true);
  dnsServer.stop();

  MDNS.begin(ota_hostname);
  ArduinoOTA.setHostname(ota_hostname);
  ArduinoOTA.begin();

  telnetServer.onClient(&handleTelnetClient, NULL);
  telnetServer.begin();

  Serial.println("[STA] All services ready");
}

// ============================================================
// CAPTIVE PORTAL HTML
// ============================================================
// Theme: dark green monospace (matches existing brand)
// UX flow:
//   1. Dropdown populated from /scan (auto-refresh every 10s)
//   2. Password field revealed after SSID selected
//   3. Button glows when ready; dims + shows spinner on tap
//   4. Steps animate: Saving → Connecting → Fetching IP → Ready
//   5. On success: show IP + Open Dashboard + Copy IP buttons
//   6. On failure: show error + Try Again button
// ============================================================
const char _login_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hydroponics Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0f0a;color:#a8d5a2;font-family:monospace;
     display:flex;align-items:center;justify-content:center;
     min-height:100vh;padding:1rem}
.card{border:1px solid #2a4a2a;padding:2rem;width:340px;
      border-radius:4px;background:#0d150d}
h2{color:#5dbb63;font-size:1.1rem;letter-spacing:.15em;
   text-transform:uppercase;margin-bottom:1.5rem}
label{display:block;font-size:.7rem;letter-spacing:.1em;color:#5a7a5a;
      margin-bottom:.35rem;text-transform:uppercase}
.row{display:flex;gap:.5rem;margin-bottom:.2rem}
select,input{background:#111f11;border:1px solid #2a4a2a;color:#a8d5a2;
             padding:.6rem .8rem;font-family:monospace;font-size:.85rem;
             border-radius:2px;outline:none;width:100%;-webkit-appearance:none}
select:focus,input:focus{border-color:#5dbb63}
select option{background:#0d150d}
.snote{font-size:.63rem;color:#3a5a3a;margin-bottom:1rem;
       letter-spacing:.05em;min-height:.85rem}
.rfsh{background:#111f11;border:1px solid #2a4a2a;color:#5dbb63;
      padding:.6rem .75rem;border-radius:2px;cursor:pointer;
      flex-shrink:0;font-size:1.1rem;line-height:1}
.rfsh:active{background:#1a3a1a}
#pw-wrap{display:none;margin-top:.2rem}
/* Button — glowing solid green = call-to-action (enabled) state */
.btn{width:100%;background:#5dbb63;border:1px solid #5dbb63;
     color:#0a0f0a;padding:.75rem;font-family:monospace;
     letter-spacing:.15em;text-transform:uppercase;cursor:pointer;
     font-size:.85rem;border-radius:2px;margin-top:.9rem;
     box-shadow:0 0 14px rgba(93,187,99,.45);transition:all .25s;
     display:flex;align-items:center;justify-content:center;gap:.5rem}
/* Disabled = dim outline (before selection OR during processing) */
.btn:disabled{background:#0d150d;border-color:#2a4a2a;
              color:#3a5a3a;box-shadow:none;cursor:not-allowed}
@keyframes spin{to{transform:rotate(360deg)}}
.sp{width:13px;height:13px;border:2px solid #2a4a2a;
    border-top-color:#5dbb63;border-radius:50%;
    animation:spin .75s linear infinite;flex-shrink:0;display:none}
.sp.on{display:block}
.err{color:#ff6666;font-size:.72rem;margin-top:.6rem;display:none}
/* Steps */
#steps{display:none;margin-top:1.4rem}
.step{display:flex;align-items:center;gap:.6rem;padding:.35rem 0;
      font-size:.73rem;color:#3a5a3a;letter-spacing:.07em}
.step.active{color:#a8d5a2}
.step.done{color:#5dbb63}
.step.fail{color:#ff6666}
.si{width:15px;text-align:center;flex-shrink:0;font-size:.75rem}
.si .sp{width:12px;height:12px}
/* IP result box */
#result{display:none;margin-top:1rem;border:1px solid #2a4a2a;
        padding:1rem;border-radius:2px}
.iplbl{font-size:.63rem;color:#5a7a5a;letter-spacing:.1em;text-transform:uppercase}
.ipval{font-size:1rem;color:#5dbb63;letter-spacing:.05em;
       margin:.4rem 0;word-break:break-all}
.ipnote{font-size:.63rem;color:#5a7a5a;line-height:1.6;margin-bottom:.75rem}
.ipbtns{display:flex;gap:.5rem}
.ipbtn{flex:1;background:#0d150d;border:1px solid #5dbb63;
       color:#5dbb63;padding:.55rem;font-family:monospace;
       font-size:.68rem;letter-spacing:.1em;text-transform:uppercase;
       cursor:pointer;border-radius:2px}
.ipbtn:active{background:#1a3a1a}
.retry{margin-top:.75rem;width:100%;background:#0d150d;
       border:1px solid #2a4a2a;color:#a8d5a2;padding:.55rem;
       font-family:monospace;font-size:.72rem;letter-spacing:.1em;
       text-transform:uppercase;cursor:pointer;border-radius:2px}
</style>
</head>
<body>
<div class="card">
  <h2>WiFi Setup</h2>

  <div id="fw">
    <label>Available Networks</label>
    <div class="row">
      <select id="sel" onchange="onSel()">
        <option value="">Scanning...</option>
      </select>
      <button class="rfsh" onclick="doScan()" title="Refresh">&#8635;</button>
    </div>
    <div class="snote" id="snote">Scanning for networks...</div>

    <div id="pw-wrap">
      <label>Password</label>
      <input type="password" id="pw"
             placeholder="Enter password" autocomplete="off">
    </div>

    <button class="btn" id="cbtn" onclick="onSubmit()" disabled>
      <div class="sp" id="bsp"></div>
      <span id="btxt">SELECT A NETWORK</span>
    </button>
    <div class="err" id="ferr"></div>
  </div>

  <div id="steps">
    <div class="step" id="s1">
      <span class="si" id="i1">&#9675;</span>
      <span>Saving credentials</span>
    </div>
    <div class="step" id="s2">
      <span class="si" id="i2">&#9675;</span>
      <span>Connecting to router</span>
    </div>
    <div class="step" id="s3">
      <span class="si" id="i3">&#9675;</span>
      <span>Fetching IP address</span>
    </div>
    <div class="step" id="s4">
      <span class="si" id="i4">&#9675;</span>
      <span>Setting up dashboard</span>
    </div>
    <div class="err" id="cerr"></div>

    <div id="result">
      <div class="iplbl">Dashboard Address</div>
      <div class="ipval" id="ipval"></div>
      <div class="ipnote">
        Disconnect from ESP32_SETUP_AP,<br>
        reconnect to your home WiFi,<br>
        then tap Open Dashboard.
      </div>
      <div class="ipbtns">
        <button class="ipbtn" id="opbtn" onclick="openDash()">
          Open Dashboard
        </button>
        <button class="ipbtn" id="cpbtn" onclick="copyIP()">
          Copy IP
        </button>
      </div>
    </div>

    <button class="retry" id="rtbtn"
            style="display:none"
            onclick="location.reload()">
      &#8635; Try Again
    </button>
  </div>
</div>

<script>
var nets=[], scanTmr;

function bars(r){
  if(r>=-50) return'\u2588\u2588\u2588\u2588';
  if(r>=-65) return'\u2588\u2588\u2588\u2591';
  if(r>=-75) return'\u2588\u2588\u2591\u2591';
  return '\u2588\u2591\u2591\u2591';
}
function enc(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')
          .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function fetchScan(){
  fetch('/scan').then(function(r){return r.json();})
  .then(function(data){
    nets=data;
    var sel=document.getElementById('sel');
    var prev=sel.value;
    var html='<option value="">-- Select network --</option>';
    data.forEach(function(n){
      html+='<option value="'+enc(n.ssid)+'" data-open="'+n.open+'">'
           +bars(n.rssi)+'  '+enc(n.ssid)
           +(n.open?'  [OPEN]':'  \uD83D\uDD12')+'</option>';
    });
    sel.innerHTML=html;
    if(prev && data.find(function(n){return n.ssid===prev;}))
      sel.value=prev;
    document.getElementById('snote').textContent=
      data.length
        ? data.length+' network'+(data.length!==1?'s':'')+' found'
        : 'No networks found — tap ↻ to rescan';
  }).catch(function(){
    document.getElementById('snote').textContent=
      'Scan failed — retrying...';
  });
}

function doScan(){
  document.getElementById('snote').textContent='Scanning...';
  document.getElementById('sel').innerHTML=
    '<option value="">Scanning...</option>';
  fetchScan();
}

function onSel(){
  var sel=document.getElementById('sel');
  var opt=sel.options[sel.selectedIndex];
  var pww=document.getElementById('pw-wrap');
  var btn=document.getElementById('cbtn');
  var btxt=document.getElementById('btxt');
  if(!sel.value){
    pww.style.display='none';
    btn.disabled=true;
    btxt.textContent='SELECT A NETWORK';
    return;
  }
  pww.style.display='block';
  btn.disabled=false;
  btxt.textContent='CONNECT & SAVE';
  document.getElementById('pw').placeholder=
    opt.getAttribute('data-open')==='true'
      ? 'Leave blank (open network)'
      : 'Enter password';
}

function step(n,state){
  var s=document.getElementById('s'+n);
  var ic=document.getElementById('i'+n);
  s.className='step '+state;
  if(state==='active') ic.innerHTML='<div class="sp on"></div>';
  else if(state==='done') ic.textContent='\u2713';
  else if(state==='fail') ic.textContent='\u2717';
}

function showErr(id,msg){
  var e=document.getElementById(id);
  e.textContent=msg; e.style.display='block';
}

function onSubmit(){
  var ssid=document.getElementById('sel').value;
  var pass=document.getElementById('pw').value;
  if(!ssid) return;

  clearInterval(scanTmr);

  var btn=document.getElementById('cbtn');
  btn.disabled=true;
  document.getElementById('btxt').textContent='SAVING...';
  document.getElementById('bsp').className='sp on';
  document.getElementById('ferr').style.display='none';
  document.getElementById('steps').style.display='block';
  step(1,'active');

  fetch('/save',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'s='+encodeURIComponent(ssid)+'&p='+encodeURIComponent(pass)
  })
  .then(function(r){return r.json();})
  .then(function(d){
    if(d.status==='connecting'){
      step(1,'done');
      step(2,'active');
      document.getElementById('btxt').textContent='CONNECTING...';
      poll();
    } else {
      step(1,'fail');
      showErr('ferr','Error: '+(d.msg||d.status));
      btn.disabled=false;
      document.getElementById('btxt').textContent='CONNECT & SAVE';
      document.getElementById('bsp').className='sp';
    }
  })
  .catch(function(){
    showErr('ferr','Request failed — check connection.');
    btn.disabled=false;
    document.getElementById('btxt').textContent='CONNECT & SAVE';
    document.getElementById('bsp').className='sp';
  });
}

function poll(){
  setTimeout(function(){
    fetch('/status')
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.status==='connected'){
        step(2,'done'); step(3,'done'); step(4,'active');
        document.getElementById('btxt').textContent='READY';
        setTimeout(function(){
          step(4,'done');
          document.getElementById('ipval').textContent='http://'+d.ip;
          document.getElementById('opbtn').setAttribute('data-ip',d.ip);
          document.getElementById('result').style.display='block';
          document.getElementById('bsp').className='sp';
        },800);
      } else if(d.status==='failed'){
        step(2,'fail');
        showErr('cerr','Connection failed. Wrong password?');
        document.getElementById('rtbtn').style.display='block';
        document.getElementById('bsp').className='sp';
      } else {
        poll();
      }
    })
    .catch(function(){ poll(); });
  },2000);
}

function openDash(){
  window.open('http://'+document.getElementById('opbtn')
    .getAttribute('data-ip'),'_blank');
}

function copyIP(){
  var txt='http://'+document.getElementById('opbtn')
    .getAttribute('data-ip');
  if(navigator.clipboard){ navigator.clipboard.writeText(txt); }
  else {
    var t=document.createElement('textarea');
    t.value=txt; document.body.appendChild(t);
    t.select(); document.execCommand('copy');
    document.body.removeChild(t);
  }
  var b=document.getElementById('cpbtn');
  b.textContent='COPIED!';
  setTimeout(function(){b.textContent='COPY IP';},2000);
}

fetchScan();
scanTmr=setInterval(fetchScan,10000);
</script>
</body></html>
)rawliteral";

// ============================================================
// DASHBOARD HTML
// Existing dark teal theme.
// Sensor health section added — shows live ONLINE/OFFLINE
// status for each registered sensor using the `sensors`
// object from getSensorJSON().
// Placeholders (--) are replaced by WebSocket data on connect.
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
    /* Sensor health strip */
    #sens {
      margin-top: 16px;
      padding-top: 12px;
      border-top: 1px solid #1a3a3a;
      font-size: 12px;
      letter-spacing: .08em;
      min-height: 20px;
    }
    .s-alive { color: #00ffcc; }
    .s-dead  { color: #ff4444; }
  </style>
</head>
<body>
  <div class="box">
    <h1>Hydroponics v1</h1>

    <div class="data">
      <span class="label">TEMPERATURE</span>
      <span id="t">--</span>&deg;C
    </div>
    <div class="data">
      <span class="label">HUMIDITY</span>
      <span id="h">--</span>%
    </div>
    <div class="data">
      <span class="label">WATER FLOW</span>
      <span id="f">--</span> L/min
    </div>

    <!-- Sensor health — populated live from WebSocket -->
    <div id="sens"></div>

    <button onclick="reboot()">RESTART DEVICE</button>
    <div id="stat">Connecting...</div>
  </div>

  <script>
    var gateway = 'ws://' + window.location.hostname + '/ws';
    var ws;

    function init() {
      ws = new WebSocket(gateway);

      ws.onopen = function() {
        document.getElementById('stat').innerHTML = 'LIVE';
      };

      ws.onmessage = function(e) {
        var d = JSON.parse(e.data);

        // Sensor readings
        document.getElementById('t').innerHTML =
          (d.temp !== undefined) ? d.temp.toFixed(1) : '--';
        document.getElementById('h').innerHTML =
          (d.hum  !== undefined) ? d.hum.toFixed(1)  : '--';
        document.getElementById('f').innerHTML =
          (d.flow !== undefined) ? d.flow.toFixed(2) : '--';

        // Sensor health — build from dynamic sensors object
        if (d.sensors) {
          var parts = [];
          Object.keys(d.sensors).forEach(function(k) {
            var alive = d.sensors[k];
            parts.push(
              '<span class="' + (alive ? 's-alive' : 's-dead') + '">'
              + k.toUpperCase()
              + (alive ? ' &#9679;' : ' &#9675;')
              + '</span>'
            );
          });
          document.getElementById('sens').innerHTML = parts.join('&nbsp;&nbsp;');
        }
      };

      ws.onclose = function() {
        document.getElementById('stat').innerHTML = 'RECONNECTING...';
        setTimeout(init, 2000);
      };

      ws.onerror = function() { ws.close(); };
    }

    function reboot() {
      if (confirm('Restart device?')) ws.send('reboot');
    }

    window.onload = init;
  </script>
</body>
</html>
)rawliteral";

// ============================================================
// _startProvisioning
// Starts WIFI_AP_STA so STA scan works while AP is serving
// the captive portal. No reboot — WiFi.begin() is called
// from the /save handler and monitored in handleOTA().
// ============================================================
void _startProvisioning() {
  _apMode = true;

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);

  Serial.println("[AP] Started: " + apIP.toString());

  // Trigger first async scan immediately
  WiFi.scanNetworks(true);

  // ── Serve captive portal (switches to dashboard when _staReady) ──
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (_staReady)
      req->send_P(200, "text/html", _dash_html);
    else
      req->send_P(200, "text/html", _login_html);
  });

  // ── WiFi scan results ──
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = getScanJSON();
    // Trigger fresh scan after delivering current results
    if (WiFi.scanComplete() >= 0) WiFi.scanNetworks(true);
    req->send(200, "application/json", json);
  });

  // ── Save credentials & start connection (no reboot) ──
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (_connecting || _staReady) {
      req->send(200, "application/json", "{\"status\":\"busy\"}");
      return;
    }

    String ssid = req->arg("s");
    String pass = req->arg("p");

    if (ssid.length() == 0) {
      req->send(400, "application/json",
        "{\"status\":\"error\",\"msg\":\"SSID required\"}");
      return;
    }

    // Persist to NVS
    Preferences p;
    p.begin("wifi", false);
    p.putString("s", ssid);
    p.putString("p", pass);
    p.end();

    // Begin STA connection — AP stays alive
    _connecting  = true;
    _connFailed  = false;
    _connStart   = millis();

    WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SRV);
    WiFi.begin(ssid.c_str(), pass.c_str());

    Serial.println("[WiFi] Connecting to: " + ssid);

    req->send(200, "application/json", "{\"status\":\"connecting\"}");
  });

  // ── Connection status (polled by captive portal JS) ──
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json;
    if (_staReady) {
      json = "{\"status\":\"connected\",\"ip\":\"" + _staIP + "\"}";
    } else if (_connFailed) {
      json = "{\"status\":\"failed\"}";
    } else if (_connecting) {
      json = "{\"status\":\"connecting\"}";
    } else {
      json = "{\"status\":\"idle\"}";
    }
    req->send(200, "application/json", json);
  });

  // ── Sensor data (available once STA connects) ──
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!_staReady) {
      req->send(503, "application/json", "{\"error\":\"not connected\"}");
      return;
    }
    req->send(200, "application/json", getSensorJSON());
  });

  // ── WebSocket (active once STA connects and clients use STA IP) ──
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  // ── Captive portal redirect for all unknown URLs ──
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (!_staReady)
      req->redirect("http://192.168.4.1/");
    else
      req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[AP] Server started");
}

// ============================================================
// setupWiFiAndOTA
// Called once from setup().
// If credentials exist → direct STA (fast path, no AP).
// If no credentials → provisioning mode (AP+STA).
// ============================================================
void setupWiFiAndOTA() {
  Preferences prefs;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("s", "");
  String pass = prefs.getString("p", "");
  prefs.end();

  if (ssid == "") {
    // No credentials — start captive portal
    _startProvisioning();
    return;
  }

  // ── Direct STA connection (credentials already saved) ──
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);

  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_SRV)) {
    Serial.println("[WiFi] Static IP failed — falling back to DHCP");
  }

  WiFi.begin(ssid.c_str(), pass.c_str());

  Serial.print("[WiFi] Connecting");
  int c = 0;
  while (WiFi.status() != WL_CONNECTED && c < 20) {
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
  _apMode   = false;
  _staReady = true;
  _staIP    = WiFi.localIP().toString();

  MDNS.begin(ota_hostname);

  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", _dash_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", getSensorJSON());
  });

  telnetServer.onClient(&handleTelnetClient, NULL);
  telnetServer.begin();

  ArduinoOTA.setHostname(ota_hostname);
  ArduinoOTA.begin();

  server.begin();
  Serial.println("[HTTP] Server started at " + _staIP);
}

// ============================================================
// pushSensorUpdate — called by pushTask on tray change
// ============================================================
void pushSensorUpdate() {
  if (ws.count() > 0) {
    ws.textAll(getSensorJSON());
  }
}

// ============================================================
// handleOTA — called every loop()
// ============================================================
void handleOTA() {
  if (_apMode) {
    dnsServer.processNextRequest();

    // ── Monitor ongoing connection attempt ──
    if (_connecting) {
      wl_status_t wst = WiFi.status();

      if (wst == WL_CONNECTED) {
        _setupSTAServices();

      } else if (wst == WL_CONNECT_FAILED ||
                 wst == WL_NO_SSID_AVAIL) {
        // Explicit failure — no need to wait for timeout
        _connecting  = false;
        _connFailed  = true;
        _connStart   = 0;
        Serial.println("[WiFi] Connection failed (explicit)");

      } else if (_connStart > 0 && millis() - _connStart > 30000) {
        // Timeout — wrong password typically lands here
        _connecting  = false;
        _connFailed  = true;
        _connStart   = 0;
        Serial.println("[WiFi] Connection timed out");
      }
    }

    // ── Auto-rescan every 10s when idle (not connecting) ──
    static unsigned long lastScan = 0;
    if (!_connecting && !_staReady &&
        millis() - lastScan > 10000) {
      if (WiFi.scanComplete() >= 0) {  // don't interrupt in-progress scan
        WiFi.scanNetworks(true);
        lastScan = millis();
      }
    }

  } else {
    // ── STA mode ──
    ArduinoOTA.handle();

    // WiFi reconnect with 30s backoff
    static unsigned long lastReconnect = 0;
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastReconnect > 30000) {
        Serial.println("[WiFi] Lost connection, reconnecting...");
        WiFi.reconnect();
        lastReconnect = millis();
      }
    }

    // WebSocket cleanup every 5s
    static unsigned long lastClean = 0;
    if (millis() - lastClean > 5000) {
      ws.cleanupClients();
      lastClean = millis();
    }

    // WebSocket keepalive ping every 10s
    static unsigned long lastPing = 0;
    if (millis() - lastPing > 10000) {
      ws.pingAll();
      lastPing = millis();
    }

    // Fallback push every 3s (catches missed notifications)
    static unsigned long lastPush = 0;
    if (millis() - lastPush > 3000) {
      if (ws.count() > 0) ws.textAll(getSensorJSON());
      lastPush = millis();
    }
  }
}

bool isAPMode() { return _apMode; }

#endif // OTA_MANAGER_H