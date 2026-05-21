#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <FastLED.h>

// ═══════════════════════════════════════════════════════════════ HARDWARE ══
#define W 64
#define H 64
#define MAP_W 512
#define MAP_H 512
#define MAP_MX (MAP_W - 1)  // bitmask — MAP_W/H are powers of 2
#define MAP_MY (MAP_H - 1)
#define BTN_UP 6
#define BTN_DOWN 7

static uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ══════════════════════════════════════════════════════════════ NOISE MAP ══
// 512×512 uint8_t baked once at startup into PSRAM.
// Diverse regions: large-scale blobs, fine grain, 1D-stretched stripes,
// domain-warped swirls — blended spatially by two meta noise layers.
// MAP_W/H = 512 = 2×256 (Perlin period) — all integer-scale inoise8 calls
// tile seamlessly, so the map is a valid torus with no seam.
uint8_t *noiseMap = nullptr;

// ══════════════════════════════════════════════════════════ NAVIGATION ══
// Primary: smooth random walk across the torus.
// Probe:   polar offset from the primary, bounded to [0, probeAmp] map px.
static float walkX      = 128.0f;
static float walkY      = 128.0f;
static float walkAngle  = 0.0f;   // heading in radians
static float walkAngleT = 0.0f;   // noise-time for heading perturbation

static float probeAngle   = 0.0f;
static float probeRadius  = 0.0f;  // smoothed toward target each frame
static float probeAngleT  = 314.0f; // different seed from walkAngleT
static float probeRadiusT = 628.0f; // golden-ratio offset decouples from angle

// Derived each frame; available to renderFrame (and future two-walker render).
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

// Active palette — blend + hue-shift of adjacent palette pair.
// Rebuilt once on preset load (not per frame).
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

// ═════════════════════════════════════════════════════════════════ PRESETS ══
// palIdx:       0=Zebra  42=Hello  85=Smoothie  127=XGA  170=Arctic  213=Italy  255=HugMe
// rippleAmt:    palette scroll speed — 0=frozen  100=gentle  200=fast  255=full
// rippleFreq:   palette cycles over noise range: 4=1x  16=4x  32=8x  55=14x
// zoom:         map px per display px: 0.01=100x-in  1.0=normal  2.5=2.5x-out
// walkSpeed:    primary walker speed in map pixels per frame
// walkTurnRate: noise-time advance/frame for heading — low=long arcs  high=jittery
// probeAmp:     max probe radius in map pixels (0=same spot  200=far)
// probeSpeed:   noise-time advance/frame for probe dynamics
struct Preset
{
    uint8_t  palIdx;
    uint8_t  hueShift;
    uint8_t  phaseOffset;
    uint8_t  rippleAmt;
    uint8_t  rippleFreq;
    float    zoom;
    float    walkSpeed;
    float    walkTurnRate;
    float    probeAmp;
    float    probeSpeed;
    const char *name;
};

