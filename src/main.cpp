#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <FastLED.h>

// ═══════════════════════════════════════════════════════════════ HARDWARE ══
#define W 64
#define H 64
#define MAP_W 512
#define MAP_H 512
#define MAP_MX (MAP_W - 1)
#define MAP_MY (MAP_H - 1)
#define BTN_UP 6
#define BTN_DOWN 7

static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ══════════════════════════════════════════════════════════════ NOISE MAP ══
// MAP_W/H = 512 = 2×256 (Perlin period) — all integer-scale inoise8 calls
// tile seamlessly, so the map is a valid torus with no seam.
uint8_t *noiseMap = nullptr;

// ══════════════════════════════════════════════════════════ NAVIGATION ══
static float walkX = 128.0f;
static float walkY = 128.0f;
static float walkAngle = 0.0f;
static float walkAngleT = 0.0f;

static float probeDX = 0.0f; // Cartesian offset from primary (map px)
static float probeDY = 0.0f;
static float probeWalkAngle = 0.5f;
static float probeAngleT = 314.0f;

static float probeX = 128.0f;
static float probeY = 128.0f;

// ══════════════════════════════════════════════════════════════════ STATE ══
static CRGB pixels[W * H];

// ═════════════════════════════════════════════════════════════════ PALETTES ══
const CRGBPalette16 Zebra = CRGBPalette16(
    CRGB::Black, CRGB::GhostWhite, CRGB::DarkGrey, CRGB::Black,
    CRGB::GhostWhite, CRGB::DarkGrey, CRGB::Black, CRGB::GhostWhite,
    CRGB::Black, CRGB::DarkGrey, CRGB::Black, CRGB::DarkGrey,
    CRGB::Black, CRGB::DarkGrey, CRGB::Black, CRGB::DarkGrey);
const CRGBPalette16 Hello = CRGBPalette16(
    CRGB::Red, CRGB::Orange, CRGB::Yellow, CRGB::DarkGreen,
    CRGB::DarkBlue, CRGB::Purple, CRGB::Red, CRGB::Orange,
    CRGB::Yellow, CRGB::DarkGreen, CRGB::DarkBlue, CRGB::Purple,
    CRGB::Red, CRGB::Orange, CRGB::Yellow, CRGB::DarkGreen);
const CRGBPalette16 Smoothie = CRGBPalette16(
    CRGB::DarkOrchid, CRGB::Olive, CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki, CRGB::RoyalBlue, CRGB::Black,
    CRGB::DarkOrchid, CRGB::Olive, CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki, CRGB::RoyalBlue, CRGB::Black);
const CRGBPalette16 XGAColors = CRGBPalette16(
    CRGB::Black, CRGB::Yellow, CRGB::Magenta, CRGB::Cyan,
    CRGB::Black, CRGB::Yellow, CRGB::Magenta, CRGB::Cyan,
    CRGB::Black, CRGB::Yellow, CRGB::Magenta, CRGB::Cyan,
    CRGB::Black, CRGB::Yellow, CRGB::Magenta, CRGB::Cyan);
const CRGBPalette16 Arctic = CRGBPalette16(
    CRGB::White, CRGB::Blue, CRGB::Red, CRGB::White,
    CRGB::Blue, CRGB::Red, CRGB::DarkBlue, CRGB::Gold,
    CRGB::DarkBlue, CRGB::Gold, CRGB::DarkBlue, CRGB::Gold,
    CRGB::White, CRGB::Blue, CRGB::Red, CRGB::Gold);
const CRGBPalette16 Italy = CRGBPalette16(
    CRGB::GhostWhite, CRGB::Red, CRGB::Green, CRGB::Black,
    CRGB::GhostWhite, CRGB::Red, CRGB::Green, CRGB::Black,
    CRGB::WhiteSmoke, CRGB::Blue, CRGB::WhiteSmoke, CRGB::Blue,
    CRGB::WhiteSmoke, CRGB::Blue, CRGB::WhiteSmoke, CRGB::Blue);
