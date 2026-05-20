#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <FastLED.h>

// ═══════════════════════════════════════════════════════════════ HARDWARE ══
#define W 64
#define H 64
#define MAP_W 512
#define MAP_H 512
#define BTN_UP 6
#define BTN_DOWN 7

static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ══════════════════════════════════════════════════════════════ NOISE MAP ══
// 512×512 uint8_t baked once at startup into PSRAM.
// Diverse regions: large-scale blobs, fine grain, 1D-stretched stripes,
// domain-warped swirls — blended spatially by two meta noise layers.
uint8_t *noiseMap = nullptr;

// ══════════════════════════════════════════════════════════════ NAVIGATION ══
// Lissajous path through the map — two independent phase accumulators
// at incommensurable rates keep the trajectory from repeating quickly.
static float navPhaseX = 0.0f;
static float navPhaseY = 16384.0f; // quarter-cycle offset
static int mapOriginX = 224, mapOriginY = 224;
static uint8_t mapFracX = 0, mapFracY = 0; // sub-pixel fractions (8-bit)

// ══════════════════════════════════════════════════════════════════ STATE ══
static uint32_t timeCounter = 0;
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
// palIdx:      0=Zebra  42=Hello  85=Smoothie  127=XGA  170=Arctic  213=Italy  255=HugMe
// rippleAmt:   palette scroll speed — 0=frozen  100=gentle  200=fast  255=full
// navDX/navDY: Lissajous phase delta/frame (φ-ratio pairs) — 0.1≈9hr  1≈55min  15≈3.6min
struct Preset
{
    uint8_t palIdx;
    uint8_t hueShift;
    uint8_t phaseOffset;
    uint8_t rippleAmt;
    uint8_t rippleFreq; // palette cycles over noise range: 4=1x  16=4x  32=8x  55=14x
    float zoom;         // map px per display px: 0.01=100x-in  1.0=normal  4.0=4x-out
    float navDX;
    float navDY;
    const char *name;
};

// palIdx: Zebra=0 Hello=42 Smoothie=85 XGA=127 Arctic=170 Italy=213 HugMe=255
// zoom: <1=zoomed-in(large blobs)  1.0=natural  >1=zoomed-out(fine grain)
const Preset presets[] = {
    // ── HYPNOTIC — near-static, 11–14 palette cycles, extreme zoom-in ────────
    {42,  100, 60, 130, 55, 0.010f, 0.10f, 0.06f, "HYPNO"},
    {170,  30, 30, 110, 48, 0.012f, 0.13f, 0.08f, "VOID"},
    {85,   60, 90, 120, 52, 0.014f, 0.08f, 0.05f, "ABYSS"},
    {200,  80,150, 105, 44, 0.018f, 0.16f, 0.10f, "TRANCE"},
    // ── DREAM — very slow drift, 6–9 cycles, deep zoom-in ────────────────────
    {42,    0, 40, 140, 36, 0.040f, 0.50f, 0.31f, "LAVA"},
    {85,   40, 80, 135, 32, 0.060f, 0.70f, 0.43f, "DECAY"},
    {170,   0, 10, 135, 28, 0.050f, 0.40f, 0.25f, "GLACIER"},
    {127, 180, 20, 125, 24, 0.040f, 0.62f, 0.38f, "NEBULA"},
    {255,  60,100, 140, 32, 0.055f, 0.52f, 0.32f, "SILK"},
    {213, 120, 80, 130, 28, 0.050f, 0.31f, 0.19f, "DUSK"},
    // ── ORGANIC — gentle flow, 4–6 cycles, moderate zoom ─────────────────────
    {42,   40,  0, 185, 24, 0.100f, 2.0f,  1.2f,  "EMBER"},
    {170,  10, 20, 175, 20, 0.150f, 3.1f,  1.9f,  "AURORA"},
    {85,    0, 50, 165, 20, 0.100f, 2.5f,  1.5f,  "MARBLE"},
    {42,  200, 30, 175, 28, 0.080f, 1.0f,  0.62f, "BLOOD"},
    {127,  90, 70, 170, 18, 0.200f, 4.0f,  2.5f,  "FROST"},
    {255,  20, 15, 180, 22, 0.120f, 1.8f,  1.1f,  "DREAM"},
    // ── VIVID — active navigation, 4–6 cycles, natural–zoomed-out ────────────
    {42,    0,  0, 220, 20, 1.0f,   8.0f,  5.0f,  "RAINBOW"},
    {127,   0,  0, 240, 24, 1.0f,  15.0f,  9.3f,  "MOSAIC"},
    {170,   0,  5, 200, 16, 2.0f,   6.0f,  9.7f,  "ARCTIC"},
    {42,  100, 60, 230, 20, 1.0f,  20.0f, 12.4f,  "ACID"},
    {255, 120,140, 255, 24, 1.0f,  32.0f, 19.8f,  "DISCO"},
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
            const uint8_t fine = inoise8(mx * 480, my * 480, 222);
            const uint8_t stripe = inoise8(mx * 38, my * 750, 333); // extreme 1D scale
            const uint8_t swirl = inoise8(                          // domain-warped by large+fine
                (uint16_t)((int)(mx * 180) + (int)large - 128),
                (uint16_t)((int)(my * 180) + (int)fine - 128), 444);

            // meta2 high (>128) → stripe bleeds in quadratically
            // meta2 low  (<128) → swirl bleeds in quadratically
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
                matrix.drawPixel(bx, 0, 0x7BEF); // mid-grey
            matrix.show();
        }
    }

    Serial.printf(" done in %lu ms\n", millis() - t0);
}

