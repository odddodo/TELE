#pragma once
#include <stdint.h>

#define COMMS_MAX_VALS 16   // supports 8–16 values per packet

// Same struct on sender and receiver — keep in sync.
struct __attribute__((packed)) CommsPacket
{
    uint8_t  count;                   // how many vals are valid (8–16)
    uint16_t vals[COMMS_MAX_VALS];    // raw ADC, 0–4095
};

void commsInit();
bool commsFresh();                    // true once per received packet, then resets
void commsGet(CommsPacket &out);      // copy of the latest packet