const CRGBPalette16 HugMeColors = CRGBPalette16(
    CRGB::HotPink, CRGB::Olive, CRGB::YellowGreen, CRGB::Black,
    CRGB::DarkSalmon, CRGB::Olive, CRGB::DarkBlue, CRGB::Black,
    CRGB::DarkOrchid, CRGB::Olive, CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki, CRGB::RoyalBlue, CRGB::Black);

const uint8_t NUM_PALETTES = 7;
const CRGBPalette16 palettes[NUM_PALETTES] = {
    Zebra, Hello, Smoothie, XGAColors, Arctic, Italy, HugMeColors};

CRGBPalette16 shiftedPalette;

void preparePalette(uint8_t palIdx, uint8_t hueShift)
{
    float pos = (palIdx / 255.0f) * (NUM_PALETTES - 1);
    uint8_t a = (uint8_t)pos;
    uint8_t b = min(a + 1, (int)(NUM_PALETTES - 1));
    uint8_t amt = (uint8_t)((pos - a) * 255);
    CRGBPalette16 blended;
    for (int i = 0; i < 16; i++)
        blended[i] = blend(palettes[a][i], palettes[b][i], amt);
    for (int i = 0; i < 16; i++)
    {
        CHSV hsv = rgb2hsv_approximate(blended[i]);
        hsv.hue += hueShift;
        hsv2rgb_rainbow(hsv, shiftedPalette[i]);
    }
}

// ═══════════════════════════════════════════════════════════ COMBINE MODE ══
enum CombineMode : uint8_t
{
    CM_PRIMARY = 0,
    CM_MUL,
    CM_XOR,
    CM_DIFF,
    CM_ADD,
};

// ═════════════════════════════════════════════════════════════════ PRESETS ══
// 50 presets across 5 zoom tiers × 10 presets, all CM_XOR.
// Calibration: walkSpeed = displayRate × zoom  (rates: slow≈0.02 med≈0.05 fast≈0.15)
//              probeSpeed ≈ walkSpeed × 18
//              probeAmp   = displayOffset × zoom  (tight≈5dp med≈12dp wide≈30dp)
//              rippleAmt ≤ 20
// Palettes: 0=Zebra 42=Hello 85=Smoothie 127=XGA 170=Arctic 213=Italy 255=HugMe
struct Preset
{
    uint8_t palIdx, hueShift, phaseOffset, rippleAmt, rippleFreq;
    float zoom, walkSpeed, walkTurnRate, probeAmp, probeSpeed;
    CombineMode combineMode;
    const char *name;
};

#define P(pi, hs, po, ra, rf, z, ws, wt, pa, ps, n) \
    {(pi), (hs), (po), (ra), (rf), (z), (ws), (wt), (pa), (ps), CM_XOR, (n)}

