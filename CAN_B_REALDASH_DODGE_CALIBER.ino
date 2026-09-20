/*
   RACECUBE COMFORT MODULE (CAN-B) - VERSION 7.2 (OTA ENABLED)
   - Feature: Added Handbrake detection
   - Feature: ArduinoOTA flashing over Wi-Fi
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "driver/twai.h"
#include <ArduinoOTA.h> // <-- Добавлена библиотека OTA

#define CAN_RX_PIN 4
#define CAN_TX_PIN 5

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

#pragma pack(push, 1)
typedef struct {
    uint8_t leftTurn; uint8_t rightTurn; uint8_t lowBeam;
    uint8_t highBeam; uint8_t parkLights;
    uint8_t handbrake;
} ComfortTelemetry;

typedef struct {
    uint8_t busType; uint32_t id; uint8_t dlc; uint8_t data[8];
} RawCanMsg;
#pragma pack(pop)

ComfortTelemetry comfortData = {0};
unsigned long lastSend = 0;
struct KnownMsg { uint32_t id; uint8_t dlc; uint8_t data[8]; };
KnownMsg knownB[50]; int knownB_count = 0;

void setup() {
    WiFi.mode(WIFI_STA); 
    WiFi.begin("RACECUBE_CAN", "012345678"); // Подключаемся к CYD для OTA
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    
    esp_now_init();
    memcpy(peerInfo.peer_addr, broadcastAddress, 6); peerInfo.channel = 1; peerInfo.encrypt = false; esp_now_add_peer(&peerInfo);

    // --- Настройка OTA ---
    ArduinoOTA.setHostname("RaceCube-Comfort");
    ArduinoOTA.begin();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t t_config = { .brp = 60, .tseg_1 = 11, .tseg_2 = 4, .sjw = 3, .triple_sampling = false };
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) twai_start();
}

void loop() {
    ArduinoOTA.handle(); // Обслуживаем OTA запросы

    twai_message_t rx_msg;
    while (twai_receive(&rx_msg, 0) == ESP_OK) {
        
        if (rx_msg.identifier == 0x006 && rx_msg.data_length_code > 0) {
            uint8_t b0 = rx_msg.data[0];
            comfortData.leftTurn   = (b0 & 0x01) ? 1 : 0;
            comfortData.rightTurn  = (b0 & 0x02) ? 1 : 0;
            comfortData.parkLights = (b0 & 0x08) ? 1 : 0;
            comfortData.highBeam   = (b0 & 0x10) ? 1 : 0;
        }
        else if (rx_msg.identifier == 0x1C8 && rx_msg.data_length_code > 0) {
            comfortData.lowBeam = (rx_msg.data[0] == 0x02) ? 1 : 0;
        }
        else if (rx_msg.identifier == 0x211 && rx_msg.data_length_code > 0) {
            comfortData.handbrake = (rx_msg.data[0] & 0x01) ? 1 : 0;
        }

        bool shouldSend = false; int foundIdx = -1;
        for (int i = 0; i < knownB_count; i++) {
            if (knownB[i].id == rx_msg.identifier) {
                foundIdx = i;
                if (knownB[i].dlc != rx_msg.data_length_code || memcmp(knownB[i].data, rx_msg.data, rx_msg.data_length_code) != 0) {
                    knownB[i].dlc = rx_msg.data_length_code; memcpy(knownB[i].data, rx_msg.data, 8); shouldSend = true;
                }
                break;
            }
        }
        if (foundIdx == -1 && knownB_count < 50) {
            knownB[knownB_count].id = rx_msg.identifier; knownB[knownB_count].dlc = rx_msg.data_length_code;
            memcpy(knownB[knownB_count].data, rx_msg.data, 8); knownB_count++; shouldSend = true;
        }

        if (shouldSend) {
            RawCanMsg raw; raw.busType = 0; raw.id = rx_msg.identifier; raw.dlc = rx_msg.data_length_code;
            memcpy(raw.data, rx_msg.data, 8); esp_now_send(broadcastAddress, (uint8_t *)&raw, sizeof(raw));
        }
    }

    if (millis() - lastSend > 50) {
        esp_now_send(broadcastAddress, (uint8_t *) &comfortData, sizeof(comfortData)); lastSend = millis();
    }
}