const Preset presets[] = {
    // ── HYPNOTIC — near-static, 11–14 palette cycles, extreme zoom-in ────────
    {42,  100, 60, 130, 55, 0.010f, 0.0005f, 0.20f,   8.0f, 0.20f, "HYPNO"},
    {170,  30, 30, 110, 48, 0.012f, 0.0006f, 0.25f,  10.0f, 0.25f, "VOID"},
    {85,   60, 90, 120, 52, 0.014f, 0.0004f, 0.15f,   6.0f, 0.18f, "ABYSS"},
    {200,  80,150, 105, 44, 0.018f, 0.0008f, 0.30f,  12.0f, 0.30f, "TRANCE"},
    // ── DREAM — very slow drift, 6–9 cycles, deep zoom-in ────────────────────
    {42,    0, 40, 140, 36, 0.040f, 0.0025f, 0.40f,  20.0f, 0.35f, "LAVA"},
    {85,   40, 80, 135, 32, 0.060f, 0.0035f, 0.50f,  25.0f, 0.40f, "DECAY"},
    {170,   0, 10, 135, 28, 0.050f, 0.0020f, 0.35f,  18.0f, 0.30f, "GLACIER"},
    {127, 180, 20, 125, 24, 0.040f, 0.0030f, 0.45f,  22.0f, 0.38f, "NEBULA"},
    {255,  60,100, 140, 32, 0.055f, 0.0028f, 0.42f,  20.0f, 0.35f, "SILK"},
    {213, 120, 80, 130, 28, 0.050f, 0.0022f, 0.38f,  16.0f, 0.32f, "DUSK"},
    // ── ORGANIC — gentle flow, 4–6 cycles, moderate zoom ─────────────────────
    {42,   40,  0, 185, 24, 0.100f, 0.010f,  0.80f,  40.0f, 0.60f, "EMBER"},
    {170,  10, 20, 175, 20, 0.150f, 0.015f,  1.00f,  50.0f, 0.80f, "AURORA"},
    {85,    0, 50, 165, 20, 0.100f, 0.012f,  0.90f,  45.0f, 0.70f, "MARBLE"},
    {42,  200, 30, 175, 28, 0.080f, 0.008f,  0.70f,  35.0f, 0.55f, "BLOOD"},
    {127,  90, 70, 170, 18, 0.200f, 0.020f,  1.20f,  60.0f, 1.00f, "FROST"},
    {255,  20, 15, 180, 22, 0.120f, 0.010f,  0.85f,  40.0f, 0.65f, "DREAM"},
    // ── VIVID — active navigation, natural–zoomed-out ────────────────────────
    {42,    0,  0, 200, 18, 0.8f,   0.040f,  1.50f,  80.0f, 1.20f, "RAINBOW"},
    {127,   0,  0, 190, 20, 1.5f,   0.070f,  2.00f, 100.0f, 1.50f, "MOSAIC"},
    {170,   0,  5, 190, 16, 2.5f,   0.050f,  1.80f,  90.0f, 1.40f, "ARCTIC"},
    {42,  100, 60, 220, 22, 1.0f,   0.100f,  3.00f, 150.0f, 2.00f, "ACID"},
    {255, 120,140, 240, 24, 1.2f,   0.150f,  4.00f, 200.0f, 2.50f, "DISCO"},
};

const int NUM_PRESETS = sizeof(presets) / sizeof(presets[0]);
int currentPreset = 0;

// ═══════════════════════════════════════════════════════════════ HELPERS ══
static inline uint16_t to565(CRGB c)
{
    return ((uint16_t)(c.r & 0xF8) << 8) | ((uint16_t)(c.g & 0xFC) << 3) | (c.b >> 3);
}

// ══════════════════════════════════════════════════════════ MAP BUILD ══
// 6 inoise8 calls/pixel × 512×512 ≈ 1.6M calls → ~3-5s at boot.
// meta1 blends large-scale blobs ↔ fine grain
// meta2 (quadratic response) fades in stripe or swirl at extremes
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

    // Primary: noise-driven heading accumulated each frame.
    // Turn magnitude scales with walkSpeed so curvature radius (~20 map px)
    // stays roughly constant regardless of speed.
    walkAngleT += p.walkTurnRate;
    const int dn = (int)inoise8((uint16_t)walkAngleT) - 128;
    walkAngle  += dn * (p.walkSpeed * 3.14159f / 8192.0f);
    walkX = fmodf(walkX + cosf(walkAngle) * p.walkSpeed + MAP_W, MAP_W);
    walkY = fmodf(walkY + sinf(walkAngle) * p.walkSpeed + MAP_H, MAP_H);

    // Probe: polar offset from primary — angle does its own slow random walk,
    // radius smoothly tracks a noise-driven target in [0, probeAmp].
    // probeRadiusT advances at golden-ratio multiple of probeAngleT to
    // keep angle and radius dynamics independent.
    probeAngleT  += p.probeSpeed;
    probeRadiusT += p.probeSpeed * 0.618f;
    const int da  = (int)inoise8((uint16_t)probeAngleT) - 128;
    probeAngle   += da * (3.14159f / 4096.0f);
    const float targetR = (inoise8((uint16_t)probeRadiusT) / 255.0f) * p.probeAmp;
    probeRadius  += (targetR - probeRadius) * 0.04f;  // ~25-frame smooth approach

    probeX = fmodf(walkX + cosf(probeAngle) * probeRadius + MAP_W, MAP_W);
    probeY = fmodf(walkY + sinf(probeAngle) * probeRadius + MAP_H, MAP_H);
}