const Preset presets[] = {
    // ── T1: HYPNOTIC — zoom 0.010–0.018 ──────────────────────────────────────
    P(0, 0, 0, 0, 55, 0.010f, 0.000200f, 0.20f, 0.050f, 0.0036f, "Z1A"),
    P(0, 60, 128, 8, 52, 0.016f, 0.000800f, 0.26f, 0.192f, 0.0144f, "Z1B"),
    P(42, 0, 0, 5, 48, 0.012f, 0.000240f, 0.22f, 0.060f, 0.0043f, "H1A"),
    P(42, 120, 200, 15, 52, 0.018f, 0.002700f, 0.30f, 0.540f, 0.0486f, "H1B"),
    P(85, 40, 64, 8, 44, 0.014f, 0.000280f, 0.24f, 0.070f, 0.0050f, "S1A"),
    P(127, 0, 0, 12, 55, 0.012f, 0.000600f, 0.22f, 0.144f, 0.0108f, "X1A"),
    P(127, 80, 128, 5, 48, 0.018f, 0.002700f, 0.30f, 0.540f, 0.0486f, "X1B"),
    P(170, 0, 0, 0, 52, 0.010f, 0.000500f, 0.20f, 0.120f, 0.0090f, "A1A"),
    P(213, 60, 100, 15, 44, 0.016f, 0.000320f, 0.26f, 0.080f, 0.0058f, "I1A"),
    P(255, 0, 0, 18, 55, 0.018f, 0.002700f, 0.30f, 0.216f, 0.0486f, "G1A"),
    // ── T2: DREAM — zoom 0.040–0.060 ─────────────────────────────────────────
    P(0, 0, 0, 0, 36, 0.040f, 0.000800f, 0.35f, 0.200f, 0.0144f, "Z2A"),
    P(0, 60, 128, 8, 32, 0.055f, 0.002750f, 0.42f, 0.660f, 0.0495f, "Z2B"),
    P(42, 0, 0, 10, 32, 0.050f, 0.001000f, 0.40f, 0.250f, 0.0180f, "H2A"),
    P(42, 100, 200, 15, 28, 0.060f, 0.009000f, 0.45f, 1.800f, 0.1620f, "H2B"),
    P(85, 40, 64, 5, 30, 0.045f, 0.002250f, 0.38f, 0.540f, 0.0405f, "S2A"),
    P(85, 200, 128, 12, 28, 0.060f, 0.001200f, 0.45f, 0.300f, 0.0216f, "S2B"),
    P(127, 0, 0, 12, 36, 0.040f, 0.002000f, 0.35f, 0.480f, 0.0360f, "X2A"),
    P(127, 180, 128, 10, 28, 0.045f, 0.006750f, 0.38f, 1.350f, 0.1215f, "X2B"),
    P(170, 0, 100, 8, 32, 0.050f, 0.001000f, 0.40f, 0.600f, 0.0180f, "A2A"),
    P(213, 80, 0, 15, 24, 0.055f, 0.002750f, 0.42f, 0.660f, 0.0495f, "I2A"),
    // ── T3: ORGANIC — zoom 0.100–0.200 ───────────────────────────────────────
    P(0, 0, 0, 0, 24, 0.100f, 0.002000f, 0.70f, 0.500f, 0.0360f, "Z3A"),
    P(0, 80, 128, 10, 22, 0.160f, 0.008000f, 0.90f, 1.920f, 0.1440f, "Z3B"),
    P(42, 0, 0, 8, 20, 0.120f, 0.002400f, 0.75f, 0.600f, 0.0432f, "H3A"),
    P(42, 160, 200, 16, 20, 0.200f, 0.030000f, 1.00f, 6.000f, 0.5400f, "H3B"),
    P(85, 0, 64, 12, 24, 0.130f, 0.006500f, 0.80f, 1.560f, 0.1170f, "S3A"),
    P(85, 200, 128, 20, 18, 0.200f, 0.004000f, 1.00f, 2.400f, 0.0720f, "S3B"),
    P(127, 0, 0, 5, 20, 0.150f, 0.003000f, 0.85f, 0.750f, 0.0540f, "X3A"),
    P(170, 0, 100, 18, 18, 0.180f, 0.027000f, 0.95f, 2.160f, 0.4860f, "A3A"),
    P(213, 0, 0, 8, 22, 0.110f, 0.002200f, 0.72f, 0.550f, 0.0396f, "I3A"),
    P(255, 60, 0, 14, 20, 0.150f, 0.022500f, 0.85f, 4.500f, 0.4050f, "G3A"),
    // ── T4: VIVID — zoom 0.500–1.200 ─────────────────────────────────────────
    P(0, 0, 0, 5, 18, 0.500f, 0.010000f, 1.20f, 2.500f, 0.1800f, "Z4A"),
    P(0, 60, 128, 12, 20, 0.900f, 0.045000f, 1.70f, 5.400f, 0.8100f, "Z4B"),
    P(42, 0, 0, 10, 18, 0.600f, 0.012000f, 1.30f, 3.000f, 0.2160f, "H4A"),
    P(42, 140, 200, 18, 16, 1.000f, 0.150000f, 1.80f, 30.000f, 2.7000f, "H4B"),
    P(85, 80, 64, 8, 20, 0.700f, 0.014000f, 1.40f, 3.500f, 0.2520f, "S4A"),
    P(127, 0, 0, 15, 20, 0.800f, 0.040000f, 1.50f, 24.000f, 0.7200f, "X4A"),
    P(127, 90, 128, 10, 18, 1.200f, 0.060000f, 2.00f, 6.000f, 1.0800f, "X4B"),
    P(170, 0, 0, 6, 20, 0.600f, 0.030000f, 1.30f, 7.200f, 0.5400f, "A4A"),
    P(213, 0, 100, 14, 16, 0.900f, 0.045000f, 1.70f, 27.000f, 0.8100f, "I4A"),
    P(255, 40, 0, 20, 18, 1.000f, 0.020000f, 1.80f, 5.000f, 0.3600f, "G4A"),

};

