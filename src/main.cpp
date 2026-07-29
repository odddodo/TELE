#include <Arduino.h>
#include <Preferences.h>
#include "graphics.h"
#include "comms.h"

static float smooth[COMMS_MAX_VALS]; // EMA-smoothed 0–1 values from remote pots

// ─── persisted state / recall ─────────────────────────────────────────────────
static const uint16_t STATE_MAGIC = 0x7E1E;    // bump if smooth[] layout changes
static const uint32_t COMMS_TIMEOUT_MS = 1200; // no packets this long ⇒ remote silent
static const float FADE_S = 3.0f;              // glide-to-saved duration (seconds)
static const uint32_t BTN_DEBOUNCE_MS = 200;   // save-button debounce / repeat lock

static Preferences prefs;
static float saved[COMMS_MAX_VALS]; // last "liked" state (mirror of NVS blob)
static uint32_t lastPacketMs = 0;
static bool everReceived = false; // gates commsActive until first packet

static inline float toScale(float v) { return SCALE_MIN + v * (SCALE_MAX - SCALE_MIN); }

void stateInit()
{
    bool ok = false;
    prefs.begin("tele", true); // read-only; missing namespace ⇒ defaults
    if (prefs.getUShort("magic", 0) == STATE_MAGIC &&
        prefs.getBytesLength("pots") == sizeof(saved))
    {
        prefs.getBytes("pots", saved, sizeof(saved));
        ok = true;
    }
    prefs.end();

    if (!ok)
        for (int k = 0; k < COMMS_MAX_VALS; k++)
            saved[k] = 0.5f; // first-boot fallback

    for (int k = 0; k < COMMS_MAX_VALS; k++)
        smooth[k] = saved[k]; // come up correct, no ramp
}

void stateSave()
{
    prefs.begin("tele", false);
    prefs.putUShort("magic", STATE_MAGIC);
    prefs.putBytes("pots", saved, sizeof(saved));
    prefs.end();
    Serial.println("state saved");
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
    pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);

    stateInit(); // load saved[] from NVS (or 0.5 fallback) and seed smooth[] = saved[]

    graphicsInit(0);
    commsInit();

    Serial.println("ready");
}

