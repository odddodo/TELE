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

    // ── pot 14/15 → symmetry mode (H and V independently) ──────────────────
    // each pot: [0, 1/3) = none, [1/3, 2/3) = mirror, [2/3, 1] = doubled/folded
    // missing combos (mirror+doubled across axes): doubled axis wins
    int sym;
    {
        int hMode = smooth[14] < (1.0f/3.0f) ? 0 : smooth[14] < (2.0f/3.0f) ? 1 : 2;
        int vMode = smooth[15] < (1.0f/3.0f) ? 0 : smooth[15] < (2.0f/3.0f) ? 1 : 2;
        static const int symTable[3][3] = {{1,3,6},{2,4,6},{5,5,7}};
        sym = symTable[hMode][vMode];
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
    // 13:  glitch amount (0 = clean, 1 = full mayhem; latch triggered by knob motion)
    // 14:  horizontal symmetry (none / mirror / doubled)
    // 15:  vertical symmetry  (none / mirror / doubled)
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

    // ── pot 13 → glitch amount + latch trigger ─────────────────────────────────
    float gGlitch = smooth[13];
    static float prevG13 = 0.0f;
    bool glitchMoving = fabsf(gGlitch - prevG13) > LATCH_DEADBAND;
    prevG13 = gGlitch;

    renderFrame(sharp, scAX, scAY, scBX, scBY, sfA, sfB, sfC, blur, sym);
    glitchUpdate(gGlitch, glitchMoving);   // latch tables from fresh smoothCoarse
    glitchApply();                         // combined-gather geometry on fb
    pushToPanel();
    graphicsTick(0.008f * tscA, 0.001f * tscB, 0.004f * tscC);

    Serial.printf("fps=%.1f  scAX=%.2f scAY=%.2f  scBX=%.2f scBY=%.2f  tscA=%.2f tscB=%.2f tscC=%.2f  sfA=%.1f sfB=%.1f  pal=%.2f sharp=%.1f  sfC=%.1f blur=%.2f  sym=%d\n",
                  fps, scAX, scAY, scBX, scBY, tscA, tscB, tscC, sfA, sfB,
                  palT, sharp, sfC, blur, sym);
}