#undef P

const int NUM_PRESETS = sizeof(presets) / sizeof(presets[0]);
int currentPreset = 0;

// ═══════════════════════════════════════════════════════════════ HELPERS ══
static inline uint16_t to565(CRGB c)
{
    return ((uint16_t)(c.r & 0xF8) << 8) | ((uint16_t)(c.g & 0xFC) << 3) | (c.b >> 3);
}

// ══════════════════════════════════════════════════════════ MAP BUILD ══
void buildNoiseMap()
{
    Serial.print("Building noise map...");
    const uint32_t t0 = millis();

    for (int y = 0; y < MAP_H; y++)
    {
        const uint16_t my = (uint16_t)y;
        for (int x = 0; x < MAP_W; x++)
        {
            const uint16_t mx = (uint16_t)x;

            const uint8_t meta1 = inoise8(mx * 28, my * 28, 0);
            const uint8_t meta2 = inoise8(mx * 18, my * 18, 99);
            const uint8_t large = inoise8(mx * 55, my * 55, 111);
            const uint8_t fine = inoise8(mx * 480, my * 480, 222);
            const uint8_t stripe = inoise8(mx * 38, my * 750, 333);
            const uint8_t swirl = inoise8(
                (uint16_t)((int)(mx * 180) + (int)large - 128),
                (uint16_t)((int)(my * 180) + (int)fine - 128), 444);

            const uint8_t base = lerp8by8(large, fine, meta1);
            const uint8_t w_stripe = lerp8by8(base, stripe, scale8(meta2, meta2));
            const uint8_t v = lerp8by8(w_stripe, swirl, scale8(255 - meta2, 255 - meta2));

            noiseMap[y * MAP_W + x] = v;
        }

        if ((y & 15) == 0)
        {
            const int barW = (y * W) / MAP_H;
            matrix.fillScreen(0);
            for (int bx = 0; bx < barW; bx++)
                matrix.drawPixel(bx, 0, 0x7BEF);
            matrix.show();
        }
    }

    Serial.printf(" done in %lu ms\n", millis() - t0);
}

// ══════════════════════════════════════════════════════════ WALKERS ══
void advanceWalkers()
{
    const Preset &p = presets[currentPreset];

    // Primary: noise-driven heading; turn magnitude ∝ walkSpeed → constant curvature.
    walkAngleT += p.walkTurnRate;
    const int dn = (int)inoise8((uint16_t)walkAngleT) - 128;
    walkAngle += dn * (p.walkSpeed * 3.14159f / 8192.0f);
    walkX = fmodf(walkX + cosf(walkAngle) * p.walkSpeed + MAP_W, MAP_W);
    walkY = fmodf(walkY + sinf(walkAngle) * p.walkSpeed + MAP_H, MAP_H);

    // Probe: random walk inside a disk of radius probeAmp centered on primary.
    // Hard clamp replaces the spring to prevent resonant oscillation.
    probeAngleT += p.probeSpeed * 0.618f;
    const int dp = (int)inoise8((uint16_t)probeAngleT) - 128;
    probeWalkAngle += dp * (p.probeSpeed * 3.14159f / 8192.0f);
    probeDX += cosf(probeWalkAngle) * p.probeSpeed;
    probeDY += sinf(probeWalkAngle) * p.probeSpeed;

    const float dist2 = probeDX * probeDX + probeDY * probeDY;
    if (dist2 > p.probeAmp * p.probeAmp)
    {
        const float inv = p.probeAmp / sqrtf(dist2);
        probeDX *= inv;
        probeDY *= inv;
    }

    probeX = fmodf(walkX + probeDX + MAP_W, MAP_W);
    probeY = fmodf(walkY + probeDY + MAP_H, MAP_H);
}

