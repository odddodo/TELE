#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <math.h>

// ─── hardware ─────────────────────────────────────────────────────────────────
#define W 64
#define H 64
#define BTN_UP 6
#define BTN_DOWN 7

static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ─── rendering config ─────────────────────────────────────────────────────────
#define BITPLANES 6
#define CW 32 // coarse noise grid width  (must divide evenly or <= W)
#define CH 32 // coarse noise grid height

static float coarse[CW * CH]; // noise values on coarse grid
static uint16_t fb[W * H];    // RGB565 output (bilinear-upsampled)
static uint16_t paletteLUT[256];
static float gtime = 0.0f;
static float gSharp = 8.0f;

// ─── math helpers ─────────────────────────────────────────────────────────────
static inline int ffloor(float x)
{
    int xi = (int)x;
    return (x < xi) ? xi - 1 : xi;
}

static const float INV24 = 1.0f / 16777215.0f;

static inline float hash3(int x, int y, int z, uint32_t s)
{
    uint32_t h = (uint32_t)(x * 374761393) ^ (uint32_t)(y * 668265263) ^ (uint32_t)(z * 2147483647u) ^ (s * 1013904223u);
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

// ─── palette: spectral ROYGBIV ────────────────────────────────────────────────
static void buildPalette()
{
    struct Stop
    {
        float t, r, g, b;
    };
    static const Stop stops[] = {
        {0.000f, 1.0f, 0.0f, 0.0f},
        {0.167f, 1.0f, 0.5f, 0.0f},
        {0.333f, 1.0f, 1.0f, 0.0f},
        {0.500f, 0.0f, 0.0f, 0.0f},
        {0.667f, 0.0f, 0.2f, 1.0f},
        {0.833f, 0.4f, 0.0f, 0.8f},
        {1.000f, 0.0f, 0.0f, 0.0f},
    };
    const int ns = 7;
    for (int i = 0; i < 256; i++)
    {
        float t = i / 255.0f;
        int j = 0;
        while (j < ns - 2 && t > stops[j + 1].t)
            j++;
        float dt = (t - stops[j].t) / (stops[j + 1].t - stops[j].t);
        paletteLUT[i] = rgb565(
            stops[j].r + dt * (stops[j + 1].r - stops[j].r),
            stops[j].g + dt * (stops[j + 1].g - stops[j].g),
            stops[j].b + dt * (stops[j + 1].b - stops[j].b));
    }
}

// ─── render: coarse noise grid + bilinear upsample ────────────────────────────
static void renderFrame(float soft)
{
    const float sc = 5.0f;

    // step 1 — evaluate noise on CW×CH grid
    for (int j = 0; j < CH; j++)
    {
        float ny = (float)j / CH * sc;
        for (int i = 0; i < CW; i++)
        {
            float nx = (float)i / CW * sc;
            float A = fbm(nx, ny, gtime, 1);
            float B = fbm(nx + 5.2f, ny + 1.3f, gtime * 0.85f, 7);
            coarse[j * CW + i] = softXor(A, B, soft);
        }
    }

    // step 2 — bilinear upsample to W×H, palette-map into fb[]
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

// ─── entry points ─────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);

    ProtomatterStatus s = matrix.begin();
    Serial.printf("matrix: %d  heap: %u\n", (int)s, ESP.getFreeHeap());
    if (s != PROTOMATTER_OK)
        for (;;)
            ;

    buildPalette();
    Serial.printf("ready  sharp=%.0f  coarse=%dx%d\n", gSharp, CW, CH);
}

void loop()
{
    if (!digitalRead(BTN_UP) && gSharp < 10.0f)
    {
        gSharp += 1.0f;
        Serial.printf("sharp=%.0f\n", gSharp);
        delay(200);
    }
    if (!digitalRead(BTN_DOWN) && gSharp > 6.0f)
    {
        gSharp -= 1.0f;
        Serial.printf("sharp=%.0f\n", gSharp);
        delay(200);
    }

    uint32_t t0 = micros();
    renderFrame(gSharp);
    uint32_t tRender = micros() - t0;

    uint32_t t1 = micros();
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            matrix.drawPixel(i, j, fb[j * W + i]);
    matrix.show();
    uint32_t tPush = micros() - t1;

    gtime += 0.008f;
    Serial.printf("render %lu us | push %lu us | fps %.1f\n",
                  (unsigned long)tRender, (unsigned long)tPush,
                  1e6f / (tRender + tPush));
}
