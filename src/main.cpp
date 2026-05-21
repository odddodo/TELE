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

static uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ══════════════════════════════════════════════════════════════ NOISE MAP ══
// MAP_W/H = 512 = 2×256 (Perlin period) — all integer-scale inoise8 calls
// tile seamlessly, so the map is a valid torus with no seam.
uint8_t *noiseMap = nullptr;

// ══════════════════════════════════════════════════════════ NAVIGATION ══
static float walkX      = 128.0f;
static float walkY      = 128.0f;
static float walkAngle  = 0.0f;
static float walkAngleT = 0.0f;

static float probeAngle   = 0.0f;
static float probeRadius  = 0.0f;
static float probeAngleT  = 314.0f;
static float probeRadiusT = 628.0f;

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
// How the primary (v) and probe (pv) noise samples are mixed per pixel.
// v and pv are both uint8_t [0,255].
enum CombineMode : uint8_t {
    CM_PRIMARY = 0,  // v only — probe not sampled
    CM_MUL,          // scale8(v, pv) = v×pv/255  — bright only at intersections
    CM_XOR,          // v ^ pv                    — digital interference / grid
    CM_DIFF,         // |v − pv|                  — isocontours of the offset
    CM_ADD,          // (v + pv) >> 1             — moiré superposition
};

// ═════════════════════════════════════════════════════════════════ PRESETS ══
// All presets: CM_XOR, probeAmp=0.5, zoom=0.055, walkTurnRate=0.42,
//              phaseOffset=100, rippleAmt=140, rippleFreq=32.
//
// probeAmp=0.5 map px ≈ 9 display px at zoom=0.055 — one noise-grain period,
// the sweet spot for visible XOR grid interference.
//
// 5 speed tiers (log-spaced, 1× to 100×  slower):
//   _1 = 1×    walkSpeed=0.00280  probeSpeed=0.0500
//   _2 = 3×    walkSpeed=0.00084  probeSpeed=0.0150
//   _3 = 10×   walkSpeed=0.00028  probeSpeed=0.0050
//   _4 = 30×   walkSpeed=0.000084 probeSpeed=0.0015
//   _5 = 100×  walkSpeed=0.000028 probeSpeed=0.0005
//
// 4 palettes:
//   ZEB = Zebra  (0)   — monochrome, sharpest grid contrast
//   HLO = Hello  (42)  — rainbow
//   XGA = XGA    (127) — black/yellow/magenta/cyan
//   ARC = Arctic (170) — white/blue/red/gold
struct Preset
{
    uint8_t     palIdx;
    uint8_t     hueShift;
    uint8_t     phaseOffset;
    uint8_t     rippleAmt;
    uint8_t     rippleFreq;
    float       zoom;
    float       walkSpeed;
    float       walkTurnRate;
    float       probeAmp;
    float       probeSpeed;
    CombineMode combineMode;
    const char *name;
};

// XOR grid study macro — varies palette (pi,hs), walkSpeed (ws), probeSpeed (ps).
#define XS(pi, hs, ws, ps, n) {(pi),(hs), 100,   0, 32, 0.055f, (ws), 0.42f, 0.5f, (ps), CM_XOR, (n)}

const Preset presets[] = {
    // ── Zebra — monochrome, sharpest contrast ─────────────────────────────────
    XS(  0, 0, 0.002800f, 0.05000f, "ZEB_1"),
    XS(  0, 0, 0.000840f, 0.01500f, "ZEB_2"),
    XS(  0, 0, 0.000280f, 0.00500f, "ZEB_3"),
    XS(  0, 0, 0.000084f, 0.00150f, "ZEB_4"),
    XS(  0, 0, 0.000028f, 0.00050f, "ZEB_5"),
    // ── Hello — rainbow ───────────────────────────────────────────────────────
    XS( 42, 0, 0.002800f, 0.05000f, "HLO_1"),
    XS( 42, 0, 0.000840f, 0.01500f, "HLO_2"),
    XS( 42, 0, 0.000280f, 0.00500f, "HLO_3"),
    XS( 42, 0, 0.000084f, 0.00150f, "HLO_4"),
    XS( 42, 0, 0.000028f, 0.00050f, "HLO_5"),
    // ── XGA — black / yellow / magenta / cyan ─────────────────────────────────
    XS(127, 0, 0.002800f, 0.05000f, "XGA_1"),
    XS(127, 0, 0.000840f, 0.01500f, "XGA_2"),
    XS(127, 0, 0.000280f, 0.00500f, "XGA_3"),
    XS(127, 0, 0.000084f, 0.00150f, "XGA_4"),
    XS(127, 0, 0.000028f, 0.00050f, "XGA_5"),
    // ── Arctic — white / blue / red / gold ────────────────────────────────────
    XS(170, 0, 0.002800f, 0.05000f, "ARC_1"),
    XS(170, 0, 0.000840f, 0.01500f, "ARC_2"),
    XS(170, 0, 0.000280f, 0.00500f, "ARC_3"),
    XS(170, 0, 0.000084f, 0.00150f, "ARC_4"),
    XS(170, 0, 0.000028f, 0.00050f, "ARC_5"),
};

