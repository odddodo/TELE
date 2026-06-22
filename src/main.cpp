#include <Arduino.h>
#include "graphics.h"
#include "comms.h"

#define BTN_UP   6
#define BTN_DOWN 7

static int   paletteIdx = 0;
static float smooth[COMMS_MAX_VALS];   // EMA-smoothed 0–1 values from remote pots

static inline float toScale(float v) { return SCALE_MIN + v * (SCALE_MAX - SCALE_MIN); }

void setup()
{
    Serial.begin(115200);
    delay(500);
    pinMode(BTN_UP,   INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);

    // start smooth[] at mid-range so the noise looks reasonable before any packet arrives
    for (int k = 0; k < COMMS_MAX_VALS; k++) smooth[k] = 0.5f;

    graphicsInit(paletteIdx);
    commsInit();

    Serial.printf("ready  palette=%s\n", paletteName(paletteIdx));
}

void loop()
{
    // ── palette switching (falling-edge detect, no blocking delay) ──────────
    static bool upPrev = true, downPrev = true;
    bool upNow   = digitalRead(BTN_UP);
    bool downNow = digitalRead(BTN_DOWN);
    if (!upNow && upPrev)
    {
        paletteIdx = (paletteIdx + 1) % PALETTE_COUNT;
        buildPalette(paletteIdx);
        Serial.printf("palette=%s\n", paletteName(paletteIdx));
    }
    if (!downNow && downPrev)
    {
        paletteIdx = (paletteIdx + 9) % PALETTE_COUNT;   // +9 mod 10 = -1 with wrap
        buildPalette(paletteIdx);
        Serial.printf("palette=%s\n", paletteName(paletteIdx));
    }
    upPrev   = upNow;
    downPrev = downNow;

    // ── absorb latest ESP-NOW packet into smoothed values ───────────────────
    if (commsFresh())
    {
        CommsPacket pkt;
        commsGet(pkt);
        for (int k = 0; k < pkt.count && k < COMMS_MAX_VALS; k++)
        {
            float raw = pkt.vals[k] / 4095.0f;
            smooth[k] += 0.15f * (raw - smooth[k]);   // α = 0.15 EMA
        }
    }

    // ── map all 8 pots ──────────────────────────────────────────────────────
    // 0–3: noise spatial scale X/Y per channel (1–10)
    // 4–5: animation time scale per channel (0 = frozen, 1 = base rate, 4 = 4×)
    // 6–7: sinusoidal color-fold frequency per channel (1–8 cycles)
    float tscA = smooth[4] * TSCALE_MAX;
    float tscB = smooth[5] * TSCALE_MAX;
    float sfA  = SF_MIN + smooth[6] * (SF_MAX - SF_MIN);
    float sfB  = SF_MIN + smooth[7] * (SF_MAX - SF_MIN);

    uint32_t t0 = micros();
    renderFrame(SHARP,
                toScale(smooth[0]), toScale(smooth[1]),   // ch A  scl x, scl y
                toScale(smooth[2]), toScale(smooth[3]),   // ch B  scl x, scl y
                sfA, sfB);                                // sin freq ch A, ch B
    uint32_t tRender = micros() - t0;

    uint32_t t1 = micros();
    pushToPanel();
    uint32_t tPush = micros() - t1;

    graphicsTick(0.008f * tscA, 0.001f * tscB);

    Serial.printf("render %lu us | push %lu us | fps %.1f\n",
                  (unsigned long)tRender, (unsigned long)tPush,
                  1e6f / (tRender + tPush));
}
