#include "graphics.h"
#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <math.h>

// ─── hardware ─────────────────────────────────────────────────────────────────
static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
static Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ─── animation time ───────────────────────────────────────────────────────────
static float gtime = 0.0f;
static float qtime = 0.0f;
static float btime = 0.0f; // blur channel — independent of A/B

void graphicsTick(float dtG, float dtQ, float dtC)
{
    gtime += dtG;
    qtime += dtQ;
    btime += dtC;
}

// ─── buffers ──────────────────────────────────────────────────────────────────
static float coarse[CW * CH];
static float smoothCoarse[CW * CH]; // temporal low-pass state (flicker control)
static float mip1[16 * 16];
static float mip2[8 * 8];
static float blurC[BW * BH]; // C channel — spatially-varying blur weight
static uint16_t fb[W * H];
static uint16_t paletteLUT[256];
static uint16_t paletteLUT2[256]; // scratch for palette blend

// ─── glitch state (frozen remaps; rebuilt only on knob motion) ─────────────────
static int      rowShift[H]           = {0}; // horizontal displacement per dest row
static int      colShift[W]           = {0}; // vertical displacement per dest column
static int      dxR = 0, dxB = 0;            // chromatic split (R/B source x offset)
static uint8_t  glitchColorXor[CW * CH] = {0}; // per-cell index-xor (0 = identity)
static uint32_t latchSeed              = 0;   // frozen seed; reselects while knob moves
static bool     glitchActive           = false;
static uint16_t fbSnap[W * H];               // source snapshot for the gather

// ─── math helpers ─────────────────────────────────────────────────────────────
static inline int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

// field-sample coordinates for optional field-weight term in glitch seeding
static const int AMT_COL = 23; // column of smoothCoarse for row field term
static const int AMT_ROW = 23; // row of smoothCoarse for column field term

static inline uint32_t hash32(uint32_t x)   // lowbias32 finalizer, good distribution
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static inline int ffloor(float x)
{
    int xi = (int)x;
    return (x < xi) ? xi - 1 : xi;
}

static const float INV24 = 1.0f / 16777215.0f;

static inline float hash3(int x, int y, int z, uint32_t s)
{
    uint32_t h = (uint32_t)(x * 374761393) ^ (uint32_t)(y * 668265263) ^
                 (uint32_t)(z * 2147483647u) ^ (s * 1013904223u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) * INV24;
}

