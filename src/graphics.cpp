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
static float    coarse[CW * CH];
static float    coarseB[CW * CH]; // scratch copy for blur pass
static float    blurC[BW * BH];   // C channel — spatially-varying blur weight
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

static inline float fbm(float x, float y, float z, uint32_t s)
{
    float val = 0, a = 0.6f, f = 1.0f, n = 0;
    for (int o = 0; o < 2; o++)
    {
        val += a * vnoise(x * f, y * f, z * f, s + o * 17);
        n += a;
        a *= 0.5f;
        f *= 2.3f;
    }
    return val / n;
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
    float sum = 0, norm = 0, freq = 1.0f;
    for (int i = 1; i <= BITPLANES; i++)
    {
        float sa = 0.5f + 0.5f * soft * tri(a * freq);
        float sb = 0.5f + 0.5f * soft * tri(b * freq);
        sa = sa < 0 ? 0 : (sa > 1 ? 1 : sa);
        sb = sb < 0 ? 0 : (sb > 1 ? 1 : sb);
        float x = sa + sb - 2.0f * sa * sb;
        float wt = 1.0f / (float)(1 << i);
        sum += wt * x;
        norm += wt;
        freq *= 2.0f;
    }
    return sum / norm;
}

static inline uint16_t rgb565(float r, float g, float b)
{
    uint16_t R = (uint16_t)(r * 31) & 0x1F;
    uint16_t G = (uint16_t)(g * 63) & 0x3F;
    uint16_t B = (uint16_t)(b * 31) & 0x1F;
    return (R << 11) | (G << 5) | B;
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
    Stop stops[8];
    int n;
};

static const Palette palettes[PALETTE_COUNT] = {
    {"dark-spectral", {
                          {0.000f, Col::Red},
                          {0.167f, Col::Orange},
                          {0.333f, Col::Yellow},
                          {0.500f, Col::Black},
                          {0.667f, Col::Blue},
                          {0.833f, Col::Violet},
                          {1.000f, Col::Black},
                      },
     7},
    {"spectral", {
                     {0.000f, Col::Violet},
                     {0.167f, Col::Orange},
                     {0.333f, Col::Cyan},
                     {0.500f, Col::Black},
                     {0.667f, Col::White},
                     {0.833f, Col::DarkViolet},
                     {1.000f, Col::Black},
                 },
     7},
    {"fire", {
                 {0.000f, Col::Purple},
                 {0.167f, Col::Green},
                 {0.333f, Col::Yellow},
                 {0.500f, Col::Black},
                 {0.667f, Col::Blue},
                 {0.833f, Col::DarkViolet},
                 {1.000f, Col::Black},
             },
     7},
    {"cold", {
                 {0.000f, Col::Cyan},
                 {0.167f, Col::Blue},
                 {0.333f, Col::DarkBlue},
                 {0.500f, Col::Black},
                 {0.667f, Col::Violet},
                 {0.833f, Col::Purple},
                 {1.000f, Col::Black},
             },
     7},
    {"nature", {
                   {0.000f, Col::Yellow},
                   {0.167f, 0x66E500},      // yellow-green
                   {0.333f, Col::Green},
                   {0.500f, Col::Black},
                   {0.667f, 0x00B219},      // forest green
                   {0.833f, Col::DarkGreen},
                   {1.000f, Col::Black},
               },
     7},
    {"ember", {
                  {0.000f, Col::Yellow},
                  {0.167f, Col::Orange},
                  {0.333f, Col::Red},
                  {0.500f, Col::Black},
                  {0.667f, Col::DarkRed},
                  {0.833f, 0x330000},       // near-black red
                  {1.000f, Col::Black},
              },
     7},
    {"dusk", {
                 {0.000f, Col::White},
                 {0.167f, Col::Gray},
                 {0.333f, Col::Cyan},
                 {0.500f, Col::Black},
                 {0.667f, Col::DarkBlue},
                 {0.833f, 0x000033},        // near-black blue
                 {1.000f, Col::Black},
             },
     7},
    {"neon", {
                 {0.000f, 0xFF0080},        // hot pink
                 {0.167f, Col::Violet},
                 {0.333f, Col::Blue},
                 {0.500f, Col::Black},
                 {0.667f, Col::Cyan},
                 {0.833f, Col::Green},
                 {1.000f, Col::Black},
             },
     7},
    {"rose", {
                 {0.000f, Col::White},
                 {0.167f, 0xFFCCFF},        // pale lavender
                 {0.333f, 0xFF99CC},        // soft pink
                 {0.500f, Col::Black},
                 {0.667f, 0xFF0080},        // hot pink
                 {0.833f, Col::Purple},
                 {1.000f, Col::Black},
             },
     7},
    {"gold", {
                 {0.000f, Col::White},
                 {0.167f, 0xFFFF80},        // pale yellow
                 {0.333f, Col::Yellow},
                 {0.500f, Col::Black},
                 {0.667f, Col::Orange},
                 {0.833f, 0xFF6600},        // deep orange
                 {1.000f, Col::Black},
             },
     7},
};