#undef XS

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
            const uint8_t fine  = inoise8(mx * 480, my * 480, 222);
            const uint8_t stripe = inoise8(mx * 38, my * 750, 333);
            const uint8_t swirl  = inoise8(
                (uint16_t)((int)(mx * 180) + (int)large - 128),
                (uint16_t)((int)(my * 180) + (int)fine  - 128), 444);

            const uint8_t base     = lerp8by8(large, fine, meta1);
            const uint8_t w_stripe = lerp8by8(base, stripe, scale8(meta2, meta2));
            const uint8_t v        = lerp8by8(w_stripe, swirl, scale8(255 - meta2, 255 - meta2));

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

    walkAngleT += p.walkTurnRate;
    const int dn = (int)inoise8((uint16_t)walkAngleT) - 128;
    walkAngle  += dn * (p.walkSpeed * 3.14159f / 8192.0f);
    walkX = fmodf(walkX + cosf(walkAngle) * p.walkSpeed + MAP_W, MAP_W);
    walkY = fmodf(walkY + sinf(walkAngle) * p.walkSpeed + MAP_H, MAP_H);

    probeAngleT  += p.probeSpeed;
    probeRadiusT += p.probeSpeed * 0.618f;
    const int da  = (int)inoise8((uint16_t)probeAngleT) - 128;
    probeAngle   += da * (3.14159f / 4096.0f);
    const float targetR = (inoise8((uint16_t)probeRadiusT) / 255.0f) * p.probeAmp;
    probeRadius  += (targetR - probeRadius) * 0.04f;

    probeX = fmodf(walkX + cosf(probeAngle) * probeRadius + MAP_W, MAP_W);
    probeY = fmodf(walkY + sinf(probeAngle) * probeRadius + MAP_H, MAP_H);
}

// ══════════════════════════════════════════════════════ RENDER FRAME ══
// For CM_PRIMARY: palIdx = (v16 * rf >> 10) + po + timeOff  (sub-pixel smooth)
// For others:     palIdx = (combine(v,pv) * rf >> 2) + po + timeOff
//   where v = v16 >> 8 and pv is the 8-bit probe bilinear sample.
//   (v16 * rf >> 10) == (v8 * rf >> 2) — same formula, integer v8.
IRAM_ATTR void renderFrame()
{
    const Preset &p   = presets[currentPreset];
    const uint8_t po  = p.phaseOffset;
    const uint8_t ra  = p.rippleAmt;
    const uint8_t rf  = p.rippleFreq;
    const uint16_t zoomFP = (uint16_t)(p.zoom * 256.0f + 0.5f);
    const CombineMode cm  = p.combineMode;

    static uint32_t palTimeAcc = 0;
    palTimeAcc += ra;
    const uint8_t timeOff = (uint8_t)(palTimeAcc >> 8);

    // Primary viewport origin (top-left; walkX/Y is center).
    const float oxf = fmodf(walkX - (W * p.zoom) * 0.5f + MAP_W, MAP_W);
    const float oyf = fmodf(walkY - (H * p.zoom) * 0.5f + MAP_H, MAP_H);
    const int     originX = (int)oxf;
    const uint8_t fracX   = (uint8_t)((oxf - originX) * 256.0f);
    const int     originY = (int)oyf;
    const uint8_t fracY   = (uint8_t)((oyf - originY) * 256.0f);

    // Probe viewport origin (same zoom, different center).
    const float poxf = fmodf(probeX - (W * p.zoom) * 0.5f + MAP_W, MAP_W);
    const float poyf = fmodf(probeY - (H * p.zoom) * 0.5f + MAP_H, MAP_H);
    const int     pOriginX = (int)poxf;
    const uint8_t pFracX   = (uint8_t)((poxf - pOriginX) * 256.0f);
    const int     pOriginY = (int)poyf;
    const uint8_t pFracY   = (uint8_t)((poyf - pOriginY) * 256.0f);

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
            const uint16_t v16   = (uint16_t)((top32 * (256u - sub_fy) + bot32 * sub_fy) >> 8);

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
                const uint8_t v  = (uint8_t)(v16 >> 8);

                uint8_t c;
                switch (cm)
                {
                    case CM_MUL:  c = scale8(v, pv);                      break;
                    case CM_XOR:  c = v ^ pv;                             break;
                    case CM_DIFF: c = (v > pv) ? (v - pv) : (pv - v);    break;
                    default:      c = (uint8_t)((uint16_t)(v + pv) >> 1); break; // CM_ADD
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
    Serial.printf("► %s (%d/%d)  cm=%d amp=%.2f\n",
                  p.name, idx + 1, NUM_PRESETS, (int)p.combineMode, p.probeAmp);
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
        for (;;) ;

    noiseMap = (uint8_t *)ps_malloc(MAP_W * MAP_H);
    if (!noiseMap)
    {
        Serial.println("PSRAM alloc failed!");
        for (;;) ;
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
        Serial.printf("[%s] %.1f fps  walk(%.1f,%.1f) r=%.2f\n",
                      p.name, 1000.f * frames / (now - lastMs),
                      walkX, walkY, probeRadius);
        frames = 0;
        lastMs = now;
    }
}