static inline float smoothf(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

#define SINLUT_N 256
static float sinLUT[SINLUT_N + 1];
static void buildSinLUT()
{
    for (int i = 0; i <= SINLUT_N; i++)
        sinLUT[i] = sinf(TWO_PI * (float)i / SINLUT_N);
}
// sin(2*pi*turns) via LUT; turns must be >= 0 (always true here: A,B,C in [0,1], sf >= 0)
static inline float fastSin(float turns)
{
    turns -= ffloor(turns);
    float f = turns * SINLUT_N;
    int i = (int)f;
    float fr = f - i;
    return sinLUT[i] + (sinLUT[i + 1] - sinLUT[i]) * fr;
}

static float vnoise(float x, float y, float z, uint32_t s)
{
    int xi = ffloor(x), yi = ffloor(y), zi = ffloor(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    float u = smoothf(xf), v = smoothf(yf), w = smoothf(zf);
    float c000 = hash3(xi, yi, zi, s), c100 = hash3(xi + 1, yi, zi, s);
    float c010 = hash3(xi, yi + 1, zi, s), c110 = hash3(xi + 1, yi + 1, zi, s);
    float c001 = hash3(xi, yi, zi + 1, s), c101 = hash3(xi + 1, yi, zi + 1, s);
    float c011 = hash3(xi, yi + 1, zi + 1, s), c111 = hash3(xi + 1, yi + 1, zi + 1, s);
    float x00 = lerpf(c000, c100, u), x10 = lerpf(c010, c110, u);
    float x01 = lerpf(c001, c101, u), x11 = lerpf(c011, c111, u);
    return lerpf(lerpf(x00, x10, v), lerpf(x01, x11, v), w);
}

static const float SOFTXOR_W[BITPLANES] = {0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f};
static const float SOFTXOR_INVNORM = 1.0f / 0.984375f; // 1 / (sum of weights, 63/64)
static const float FBM_INVN = 1.0f / 0.9f;             // fbm n is always 0.6 + 0.3
static const float FLICK_AMIN = 0.08f;                 // min smoothing when near-still (lower = calmer)
static const float FLICK_GAIN = 40.0f;                 // how fast real motion re-opens the filter (higher = snappier)

static inline float fbm(float x, float y, float z, uint32_t s)
{
    float val = 0, a = 0.6f, f = 1.0f;
    for (int o = 0; o < 2; o++)
    {
        val += a * vnoise(x * f, y * f, z * f, s + o * 17);
        a *= 0.5f;
        f *= 2.3f;
    }
    return val * FBM_INVN;
}

static inline float tri(float p)
{
    p -= ffloor(p);
    if (p < 0.25f)
        return 4.0f * p;
    if (p < 0.75f)
        return 2.0f - 4.0f * p;
    return 4.0f * p - 4.0f;
}

static inline float softXor(float a, float b, float soft)
{
    float sum = 0.0f, freq = 1.0f;
    for (int k = 0; k < BITPLANES; k++)
    {
        float sa = 0.5f + 0.5f * soft * tri(a * freq);
        float sb = 0.5f + 0.5f * soft * tri(b * freq);
        sa = sa < 0 ? 0 : (sa > 1 ? 1 : sa);
        sb = sb < 0 ? 0 : (sb > 1 ? 1 : sb);
        float x = sa + sb - 2.0f * sa * sb;
        sum += SOFTXOR_W[k] * x;
        freq *= 2.0f;
    }
    return sum * SOFTXOR_INVNORM;
}

static inline float sampleGrid(const float *buf, int w, int h, float fx, float fy)
{
    if (fx < 0)
        fx = 0;
    if (fx > w - 1)
        fx = w - 1;
    if (fy < 0)
        fy = 0;
    if (fy > h - 1)
        fy = h - 1;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;
    float a = lerpf(buf[y0 * w + x0], buf[y0 * w + x1], tx);
    float b = lerpf(buf[y1 * w + x0], buf[y1 * w + x1], tx);
    return lerpf(a, b, ty);
}

static inline uint16_t rgb565(float r, float g, float b)
{
    uint16_t R = (uint16_t)(r * 31) & 0x1F;
    uint16_t G = (uint16_t)(g * 63) & 0x3F;
    uint16_t B = (uint16_t)(b * 31) & 0x1F;
    return (R << 11) | (G << 5) | B;
}

static inline uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
    int ra = (a >> 11) & 0x1F, rb = (b >> 11) & 0x1F;
    int ga = (a >> 5) & 0x3F, gb = (b >> 5) & 0x3F;
    int ba = a & 0x1F, bb = b & 0x1F;
    int r = (int)(ra + (rb - ra) * t);
    int g = (int)(ga + (gb - ga) * t);
    int bl = (int)(ba + (bb - ba) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static inline uint16_t lerpColor(Color c0, Color c1, float dt)
{
    float r0 = ((c0 >> 16) & 0xFF) * (1.0f / 255.0f);
    float g0 = ((c0 >> 8) & 0xFF) * (1.0f / 255.0f);
    float b0 = (c0 & 0xFF) * (1.0f / 255.0f);
    float r1 = ((c1 >> 16) & 0xFF) * (1.0f / 255.0f);
    float g1 = ((c1 >> 8) & 0xFF) * (1.0f / 255.0f);
    float b1 = (c1 & 0xFF) * (1.0f / 255.0f);
    return rgb565(r0 + dt * (r1 - r0), g0 + dt * (g1 - g0), b0 + dt * (b1 - b0));
}

// ─── palettes ─────────────────────────────────────────────────────────────────
// Each stop: { position 0–1, Color (0x00RRGGBB hex or Col:: named constant) }
// Use Col:: names for standard colours; use hex literals for custom shades.
struct Stop
{
    float t;
    Color c;
};
struct Palette
{
    const char *name;
    Stop stops[16];
    int n;
};

// Each triplet between blacks: [deep-navy, mid, brightest] at stops {0,4,8,12}, {1,5,9,13}, {2,6,10,14}.
// Deep-navy (0x000066) is at gap 0.030 from each adjacent black (was 0.020, +50%) → blue spike.
// Two edits applied on top of the original design, each keeping total widths (and the fixed
// 0/0.200/0.467/0.733/1.000 black anchors) unchanged:
//  1. mid→brightest span (the bright colour band) narrowed 70%, freed space folded into
//     navy→mid and brightest→black proportionally.
//  2. black→navy spike and brightest→black span (black band) each expanded 50%, funded by
//     shrinking navy→mid and mid→brightest (mid/bright bands) proportionally.
// Mid/bright colors sourced from maly_janusz myPalettes.h — no generic Col:: primaries.
static const Palette palettes[PALETTE_COUNT] = {
    {"dark-spectral", {
                          {0.000f, 0x000022}, // deep navy
                          {0.017f, Col::Red},
                          {0.032f, Col::Yellow},
                          {0.200f, Col::Black},
                          {0.230f, 0x000022}, // deep navy
                          {0.350f, 0xFFA500}, // Orange (CRGB::Orange)
                          {0.370f, Col::Yellow},
                          {0.467f, Col::Black},
                          {0.497f, 0x000022}, // deep navy
                          {0.617f, 0x9932CC}, // DarkOrchid
                          {0.637f, Col::Yellow},
                          {0.733f, Col::Black},
                          {0.763f, 0x000022}, // deep navy
                          {0.883f, 0xFFA500}, // Orange
                          {0.903f, Col::Yellow},
                          {1.000f, Col::Black},
                      },
     16},
    {"spectral", {
                     {0.000f, 0x000022}, // deep navy
                     {0.017f, 0x9932CC}, // DarkOrchid
                     {0.032f, Col::Yellow},
                     {0.200f, Col::Black},
                     {0.230f, 0x000022}, // deep navy
                     {0.350f, 0xFFA500}, // Orange
                     {0.370f, 0xF8F8FF}, // GhostWhite
                     {0.467f, Col::Black},
                     {0.497f, 0x000022}, // deep navy
                     {0.617f, 0x00FFFF}, // Cyan (CRGB::Cyan)
                     {0.637f, 0xF8F8FF}, // GhostWhite
                     {0.733f, Col::Black},
                     {0.763f, 0x000022}, // deep navy
                     {0.883f, 0x9932CC}, // DarkOrchid
                     {0.903f, 0xF8F8FF}, // GhostWhite
                     {1.000f, Col::Black},
                 },
     16},
    {"fire", {
                 {0.000f, 0x000022}, // deep navy
                 {0.017f, 0x9ACD32}, // YellowGreen
                 {0.032f, Col::Yellow},
                 {0.200f, Col::Black},
                 {0.230f, 0x000022}, // deep navy
                 {0.350f, Col::Purple},
                 {0.370f, Col::Yellow},
                 {0.467f, Col::Black},
                 {0.497f, 0x000022}, // deep navy
                 {0.617f, 0x8B008B}, // DarkMagenta
                 {0.637f, Col::Yellow},
                 {0.733f, Col::Black},
                 {0.763f, 0x000022}, // deep navy
                 {0.883f, 0x9ACD32}, // YellowGreen
                 {0.903f, Col::Yellow},
                 {1.000f, Col::Black},
             },
     16},
    {"zebra", {
                  // no blue → deep navy added
                  {0.000f, 0x000022}, // deep navy
                  {0.017f, 0xA9A9A9}, // DarkGrey (CRGB::DarkGrey)
                  {0.032f, 0xF8F8FF}, // GhostWhite (CRGB::GhostWhite)
                  {0.200f, Col::Black},
                  {0.230f, 0x000022}, // deep navy
                  {0.350f, 0xA9A9A9}, // DarkGrey
                  {0.370f, 0xF8F8FF}, // GhostWhite
                  {0.467f, Col::Black},
                  {0.497f, 0x000022}, // deep navy
                  {0.617f, 0xA9A9A9}, // DarkGrey
                  {0.637f, 0xF8F8FF}, // GhostWhite
                  {0.733f, Col::Black},
                  {0.763f, 0x000022}, // deep navy
                  {0.883f, 0xA9A9A9}, // DarkGrey
                  {0.903f, 0xF8F8FF}, // GhostWhite
                  {1.000f, Col::Black},
              },
     16},
    {"hello", {
                  {0.000f, 0x000022}, // deep navy
                  {0.017f, Col::Red},
                  {0.032f, Col::Yellow},
                  {0.200f, Col::Black},
                  {0.230f, 0x000022}, // deep navy
                  {0.350f, 0xFFA500}, // Orange (CRGB::Orange)
                  {0.370f, Col::Yellow},
                  {0.467f, Col::Black},
                  {0.497f, 0x000022}, // deep navy
                  {0.617f, 0x006400}, // DarkGreen (CRGB::DarkGreen)
                  {0.637f, Col::Yellow},
                  {0.733f, Col::Black},
                  {0.763f, 0x000022}, // deep navy
                  {0.883f, 0xA9A9A9}, // DarkGrey
                  {0.903f, 0xF8F8FF}, // GhostWhite
                  {1.000f, Col::Black},
              },
     16},
    {"smoothie", {
                     // all colors exact from maly_janusz Smoothie
                     {0.000f, 0x000022}, // deep navy
                     {0.017f, 0x9932CC}, // DarkOrchid
                     {0.032f, 0xBDB76B}, // DarkKhaki
                     {0.200f, Col::Black},
                     {0.230f, 0x000022}, // deep navy
                     {0.350f, 0x808000}, // Olive
                     {0.370f, 0xBDB76B}, // DarkKhaki
                     {0.467f, Col::Black},
                     {0.497f, 0x000022}, // deep navy
                     {0.617f, 0xB8860B}, // DarkGoldenrod
                     {0.637f, 0xBDB76B}, // DarkKhaki
                     {0.733f, Col::Black},
                     {0.763f, 0x000022}, // deep navy
                     {0.883f, 0x8B008B}, // DarkMagenta
                     {0.903f, 0xBDB76B}, // DarkKhaki
                     {1.000f, Col::Black},
                 },
     16},
    {"xga", {
                // colors from maly_janusz XGAColors
                {0.000f, 0x000022},    // deep navy
                {0.017f, 0xFF00FF},    // Magenta (CRGB::Magenta)
                {0.032f, Col::Yellow}, // CRGB::Yellow
                {0.200f, Col::Black},
                {0.230f, 0x000022}, // deep navy
                {0.350f, 0xFF00FF}, // Magenta
                {0.370f, Col::Yellow},
                {0.467f, Col::Black},
                {0.497f, 0x000022}, // deep navy
                {0.617f, 0xFF00FF}, // Magenta
                {0.637f, Col::Yellow},
                {0.733f, Col::Black},
                {0.763f, 0x000022}, // deep navy
                {0.883f, 0xFF00FF}, // Magenta
                {0.903f, Col::Yellow},
                {1.000f, Col::Black},
            },
     16},
    {"arctic", {
                   // colors from maly_janusz Arctic
                   {0.000f, 0x000022},   // deep navy
                   {0.017f, Col::Red},   // CRGB::Red
                   {0.032f, Col::White}, // CRGB::White (maly_janusz Arctic uses White not GhostWhite)
                   {0.200f, Col::Black},
                   {0.230f, 0x000022}, // deep navy
                   {0.350f, 0x0000FF}, // Blue (CRGB::Blue)
                   {0.370f, Col::White},
                   {0.467f, Col::Black},
                   {0.497f, 0x000022}, // deep navy
                   {0.617f, 0xFFD700}, // Gold (CRGB::Gold)
                   {0.637f, Col::White},
                   {0.733f, Col::Black},
                   {0.763f, 0x000022}, // deep navy
                   {0.883f, Col::Red},
                   {0.903f, Col::White},
                   {1.000f, Col::Black},
               },
     16},
    {"italy", {
                  // colors from maly_janusz Italy
                  {0.000f, 0x000022}, // deep navy
                  {0.017f, Col::Red}, // CRGB::Red
                  {0.032f, 0xF8F8FF}, // GhostWhite (CRGB::GhostWhite)
                  {0.200f, Col::Black},
                  {0.230f, 0x000022}, // deep navy
                  {0.350f, 0x008000}, // Green (CRGB::Green)
                  {0.370f, 0xF8F8FF}, // GhostWhite
                  {0.467f, Col::Black},
                  {0.497f, 0x000022}, // deep navy
                  {0.617f, Col::Red},
                  {0.637f, 0xF8F8FF}, // GhostWhite
                  {0.733f, Col::Black},
                  {0.763f, 0x000022}, // deep navy
                  {0.883f, 0x008000}, // Green
                  {0.903f, 0xF8F8FF}, // GhostWhite
                  {1.000f, Col::Black},
              },
     16},
    {"hugme", {
                  // all colors exact from maly_janusz HugMeColors
                  {0.000f, 0x000022}, // deep navy
                  {0.017f, 0x808000}, // Olive
                  {0.032f, 0xFF69B4}, // HotPink
                  {0.200f, Col::Black},
                  {0.230f, 0x000022}, // deep navy
                  {0.350f, 0x9ACD32}, // YellowGreen
                  {0.370f, 0xFF69B4}, // HotPink
                  {0.467f, Col::Black},
                  {0.497f, 0x000022}, // deep navy
                  {0.617f, 0x9932CC}, // DarkOrchid
                  {0.637f, 0xFF69B4}, // HotPink
                  {0.733f, Col::Black},
                  {0.763f, 0x000022}, // deep navy
                  {0.883f, 0xB8860B}, // DarkGoldenrod
                  {0.903f, 0xFF69B4}, // HotPink
                  {1.000f, Col::Black},
              },
     16},
};

static void buildPaletteInto(int idx, uint16_t *lut, float fold)
{
    const Palette &p = palettes[idx];
    for (int i = 0; i < 256; i++)
    {
        // fold repeats the 0.000f–1.000f palette mapping `fold` times across
        // the LUT (1x = original single pass); index 255 always closes on
        // the final stop so the top of the LUT still lands exactly on it.
        float t = (i == 255) ? 1.0f : fmodf((i / 255.0f) * fold, 1.0f);
        int j = 0;
        while (j < p.n - 2 && t > p.stops[j + 1].t)
            j++;
        float dt = (t - p.stops[j].t) / (p.stops[j + 1].t - p.stops[j].t);
        lut[i] = lerpColor(p.stops[j].c, p.stops[j + 1].c, dt);
    }
}

void buildPalette(int idx) { buildPaletteInto(idx, paletteLUT, 1.0f); }

void buildPaletteBlend(float t, float fold)
{
    int a = (int)t % PALETTE_COUNT;
    float fr = t - (int)t;
    int b = (a + 1) % PALETTE_COUNT;

    buildPaletteInto(a, paletteLUT, fold);
    if (fr < 0.005f)
        return;

    buildPaletteInto(b, paletteLUT2, fold);
    for (int i = 0; i < 256; i++)
    {
        uint16_t ca = paletteLUT[i], cb = paletteLUT2[i];
        int ra = (ca >> 11) & 0x1F, rb = (cb >> 11) & 0x1F;
        int ga = (ca >> 5) & 0x3F, gb = (cb >> 5) & 0x3F;
        int ba = ca & 0x1F, bb = cb & 0x1F;
        paletteLUT[i] = ((uint16_t)(ra + (int)(fr * (rb - ra))) << 11) |
                        ((uint16_t)(ga + (int)(fr * (gb - ga))) << 5) |
                        (uint16_t)(ba + (int)(fr * (bb - ba)));
    }
}

const char *paletteName(int idx) { return palettes[idx].name; }

// ─── render ───────────────────────────────────────────────────────────────────
void renderFrame(float soft, float scAX, float scAY, float scBX, float scBY,
                 float sfA, float sfB, float sfC, float blurAmount, int sym)
{
    // ── A/B noise → softXor (unique region only, then mirror) ────────────────
    int uniJ = (sym == 6 || sym == 7) ? CH / 4 : (sym == 3 || sym == 4) ? CH / 2
                                                                        : CH;
    int uniI = (sym == 5 || sym == 7) ? CW / 4 : (sym == 2 || sym == 4) ? CW / 2
                                                                        : CW;
    for (int j = 0; j < uniJ; j++)
    {
        float ny = (float)j / CH;
        for (int i = 0; i < uniI; i++)
        {
            float nx = (float)i / CW;
            float A = fbm(nx * scAX, ny * scAY, gtime, 1);
            float B = fbm(nx * scBX + 5.2f, ny * scBY + 1.3f, qtime, 7);
            float Aw = 0.5f + 0.5f * fastSin(A * sfA);
            float Bw = 0.5f + 0.5f * fastSin(B * sfB);
            coarse[j * CW + i] = softXor(Aw, Bw, soft);
        }
    }
    // ── horizontal mirror ─────────────────────────────────────────────────────
    if (sym == 5 || sym == 7) // step 1: inner half → CW/4..CW/2-1
    {
        for (int j = 0; j < uniJ; j++)
            for (int i = 0; i < CW / 4; i++)
                coarse[j * CW + (CW / 2 - 1 - i)] = coarse[j * CW + i];
    }
    if (sym == 2 || sym == 4 || sym == 5 || sym == 7) // left half → right half
    {
        for (int j = 0; j < uniJ; j++)
            for (int i = 0; i < CW / 2; i++)
                coarse[j * CW + (CW - 1 - i)] = coarse[j * CW + i];
    }
    // ── vertical mirror ───────────────────────────────────────────────────────
    if (sym == 6 || sym == 7) // step 1: inner half → CH/4..CH/2-1
    {
        for (int j = 0; j < CH / 4; j++)
            for (int i = 0; i < CW; i++)
                coarse[(CH / 2 - 1 - j) * CW + i] = coarse[j * CW + i];
    }
    if (sym == 3 || sym == 4 || sym == 6 || sym == 7) // top half → bottom half
    {
        for (int j = 0; j < CH / 2; j++)
            for (int i = 0; i < CW; i++)
                coarse[(CH - 1 - j) * CW + i] = coarse[j * CW + i];
    }

    // ── C channel blur at 16×16 → spatially-varying smooth ───────────────────
    if (blurAmount > 0.005f)
    {
        // ── C channel blur-weight map at 16×16 (UNCHANGED — keep existing loop) ──
        for (int j = 0; j < BH; j++)
        {
            float ny = (float)j / BH;
            for (int i = 0; i < BW; i++)
            {
                float nx = (float)i / BW;
                float C = fbm(nx * 1.5f, ny * 1.5f, btime, 13);
                float cw = fastSin(C * sfC) * 3.0f;
                cw = cw < -1.0f ? -1.0f : (cw > 1.0f ? 1.0f : cw);
                blurC[j * BW + i] = 0.5f + 0.5f * cw;
            }
        }

        // ── build mip pyramid from coarse[] (mip0 = coarse, mip1 = 16, mip2 = 8) ──
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 16; i++)
            {
                int si = 2 * i, sj = 2 * j;
                mip1[j * 16 + i] = 0.25f * (coarse[sj * CW + si] + coarse[sj * CW + si + 1] +
                                            coarse[(sj + 1) * CW + si] + coarse[(sj + 1) * CW + si + 1]);
            }
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
            {
                int si = 2 * i, sj = 2 * j;
                mip2[j * 8 + i] = 0.25f * (mip1[sj * 16 + si] + mip1[sj * 16 + si + 1] +
                                           mip1[(sj + 1) * 16 + si] + mip1[(sj + 1) * 16 + si + 1]);
            }

        // ── per coarse cell: trilinear blur by the upsampled C weight ────────────
        // level 0 = sharp (mip0), 1 = mip1, 2 = mip2; fractional levels lerp between.
        const float bsx = (float)(BW - 1) / (CW - 1);
        const float bsy = (float)(BH - 1) / (CH - 1);
        for (int j = 0; j < CH; j++)
        {
            float bfy = j * bsy;
            for (int i = 0; i < CW; i++)
            {
                float bfx = i * bsx;
                float cw = sampleGrid(blurC, BW, BH, bfx, bfy);
                float level = cw * blurAmount * 2.0f; // 2.0 = BLUR_LEVELMAX
                if (level < 0.005f)
                    continue;
                if (level > 1.999f)
                    level = 1.999f;

                int lo = (int)level; // 0 or 1
                float frac = level - lo;
                float fx1 = i * 15.0f / (CW - 1), fy1 = j * 15.0f / (CH - 1);
                float vLo, vHi;
                if (lo == 0)
                {
                    vLo = coarse[j * CW + i]; // mip0 at this exact cell
                    vHi = sampleGrid(mip1, 16, 16, fx1, fy1);
                }
                else
                {
                    vLo = sampleGrid(mip1, 16, 16, fx1, fy1);
                    vHi = sampleGrid(mip2, 8, 8, i * 7.0f / (CW - 1), j * 7.0f / (CH - 1));
                }
                coarse[j * CW + i] = lerpf(vLo, vHi, frac);
            }
        }
    }

    // ── temporal low-pass: calm low-timescale flicker, stay UI-responsive ────────
    for (int p = 0; p < CW * CH; p++)
    {
        float d = coarse[p] - smoothCoarse[p];
        float ad = d < 0 ? -d : d;
        float g = ad * FLICK_GAIN;
        float alpha = FLICK_AMIN + (1.0f - FLICK_AMIN) * (g > 1.0f ? 1.0f : g);
        smoothCoarse[p] += alpha * d;
    }

    const float sx = (float)(CW - 1) / (W - 1);
    const float sy = (float)(CH - 1) / (H - 1);
    for (int j = 0; j < H; j++)
    {
        float fy = j * sy;
        int cy = (int)fy;
        float ty = fy - cy;
        if (cy >= CH - 1)
        {
            cy = CH - 2;
            ty = 1.0f;
        }
        const float *row0 = smoothCoarse + cy * CW;
        const float *row1 = row0 + CW;
        for (int i = 0; i < W; i++)
        {
            float fx = i * sx;
            int cx = (int)fx;
            float tx = fx - cx;
            if (cx >= CW - 1)
            {
                cx = CW - 2;
                tx = 1.0f;
            }
            float v0 = lerpf(row0[cx], row0[cx + 1], tx);
            float v1 = lerpf(row1[cx], row1[cx + 1], tx);
            float val = lerpf(v0, v1, ty);
            float fidx = val * 255.0f;
            fidx = fidx < 0 ? 0 : (fidx > 255.0f ? 255.0f : fidx);
            int i0 = (int)fidx;
            int i1 = i0 < 255 ? i0 + 1 : i0;

            // ── frozen index corruption ──────────────────────────────────────
            uint8_t cm = glitchColorXor[cy * CW + cx];
            if (cm) { i0 ^= cm; i1 = i0; } // per-cell xor; flat cell → clean stripe-jump

            fb[j * W + i] = lerp565(paletteLUT[i0], paletteLUT[i1], fidx - i0);
        }
    }
}

void glitchUpdate(float g, bool moving)
{
    if (!moving) return;

    // frozen seed: mixes a few field cells, drifts while turning and freezes on release
    latchSeed = hash32((uint32_t)(smoothCoarse[ 5 * CW +  7] * 65535.0f)
                     ^  (uint32_t)(smoothCoarse[17 * CW + 23] * 65535.0f) * 0x9E3779B1u
                     ^  (uint32_t)(smoothCoarse[28 * CW + 11] * 65535.0f) * 0x85EBCA77u);

    // ── horizontal tear: amplitude from hash (full variance at any density) ───────
    for (int y = 0; y < H; y++)
    {
        uint32_t h      = hash32((uint32_t)y * 0x9E3779B1u ^ latchSeed);
        bool     active = (h & 0xFF) < (uint32_t)(g * 256.0f);
        float    hashT  = ((int)((h >> 8) & 0xFF) - 128) * (1.0f / 128.0f);
        float    fieldT = (smoothCoarse[(y >> 1) * CW + AMT_COL] - 0.5f) * 2.0f;
        float    base   = (1.0f - FIELD_WEIGHT) * hashT + FIELD_WEIGHT * fieldT;
        rowShift[y]     = active ? (int)(base * MAXSHIFT_H * g) : 0;
    }

    // ── vertical tear (later onset) ──────────────────────────────────────────────
    for (int x = 0; x < W; x++)
    {
        uint32_t h      = hash32((uint32_t)x * 0x85EBCA77u ^ ~latchSeed);
        bool     active = (g > VERT_ONSET) && ((h & 0xFF) < (uint32_t)(g * 256.0f));
        float    hashT  = ((int)((h >> 8) & 0xFF) - 128) * (1.0f / 128.0f);
        float    fieldT = (smoothCoarse[AMT_ROW * CW + (x >> 1)] - 0.5f) * 2.0f;
        float    base   = (1.0f - FIELD_WEIGHT) * hashT + FIELD_WEIGHT * fieldT;
        colShift[x]     = active ? (int)(base * MAXSHIFT_V * g) : 0;
    }

    // ── chromatic split: scalar, upper travel (unchanged) ───────────────────────
    float gc = (g - CHROMA_ONSET) / (1.0f - CHROMA_ONSET);
    if (gc < 0) gc = 0;
    dxR =  (int)(gc * MAXCHROMA);
    dxB = -(int)(gc * MAXCHROMA);

    // ── per-cell colour corruption: coverage and magnitude both ramp with gcol so
    //    colour blooms in instead of popping. Early = low bits (±1–3 indices), late = full.
    float gcol = (g - COLOR_ONSET) / (1.0f - COLOR_ONSET);
    if (gcol < 0) gcol = 0;
    float bits   = gcol * 8.0f;
    int   bWhole = (int)bits;
    float bFrac  = bits - bWhole;
    for (int p = 0; p < CW * CH; p++)
    {
        uint32_t h      = hash32((uint32_t)p * 0x27D4EB2Fu ^ latchSeed);
        bool     active = (h & 0xFF) < (uint32_t)(gcol * 256.0f);
        int      nb     = bWhole + (((h >> 24) & 0xFF) < (uint32_t)(bFrac * 256.0f) ? 1 : 0);
        uint8_t  budget = (uint8_t)(((1u << nb) - 1) & XOR_BITS);
        glitchColorXor[p] = active ? (uint8_t)((h >> 8) & budget) : 0;
    }

    glitchActive = (g > 0.0001f);
}

void glitchApply()
{
    if (!glitchActive) return;
    memcpy(fbSnap, fb, sizeof(fb));

    for (int y = 0; y < H; y++)
    {
        int rs = rowShift[y];
        for (int x = 0; x < W; x++)
        {
            int cs  = colShift[x];
            int sxG = wrapi(x - rs, W);
            int syG = wrapi(y - cs, H);
            int sxR = wrapi(sxG + dxR, W);
            int sxB = wrapi(sxG + dxB, W);

            uint16_t pr = fbSnap[syG * W + sxR];
            uint16_t pg = fbSnap[syG * W + sxG];
            uint16_t pb = fbSnap[syG * W + sxB];

            fb[y * W + x] = (pr & 0xF800) | (pg & 0x07E0) | (pb & 0x001F);
        }
    }
}

void pushToPanel()
{
    matrix.drawRGBBitmap(0, 0, fb, W, H);
    matrix.show();
}

// ─── init ─────────────────────────────────────────────────────────────────────
void graphicsInit(int paletteIdx)
{
    ProtomatterStatus s = matrix.begin();
    Serial.printf("matrix: %d  heap: %u\n", (int)s, ESP.getFreeHeap());
    if (s != PROTOMATTER_OK)
        for (;;)
            ;
    buildSinLUT();
    buildPalette(paletteIdx);
}