// ══════════════════════════════════════════════════════ RENDER FRAME ══
// Map lookup + additive sin-ripple overlay (wraps palette index).
// Three traveling waves at different spatial/temporal frequencies
// interfere to create complex moving color patterns over the static map.
IRAM_ATTR void renderFrame()
{
    const uint8_t po = presets[currentPreset].phaseOffset;
    const uint8_t ra = presets[currentPreset].rippleAmt;
    const uint8_t rf = presets[currentPreset].rippleFreq;
    // 8.8 fixed-point zoom: zoom*256, e.g. 0.03→7, 0.33→84, 1.0→256, 4.0→1024
    const uint16_t zoomFP = (uint16_t)(presets[currentPreset].zoom * 256.0f + 0.5f);
    const uint8_t tc = (uint8_t)timeCounter;

    for (int oy = 0; oy < H; oy++)
    {
        const uint32_t my_fp = ((uint32_t)mapOriginY << 8) + mapFracY + (uint32_t)oy * zoomFP;
        const int my = (int)(my_fp >> 8);
        const uint8_t sub_fy = (uint8_t)(my_fp & 0xFF);
        const uint8_t *row0 = noiseMap + my * MAP_W;
        const uint8_t *row1 = noiseMap + (my + 1) * MAP_W;

        for (int ox = 0; ox < W; ox++)
        {
            const uint32_t mx_fp = ((uint32_t)mapOriginX << 8) + mapFracX + (uint32_t)ox * zoomFP;
            const int mx = (int)(mx_fp >> 8);
            const uint8_t sub_fx = (uint8_t)(mx_fp & 0xFF);
            const uint8_t top = lerp8by8(row0[mx], row0[mx + 1], sub_fx);
            const uint8_t bot = lerp8by8(row1[mx], row1[mx + 1], sub_fx);
            uint8_t v = lerp8by8(top, bot, sub_fy);

            // palette tiled rf/4 times over the noise range, scrolled by time
            const uint8_t palIdx = (uint8_t)((uint16_t)v * rf / 4 + po + scale8(tc, ra));
            pixels[ox + W * oy] = ColorFromPalette(shiftedPalette,
                                                   palIdx, 255, LINEARBLEND);
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

    // Advance Lissajous navigation
    const Preset &p = presets[currentPreset];
    navPhaseX += p.navDX;
    navPhaseY += p.navDY;
    // Navigation range shrinks with zoom so the window always stays inside the map.
    // halfRange = (MAP_W - W*zoom) / 2  →  origin oscillates 0..2*halfRange
    const int halfRange = max(1, (int)((MAP_W - W * p.zoom) * 0.5f));
    const int32_t fpX = ((int32_t)halfRange << 8) + (int32_t)(sin16((uint16_t)navPhaseX) * ((long)halfRange << 8) / 32767L);
    const int32_t fpY = ((int32_t)halfRange << 8) + (int32_t)(sin16((uint16_t)navPhaseY) * ((long)halfRange << 8) / 32767L);
    mapOriginX = fpX >> 8;
    mapOriginY = fpY >> 8;
    mapFracX = (uint8_t)(fpX & 0xFF);
    mapFracY = (uint8_t)(fpY & 0xFF);

    renderFrame();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            matrix.drawPixel(x, y, to565(pixels[x + W * y]));
    matrix.show();

    timeCounter++;
    frames++;

    const uint32_t now = millis();
    if (now - lastMs >= 2000)
    {
        Serial.printf("[%s] %.1f fps  map(%d,%d)\n",
                      p.name, 1000.f * frames / (now - lastMs),
                      mapOriginX, mapOriginY);
        frames = 0;
        lastMs = now;
    }
}
