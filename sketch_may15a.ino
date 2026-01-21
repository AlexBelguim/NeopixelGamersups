#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <ESPmDNS.h>

// ——— USER CONFIG ———
// Total LEDs (must be multiple of 7)
#define LED_COUNT   1400   // Changed from 700000 - this is more realistic!
#define GROUP_SIZE  7
#define MAX_GROUPS  (LED_COUNT/GROUP_SIZE)
#define LED_PIN     13   // ← your NeoPixel data pin

// AP for initial config
const char* AP_SSID = "NeoPixel-Config";
const char* AP_PASS = "";
IPAddress apIP(192,168,4,1), netMsk(255,255,255,0);

// ——— GLOBALS ———
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB+NEO_KHZ800);
AsyncWebServer   server(80);
AsyncWebSocket   ws("/ws");
Preferences      prefs;

// State
int     groupCount      = 3;
struct { uint8_t r,g,b; } groupColors[MAX_GROUPS];
bool    groupOn[MAX_GROUPS];
unsigned long autoOffTime = 0, lastTimerRem = -1;

// Effects
bool    effectActive = false;
int     currentEffect = 0;  // 0=spotlight, 1=wave, 2=fade, 3=rainbow
unsigned long effectNext = 0;
unsigned long effectInterval = 15000;
int     effectStep = 0;

// Spotlight (legacy support)
std::vector<int> bag;
int     bagIndex       = 0;
int     lastSpotRem    = -1;