static void buildPaletteInto(int idx, uint16_t *lut)
{
    const Palette &p = palettes[idx];
    for (int i = 0; i < 256; i++)
    {
        float t = i / 255.0f;
        int j = 0;
        while (j < p.n - 2 && t > p.stops[j + 1].t) j++;
        float dt = (t - p.stops[j].t) / (p.stops[j + 1].t - p.stops[j].t);
        lut[i] = lerpColor(p.stops[j].c, p.stops[j + 1].c, dt);
    }
}

void buildPalette(int idx) { buildPaletteInto(idx, paletteLUT); }

void buildPaletteBlend(float t)
{
    int a    = (int)t % PALETTE_COUNT;
    float fr = t - (int)t;
    int b    = (a + 1) % PALETTE_COUNT;

    buildPaletteInto(a, paletteLUT);
    if (fr < 0.005f) return;

    buildPaletteInto(b, paletteLUT2);
    for (int i = 0; i < 256; i++)
    {
        uint16_t ca = paletteLUT[i], cb = paletteLUT2[i];
        int ra = (ca >> 11) & 0x1F, rb = (cb >> 11) & 0x1F;
        int ga = (ca >>  5) & 0x3F, gb = (cb >>  5) & 0x3F;
        int ba =  ca        & 0x1F, bb =  cb        & 0x1F;
        paletteLUT[i] = ((uint16_t)(ra + (int)(fr * (rb - ra))) << 11) |
                        ((uint16_t)(ga + (int)(fr * (gb - ga))) <<  5) |
                         (uint16_t)(ba + (int)(fr * (bb - ba)));
    }
}

const char *paletteName(int idx) { return palettes[idx].name; }

// ─── render ───────────────────────────────────────────────────────────────────
void renderFrame(float soft, float scAX, float scAY, float scBX, float scBY,
                 float sfA, float sfB, float sfC, float blurAmount)
{
    // ── A/B noise → softXor at 32×32 ─────────────────────────────────────────
    for (int j = 0; j < CH; j++)
    {
        float ny = (float)j / CH;
        for (int i = 0; i < CW; i++)
        {
            float nx = (float)i / CW;
            float A  = fbm(nx * scAX,        ny * scAY,        gtime, 1);
            float B  = fbm(nx * scBX + 5.2f, ny * scBY + 1.3f, qtime, 7);
            float Aw = 0.5f + 0.5f * sinf(A * sfA * TWO_PI);
            float Bw = 0.5f + 0.5f * sinf(B * sfB * TWO_PI);
            coarse[j * CW + i] = softXor(Aw, Bw, soft);
        }
    }

    // ── C channel blur at 16×16 → spatially-varying smooth ───────────────────
    if (blurAmount > 0.005f)
    {
        // compute blur-weight map at BW×BH
        // sin is amplified then clamped → wide flat bands with narrow transitions
        for (int j = 0; j < BH; j++)
        {
            float ny = (float)j / BH;
            for (int i = 0; i < BW; i++)
            {
                float nx = (float)i / BW;
                float C  = fbm(nx * 1.5f, ny * 1.5f, btime, 13);
                float cw = sinf(C * sfC * TWO_PI) * 3.0f; // amplify before clip
                cw = cw < -1.0f ? -1.0f : (cw > 1.0f ? 1.0f : cw);
                blurC[j * BW + i] = 0.5f + 0.5f * cw;
            }
        }

        // snapshot coarse[] so the 3×3 reads are always from the unblurred frame
        for (int k = 0; k < CW * CH; k++) coarseB[k] = coarse[k];

        // bilinear upsample blur weights and apply per-cell weighted box blur
        const float bsx = (float)(BW - 1) / (CW - 1);
        const float bsy = (float)(BH - 1) / (CH - 1);
        for (int j = 0; j < CH; j++)
        {
            float bfy = j * bsy;
            int   bcy = (int)bfy;
            float bty = bfy - bcy;
            if (bcy >= BH - 1) { bcy = BH - 2; bty = 1.0f; }

            for (int i = 0; i < CW; i++)
            {
                float bfx = i * bsx;
                int   bcx = (int)bfx;
                float btx = bfx - bcx;
                if (bcx >= BW - 1) { bcx = BW - 2; btx = 1.0f; }

                float cw = lerpf(lerpf(blurC[ bcy      * BW + bcx], blurC[ bcy      * BW + bcx + 1], btx),
                                 lerpf(blurC[(bcy + 1)  * BW + bcx], blurC[(bcy + 1) * BW + bcx + 1], btx), bty);
                float w = cw * blurAmount;
                if (w < 0.005f) continue;

                float sum = 0.0f; int n = 0;
                for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++)
                {
                    int ni = i + di, nj = j + dj;
                    if ((unsigned)ni < (unsigned)CW && (unsigned)nj < (unsigned)CH)
                        { sum += coarseB[nj * CW + ni]; n++; }
                }
                coarse[j * CW + i] = lerpf(coarseB[j * CW + i], sum / n, w);
            }
        }
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
        const float *row0 = coarse + cy * CW;
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
            int idx = (int)(val * 255.0f);
            idx = idx < 0 ? 0 : (idx > 255 ? 255 : idx);
            fb[j * W + i] = paletteLUT[idx];
        }
    }
}

void pushToPanel()
{
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            matrix.drawPixel(i, j, fb[j * W + i]);
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
    buildPalette(paletteIdx);
}