// ══════════════════════════════════════════════════════ RENDER FRAME ══
// Viewport is centered on walkX/Y. Each pixel bilinearly samples the torus
// noise map; torus wrap is a bitmask since MAP_W/H are powers of 2.
IRAM_ATTR void renderFrame()
{
    const Preset &p = presets[currentPreset];
    const uint8_t po = p.phaseOffset;
    const uint8_t ra = p.rippleAmt;
    const uint8_t rf = p.rippleFreq;
    const uint16_t zoomFP = (uint16_t)(p.zoom * 256.0f + 0.5f);

    static uint32_t palTimeAcc = 0;
    palTimeAcc += ra;
    const uint8_t timeOff = (uint8_t)(palTimeAcc >> 8);

    // Viewport top-left in map space; +MAP_W ensures positive before fmodf.
    const float oxf = fmodf(walkX - (W * p.zoom) * 0.5f + MAP_W, MAP_W);
    const float oyf = fmodf(walkY - (H * p.zoom) * 0.5f + MAP_H, MAP_H);
    const int     originX = (int)oxf;
    const uint8_t fracX   = (uint8_t)((oxf - originX) * 256.0f);
    const int     originY = (int)oyf;
    const uint8_t fracY   = (uint8_t)((oyf - originY) * 256.0f);

    for (int oy = 0; oy < H; oy++)
    {
        const uint32_t my_fp = ((uint32_t)originY << 8) + fracY + (uint32_t)oy * zoomFP;
        const int my = (int)(my_fp >> 8);
        const uint8_t sub_fy = (uint8_t)(my_fp & 0xFF);
        // Torus row pointers — bitmask wraps at MAP_H boundary.
        const uint8_t *row0 = noiseMap + (my & MAP_MY) * MAP_W;
        const uint8_t *row1 = noiseMap + ((my + 1) & MAP_MY) * MAP_W;

        for (int ox = 0; ox < W; ox++)
        {
            const uint32_t mx_fp = ((uint32_t)originX << 8) + fracX + (uint32_t)ox * zoomFP;
            const int mx = (int)(mx_fp >> 8);
            const uint8_t sub_fx = (uint8_t)(mx_fp & 0xFF);
            // Torus column indices — bitmask wraps at MAP_W boundary.
            const int mx0 = mx & MAP_MX;
            const int mx1 = (mx + 1) & MAP_MX;

            const uint32_t top32 = (uint32_t)row0[mx0] * (256u - sub_fx) + (uint32_t)row0[mx1] * sub_fx;
            const uint32_t bot32 = (uint32_t)row1[mx0] * (256u - sub_fx) + (uint32_t)row1[mx1] * sub_fx;
            const uint16_t v16   = (uint16_t)((top32 * (256u - sub_fy) + bot32 * sub_fy) >> 8);

            const uint8_t palIdx = (uint8_t)((((uint32_t)v16 * rf) >> 10) + po + timeOff);
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
    Serial.printf("► %s (%d/%d)\n", p.name, idx + 1, NUM_PRESETS);
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
        Serial.printf("[%s] %.1f fps  walk(%.0f,%.0f) probe(%.0f,%.0f) r=%.1f\n",
                      p.name, 1000.f * frames / (now - lastMs),
                      walkX, walkY, probeX, probeY, probeRadius);
        frames = 0;
        lastMs = now;
    }
}
