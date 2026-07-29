#pragma once
#include <stdint.h>

// ─── pot → parameter mapping ──────────────────────────────────────────────────
//  0   noise spatial scale X  — channel A   (SCALE_MIN–SCALE_MAX)
//  1   noise spatial scale Y  — channel A   (SCALE_MIN–SCALE_MAX)
//  2   noise spatial scale X  — channel B   (SCALE_MIN–SCALE_MAX)
//  3   noise spatial scale Y  — channel B   (SCALE_MIN–SCALE_MAX)
//  4   animation time scale   — channel A   (0 = frozen, max = TSCALE_MAX)
//  5   animation time scale   — channel B   (0 = frozen, max = TSCALE_MAX)
//  6   sin-fold frequency     — channel A   (SF_MIN–SF_MAX)
//  7   sin-fold frequency     — channel B   (SF_MIN–SF_MAX)
//  8   palette blend position              (0 = first palette, 1 = last)
//  9   softXor sharpness                   (SHARP_MIN–SHARP_MAX)
// 10   sin-fold frequency     — channel C (blur)  (SFC_MIN–SFC_MAX)
// 11   blur amount                         (0 = off, 1 = full)
// 12   palette fold multiplier                    (1x–10x, repeats palette mapping across LUT)
//      (channel C blur time scale is now fixed at 0.5, no longer pot-driven)
// 13   glitch amount                      (0 = clean, 1 = full mayhem; latches on motion)
// 14   horizontal symmetry                 (0–⅓ = none, ⅓–⅔ = mirror, ⅔–1 = doubled)
// 15   vertical symmetry                   (0–⅓ = none, ⅓–⅔ = mirror, ⅔–1 = doubled)
// ─────────────────────────────────────────────────────────────────────────────

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
