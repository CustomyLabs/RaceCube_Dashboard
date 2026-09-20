/*
   RACECUBE - CYD GATEWAY & UNIVERSAL WEB SNIFFER - VERSION 7.2 (OTA ENABLED)
   - Feature: Dark Neon UI Design for Web Monitor
   - Feature: Pin state visualization
   - Fixed: Handbrake added to RealDash frame3
   - Feature: Oil Pressure Lamp mapped to RealDash & Web Monitor
   - Fixed: ESP-NOW callback signature for ESP32 Core 3.x+
   - Feature: ArduinoOTA flashing over Wi-Fi
*/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ArduinoOTA.h> // <-- Добавлена библиотека OTA

#pragma pack(push, 1)
typedef struct {
    uint8_t sigR; uint8_t sigN; uint8_t sigD;
    uint8_t sig2; uint8_t sig3; uint8_t sig4; uint8_t sig5;
    uint8_t isSport;
    float atfTempNTC; float oilT; float rpm; float speed;
    float water; float volt; float boost; float afr; float exh;        
    float fuel; float fuelPress; float intakeT; float throttle;    
    float load; float timing; uint8_t canConnected; 
    uint8_t checkEngine;
    uint8_t rawP; uint8_t raw1; uint8_t rawPlus; uint8_t rawMinus;
    uint8_t oilLamp;
} SuperTelemetry;

typedef struct {
    uint8_t leftTurn; uint8_t rightTurn; uint8_t lowBeam;
    uint8_t highBeam; uint8_t parkLights; uint8_t handbrake;
} ComfortTelemetry;

typedef struct {
    uint8_t busType; uint32_t id; uint8_t dlc; uint8_t data[8];
} RawCanMsg;
#pragma pack(pop)

SuperTelemetry receivedTelemetry;
ComfortTelemetry comfortData = {0};

struct SniffedID { uint32_t id; uint8_t dlc; uint8_t data[8]; };
SniffedID canB_sniff[50]; int canB_count = 0;
SniffedID canC_sniff[50]; int canC_count = 0;

volatile int8_t raw_gear = -2;      
volatile uint8_t raw_sport = 0;
int8_t pending_gear = -2;          
int8_t realdash_gear = -2;          
unsigned long gear_debounce_timer = 0;

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   21
SPIClass hspi(HSPI);
Adafruit_ILI9341 tft = Adafruit_ILI9341(&hspi, TFT_DC, TFT_CS, TFT_RST);

unsigned long lastRealDashSend = 0;
WebServer server(80);

