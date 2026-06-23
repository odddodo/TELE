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

// ─── math helpers ─────────────────────────────────────────────────────────────
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
static const float FLICK_AMIN = 0.08f; // min smoothing when near-still (lower = calmer)
static const float FLICK_GAIN = 40.0f; // how fast real motion re-opens the filter (higher = snappier)

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
    int ga = (a >> 5)  & 0x3F, gb = (b >> 5)  & 0x3F;
    int ba =  a        & 0x1F, bb =  b        & 0x1F;
    int r  = (int)(ra + (rb - ra) * t);
    int g  = (int)(ga + (gb - ga) * t);
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
// Deep-navy (0x000066) is at gap 0.020 from each adjacent black (was 0.047) → narrow blue spike.
static const Palette palettes[PALETTE_COUNT] = {
    {"dark-spectral", {
                          {0.000f, 0x000066}, // deep navy
                          {0.020f, Col::Red},
                          {0.153f, Col::Yellow},
                          {0.200f, Col::Black},
                          {0.220f, 0x000066}, // deep navy
                          {0.333f, Col::Orange},
                          {0.420f, Col::Yellow},
                          {0.467f, Col::Black},
                          {0.487f, 0x000066}, // deep navy
                          {0.600f, Col::Violet},
                          {0.687f, Col::Yellow},
                          {0.733f, Col::Black},
                          {0.753f, 0x000066}, // deep navy
                          {0.867f, Col::Orange},
                          {0.953f, Col::Yellow},
                          {1.000f, Col::Black},
                      },
     16},
    {"spectral", {
                     {0.000f, 0x000066}, // deep navy
                     {0.020f, Col::Violet},
                     {0.153f, Col::White},
                     {0.200f, Col::Black},
                     {0.220f, 0x000066}, // deep navy
                     {0.333f, Col::Orange},
                     {0.420f, Col::White},
                     {0.467f, Col::Black},
                     {0.487f, 0x000066}, // deep navy
                     {0.600f, Col::Cyan},
                     {0.687f, Col::White},
                     {0.733f, Col::Black},
                     {0.753f, 0x000066}, // deep navy
                     {0.867f, Col::Violet},
                     {0.953f, Col::White},
                     {1.000f, Col::Black},
                 },
     16},
    {"fire", {
                 {0.000f, 0x000066}, // deep navy
                 {0.020f, Col::Green},
                 {0.153f, Col::Yellow},
                 {0.200f, Col::Black},
                 {0.220f, 0x000066}, // deep navy
                 {0.333f, Col::Purple},
                 {0.420f, Col::Yellow},
                 {0.467f, Col::Black},
                 {0.487f, 0x000066}, // deep navy
                 {0.600f, Col::DarkViolet},
                 {0.687f, Col::Yellow},
                 {0.733f, Col::Black},
                 {0.753f, 0x000066}, // deep navy
                 {0.867f, Col::Green},
                 {0.953f, Col::Yellow},
                 {1.000f, Col::Black},
             },
     16},
    {"zebra", {                           // no blue → deep navy added
                  {0.000f, 0x000066}, // deep navy
                  {0.020f, 0xA9A9A9}, // DarkGrey
                  {0.153f, Col::White},
                  {0.200f, Col::Black},
                  {0.220f, 0x000066}, // deep navy
                  {0.333f, 0xA9A9A9}, // DarkGrey
                  {0.420f, Col::White},
                  {0.467f, Col::Black},
                  {0.487f, 0x000066}, // deep navy
                  {0.600f, 0xA9A9A9}, // DarkGrey
                  {0.687f, Col::White},
                  {0.733f, Col::Black},
                  {0.753f, 0x000066}, // deep navy
                  {0.867f, 0xA9A9A9}, // DarkGrey
                  {0.953f, Col::White},
                  {1.000f, Col::Black},
              },
     16},
    {"hello", {
                  {0.000f, 0x000066}, // deep navy
                  {0.020f, Col::Red},
                  {0.153f, Col::Yellow},
                  {0.200f, Col::Black},
                  {0.220f, 0x000066}, // deep navy
                  {0.333f, Col::Orange},
                  {0.420f, Col::Yellow},
                  {0.467f, Col::Black},
                  {0.487f, 0x000066}, // deep navy
                  {0.600f, 0x008000}, // DarkGreen
                  {0.687f, Col::Yellow},
                  {0.733f, Col::Black},
                  {0.753f, 0x000066}, // deep navy
                  {0.867f, Col::Purple},
                  {0.953f, Col::Yellow},
                  {1.000f, Col::Black},
              },
     16},
    {"smoothie", {
                     {0.000f, 0x000066}, // deep navy
                     {0.020f, 0x9932CC}, // DarkOrchid
                     {0.153f, 0xBDB76B}, // DarkKhaki (brightest)
                     {0.200f, Col::Black},
                     {0.220f, 0x000066}, // deep navy
                     {0.333f, 0x808000}, // Olive
                     {0.420f, 0xBDB76B}, // DarkKhaki
                     {0.467f, Col::Black},
                     {0.487f, 0x000066}, // deep navy
                     {0.600f, 0xB8860B}, // DarkGoldenrod
                     {0.687f, 0xBDB76B}, // DarkKhaki
                     {0.733f, Col::Black},
                     {0.753f, 0x000066}, // deep navy
                     {0.867f, 0x8B008B}, // DarkMagenta
                     {0.953f, 0xBDB76B}, // DarkKhaki
                     {1.000f, Col::Black},
                 },
     16},
    {"xga", {                             // deep navy replaces Cyan at black boundary
                {0.000f, 0x000066}, // deep navy
                {0.020f, 0xFF00FF}, // Magenta
                {0.153f, Col::Yellow},
                {0.200f, Col::Black},
                {0.220f, 0x000066}, // deep navy
                {0.333f, 0xFF00FF}, // Magenta
                {0.420f, Col::Yellow},
                {0.467f, Col::Black},
                {0.487f, 0x000066}, // deep navy
                {0.600f, 0xFF00FF}, // Magenta
                {0.687f, Col::Yellow},
                {0.733f, Col::Black},
                {0.753f, 0x000066}, // deep navy
                {0.867f, 0xFF00FF}, // Magenta
                {0.953f, Col::Yellow},
                {1.000f, Col::Black},
            },
     16},
    {"arctic", {
                   {0.000f, 0x000066}, // deep navy
                   {0.020f, Col::Red},
                   {0.153f, Col::White},
                   {0.200f, Col::Black},
                   {0.220f, 0x000066}, // deep navy
                   {0.333f, Col::Blue},
                   {0.420f, Col::White},
                   {0.467f, Col::Black},
                   {0.487f, 0x000066}, // deep navy
                   {0.600f, 0xFFD700}, // Gold
                   {0.687f, Col::White},
                   {0.733f, Col::Black},
                   {0.753f, 0x000066}, // deep navy
                   {0.867f, Col::Red},
                   {0.953f, Col::White},
                   {1.000f, Col::Black},
               },
     16},
    {"italy", {
                  {0.000f, 0x000066}, // deep navy
                  {0.020f, Col::Red},
                  {0.153f, Col::White},
                  {0.200f, Col::Black},
                  {0.220f, 0x000066}, // deep navy
                  {0.333f, Col::Green},
                  {0.420f, Col::White},
                  {0.467f, Col::Black},
                  {0.487f, 0x000066}, // deep navy
                  {0.600f, Col::Red},
                  {0.687f, Col::White},
                  {0.733f, Col::Black},
                  {0.753f, 0x000066}, // deep navy
                  {0.867f, Col::Green},
                  {0.953f, Col::White},
                  {1.000f, Col::Black},
              },
     16},
    {"hugme", {
                  {0.000f, 0x000066}, // deep navy
                  {0.020f, 0x808000}, // Olive
                  {0.153f, 0xFF69B4}, // HotPink (brightest)
                  {0.200f, Col::Black},
                  {0.220f, 0x000066}, // deep navy
                  {0.333f, 0x9ACD32}, // YellowGreen
                  {0.420f, 0xFF69B4}, // HotPink
                  {0.467f, Col::Black},
                  {0.487f, 0x000066}, // deep navy
                  {0.600f, 0x9932CC}, // DarkOrchid
                  {0.687f, 0xFF69B4}, // HotPink
                  {0.733f, Col::Black},
                  {0.753f, 0x000066}, // deep navy
                  {0.867f, 0xB8860B}, // DarkGoldenrod
                  {0.953f, 0xFF69B4}, // HotPink
                  {1.000f, Col::Black},
              },
     16},
};

