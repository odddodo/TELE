#include <Arduino.h>
#include "graphics.h"
#include "comms.h"

static float smooth[COMMS_MAX_VALS];   // EMA-smoothed 0–1 values from remote pots

static inline float toScale(float v) { return SCALE_MIN + v * (SCALE_MAX - SCALE_MIN); }

void setup()
{
    Serial.begin(115200);
    delay(500);

    pinMode(PIN_BUTTON_UP,   INPUT_PULLUP);
    pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

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

    // ── on-board buttons cycle symmetry mode (UP=next, DOWN=prev) ───────────
    // modes 1–7: none, H, V, H+V, doubled-H, doubled-V, doubled-H+V
    static int     sym      = 1;
    static bool    upPrev   = HIGH, downPrev = HIGH;
    static uint32_t upMs   = 0,    downMs   = 0;
    {
        bool upNow   = digitalRead(PIN_BUTTON_UP);
        bool downNow = digitalRead(PIN_BUTTON_DOWN);
        uint32_t ms  = millis();
        if (upPrev == HIGH && upNow == LOW && ms - upMs > 200)
            { sym = sym % 7 + 1; upMs = ms; }
        if (downPrev == HIGH && downNow == LOW && ms - downMs > 200)
            { sym = (sym == 1) ? 7 : sym - 1; downMs = ms; }
        upPrev = upNow; downPrev = downNow;
    }

    // ── map remote pots ──────────────────────────────────────────────────────
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
    static uint32_t lastFrameMs = 0;
    uint32_t now = millis();
    float fps = lastFrameMs ? 1000.0f / (now - lastFrameMs) : 0.0f;
    lastFrameMs = now;

    renderFrame(sharp, scAX, scAY, scBX, scBY, sfA, sfB, sfC, blur, sym);
    pushToPanel();
    graphicsTick(0.008f * tscA, 0.001f * tscB, 0.004f * tscC);

    Serial.printf("fps=%.1f  scAX=%.2f scAY=%.2f  scBX=%.2f scBY=%.2f  tscA=%.2f tscB=%.2f tscC=%.2f  sfA=%.1f sfB=%.1f  pal=%.2f sharp=%.1f  sfC=%.1f blur=%.2f  sym=%d\n",
                  fps, scAX, scAY, scBX, scBY, tscA, tscB, tscC, sfA, sfB,
                  palT, sharp, sfC, blur, sym);
}