const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>RACECUBE CAN MONITOR</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  :root { --bg: #0a0e17; --card: #121927; --text: #e2e8f0; --accent: #00d2ff; --green: #10b981; --red: #ef4444; }
  body { background: var(--bg); color: var(--text); font-family: 'Segoe UI', system-ui, sans-serif; margin: 0; padding: 15px; }
  h1 { text-align: center; color: var(--accent); font-weight: 800; letter-spacing: 2px; text-shadow: 0 0 10px rgba(0,210,255,0.3); margin-top: 0;}
  .grid { display: flex; flex-wrap: wrap; gap: 15px; }
  .col { flex: 1; min-width: 320px; background: var(--card); border-radius: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.4); padding: 15px; display: flex; flex-direction: column; height: 80vh; border: 1px solid rgba(255,255,255,0.05); }
  h3 { margin: 0 0 15px 0; border-bottom: 2px solid var(--accent); padding-bottom: 10px; display: flex; justify-content: space-between; align-items: center; color: white; }
  button { background: var(--accent); color: #000; border: none; padding: 6px 15px; cursor: pointer; font-weight: bold; border-radius: 6px; transition: 0.2s; }
  button:hover { background: #fff; box-shadow: 0 0 15px var(--accent); }
  .data { flex-grow: 1; overflow-y: auto; font-family: monospace; font-size: 15px; }
  .msg { margin: 4px 0; padding: 6px; background: rgba(0,0,0,0.2); border-radius: 4px; border-left: 3px solid var(--accent); }
  .id { color: #facc15; font-weight: bold; width: 45px; display: inline-block; }
  .highlight { color: #94a3b8; margin: 0 4px; }
  .pins { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
  .pin { padding: 5px 10px; border-radius: 20px; font-size: 12px; font-weight: bold; background: #334155; }
  .pin.on { background: var(--green); color: black; box-shadow: 0 0 10px var(--green); }
  .dash-block { background: rgba(0,0,0,0.3); padding: 10px; border-radius: 8px; margin-bottom: 10px; }
  .dash-val { color: var(--accent); font-weight: bold; font-size: 1.2em; }
</style>
</head>
<body>
<h1>RACECUBE CAN MONITOR</h1>
<div class="grid">
  <div class="col">
    <h3>TRANSMISSION & OBD <button onclick="cpy('a')">COPY</button></h3>
    <div class="data" id="a">Waiting...</div>
  </div>
  <div class="col">
    <h3>CAN-C (FAST BUS) <button onclick="cpy('c')">COPY</button></h3>
    <div class="data" id="c">Waiting...</div>
  </div>
  <div class="col">
    <h3>CAN-B (COMFORT) <button onclick="cpy('b')">COPY</button></h3>
    <div class="data" id="b">Waiting...</div>
  </div>
</div>
<script>
function cpy(id){ navigator.clipboard.writeText(document.getElementById(id).innerText); }
function renderPackets(arr) {
  let h = ""; arr.forEach(m => {
    h += `<div class="msg"><span class="id">${m.id}</span> [${m.dlc}] `;
    m.data.forEach(x => { h += `<span class="highlight">${x}</span>`; }); h += `</div>`;
  }); return h;
}
setInterval(()=>{
  fetch('/json').then(r=>r.json()).then(d=>{
    document.getElementById('b').innerHTML = renderPackets(d.canB);
    document.getElementById('c').innerHTML = renderPackets(d.canC);

    let ht = `<div class="dash-block">RPM: <span class="dash-val">${d.engine.rpm}</span> | SPEED: <span class="dash-val">${d.engine.speed} km/h</span><br>`;
    ht += `WATER: <span class="dash-val">${d.engine.water} C</span> | MIL: <span class="dash-val" style="color:${d.engine.mil?'var(--red)':'#fff'}">${d.engine.mil?'ON':'OFF'}</span><br>`;
    ht += `OIL LAMP: <span class="dash-val" style="color:${d.engine.oilLamp?'var(--red)':'var(--green)'}">${d.engine.oilLamp?'LOW PRESS!':'OK'}</span></div>`;
    
    ht += `<div class="dash-block">TARGET GEAR: <span class="dash-val">${d.at.gear}</span> | SPORT: <span class="dash-val">${d.at.sport?'ON':'OFF'}</span><br>`;
    ht += `ATF TEMP: <span class="dash-val">${parseFloat(d.at.atf).toFixed(1)} C</span></div>`;

    ht += `<div><strong>HARDWARE PINS DIAGNOSTICS:</strong></div><div class="pins">`;
    ht += `<div class="pin ${d.pins.P?'on':''}">P: ${d.pins.P}</div>`;
    ht += `<div class="pin ${d.pins.R?'on':''}">R: ${d.pins.R}</div>`;
    ht += `<div class="pin ${d.pins.N?'on':''}">N: ${d.pins.N}</div>`;
    ht += `<div class="pin ${d.pins.D?'on':''}">D: ${d.pins.D}</div>`;
    ht += `<div class="pin ${d.pins.g1?'on':''}">1: ${d.pins.g1}</div>`;
    ht += `<div class="pin ${d.pins.g2?'on':''}">2: ${d.pins.g2}</div>`;
    ht += `<div class="pin ${d.pins.g3?'on':''}">3: ${d.pins.g3}</div>`;
    ht += `<div class="pin ${d.pins.p?'on':''}">+: ${d.pins.p}</div>`;
    ht += `<div class="pin ${d.pins.m?'on':''}">-: ${d.pins.m}</div>`;
    ht += `</div>`;
    document.getElementById('a').innerHTML = ht;
  });
}, 300);
</script>
</body></html>
)=====";

void appendSniffToJson(String &out, SniffedID* arr, int count) {
    out += "[";
    for(int i=0; i<count; i++) {
        char buf[64]; sprintf(buf, "{\"id\":\"%03X\",\"dlc\":%d,\"data\":[", arr[i].id, arr[i].dlc); out += buf;
        for(int j=0; j<arr[i].dlc; j++) { sprintf(buf, "\"%02X\"", arr[i].data[j]); out += buf; if(j < arr[i].dlc - 1) out += ","; }
        out += "]}"; if(i < count - 1) out += ",";
    }
    out += "]";
}

void handleJson() {
    String out = "{";
    out += "\"canB\":"; appendSniffToJson(out, canB_sniff, canB_count); out += ",";
    out += "\"canC\":"; appendSniffToJson(out, canC_sniff, canC_count); out += ",";
    
    out += "\"engine\":{\"rpm\":" + String(receivedTelemetry.rpm) + ",\"speed\":" + String(receivedTelemetry.speed) + ",";
    out += "\"water\":" + String(receivedTelemetry.water) + ",\"volt\":" + String(receivedTelemetry.volt) + ",";
    out += "\"mil\":" + String(receivedTelemetry.checkEngine) + ",\"oilLamp\":" + String(receivedTelemetry.oilLamp) + "},";

    String gear = "P";
    if(realdash_gear == -1) gear = "R";
    else if(realdash_gear == 0) gear = "N";
    else if(realdash_gear >= 1) gear = "D" + String(realdash_gear);

    out += "\"at\":{\"gear\":\"" + gear + "\",\"sport\":" + String(raw_sport) + ",\"atf\":" + String(receivedTelemetry.atfTempNTC) + "},";
    
    out += "\"pins\":{\"P\":" + String(receivedTelemetry.rawP) + ",\"R\":" + String(receivedTelemetry.sigR) + ",";
    out += "\"N\":" + String(receivedTelemetry.sigN) + ",\"D\":" + String(receivedTelemetry.sigD) + ",";
    out += "\"g1\":" + String(receivedTelemetry.raw1) + ",\"g2\":" + String(receivedTelemetry.sig2) + ",";
    out += "\"g3\":" + String(receivedTelemetry.sig3) + ",\"p\":" + String(receivedTelemetry.rawPlus) + ",";
    out += "\"m\":" + String(receivedTelemetry.rawMinus) + "}";
    out += "}";
    server.send(200, "application/json", out);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
    if (len == sizeof(SuperTelemetry)) {
        memcpy(&receivedTelemetry, incomingData, sizeof(receivedTelemetry));
        if (receivedTelemetry.sigR) raw_gear = -1;
        else if (receivedTelemetry.sigN) raw_gear = 0;
        else if (receivedTelemetry.sigD) {
            if (receivedTelemetry.sig5) raw_gear = 5; else if (receivedTelemetry.sig4) raw_gear = 4;
            else if (receivedTelemetry.sig3) raw_gear = 3; else if (receivedTelemetry.sig2) raw_gear = 2; else raw_gear = 1;
        } else raw_gear = -2;
        raw_sport = receivedTelemetry.isSport ? 1 : 0;
    } 
    else if (len == sizeof(ComfortTelemetry)) { memcpy(&comfortData, incomingData, sizeof(comfortData)); }
    else if (len == sizeof(RawCanMsg)) {
        RawCanMsg raw; memcpy(&raw, incomingData, sizeof(RawCanMsg));
        SniffedID* targetArr = (raw.busType == 0) ? canB_sniff : canC_sniff;
        int* targetCount = (raw.busType == 0) ? &canB_count : &canC_count;
        bool found = false;
        for(int i=0; i<*targetCount; i++) {
            if(targetArr[i].id == raw.id) {
                targetArr[i].dlc = raw.dlc; memcpy(targetArr[i].data, raw.data, 8);
                found = true; break;
            }
        }
        if(!found && *targetCount < 50) {
            targetArr[*targetCount].id = raw.id; targetArr[*targetCount].dlc = raw.dlc;
            memcpy(targetArr[*targetCount].data, raw.data, 8); (*targetCount)++;
        }
    }
}

void sendRealDashUSB() {
    uint16_t current_rpm = (uint16_t)receivedTelemetry.rpm;
    uint16_t current_speed = (uint16_t)receivedTelemetry.speed;
    uint8_t current_water = (uint8_t)(receivedTelemetry.water + 40.0f); 
    uint8_t current_map = (uint8_t)(receivedTelemetry.boost * 100.0f + 100.0f); 
    uint8_t current_fuel = (uint8_t)(receivedTelemetry.fuel * 255.0f / 100.0f); 
    uint8_t current_volt = (uint8_t)(receivedTelemetry.volt * 10.0f);

    uint8_t frame1[16] = {
        0x44, 0x33, 0x22, 0x11, 0x80, 0x0C, 0x00, 0x00, 
        (uint8_t)(current_rpm & 0xFF), (uint8_t)((current_rpm >> 8) & 0xFF),    
        (uint8_t)(current_speed & 0xFF), (uint8_t)((current_speed >> 8) & 0xFF), 
        current_water, current_map, current_fuel, current_volt
    };
    Serial.write(frame1, 16);

    uint8_t frame2[16] = {
        0x44, 0x33, 0x22, 0x11, 0x81, 0x0C, 0x00, 0x00, 
        (uint8_t)realdash_gear, raw_sport, receivedTelemetry.checkEngine, 
        receivedTelemetry.oilLamp, 
        0x00, 0x00, 0x00, 0x00
    };
    Serial.write(frame2, 16);

    uint8_t lightBits = 0;
    if (comfortData.leftTurn) lightBits |= (1 << 0);
    if (comfortData.rightTurn) lightBits |= (1 << 1);
    if (comfortData.lowBeam) lightBits |= (1 << 2);
    if (comfortData.highBeam) lightBits |= (1 << 3);
    if (comfortData.parkLights) lightBits |= (1 << 4);

    uint8_t frame3[16] = {
        0x44, 0x33, 0x22, 0x11, 0x82, 0x0C, 0x00, 0x00, 
        lightBits, (uint8_t)(comfortData.handbrake ? 1 : 0), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Serial.write(frame3, 16);
}

void setup() {
    Serial.begin(115200); delay(500);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    hspi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
    tft.begin(); tft.setRotation(1); tft.fillScreen(0x0823); 
    
    tft.setTextSize(2); tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(10, 30); tft.print("RACECUBE CAN MONITOR");
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(10, 80); tft.print("WIFI: RACECUBE_CAN");
    tft.setCursor(10, 110); tft.print("PASS: 012345678");
    tft.setCursor(10, 140); tft.print("WEB: 192.168.4.1");

    WiFi.mode(WIFI_AP_STA); esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); 
    WiFi.softAP("RACECUBE_CAN", "012345678", 1);
    
    server.on("/", [](){ server.send(200, "text/html", HTML_PAGE); });
    server.on("/json", handleJson); server.begin();

    if (esp_now_init() == ESP_OK) esp_now_register_recv_cb(OnDataRecv);

    // --- Настройка OTA ---
    ArduinoOTA.setHostname("RaceCube-CYD");
    ArduinoOTA.begin();
}

void loop() {
    unsigned long currentMillis = millis();
    server.handleClient();
    ArduinoOTA.handle(); // Обслуживаем OTA запросы

    if (raw_gear != pending_gear) {
        pending_gear = raw_gear; gear_debounce_timer = currentMillis; 
    }
    if (currentMillis - gear_debounce_timer > 150) realdash_gear = pending_gear;

    if (currentMillis - lastRealDashSend >= 30) {
        sendRealDashUSB(); lastRealDashSend = currentMillis;
    }
}