static void buildPaletteInto(int idx, uint16_t *lut)
{
    const Palette &p = palettes[idx];
    for (int i = 0; i < 256; i++)
    {
        float t = i / 255.0f;
        int j = 0;
        while (j < p.n - 2 && t > p.stops[j + 1].t)
            j++;
        float dt = (t - p.stops[j].t) / (p.stops[j + 1].t - p.stops[j].t);
        lut[i] = lerpColor(p.stops[j].c, p.stops[j + 1].c, dt);
    }
}

void buildPalette(int idx) { buildPaletteInto(idx, paletteLUT); }

void buildPaletteBlend(float t)
{
    int a = (int)t % PALETTE_COUNT;
    float fr = t - (int)t;
    int b = (a + 1) % PALETTE_COUNT;

    buildPaletteInto(a, paletteLUT);
    if (fr < 0.005f)
        return;

    buildPaletteInto(b, paletteLUT2);
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
    int uniJ = (sym == 6 || sym == 7) ? CH / 4 : (sym == 3 || sym == 4) ? CH / 2 : CH;
    int uniI = (sym == 5 || sym == 7) ? CW / 4 : (sym == 2 || sym == 4) ? CW / 2 : CW;
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
        float d  = coarse[p] - smoothCoarse[p];
        float ad = d < 0 ? -d : d;
        float g  = ad * FLICK_GAIN;
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
            fb[j * W + i] = lerp565(paletteLUT[i0], paletteLUT[i1], fidx - i0);
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
