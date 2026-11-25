#include "comms.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

static CommsPacket   sPacket;
static volatile bool sFresh = false;
static portMUX_TYPE  sMux   = portMUX_INITIALIZER_UNLOCKED;

// command slot — one is enough, the remote blocks on each command until ACK
static const uint32_t CMD_DEDUP_MS = 2000;
static CommsCmd sCmd;
static volatile bool sCmdPending = false;
static bool     sCmdIsNew = false;
static uint8_t  sLastCmd = 0, sLastSeq = 0;
static uint32_t sLastCmdMs = 0;
static bool     sHaveLast = false;
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// arduino-esp32 3.x changed the recv callback signature
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
#else
static void onRecv(const uint8_t *mac, const uint8_t *data, int len)
#endif
{
    if (len >= (int)sizeof(CommsCmd) && data[0] == COMMS_CMD_MAGIC)
    {
        const CommsCmd *c = (const CommsCmd *)data;
        if (c->cmd == CMD_ACK) return; // not for us
        uint32_t t = millis();
        bool dup = sHaveLast && c->cmd == sLastCmd && c->seq == sLastSeq &&
                   (t - sLastCmdMs) < CMD_DEDUP_MS;
        portENTER_CRITICAL_ISR(&sMux);
        sCmd = *c;
        sCmdIsNew = !dup;
        sCmdPending = true;
        portEXIT_CRITICAL_ISR(&sMux);
        if (!dup) { sLastCmd = c->cmd; sLastSeq = c->seq; sHaveLast = true; }
        sLastCmdMs = t;
        return;
    }

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

    // broadcast peer so we can send ACKs back to the remote
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        Serial.println("ESP-NOW add broadcast peer failed (no ACKs)");
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

bool commsTakeCmd(CommsCmd &out, bool &isNew)
{
    bool got;
    portENTER_CRITICAL(&sMux);
    got = sCmdPending;
    if (got)
    {
        out = sCmd;
        isNew = sCmdIsNew;
        sCmdPending = false;
    }
    portEXIT_CRITICAL(&sMux);
    return got;
}

void commsAck(uint8_t cmd, uint8_t seq)
{
    CommsCmd m = {COMMS_CMD_MAGIC, CMD_ACK, seq, cmd};
    esp_now_send(BCAST, (const uint8_t *)&m, sizeof(m));
}