// ══════════════════════════════════════════════════════ RENDER FRAME ══
// CM_PRIMARY:  palIdx = (v16 * rf >> 10) + po + timeOff
// CM_XOR etc.: palIdx = (combine(v,pv) * rf >> 2) + po + timeOff
IRAM_ATTR void renderFrame()
{
    const Preset &p = presets[currentPreset];
    const uint8_t po = p.phaseOffset;
    const uint8_t ra = p.rippleAmt;
    const uint8_t rf = p.rippleFreq;
    const uint16_t zoomFP = (uint16_t)(p.zoom * 256.0f + 0.5f);
    const CombineMode cm = p.combineMode;

    static uint32_t palTimeAcc = 0;
    palTimeAcc += ra;
    const uint8_t timeOff = (uint8_t)(palTimeAcc >> 8);

    // Primary viewport origin (top-left; walkX/Y is center).
    const float oxf = fmodf(walkX - (W * p.zoom) * 0.5f + MAP_W, MAP_W);
    const float oyf = fmodf(walkY - (H * p.zoom) * 0.5f + MAP_H, MAP_H);
    const int originX = (int)oxf;
    const uint8_t fracX = (uint8_t)((oxf - originX) * 256.0f);
    const int originY = (int)oyf;
    const uint8_t fracY = (uint8_t)((oyf - originY) * 256.0f);

    // Probe viewport origin (same zoom, different center).
    const float poxf = fmodf(probeX - (W * p.zoom) * 0.5f + MAP_W, MAP_W);
    const float poyf = fmodf(probeY - (H * p.zoom) * 0.5f + MAP_H, MAP_H);
    const int pOriginX = (int)poxf;
    const uint8_t pFracX = (uint8_t)((poxf - pOriginX) * 256.0f);
    const int pOriginY = (int)poyf;
    const uint8_t pFracY = (uint8_t)((poyf - pOriginY) * 256.0f);

    for (int oy = 0; oy < H; oy++)
    {
        // Primary row pointers.
        const uint32_t my_fp = ((uint32_t)originY << 8) + fracY + (uint32_t)oy * zoomFP;
        const int my = (int)(my_fp >> 8);
        const uint8_t sub_fy = (uint8_t)(my_fp & 0xFF);
        const uint8_t *row0 = noiseMap + (my & MAP_MY) * MAP_W;
        const uint8_t *row1 = noiseMap + ((my + 1) & MAP_MY) * MAP_W;

        // Probe row pointers — only used when cm != CM_PRIMARY.
        const uint8_t *prow0 = nullptr;
        const uint8_t *prow1 = nullptr;
        uint8_t psub_fy = 0;
        if (cm != CM_PRIMARY)
        {
            const uint32_t pmy_fp = ((uint32_t)pOriginY << 8) + pFracY + (uint32_t)oy * zoomFP;
            const int pmy = (int)(pmy_fp >> 8);
            psub_fy = (uint8_t)(pmy_fp & 0xFF);
            prow0 = noiseMap + (pmy & MAP_MY) * MAP_W;
            prow1 = noiseMap + ((pmy + 1) & MAP_MY) * MAP_W;
        }

        for (int ox = 0; ox < W; ox++)
        {
            // Primary bilinear sample.
            const uint32_t mx_fp = ((uint32_t)originX << 8) + fracX + (uint32_t)ox * zoomFP;
            const int mx = (int)(mx_fp >> 8);
            const uint8_t sub_fx = (uint8_t)(mx_fp & 0xFF);
            const int mx0 = mx & MAP_MX;
            const int mx1 = (mx + 1) & MAP_MX;
            const uint32_t top32 = (uint32_t)row0[mx0] * (256u - sub_fx) + (uint32_t)row0[mx1] * sub_fx;
            const uint32_t bot32 = (uint32_t)row1[mx0] * (256u - sub_fx) + (uint32_t)row1[mx1] * sub_fx;
            const uint16_t v16 = (uint16_t)((top32 * (256u - sub_fy) + bot32 * sub_fy) >> 8);

            uint8_t palIdx;
            if (cm == CM_PRIMARY)
            {
                palIdx = (uint8_t)((((uint32_t)v16 * rf) >> 10) + po + timeOff);
            }
            else
            {
                // Probe bilinear sample.
                const uint32_t pmx_fp = ((uint32_t)pOriginX << 8) + pFracX + (uint32_t)ox * zoomFP;
                const int pmx = (int)(pmx_fp >> 8);
                const uint8_t psub_fx = (uint8_t)(pmx_fp & 0xFF);
                const int pmx0 = pmx & MAP_MX;
                const int pmx1 = (pmx + 1) & MAP_MX;
                const uint32_t ptop = (uint32_t)prow0[pmx0] * (256u - psub_fx) + (uint32_t)prow0[pmx1] * psub_fx;
                const uint32_t pbot = (uint32_t)prow1[pmx0] * (256u - psub_fx) + (uint32_t)prow1[pmx1] * psub_fx;
                const uint8_t pv = (uint8_t)(((ptop * (256u - psub_fy) + pbot * psub_fy) >> 8) >> 8);
                const uint8_t v = (uint8_t)(v16 >> 8);

                uint8_t c;
                switch (cm)
                {
                case CM_MUL:
                    c = scale8(v, pv);
                    break;
                case CM_XOR:
                    c = v ^ pv;
                    break;
                case CM_DIFF:
                    c = (v > pv) ? (v - pv) : (pv - v);
                    break;
                default:
                    c = (uint8_t)((uint16_t)(v + pv) >> 1);
                    break;
                }
                palIdx = (uint8_t)(((uint16_t)c * rf >> 2) + po + timeOff);
            }
            pixels[ox + W * oy] = ColorFromPalette(shiftedPalette, palIdx, 255, LINEARBLEND);
        }
    }
}

