#include "comms.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

static CommsPacket   sPacket;
static volatile bool sFresh = false;
static portMUX_TYPE  sMux   = portMUX_INITIALIZER_UNLOCKED;

// arduino-esp32 3.x changed the recv callback signature
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
#else
static void onRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
    if (len < 1) return;
    uint8_t cnt = data[0];
    if (cnt < 8 || cnt > COMMS_MAX_VALS) return;
    int expected = 1 + cnt * (int)sizeof(uint16_t);
    if (len < expected) return;

    size_t copyLen = (size_t)len < sizeof(sPacket) ? (size_t)len : sizeof(sPacket);
    portENTER_CRITICAL_ISR(&sMux);
    memcpy(&sPacket, data, copyLen);
    sFresh = true;
    portEXIT_CRITICAL_ISR(&sMux);
}

void commsInit()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(onRecv);
    Serial.printf("ESP-NOW ready  MAC=%s\n", WiFi.macAddress().c_str());
}

bool commsFresh()
{
    bool f;
    portENTER_CRITICAL(&sMux);
    f      = sFresh;
    sFresh = false;
    portEXIT_CRITICAL(&sMux);
    return f;
}

void commsGet(CommsPacket &out)
{
    portENTER_CRITICAL(&sMux);
    out = sPacket;
    portEXIT_CRITICAL(&sMux);
}
