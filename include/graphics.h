#pragma once
#include <stdint.h>

#define W 64
#define H 64
#define CW 32 // coarse noise grid
#define CH 32
#define BITPLANES 6
#define PALETTE_COUNT 10

// ─── colour type — 0x00RRGGBB, same model as FastLED CRGB ────────────────────
typedef uint32_t Color;

namespace Col
{
    const Color Black = 0x000000;
    const Color White = 0xFFFFFF;
    const Color Gray = 0x808080;
    const Color Red = 0xFF0000;
    const Color Orange = 0xFF8000;
    const Color Yellow = 0xFFFF00;
    const Color Green = 0x00FF00;
    const Color Blue = 0x0033FF;
    const Color Cyan = 0x00CCFF;
    const Color Violet = 0x6600CC;
    const Color DarkRed = 0x800000;
    const Color DarkBlue = 0x000080;
    const Color DarkGreen = 0x003300;
    const Color DarkViolet = 0x330066;
    const Color Purple = 0x800080;
}
// ─────────────────────────────────────────────────────────────────────────────

// ─── parameter ranges ─────────────────────────────────────────────────────────
// Edit here to adjust how pots map to rendering parameters.

static const float SHARP_MIN = 1.0f; // pot 9   softXor sharpness
static const float SHARP_MAX = 15.0f;

static const float SCALE_MIN = 0.01f; // pots 0–3  noise spatial scale
static const float SCALE_MAX = 5.0f;

static const float TSCALE_MAX = 2.0f; // pots 4–5  time speed × base rate (0 = frozen)

static const float SF_MIN = 0.01f; // pots 6–7  sin fold frequency in cycles
static const float SF_MAX = 10.0f;

// ─────────────────────────────────────────────────────────────────────────────

void graphicsInit(int paletteIdx);
void buildPalette(int idx);
void buildPaletteBlend(float t); // t = paletteIdx + pot fraction → blends two adjacent palettes
void renderFrame(float soft, float scAX, float scAY, float scBX, float scBY,
                 float sfA, float sfB);
void pushToPanel();
void graphicsTick(float dtG, float dtQ); // advance gtime / qtime
const char *paletteName(int idx);
