#pragma once

#define W 64
#define H 64
#define CW 32 // coarse noise grid
#define CH 32
#define BITPLANES 6
#define PALETTE_COUNT 10

// ─── parameter ranges ─────────────────────────────────────────────────────────
// Edit here to adjust how pots map to rendering parameters.

static const float SHARP = 7.0f; // softXor sharpness (fixed, not pot-mapped)

static const float SCALE_MIN = 0.1f; // pots 0–3  noise spatial scale
static const float SCALE_MAX = 10.0f;

static const float TSCALE_MAX = 2.0f; // pots 4–5  time speed × base rate (0 = frozen)

static const float SF_MIN = 0.1f; // pots 6–7  sin fold frequency in cycles
static const float SF_MAX = 30.0f;

// ─────────────────────────────────────────────────────────────────────────────

void graphicsInit(int paletteIdx);
void buildPalette(int idx);
void renderFrame(float soft, float scAX, float scAY, float scBX, float scBY,
                 float sfA, float sfB);
void pushToPanel();
void graphicsTick(float dtG, float dtQ); // advance gtime / qtime
const char *paletteName(int idx);
