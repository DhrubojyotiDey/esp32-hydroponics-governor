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
const char* ota_hostname = "hydroponics";
const char* AP_SSID      = "Hydroponics_Setup";

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
// _apShutdownTime— millis() target to kill the AP (0 = not scheduled)
// shouldReboot   — set by WS "reboot" message, checked in loop()
// ============================================================
bool           _apMode        = false;
bool           _connecting    = false;
bool           _staReady      = false;
bool           _connFailed    = false;
String         _staIP         = "";
unsigned long  _connStart     = 0;
unsigned long  _apShutdownTime = 0;   // set when home WiFi user visits dashboard
unsigned long  _dashReadyTime  = 0;   // set 4s after STA connects (sensors need warmup)
volatile bool  shouldReboot   = false;

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
// AP is kept alive for 60s so the captive page can show the
// IP address and countdown before Android closes the WebView.
// ============================================================
void _setupSTAServices() {
  _staIP          = WiFi.localIP().toString();
  _connecting     = false;
  _connFailed     = false;
  _staReady       = true;
  _connStart      = 0;
  _apShutdownTime = 0;        // NOT set here — AP stays until user visits dashboard
  _dashReadyTime  = millis() + 4000;  // 4s warmup for sensors to produce first data

  Serial.println("[STA] Connected! IP: " + _staIP);
  Serial.println("[STA] AP stays live until user visits dashboard");

  MDNS.begin(ota_hostname);
  ArduinoOTA.setHostname(ota_hostname);
  ArduinoOTA.begin();

  telnetServer.onClient(&handleTelnetClient, NULL);
  telnetServer.begin();

  Serial.println("[STA] All services ready");
}

// ============================================================
// CAPTIVE PORTAL HTML
// Custom inline dropdown (no Android system picker)
// Single scan on load — manual rescan only via ↻ button
// 6-stage button: Connecting → Obtaining IP → Provisioning
//                 → Preparing UI → UI Ready → Setup Complete
// AP stays alive until user visits dashboard from home WiFi
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
   text-transform:uppercase;margin-bottom:1.2rem}
label{display:block;font-size:.7rem;letter-spacing:.1em;color:#5a7a5a;
      margin-bottom:.35rem;text-transform:uppercase}