// ——— CONFIG PAGE ———
const char config_html[] PROGMEM = R"CONF(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Configure Wi-Fi</title>
<style>
 body{font-family:sans-serif;margin:20px;background:#121212;color:#e0e0e0}
 input,button{display:block;width:100%;margin:10px 0;padding:8px;border-radius:4px;
   border:1px solid #333;background:#2a2a2a;color:#e0e0e0;font-size:16px}
</style>
</head><body>
<h1>Configure Wi-Fi</h1>
<form action="/save" method="POST">
  <input name="ssid"    placeholder="SSID"    required>
  <input name="pass"    placeholder="Password" type="password">
  <button type="submit">Save &amp; Reboot</button>
</form>
</body></html>
)CONF";

// ——— CONTROLLER PAGE ———
const char control_html[] PROGMEM = R"CONT(
<!DOCTYPE html><html>
<head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NeoPixel Ctrl</title>
<style>
 body{background:#121212;color:#e0e0e0;font-family:sans-serif;margin:10px}
 h1{text-align:center} .section{background:#1e1e1e;padding:10px;border-radius:8px;margin:15px 0}
 .row{display:flex;gap:8px;flex-wrap:wrap} input,button,select{flex:1;
    background:#2a2a2a;color:#e0e0e0;border:1px solid #333;border-radius:4px;padding:8px;
    min-width:60px;font-size:16px}
 .group{background:#242424;border-radius:6px;padding:8px;margin:8px 0}
 label{flex:none;margin-right:8px} .colVal{font-family:monospace;width:60px;text-align:center}
</style>
</head><body>
<h1>NeoPixel Ctrl</h1>

<div class="section">
  Status: <span id="status">…</span><br>
  <div class="row">
    <button id="globalOn">All On</button>
    <button id="globalOff">All Off</button>
  </div>
</div>

<div class="section">
  <div class="row">
    <label>Groups:</label>
    <input type="number" id="groupCount" min="1" value="3">
    <button id="applyGroups">Apply</button>
  </div>
</div>

<div id="groupsContainer" class="section"></div>

<div class="section">
  <h2>Timer</h2>
  <div class="row">
    <label>Auto-off (sec):</label>
    <input type="number" id="timerSec" min="1" value="60">
    <button id="setTimer">Set</button>
  </div>
  Remaining: <span id="timerStatus">—</span>
</div>

<div class="section">
  <h2>Effects</h2>
  <div class="row">
    <label>Effect:</label>
    <select id="effectType">
      <option value="0" selected>Spotlight</option>
      <option value="1">Wave</option>
      <option value="2">Fade</option>
      <option value="3">Rainbow</option>
    </select>
    <label>Speed (sec):</label>
    <select id="effectInterval">
      <option value="1000">1</option>
      <option value="3000">3</option>
      <option value="5000">5</option>
      <option value="10000">10</option>
      <option value="15000" selected>15</option>
      <option value="20000">20</option>
    </select>
    <button id="toggleEffect">Start</button>
  </div>
  Status: <span id="effectStatus">—</span>
</div>

<script>
const MAX_GROUPS = 200;  // 1400 LEDs / 7 per group = 200 groups max
let ws, effectActive=false, groupStates=[];

// restore
let savedCount = +localStorage.getItem('groupCount')||3;
document.getElementById('groupCount').value = savedCount;

// WS
function connectWS(){
  ws=new WebSocket(`ws://${location.hostname}/ws`);
  ws.onopen   =()=>document.getElementById('status').textContent='Connected';
  ws.onclose  =()=>{document.getElementById('status').textContent='Disconnected';setTimeout(connectWS,2000)};
  ws.onerror  =e=>console.error(e);
  ws.onmessage=e=>{
    let p=e.data.split(' ');
    if(p[0]==='TIMER'){
      document.getElementById('timerStatus').textContent = (+p[1]>0? p[1]+'s':'Off');
    }
    if(p[0]==='EFFECT'){
      if(p[1]==='END'){
        effectActive=false;document.getElementById('toggleEffect').textContent='Start';
        document.getElementById('effectStatus').textContent='Done';
      } else {
        effectActive=true;
        document.getElementById('toggleEffect').textContent='Stop';
        let effectNames=['Spotlight','Wave','Fade','Rainbow'];
        document.getElementById('effectStatus').textContent=`${effectNames[p[2]]}: ${p[1]}s`;
      }
    }
    if(p[0]==='GROUPSTATE'){
      // Update button states: GROUPSTATE groupId isOn
      let groupId = +p[1], isOn = p[2]==='1';
      groupStates[groupId] = isOn;
      let btn = document.getElementById('o'+groupId);
      if(btn) {
        btn.textContent = isOn ? 'Off' : 'On';
        localStorage.setItem('on'+groupId, isOn ? '1' : '0');
      }
    }
    if(p[0]==='ALLSTATES'){
      // Update all button states: ALLSTATES on0 on1 on2...
      for(let i=1; i<p.length && i-1<groupStates.length; i++){
        let isOn = p[i]==='1';
        groupStates[i-1] = isOn;
        let btn = document.getElementById('o'+(i-1));
        if(btn) {
          btn.textContent = isOn ? 'Off' : 'On';
          localStorage.setItem('on'+(i-1), isOn ? '1' : '0');
        }
      }
    }
  };
}
connectWS();
function send(cmd){ if(ws&&ws.readyState===1) ws.send(cmd); }

// build groups UI
function buildGroups(n){
  localStorage.setItem('groupCount',n);
  send(`GROUPS ${n}`);
  const cont=document.getElementById('groupsContainer');
  cont.innerHTML='';
  groupStates = new Array(n).fill(false);
  for(let i=0;i<n;i++){
    const d=document.createElement('div');d.className='group';
    const savedCol = localStorage.getItem('col'+i)||'#ffffff';
    const savedOn  = localStorage.getItem('on'+i)==='1';
    groupStates[i] = savedOn;
    d.innerHTML=`
      <div class="row">
        <strong>G${i}</strong>
        <input type="color" id="c${i}" value="${savedCol}">
        <span class="colVal" id="v${i}">${savedCol}</span>
        <button id="o${i}">${savedOn?'Off':'On'}</button>
      </div>`;
    cont.appendChild(d);
    const ci=d.querySelector('#c'+i), vi=d.querySelector('#v'+i), bi=d.querySelector('#o'+i);
    ci.onchange=()=>{
      const c=ci.value;localStorage.setItem('col'+i,c);vi.textContent=c;
      if(groupStates[i]) {
        let [r,g,b]=[c.slice(1,3),c.slice(3,5),c.slice(5,7)].map(x=>parseInt(x,16));
        send(`COLOR ${i} ${r} ${g} ${b}`);
      }
    };
    bi.onclick=()=>{
      if(bi.textContent==='On'){
        // Turn on
        groupStates[i] = true;
        let c=ci.value,[r,g,b]=[c.slice(1,3),c.slice(3,5),c.slice(5,7)].map(x=>parseInt(x,16));
        send(`COLOR ${i} ${r} ${g} ${b}`);
      } else {
        // Turn off
        groupStates[i] = false;
        send(`COLOR ${i} 0 0 0`);
      }
    };
  }
}

// handlers
document.getElementById('applyGroups').onclick =()=>{
  let n=+document.getElementById('groupCount').value||3;
  n=Math.max(1,Math.min(MAX_GROUPS,n));
  buildGroups(n);
};
document.getElementById('globalOn').onclick  =()=>send('GLOBAL ON');
document.getElementById('globalOff').onclick =()=>send('GLOBAL OFF');
document.getElementById('setTimer').onclick  =()=>{
  let s=+document.getElementById('timerSec').value||60;
  send(`TIMER ${s}`);
};
document.getElementById('toggleEffect').onclick=()=>{
  if(!effectActive){
    let effectType=+document.getElementById('effectType').value;
    let iv=+document.getElementById('effectInterval').value;
    send(`EFFECTSTART ${effectType} ${iv}`);
  } else send('EFFECTSTOP');
};

// init
buildGroups(savedCount);
</script>
</body></html>
)CONT";

// ——— HELPERS ———
void shuffleBag(){
  bag.clear();
  for(int i=0;i<groupCount;i++) bag.push_back(i);
  std::random_shuffle(bag.begin(), bag.end());
  bagIndex=0;
}

void broadcast(const String &m){ ws.textAll(m); }

void sendGroupState(int g) {
  broadcast("GROUPSTATE " + String(g) + " " + String(groupOn[g] ? 1 : 0));
}

void sendAllStates() {
  String msg = "ALLSTATES";
  for(int i = 0; i < groupCount; i++) {
    msg += " " + String(groupOn[i] ? 1 : 0);
  }
  broadcast(msg);
}

uint32_t wheel(byte wheelPos) {
  wheelPos = 255 - wheelPos;
  if(wheelPos < 85) {
    return strip.Color(255 - wheelPos * 3, 0, wheelPos * 3);
  }
  if(wheelPos < 170) {
    wheelPos -= 85;
    return strip.Color(0, wheelPos * 3, 255 - wheelPos * 3);
  }
  wheelPos -= 170;
  return strip.Color(wheelPos * 3, 255 - wheelPos * 3, 0);
}

void runEffect() {
  switch(currentEffect) {
    case 0: // Spotlight
      if(bagIndex >= bag.size()) shuffleBag();
      {
        int g = bag[bagIndex++];
        strip.clear();
        if(groupOn[g]) {
          auto& c = groupColors[g];
          uint32_t col = strip.Color(c.r, c.g, c.b);
          for(int j = 0; j < GROUP_SIZE; j++) 
            strip.setPixelColor(g * GROUP_SIZE + j, col);
        }
        strip.show();
      }
      break;
      
    case 1: // Wave
      strip.clear();
      for(int g = 0; g < groupCount; g++) {
        if(groupOn[g]) {
          float phase = (effectStep + g * 2) % 20;
          float brightness = (sin(phase * 0.314) + 1) * 0.5; // 0 to 1
          auto& c = groupColors[g];
          uint32_t col = strip.Color(c.r * brightness, c.g * brightness, c.b * brightness);
          for(int j = 0; j < GROUP_SIZE; j++)
            strip.setPixelColor(g * GROUP_SIZE + j, col);
        }
      }
      strip.show();
      effectStep++;
      break;
      
    case 2: // Fade
      {
        float brightness = (sin(effectStep * 0.1) + 1) * 0.5; // 0 to 1
        strip.clear();
        for(int g = 0; g < groupCount; g++) {
          if(groupOn[g]) {
            auto& c = groupColors[g];
            uint32_t col = strip.Color(c.r * brightness, c.g * brightness, c.b * brightness);
            for(int j = 0; j < GROUP_SIZE; j++)
              strip.setPixelColor(g * GROUP_SIZE + j, col);
          }
        }
        strip.show();
        effectStep++;
      }
      break;
      
    case 3: // Rainbow
      strip.clear();
      for(int g = 0; g < groupCount; g++) {
        if(groupOn[g]) {
          uint32_t col = wheel((effectStep + g * 10) & 255);
          for(int j = 0; j < GROUP_SIZE; j++)
            strip.setPixelColor(g * GROUP_SIZE + j, col);
        }
      }
      strip.show();
      effectStep++;
      break;
  }
}

// ——— COMMAND PARSING ———
void handleCmd(const String &cmd){
  Serial.println("CMD> "+cmd);
  std::vector<String> p; int i=0;
  while(i<cmd.length()){
    int sp=cmd.indexOf(' ',i);
    if(sp<0) sp=cmd.length();
    p.push_back(cmd.substring(i,sp));
    i=sp+1;
  }
  if(p.empty()) return;
  if(p[0]=="GROUPS" && p.size()==2){
    groupCount=constrain(p[1].toInt(),1,MAX_GROUPS);
  }
  else if(p[0]=="COLOR" && p.size()==5){
    int g=p[1].toInt();
    if(g>=0&&g<groupCount){
      auto r=p[2].toInt(), gg=p[3].toInt(), b=p[4].toInt();
      groupColors[g]={uint8_t(r),uint8_t(gg),uint8_t(b)};
      groupOn[g] = (r > 0 || gg > 0 || b > 0);
      sendGroupState(g);
      if(!effectActive){
        int st=g*GROUP_SIZE; uint32_t col=strip.Color(r,gg,b);
        for(int j=0;j<GROUP_SIZE;j++) strip.setPixelColor(st+j,col);
        strip.show();
      }
    }
  }
  else if(p[0]=="GLOBAL"&&p.size()==2){
    if(p[1]=="ON"){
      effectActive=false; autoOffTime=0;
      for(int g=0;g<groupCount;g++) {
        groupOn[g] = true;
        auto& c=groupColors[g];
        uint32_t col=strip.Color(c.r,c.g,c.b);
        for(int j=0;j<GROUP_SIZE;j++) strip.setPixelColor(g*GROUP_SIZE+j,col);
      }
      strip.show();
      sendAllStates();
    } else {
      effectActive=false; autoOffTime=0;
      for(int g=0;g<groupCount;g++) groupOn[g] = false;
      strip.clear(); strip.show();
      sendAllStates();
    }
  }
  else if(p[0]=="TIMER"&&p.size()==2){
    autoOffTime=millis()+p[1].toInt()*1000UL;
    lastTimerRem=-1;
  }
  else if(p[0]=="EFFECTSTART"&&p.size()==3){
    effectActive=true;
    currentEffect = p[1].toInt();
    effectInterval = p[2].toInt();
    effectNext = millis();
    effectStep = 0;
    if(currentEffect == 0) shuffleBag(); // Only for spotlight
    lastSpotRem=-1;
  }
  else if(p[0]=="EFFECTSTOP"){
    effectActive=false;
    for(int g=0;g<groupCount;g++) if(groupOn[g]){
      auto& c=groupColors[g];
      uint32_t col=strip.Color(c.r,c.g,c.b);
      for(int j=0;j<GROUP_SIZE;j++) strip.setPixelColor(g*GROUP_SIZE+j,col);
    }
    strip.show();
    broadcast("EFFECT END");
  }
  // Legacy spotlight commands for backwards compatibility
  else if(p[0]=="SPOTSTART"&&p.size()==2){
    effectActive=true;
    currentEffect = 0; // Spotlight
    effectInterval = p[1].toInt();
    effectNext = millis();
    effectStep = 0;
    shuffleBag();
    lastSpotRem=-1;
  }
  else if(p[0]=="SPOTSTOP"){
    effectActive=false;
    for(int g=0;g<groupCount;g++) if(groupOn[g]){
      auto& c=groupColors[g];
      uint32_t col=strip.Color(c.r,c.g,c.b);
      for(int j=0;j<GROUP_SIZE;j++) strip.setPixelColor(g*GROUP_SIZE+j,col);
    }
    strip.show();
    broadcast("EFFECT END");
  }
}

// ——— WS CALLBACK ———
void onWsEvent(AsyncWebSocket*,AsyncWebSocketClient*,AwsEventType t,
               void*,uint8_t*d,size_t len){
  if(t==WS_EVT_DATA){
    String msg; msg.reserve(len);
    for(size_t i=0;i<len;i++) msg+=char(d[i]);
    handleCmd(msg);
  }
}

void setup(){
  Serial.begin(115200);
  strip.begin(); 
  strip.clear();
  strip.show();
  
  // Initialize group colors to white
  for(int i = 0; i < MAX_GROUPS; i++) {
    groupColors[i] = {255, 255, 255}; // White default
    groupOn[i] = false;
  }
  
  prefs.begin("wifi", false);
  // Uncomment next line to clear saved WiFi and force config portal:
  // prefs.clear();
  // load saved creds
  String ssid = prefs.getString("ssid",""),
         pass = prefs.getString("pass","");
  bool stationOK=false;
  if(ssid.length()){
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    if (MDNS.begin("neopixel")) {
  Serial.println("mDNS responder started: http://neopixel.local");
}
    Serial.println("Connecting to saved Wi-Fi…");
    if(WiFi.waitForConnectResult()==WL_CONNECTED){
      Serial.printf("Sta IP: %s\n", WiFi.localIP().toString().c_str());
      stationOK=true;
    } else {
      Serial.println("Sta connect failed");
    }
  }
  // FLAGS
  bool configMode = !stationOK;

  if(configMode){
    WiFi.softAPConfig(apIP,apIP,netMsk);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("AP IP: 192.168.4.1 (config portal)");
    // config routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
      r->send_P(200,"text/html",config_html);
    });
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest* r){
      if(auto p1=r->getParam("ssid",true)){
        prefs.putString("ssid", p1->value());
      }
      if(auto p2=r->getParam("pass",true)){
        prefs.putString("pass", p2->value());
      }
      r->send(200,"text/html",
        "<h1>Saved. Rebooting…</h1>");
      delay(500);
      ESP.restart();
    });
  } else {
    // station mode: serve controller
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
      r->send_P(200,"text/html",control_html);
    });
    server.onNotFound([](AsyncWebServerRequest* r){
      r->redirect("/");
    });
  }
  server.begin();
}

void loop(){
  unsigned long now = millis();
  if(autoOffTime){
    if(now>=autoOffTime){
      strip.clear(); strip.show(); autoOffTime=0;
      broadcast("TIMER 0");
    } else {
      int rem=(autoOffTime-now+999)/1000;
      if(rem!=lastTimerRem){
        lastTimerRem=rem;
        broadcast("TIMER "+String(rem));
      }
    }
  }
  if(effectActive){
    if(now>=effectNext){
      runEffect();
      int secs=(effectInterval+999)/1000;
      broadcast("EFFECT "+String(secs)+" "+String(currentEffect));
      lastSpotRem=-1;
      effectNext=now+effectInterval;
    } else {
      int rem=(effectNext-now+999)/1000;
      if(rem!=lastSpotRem){
        lastSpotRem=rem;
        broadcast("EFFECT "+String(rem)+" "+String(currentEffect));
      }
    }
  }
  delay(1);
}
