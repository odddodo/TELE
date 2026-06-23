#include <Arduino.h>
#include "graphics.h"
#include "comms.h"

static float smooth[COMMS_MAX_VALS];   // EMA-smoothed 0–1 values from remote pots

static inline float toScale(float v) { return SCALE_MIN + v * (SCALE_MAX - SCALE_MIN); }

void setup()
{
    Serial.begin(115200);
    delay(500);

    // start smooth[] at mid-range so the noise looks reasonable before any packet arrives
    for (int k = 0; k < COMMS_MAX_VALS; k++) smooth[k] = 0.5f;

    graphicsInit(0);
    commsInit();

    Serial.println("ready");
}

void loop()
{
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

    // ── map all 13 pots ─────────────────────────────────────────────────────
    // 0–3: noise spatial scale X/Y per channel
    // 4–5: animation time scale per channel (0 = frozen)
    // 6–7: sinusoidal color-fold frequency A/B channels
    // 8:   palette position sweeping all palettes (0 = first, 1 = last)
    // 9:   softXor sharpness
    // 10:  sin fold frequency blur channel C
    // 11:  blur amount (0 = off, 1 = full)
    // 12:  blur channel time scale (0 = frozen, matches tscA/tscB behaviour)
    float tscA  = smooth[4] * TSCALE_MAX;
    float tscB  = smooth[5] * TSCALE_MAX;
    float tscC  = smooth[12] * TSCALE_MAX;
    float sfA   = SF_MIN  + smooth[6]  * (SF_MAX  - SF_MIN);
    float sfB   = SF_MIN  + smooth[7]  * (SF_MAX  - SF_MIN);
    float sfC   = SFC_MIN + smooth[10] * (SFC_MAX - SFC_MIN);
    float sharp = SHARP_MIN + smooth[9] * (SHARP_MAX - SHARP_MIN);
    float blur  = smooth[11];

    float scAX = toScale(smooth[0]), scAY = toScale(smooth[1]);
    float scBX = toScale(smooth[2]), scBY = toScale(smooth[3]);

    static float lastPalT = -999.0f;
    float palT = smooth[8] * (PALETTE_COUNT - 1);
    if (fabsf(palT - lastPalT) > (1.0f / 512.0f)) {
        buildPaletteBlend(palT);
        lastPalT = palT;
    }
    renderFrame(sharp, scAX, scAY, scBX, scBY, sfA, sfB, sfC, blur);
    pushToPanel();
    graphicsTick(0.008f * tscA, 0.001f * tscB, 0.004f * tscC);

    Serial.printf("scAX=%.2f scAY=%.2f  scBX=%.2f scBY=%.2f  tscA=%.2f tscB=%.2f tscC=%.2f  sfA=%.1f sfB=%.1f  pal=%.2f sharp=%.1f  sfC=%.1f blur=%.2f\n",
                  scAX, scAY, scBX, scBY, tscA, tscB, tscC, sfA, sfB,
                  palT, sharp, sfC, blur);
}