// ════════════════════════════════════════════════════ PRESET / BUTTONS ══
void loadPreset(int idx)
{
    currentPreset = idx;
    const Preset &p = presets[idx];
    preparePalette(p.palIdx, p.hueShift);
    Serial.printf("► %s (%d/%d)  zoom=%.3f  amp=%.2f\n",
                  p.name, idx + 1, NUM_PRESETS, p.zoom, p.probeAmp);
}

void checkButtons()
{
    static uint32_t lastPress = 0;
    if (millis() - lastPress < 250)
        return;
    if (!digitalRead(BTN_UP))
    {
        loadPreset((currentPreset + 1) % NUM_PRESETS);
        lastPress = millis();
    }
    else if (!digitalRead(BTN_DOWN))
    {
        loadPreset((currentPreset + NUM_PRESETS - 1) % NUM_PRESETS);
        lastPress = millis();
    }
}

// ══════════════════════════════════════════════════════════ SETUP / LOOP ══
void setup()
{
    Serial.begin(115200);
    delay(500);
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);

    ProtomatterStatus s = matrix.begin();
    Serial.printf("matrix: %d  heap: %u  psram: %u\n",
                  (int)s, ESP.getFreeHeap(), ESP.getFreePsram());
    if (s != PROTOMATTER_OK)
        for (;;)
            ;

    noiseMap = (uint8_t *)ps_malloc(MAP_W * MAP_H);
    if (!noiseMap)
    {
        Serial.println("PSRAM alloc failed!");
        for (;;)
            ;
    }

    buildNoiseMap();
    loadPreset(0);

    matrix.fillScreen(0);
    matrix.show();
}

void loop()
{
    static uint32_t frames = 0;
    static uint32_t lastMs = 0;

    checkButtons();
    advanceWalkers();
    renderFrame();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            matrix.drawPixel(x, y, to565(pixels[x + W * y]));
    matrix.show();

    frames++;

    const uint32_t now = millis();
    if (now - lastMs >= 2000)
    {
        const Preset &p = presets[currentPreset];
        Serial.printf("[%s] %.1f fps  walk(%.1f,%.1f)  probe_r=%.2f\n",
                      p.name, 1000.f * frames / (now - lastMs),
                      walkX, walkY, sqrtf(probeDX * probeDX + probeDY * probeDY));
        frames = 0;
        lastMs = now;
    }
}
