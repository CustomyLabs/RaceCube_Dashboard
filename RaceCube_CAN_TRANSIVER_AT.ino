/*
   RACECUBE TRANSMISSION MODULE - VERSION 7.2 (OTA ENABLED)
   - Fixed: No RPM freeze during shifts
   - Fixed: N ghosting on P position
   - Feature: Raw pin status transmission for diagnostics
   - Feature: Added Oil Pressure Lamp parsing from CAN-C
   - Feature: ArduinoOTA flashing over Wi-Fi
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include "driver/twai.h" 
#include <ArduinoOTA.h> // <-- Добавлена библиотека OTA

#define CAN_TX_PIN 5
#define CAN_RX_PIN 4

// Пины
const int PIN_R = 21; const int PIN_N = 22; const int PIN_D = 23;
const int PIN_1 = 14; const int PIN_2 = 27; const int PIN_3 = 26; 
const int PIN_P = 25; 
const int PIN_PLUS = 18; const int PIN_MINUS = 19;
const int PIN_RELAY = 32;
const int NTC_PIN = 34; 

unsigned long plusTimer = 0; bool plusHandled = false; bool currentSportMode = false;
unsigned long lastCanRequest = 0; unsigned long lastCanResponse = 0; unsigned long lastClusterUpdate = 0;
int currentRequestStep = 0; 

int shiftState = 0; 
unsigned long shiftTimer = 0; unsigned long holdTimer = 0; unsigned long lastShiftAction = 0;  
const unsigned long SHIFT_COOLDOWN = 5000; const unsigned long RPM_HOLD_TIME = 3000;  
bool lastIsSafe = true;

uint8_t startSession[8]  = {0x02, 0x10, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00}; 
uint8_t testerPresent[8] = {0x02, 0x3E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}; 
uint8_t stopRoutine[8]   = {0x02, 0x32, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}; 
uint8_t rpmCmd[8]        = {0x04, 0x31, 0x06, 0x02, 0xEE, 0x00, 0x00, 0x00}; 

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

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
    uint8_t busType; uint32_t id; uint8_t dlc; uint8_t data[8];
} RawCanMsg;
#pragma pack(pop)

SuperTelemetry telemetry;
struct KnownMsg { uint32_t id; uint8_t dlc; uint8_t data[8]; };
KnownMsg knownC[50]; int knownC_count = 0;

struct CanCmd { uint32_t id; uint8_t mode; uint8_t pid; };
const CanCmd pollList[] = {
    {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x05}, {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x0B}, 
    {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x42}, {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x44}, 
    {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x3C}, {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x2F}, 
    {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x0A}, {0x7E1, 0x21, 0x02}, {0x7E0, 0x01, 0x0C}, 
    {0x7E0, 0x01, 0x0D}, {0x7E0, 0x01, 0x0C}, {0x7E0, 0x01, 0x0F}, {0x7E0, 0x01, 0x0C}, 
    {0x7E0, 0x01, 0x11}, {0x7E0, 0x01, 0x04}, {0x7E0, 0x01, 0x0E}, {0x7E0, 0x01, 0x01}  
};
const int POLL_LIST_SIZE = sizeof(pollList) / sizeof(pollList[0]);

float readATFTemp() {
    int adc = analogRead(NTC_PIN); if (adc == 0 || adc >= 4095) return 0.0f; 
    float voltage = adc * (3.3f / 4095.0f);
    float resistance = 10000.0 * voltage / (3.3f - voltage);
    float steinhart = log(resistance / 10000.0) / 3950.0 + (1.0 / (25.0 + 273.15));
    return (1.0 / steinhart) - 273.15; 
}

void sendCanQuery(uint32_t header, uint8_t mode, uint8_t pid) {
    twai_message_t message;
    message.identifier = header; message.extd = 0; message.rtr = 0; message.data_length_code = 8;
    message.data[0] = 0x02; message.data[1] = mode; message.data[2] = pid;  
    message.data[3] = 0; message.data[4] = 0; message.data[5] = 0; message.data[6] = 0; message.data[7] = 0;
    twai_transmit(&message, pdMS_TO_TICKS(5));
}

void sendCanRaw(uint32_t header, uint8_t* data) {
    twai_message_t msg;
    msg.identifier = header; msg.extd = 0; msg.rtr = 0; msg.data_length_code = 8;
    for (int i = 0; i < 8; i++) msg.data[i] = data[i];
    twai_transmit(&msg, pdMS_TO_TICKS(5));
}

void sendClusterGear() {
    twai_message_t msg;
    msg.identifier = 0x0D0; msg.extd = 0; msg.rtr = 0; msg.data_length_code = 2; 
    uint8_t gearByte = 0x04; // P
    if (telemetry.sigR) gearByte = 0x03; 
    else if (telemetry.sigN) gearByte = 0x02; 
    else if (telemetry.sigD) gearByte = telemetry.isSport ? 0x05 : 0x01;
    msg.data[0] = gearByte; msg.data[1] = 0x03; 
    twai_transmit(&msg, pdMS_TO_TICKS(5));
}

void setup() {
    pinMode(PIN_P, INPUT_PULLUP); pinMode(PIN_R, INPUT_PULLUP); 
    pinMode(PIN_N, INPUT_PULLUP); pinMode(PIN_D, INPUT_PULLUP);
    pinMode(PIN_1, INPUT_PULLUP); pinMode(PIN_2, INPUT_PULLUP); 
    pinMode(PIN_3, INPUT_PULLUP); 
    pinMode(PIN_PLUS, INPUT_PULLDOWN); pinMode(PIN_MINUS, INPUT_PULLDOWN);
    pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, HIGH); 

    WiFi.mode(WIFI_STA); 
    WiFi.begin("RACECUBE_CAN", "012345678"); // Подключаемся к CYD для OTA (не блокируя код)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    
    esp_now_init(); 
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1; peerInfo.encrypt = false; esp_now_add_peer(&peerInfo);

    // --- Настройка OTA ---
    ArduinoOTA.setHostname("RaceCube-Trans");
    ArduinoOTA.begin();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) twai_start();
}

void loop() {
    unsigned long currentMillis = millis();

    ArduinoOTA.handle(); // Обслуживаем OTA запросы

    if (digitalRead(PIN_PLUS) == HIGH) {
        if (plusTimer == 0) plusTimer = currentMillis; 
        else if ((currentMillis - plusTimer >= 3000) && !plusHandled) {
            currentSportMode = !currentSportMode;
            digitalWrite(PIN_RELAY, currentSportMode ? LOW : HIGH);
            plusHandled = true; 
        }
    } else { plusTimer = 0; plusHandled = false; }

    bool isP = (digitalRead(PIN_P) == LOW);
    telemetry.sigR = (digitalRead(PIN_R) == LOW); 
    telemetry.sigN = (digitalRead(PIN_N) == LOW); 
    
    // ПРИОРИТЕТ P над N
    if (isP) telemetry.sigN = 0; 

    telemetry.sigD = (digitalRead(PIN_D) == LOW); 
    telemetry.sig2 = (digitalRead(PIN_2) == LOW); 
    telemetry.sig3 = (digitalRead(PIN_3) == LOW); 
    telemetry.sig4 = 0; telemetry.sig5 = 0; 
    telemetry.isSport = currentSportMode; 
    telemetry.atfTempNTC = readATFTemp();
    
    telemetry.rawP = isP;
    telemetry.raw1 = (digitalRead(PIN_1) == LOW);
    telemetry.rawPlus = (digitalRead(PIN_PLUS) == HIGH);
    telemetry.rawMinus = (digitalRead(PIN_MINUS) == HIGH);
    telemetry.oilLamp = 0; 

    bool currentIsSafe = (isP || telemetry.sigN);
    if (lastIsSafe == true && currentIsSafe == false) {
        if (currentMillis - lastShiftAction > SHIFT_COOLDOWN) {
            shiftState = 1; shiftTimer = currentMillis; lastShiftAction = currentMillis; 
            sendCanRaw(0x7E0, startSession); 
        }
    }
    lastIsSafe = currentIsSafe;

    if (shiftState == 1 && (currentMillis - shiftTimer > 300)) { shiftState = 0; }
    if (shiftState == 2) {
        if (currentMillis - holdTimer < RPM_HOLD_TIME) {
            if (currentMillis - shiftTimer >= 500) { sendCanRaw(0x7E0, testerPresent); shiftTimer = currentMillis; }
        } else { sendCanRaw(0x7E0, stopRoutine); shiftState = 0; }
    }

    if (currentMillis - lastClusterUpdate >= 100) { sendClusterGear(); lastClusterUpdate = currentMillis; }

    if (currentMillis - lastCanRequest >= 25) { 
        CanCmd cmd = pollList[currentRequestStep];
        sendCanQuery(cmd.id, cmd.mode, cmd.pid);
        currentRequestStep++;
        if (currentRequestStep >= POLL_LIST_SIZE) currentRequestStep = 0;
        lastCanRequest = currentMillis;
    }

    twai_message_t rx_msg;
    while (twai_receive(&rx_msg, 0) == ESP_OK) {
        lastCanResponse = currentMillis;
        
        if (rx_msg.identifier == 0x212 && rx_msg.data_length_code > 0) {
            telemetry.oilLamp = (rx_msg.data[0] & 0x01) ? 1 : 0;
        }
        
        if (rx_msg.identifier == 0x7E8) {
            if (shiftState == 1 && rx_msg.data[1] == 0x50 && rx_msg.data[2] == 0x92) {
                sendCanRaw(0x7E0, rpmCmd); 
                shiftState = 2; holdTimer = currentMillis; shiftTimer = currentMillis;
            } 
            else if (rx_msg.data[1] == 0x41) {
                uint8_t pid = rx_msg.data[2]; float A = rx_msg.data[3]; float B = rx_msg.data[4];
                switch(pid) {
                    case 0x01: telemetry.checkEngine = ((int)A & 0x80) ? 1 : 0; break;
                    case 0x0C: telemetry.rpm = ((A * 256.0f) + B) / 4.0f; break;
                    case 0x0D: telemetry.speed = A; break;
                    case 0x05: telemetry.water = A - 40.0f; break;
                    case 0x0F: telemetry.intakeT = A - 40.0f; break;
                    case 0x11: telemetry.throttle = A * 100.0f / 255.0f; break;
                    case 0x04: telemetry.load = A * 100.0f / 255.0f; break;
                    case 0x0E: telemetry.timing = (A / 2.0f) - 64.0f; break;
                    case 0x42: telemetry.volt = ((A * 256.0f) + B) / 1000.0f; break;
                    case 0x0B: telemetry.boost = (A - 100.0f) / 100.0f; break; 
                    case 0x44: telemetry.afr = (((A * 256.0f) + B) / 32768.0f) * 14.7f; break;
                    case 0x3C: telemetry.exh = (((A * 256.0f) + B) / 10.0f - 40.0f) / 100.0f; break; 
                    case 0x2F: telemetry.fuel = A * 100.0f / 255.0f; break;
                    case 0x0A: telemetry.fuelPress = (A * 3.0f) / 100.0f; break; 
                }
            }
        }
        else if (rx_msg.identifier == 0x7E9 && rx_msg.data[1] == 0x61 && rx_msg.data[2] == 0x02) {
            double E = rx_msg.data[3]; double diff = 254.0 - E;
            telemetry.oilT = (0.000000002344 * pow(diff, 5)) + (-0.000001387 * pow(diff, 4)) + 
                             (0.0003193 * pow(diff, 3)) + (-0.03501 * pow(diff, 2)) + (2.302 * diff) - 36.6;
        }

        if (rx_msg.identifier < 0x7DF) { 
            bool shouldSend = false; int foundIdx = -1;
            for (int i = 0; i < knownC_count; i++) {
                if (knownC[i].id == rx_msg.identifier) {
                    foundIdx = i;
                    if (knownC[i].dlc != rx_msg.data_length_code || memcmp(knownC[i].data, rx_msg.data, rx_msg.data_length_code) != 0) {
                        knownC[i].dlc = rx_msg.data_length_code; memcpy(knownC[i].data, rx_msg.data, 8); shouldSend = true;
                    }
                    break;
                }
            }
            if (foundIdx == -1 && knownC_count < 50) {
                knownC[knownC_count].id = rx_msg.identifier; knownC[knownC_count].dlc = rx_msg.data_length_code;
                memcpy(knownC[knownC_count].data, rx_msg.data, 8); knownC_count++; shouldSend = true;
            }
            if (shouldSend) {
                RawCanMsg raw; raw.busType = 1; raw.id = rx_msg.identifier; raw.dlc = rx_msg.data_length_code;
                memcpy(raw.data, rx_msg.data, 8);
                esp_now_send(broadcastAddress, (uint8_t *)&raw, sizeof(raw));
            }
        }
    }
    
    uint32_t alerts; twai_read_alerts(&alerts, 0);
    if (alerts & TWAI_ALERT_BUS_OFF) twai_initiate_recovery(); 
    
    telemetry.canConnected = (currentMillis - lastCanResponse < 2000) ? 1 : 0;
    esp_now_send(broadcastAddress, (uint8_t *) &telemetry, sizeof(telemetry));
    delay(10); 
}