void loop()
{
    // ── time base ──────────────────────────────────────────────────────────────
    uint32_t now = millis();
    static uint32_t lastLoopMs = 0;
    float dtSec = lastLoopMs ? (now - lastLoopMs) * 0.001f : 0.0f;
    float fps = lastLoopMs ? 1000.0f / (now - lastLoopMs) : 0.0f;
    lastLoopMs = now;

    // ── absorb latest ESP-NOW packet (existing EMA, plus liveness tracking) ─────
    if (commsFresh())
    {
        CommsPacket pkt;
        commsGet(pkt);
        for (int k = 0; k < pkt.count && k < COMMS_MAX_VALS; k++)
        {
            float raw = pkt.vals[k] / 4095.0f;
            smooth[k] += 0.15f * (raw - smooth[k]);
        }
        lastPacketMs = now;
        everReceived = true;
    }

    // ── remote liveness + glide-to-saved on silence ────────────────────────────
    bool commsActive = everReceived && (now - lastPacketMs < COMMS_TIMEOUT_MS);

    static bool wasActive = false;
    static float fadeStart[COMMS_MAX_VALS];
    static float fadeT = 0.0f;

    if (wasActive && !commsActive) // remote just fell silent
    {
        for (int k = 0; k < COMMS_MAX_VALS; k++)
            fadeStart[k] = smooth[k];
        fadeT = 0.0f;
    }
    wasActive = commsActive;

    if (everReceived && !commsActive) // glide smooth[] → saved[]
    {
        fadeT += dtSec;
        float f = fadeT / FADE_S;
        if (f > 1.0f)
            f = 1.0f;
        for (int k = 0; k < COMMS_MAX_VALS; k++)
            smooth[k] = fadeStart[k] + (saved[k] - fadeStart[k]) * f;
    }

    // ── save button (PIN_BUTTON_UP, active-low) ────────────────────────────────
    static bool btnPrev = true; // released = HIGH
    static uint32_t btnMs = 0;
    bool btnNow = digitalRead(PIN_BUTTON_UP);
    if (btnPrev && !btnNow && (now - btnMs) > BTN_DEBOUNCE_MS)
    {
        btnMs = now;
        for (int k = 0; k < COMMS_MAX_VALS; k++)
            saved[k] = smooth[k];
        stateSave();
    }
    btnPrev = btnNow;

    // ── pot 14/15 → symmetry mode (H and V independently) ──────────────────
    // each pot: [0, 1/3) = none, [1/3, 2/3) = mirror, [2/3, 1] = doubled/folded
    // missing combos (mirror+doubled across axes): doubled axis wins
    int sym;
    {
        int hMode = smooth[14] < (1.0f / 3.0f) ? 0 : smooth[14] < (2.0f / 3.0f) ? 1
                                                                                : 2;
        int vMode = smooth[15] < (1.0f / 3.0f) ? 0 : smooth[15] < (2.0f / 3.0f) ? 1
                                                                                : 2;
        static const int symTable[3][3] = {{1, 3, 6}, {2, 4, 6}, {5, 5, 7}};
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
    // 12:  palette fold multiplier (1x-10x); repeats the palette mapping across the LUT
    //      (blur channel time scale is now fixed at 0.5, no longer pot-driven)
    // 13:  glitch amount (0 = clean, 1 = full mayhem; latch triggered by knob motion)
    // 14:  horizontal symmetry (none / mirror / doubled)
    // 15:  vertical symmetry  (none / mirror / doubled)
    float tscA = smooth[4] * TSCALE_MAX;
    float tscB = smooth[5] * TSCALE_MAX;
    float tscC = 0.1f * TSCALE_MAX;
    float sfA = SF_MIN + smooth[6] * (SF_MAX - SF_MIN);
    float sfB = SF_MIN + smooth[7] * (SF_MAX - SF_MIN);
    float sfC = SFC_MIN + smooth[10] * (SFC_MAX - SFC_MIN);
    float sharp = SHARP_MIN + smooth[9] * (SHARP_MAX - SHARP_MIN);
    float blur = smooth[11];

    float scAX = toScale(smooth[0]), scAY = toScale(smooth[1]);
    float scBX = toScale(smooth[2]), scBY = toScale(smooth[3]);

    static float lastPalT = -999.0f;
    static float lastPalFold = -999.0f;
    float palT = smooth[8] * (PALETTE_COUNT - 1);
    float palFold = 1.0f + smooth[12] * 9.0f; // pot 12 → palette repeat 1x-10x
    if (fabsf(palT - lastPalT) > (1.0f / 512.0f) ||
        fabsf(palFold - lastPalFold) > (1.0f / 512.0f))
    {
        buildPaletteBlend(palT, palFold);
        lastPalT = palT;
        lastPalFold = palFold;
    }
    // ── pot 13 → glitch amount + latch trigger ─────────────────────────────────
    float gGlitch = smooth[13];
    static float prevG13 = 0.0f;
    bool glitchMoving = fabsf(gGlitch - prevG13) > LATCH_DEADBAND;
    prevG13 = gGlitch;

    renderFrame(sharp, scAX, scAY, scBX, scBY, sfA, sfB, sfC, blur, sym);
    glitchUpdate(gGlitch, glitchMoving); // latch tables from fresh smoothCoarse
    glitchApply();                       // combined-gather geometry on fb
    pushToPanel();
    graphicsTick(0.008f * tscA, 0.001f * tscB, 0.004f * tscC);

    Serial.printf("fps=%.1f  scAX=%.2f scAY=%.2f  scBX=%.2f scBY=%.2f  tscA=%.2f tscB=%.2f tscC=%.2f  sfA=%.1f sfB=%.1f  pal=%.2f fold=%.2f sharp=%.1f  sfC=%.1f blur=%.2f  sym=%d\n",
                  fps, scAX, scAY, scBX, scBY, tscA, tscB, tscC, sfA, sfB,
                  palT, palFold, sharp, sfC, blur, sym);
}