.fallback{font-size:.61rem;color:#2a4a2a;letter-spacing:.05em;
          line-height:1.5;margin-bottom:1.1rem;padding:.45rem .6rem;
          border:1px solid #1a3a1a;border-radius:2px}
.fallback strong{color:#3a6a3a}
.net-row{display:flex;gap:.5rem;margin-bottom:.2rem}
.net-box{position:relative;flex:1;min-width:0}
.net-sel{background:#111f11;border:1px solid #2a4a2a;color:#a8d5a2;
         padding:.6rem .8rem;font-family:monospace;font-size:.82rem;
         border-radius:2px;cursor:pointer;
         display:flex;justify-content:space-between;align-items:center;
         user-select:none;-webkit-user-select:none;
         white-space:nowrap;overflow:hidden}
.net-sel.open{border-color:#5dbb63;border-bottom-color:#0d150d;
              border-radius:2px 2px 0 0;z-index:101;position:relative}
.net-arrow{font-size:.65rem;color:#5a7a5a;margin-left:.4rem;
           flex-shrink:0;transition:transform .2s}
.net-sel.open .net-arrow{transform:rotate(180deg)}
.net-sel-txt{overflow:hidden;text-overflow:ellipsis;flex:1;min-width:0}
.net-list{display:none;position:absolute;top:100%;left:0;right:0;
          z-index:100;background:#0d150d;
          border:1px solid #5dbb63;border-top:none;
          border-radius:0 0 2px 2px;
          max-height:190px;overflow-y:auto;
          -webkit-overflow-scrolling:touch}
.net-item{padding:.55rem .8rem;font-family:monospace;font-size:.8rem;
          color:#a8d5a2;cursor:pointer;
          border-bottom:1px solid #1a2a1a;
          white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.net-item:last-child{border-bottom:none}
.net-item:active{background:#1a3a1a;color:#5dbb63}
.net-empty{padding:.55rem .8rem;font-family:monospace;font-size:.74rem;
           color:#3a5a3a;font-style:italic}
.pbar-wrap{margin:.35rem 0 .85rem;width:97%;height:4px;
           background:#1a2a1a;border-radius:2px;overflow:hidden}
.pbar-fill{height:100%;width:0%;background:#5dbb63;
           border-radius:2px;transition:width .3s ease}
@keyframes pbar-scan{0%{width:0%;opacity:1}80%{width:88%;opacity:1}100%{width:88%;opacity:.5}}
.pbar-fill.scanning{animation:pbar-scan 2.5s ease-out forwards}
.pbar-fill.done{width:100%;transition:width .2s ease}
.rfsh{background:#111f11;border:1px solid #2a4a2a;color:#5dbb63;
      padding:.6rem .75rem;border-radius:2px;cursor:pointer;
      flex-shrink:0;font-size:1.05rem;line-height:1;
      transition:background .15s;
      display:flex;align-items:center;justify-content:center;overflow:hidden}
.rfsh:active{background:#1a3a1a}
@keyframes rfsh-spin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
#rfsh-icon{display:inline-block;transform-origin:center center;line-height:1}
#rfsh-icon.spinning{animation:rfsh-spin .7s ease-out}
#pw-wrap{display:none;margin-top:.6rem}
.pw-row{display:flex;gap:.5rem;margin-bottom:0}
input{background:#111f11;border:1px solid #5dbb63;
      color:#a8d5a2 !important;-webkit-text-fill-color:#a8d5a2 !important;
      caret-color:#5dbb63;
      padding:.6rem .8rem;font-family:monospace;font-size:.85rem;
      border-radius:2px;outline:none;width:100%;
      -webkit-appearance:none;appearance:none}
input::placeholder{color:#3a5a3a;-webkit-text-fill-color:#3a5a3a}
input:focus{border-color:#5dbb63}
.pw-err{font-size:.65rem;color:#ff6666;min-height:.85rem;
        margin-top:.25rem;letter-spacing:.05em}
.btn{width:100%;background:#5dbb63;border:1px solid #5dbb63;
     color:#0a0f0a;padding:.8rem;font-family:monospace;
     letter-spacing:.1em;text-transform:uppercase;cursor:pointer;
     font-size:.82rem;border-radius:2px;margin-top:1rem;
     box-shadow:0 0 16px rgba(93,187,99,.5);transition:all .25s;
     display:flex;align-items:center;justify-content:center;gap:.6rem}
.btn:disabled{background:#0d150d;border-color:#2a4a2a;
              color:#3a5a3a;box-shadow:none;cursor:not-allowed}
.btn.complete{flex-direction:column;gap:.4rem;padding:1rem;
              letter-spacing:.06em;text-transform:none;font-size:.78rem;line-height:1.4}
.btn-ip{color:#5dbb63;font-size:.9rem;word-break:break-all;margin:.2rem 0}
@keyframes spin{to{transform:rotate(360deg)}}
.sp{width:13px;height:13px;border:2px solid #2a4a2a;
    border-top-color:#5dbb63;border-radius:50%;
    animation:spin .75s linear infinite;flex-shrink:0;display:none}
.sp.on{display:inline-block}
#instbox{display:none;margin-top:.75rem;padding:.75rem .8rem;
         border:1px solid #2a4a2a;border-radius:2px;
         font-size:.68rem;color:#5a7a5a;line-height:1.7;
         letter-spacing:.05em}
#instbox strong{color:#5dbb63}
</style>
</head>
<body>
<div class="card">
  <h2>WiFi Setup</h2>
  <div class="fallback">
    If this page closes, open <strong>http://192.168.4.1</strong> in your browser
  </div>

  <label>Available Networks</label>
  <div class="net-row">
    <div class="net-box">
      <div class="net-sel" id="net-sel" onclick="toggleDropdown()">
        <span class="net-sel-txt" id="net-sel-txt">Tap &#8635; to scan</span>
        <span class="net-arrow">&#9660;</span>
      </div>
      <div class="net-list" id="net-list"></div>
    </div>
    <button class="rfsh" id="rfsh" onclick="doScan()" title="Scan">
      <span id="rfsh-icon">&#8635;</span>
    </button>
  </div>
  <div class="pbar-wrap"><div class="pbar-fill" id="pbar"></div></div>

  <div id="pw-wrap">
    <label>Password</label>
    <div class="pw-row">
      <input type="password" id="pw" placeholder="Enter password" autocomplete="off">
      <button class="rfsh" onclick="togglePw()" title="Show/hide">&#128065;</button>
    </div>
    <div class="pw-err" id="pw-err"></div>
  </div>

  <button class="btn" id="cbtn" onclick="onSubmit()" disabled>
    <div class="sp" id="bsp"></div>
    <span id="btxt">SELECT A NETWORK FIRST</span>
  </button>

  <div id="instbox"></div>
</div>

<script>
var scanStatusTmr,pollTmr,pollCount=0,_ip='',_local='';
var _selSSID='',_selOpen=false,_dropOpen=false;

function bars(r){
  if(r>=-50)return'\u2588\u2588\u2588\u2588';
  if(r>=-65)return'\u2588\u2588\u2588\u2591';
  if(r>=-75)return'\u2588\u2588\u2591\u2591';
  return'\u2588\u2591\u2591\u2591';
}
function enc(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')
          .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
function togglePw(){
  var pw=document.getElementById('pw');
  pw.type=pw.type==='password'?'text':'password';
}
function setBar(state){
  var p=document.getElementById('pbar');
  p.className='pbar-fill';void p.offsetWidth;
  if(state==='scanning')p.classList.add('scanning');
  else if(state==='done')p.classList.add('done');
  else if(state==='error'){
    p.style.background='#ff4444';p.classList.add('done');
    setTimeout(function(){p.className='pbar-fill';p.style.background='';},800);
  }
}
function setBtn(text,spinning,disabled){
  var btn=document.getElementById('cbtn');
  var sp=document.getElementById('bsp');
  var tx=document.getElementById('btxt');
  btn.disabled=!!disabled;
  sp.className=spinning?'sp on':'sp';
  tx.textContent=text;
  if(!disabled)btn.onclick=onSubmit;
}
function spinRfsh(){
  var ic=document.getElementById('rfsh-icon');
  ic.classList.remove('spinning');void ic.offsetWidth;
  ic.classList.add('spinning');
  ic.addEventListener('animationend',function h(){
    ic.classList.remove('spinning');
    ic.removeEventListener('animationend',h);
  });
}
function toggleDropdown(){
  _dropOpen=!_dropOpen;
  document.getElementById('net-list').style.display=_dropOpen?'block':'none';
  document.getElementById('net-sel').classList.toggle('open',_dropOpen);
}
function closeDropdown(){
  _dropOpen=false;
  document.getElementById('net-list').style.display='none';
  document.getElementById('net-sel').classList.remove('open');
}
document.addEventListener('click',function(e){
  var box=document.getElementById('net-sel').parentElement;
  if(_dropOpen&&!box.contains(e.target))closeDropdown();
});
function selectNet(el){
  _selSSID=el.getAttribute('data-ssid');
  _selOpen=el.getAttribute('data-open')==='true';
  document.getElementById('net-sel-txt').textContent=el.textContent.trim();
  closeDropdown();
  afterSelect();
}
function afterSelect(){
  if(!_selSSID){
    document.getElementById('pw-wrap').style.display='none';
    setBtn('SELECT A NETWORK FIRST',false,true);
    return;
  }
  var pw=document.getElementById('pw');
  pw.placeholder=_selOpen?'Blank if no password':'Enter password';
  document.getElementById('pw-wrap').style.display='block';
  pw.focus();
  setBtn('CONNECT',false,false);
}
function doScan(){
  clearInterval(scanStatusTmr);
  spinRfsh();setBar('scanning');closeDropdown();
  document.getElementById('net-list').innerHTML='';
  document.getElementById('net-sel-txt').textContent='Scanning...';
  _selSSID='';_selOpen=false;
  document.getElementById('pw-wrap').style.display='none';
  setBtn('SELECT A NETWORK FIRST',false,true);
  fetch('/scan').catch(function(){});
  scanStatusTmr=setInterval(function(){
    fetch('/scanstatus').then(function(r){return r.json();})
    .then(function(d){
      if(!d.scanning){clearInterval(scanStatusTmr);loadScan();}
    }).catch(function(){});
  },500);
}
function loadScan(){
  fetch('/scan').then(function(r){return r.json();})
  .then(function(data){
    data.sort(function(a,b){return b.rssi-a.rssi;});
    var list=document.getElementById('net-list');
    if(data.length===0){
      list.innerHTML='<div class="net-empty">No networks found \u2014 tap \u21BB to rescan</div>';
      document.getElementById('net-sel-txt').textContent='No networks found';
      setBar('error');
    } else {
      var html='';
      data.forEach(function(n){
        var lbl=bars(n.rssi)+'  '+enc(n.ssid)+(n.open?'  [OPEN]':'  \uD83D\uDD12');
        html+='<div class="net-item" data-ssid="'+enc(n.ssid)+'" data-open="'+n.open+'" onclick="selectNet(this)">'+lbl+'</div>';
      });
      list.innerHTML=html;
      document.getElementById('net-sel-txt').textContent='-- Select network --';
      setBar('done');
    }
  }).catch(function(){
    document.getElementById('net-sel-txt').textContent='Scan failed \u2014 tap \u21BB';
    setBar('error');
  });
}
function onSubmit(){
  if(!_selSSID)return;
  var pass=document.getElementById('pw').value;
  var errEl=document.getElementById('pw-err');
  errEl.textContent='';
  if(!_selOpen&&pass.length===0){errEl.textContent='Password required';document.getElementById('pw').focus();return;}
  if(!_selOpen&&pass.length<8){errEl.textContent='Password must be at least 8 characters';document.getElementById('pw').focus();return;}
  clearInterval(scanStatusTmr);
  pollCount=0;
  setBtn('Connecting to your WiFi\u2026',true,true);
  fetch('/save',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'s='+encodeURIComponent(_selSSID)+'&p='+encodeURIComponent(pass)
  })
  .then(function(r){return r.json();})
  .then(function(d){if(d.status==='connecting')poll();else setBtn('CONNECT',false,false);})
  .catch(function(){setBtn('CONNECT',false,false);});
}
function poll(){
  pollTmr=setTimeout(function(){
    fetch('/status').then(function(r){return r.json();})
    .then(function(d){
      pollCount++;
      if(d.status==='connected'){
        _ip=d.ip;
        setBtn('Obtaining IP address\u2026',true,true);
        setTimeout(function(){
          setBtn('Provisioning system\u2026',true,true);
          setTimeout(function(){
            setBtn('Preparing UI\u2026',true,true);
            pollDashReady();
          },1500);
        },1000);
      } else if(d.status==='failed'){
        setBtn('CONNECT',false,false);
        document.getElementById('pw-err').textContent='Wrong password or network unreachable';
        document.getElementById('pw').focus();
      } else {
        if(pollCount>=4)setBtn('Obtaining IP address\u2026',true,true);
        poll();
      }
    }).catch(function(){poll();});
  },2000);
}
function pollDashReady(){
  fetch('/dashready').then(function(r){return r.json();})
  .then(function(d){
    if(d.ready){
      _ip=d.ip||_ip;
      _local=d.local||('http://hydroponics.local');
      setBtn('UI Ready \u2714',false,true);
      setTimeout(function(){showComplete(_ip,_local);},800);
    } else {
      setTimeout(pollDashReady,1000);
    }
  }).catch(function(){setTimeout(pollDashReady,1000);});
}
function showComplete(ip,local){
  var btn=document.getElementById('cbtn');
  var sp=document.getElementById('bsp');
  var tx=document.getElementById('btxt');
  sp.className='sp';
  btn.disabled=false;
  btn.classList.add('complete');
  // Button copies the .local address — easier to type than an IP
  tx.textContent='Copy  '+local;
  btn.onclick=function(){
    doCopy(local);
    tx.textContent='Copied! \u2714';
    setTimeout(function(){tx.textContent='Copy  '+local;},2000);
  };

  // Instruction box — .local as primary, IP as fallback
  var ib=document.getElementById('instbox');
  ib.innerHTML='Please connect to <strong>'+enc(_selSSID)+'</strong>'
    +' &amp; visit <strong>'+local+'</strong>'
    +' to view your dashboard.'
    +'<br><span style="font-size:.6rem;color:#3a5a3a">'
    +'If that doesn\'t work, try <strong>http://'+ip+'</strong></span>';
  ib.style.display='block';
}
function doCopy(txt){
  if(navigator.clipboard)navigator.clipboard.writeText(txt);
  else{var t=document.createElement('textarea');t.value=txt;
       document.body.appendChild(t);t.select();
       document.execCommand('copy');document.body.removeChild(t);}
}
/* Single scan on page load */
doScan();
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

  // ── Serve captive portal / dashboard ──
  // When _staReady and visitor is from home WiFi (not 192.168.4.x AP subnet),
  // schedule AP shutdown in 3s — user has successfully reached the dashboard.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (_staReady) {
      if (_apMode && _apShutdownTime == 0) {
        IPAddress cli = req->client()->remoteIP();
        if (cli[2] != 4) {  // not 192.168.4.x — home WiFi client
          _apShutdownTime = millis() + 3000;
          Serial.println("[AP] Home WiFi client visited dashboard — AP shutdown in 3s");
        }
      }
      req->send_P(200, "text/html", _dash_html);
    } else {
      req->send_P(200, "text/html", _login_html);
    }
  });

  // ── WiFi scan results ──
  // Does NOT auto-trigger another scan — manual only via doScan() in JS
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", getScanJSON());
  });

  // ── Scan progress (polled during active scan) ──
  server.on("/scanstatus", HTTP_GET, [](AsyncWebServerRequest* req) {
    int n = WiFi.scanComplete();
    String json;
    if (n == WIFI_SCAN_RUNNING) {
      json = "{\"scanning\":true,\"count\":0}";
    } else if (n >= 0) {
      json = "{\"scanning\":false,\"count\":" + String(n) + "}";
    } else {
      json = "{\"scanning\":false,\"count\":0}";
    }
    req->send(200, "application/json", json);
  });

  // ── Dashboard ready check ──
  // Returns ready=true once STA is connected AND 4s warmup has elapsed
  // (allows FreeRTOS sensor tasks to produce their first readings).
  server.on("/dashready", HTTP_GET, [](AsyncWebServerRequest* req) {
    bool ready = _staReady && _dashReadyTime > 0 && millis() >= _dashReadyTime;
    String json = "{\"ready\":" + String(ready ? "true" : "false")
                + ",\"ip\":\"" + _staIP + "\""
                + ",\"local\":\"http://hydroponics.local\"}";
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

    // Do NOT use WiFi.config() here — injecting a static IP during
    // AP_STA mode disrupts the DHCP auth timing and causes AUTH_EXPIRE.
    // Static IP is applied only on direct STA boot (credentials already
    // exist). For first-time provisioning, DHCP is the safe path.
    WiFi.disconnect(true);   // clear any lingering connection state
    delay(100);
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

  server.onNotFound([](AsyncWebServerRequest* req) {

    String host = req->host();
    String url  = req->url();

    // ── Android / Google connectivity checks ──
    // Must be a 302 redirect — returning 200 with body renders
    // "Redirecting..." literally in the WebView and goes nowhere.
    if (host.indexOf("connectivitycheck.gstatic.com") >= 0 ||
        host.indexOf("clients3.google.com") >= 0         ||
        host.indexOf("connectivitycheck.android.com") >= 0) {
      req->redirect("http://192.168.4.1/");
      return;
    }

    // ── Windows captive portal check ──
    if (url == "/connecttest.txt") {
      req->send(200, "text/plain", "Microsoft Connect Test");
      return;
    }

    // ── Apple captive portal check ──
    if (url == "/hotspot-detect.html") {
      req->send(200, "text/html", "<HTML><BODY>Success</BODY></HTML>");
      return;
    }

    // ── Normal user traffic ──
    if (!_staReady) {
      req->redirect("http://192.168.4.1/");
    } else {
      req->send(404, "text/plain", "Not found");
    }
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

    // ── Deferred AP shutdown ──
    // Fires 60s after STA connects, giving captive page time
    // to show the IP and countdown before Android closes it.
    if (_apShutdownTime > 0 && millis() >= _apShutdownTime) {
      _apShutdownTime = 0;
      _apMode         = false;
      WiFi.softAPdisconnect(true);
      dnsServer.stop();
      Serial.println("[AP] Shut down — captive grace period expired");
    }

    // ── Monitor ongoing connection attempt ──
    if (_connecting) {
      wl_status_t wst = WiFi.status();

      if (wst == WL_CONNECTED) {
        _setupSTAServices();

      } else if (wst == WL_CONNECT_FAILED ||
                 wst == WL_NO_SSID_AVAIL) {
        // Explicit failure
        _connecting  = false;
        _connFailed  = true;
        _connStart   = 0;
        Serial.println("[WiFi] Connection failed (explicit)");

      } else if (wst == WL_DISCONNECTED &&
                 _connStart > 0 && millis() - _connStart > 5000) {
        // AUTH_EXPIRE and similar handshake failures land here —
        // status drops to DISCONNECTED but never reaches CONNECT_FAILED.
        // After 5s grace window, treat it as a failure.
        _connecting  = false;
        _connFailed  = true;
        _connStart   = 0;
        WiFi.disconnect(true);
        Serial.println("[WiFi] Connection failed (AUTH_EXPIRE or timeout)");

      } else if (_connStart > 0 && millis() - _connStart > 30000) {
        // Hard timeout fallback
        _connecting  = false;
        _connFailed  = true;
        _connStart   = 0;
        WiFi.disconnect(true);
        Serial.println("[WiFi] Connection timed out");
      }
    }

    // Auto-rescan removed — scan is manual only (user taps ↻ in the UI)

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