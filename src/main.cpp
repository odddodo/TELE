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
static uint16_t navPhaseX = 0;
static uint16_t navPhaseY = 16384; // quarter-cycle offset
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
// rippleAmt:   ripple adds to palette index (wraps) — 0=off, 255=strong color waves
// navDX/navDY: phase delta per frame — 5≈4min cycle, 15≈90s cycle, 30≈45s cycle
struct Preset
{
    uint8_t palIdx;
    uint8_t hueShift;
    uint8_t phaseOffset;
    uint8_t rippleAmt;
    uint8_t rippleFreq; // spatial freq: 4=~1 cycle, 12=~3 cycles, 24=~6 cycles/display
    float zoom;         // map px per display px: 0.25=zoom-in4x  1.0=normal  4.0=zoom-out4x
    uint16_t navDX;
    uint16_t navDY;
    const char *name;
};

// palIdx: Zebra=0 Hello=42 Smoothie=85 XGA=127 Arctic=170 Italy=213 HugMe=255
// rippleFreq: 4≈1 cycle  12≈3 cycles  24≈6 cycles across display
// zoom: <1=zoom-in(features larger)  1.0=normal  >1=zoom-out(features smaller)
const Preset presets[] = {
    // ── WARM / FIRE ──────────────────────────────────────────────────────────
    {42, 0, 0, 50, 5, 0.05f, 5, 8, "LAVA"},
    {42, 10, 20, 120, 8, .01f, 12, 9, "EMBER"},
    {213, 0, 40, 180, 10, .02f, 18, 13, "FORGE"},
    {21, 0, 0, 80, 6, 2.0f, 4, 6, "MAGMA"},
    {64, 20, 60, 140, 6, .005f, 10, 15, "SUNSET"},
    // ── COLD / SPACE ─────────────────────────────────────────────────────────
    {170, 0, 5, 30, 5, 4.0f, 6, 9, "ARCTIC"},
    {155, 0, 0, 60, 4, 4.0f, 3, 5, "GLACIER"},
    {127, 0, 80, 160, 15, 2.0f, 20, 14, "FROST"},
    {0, 0, 60, 200, 12, 1.0f, 8, 11, "COSMOS"},
    {148, 60, 30, 90, 5, 2.0f, 7, 10, "NIGHT"},
    // ── GRAPHIC ──────────────────────────────────────────────────────────────
    {0, 0, 30, 220, 8, 1.0f, 10, 13, "ZEBRA"},
    {127, 0, 0, 240, 20, 1.0f, 15, 11, "MOSAIC"},
    {127, 30, 90, 180, 15, 1.0f, 22, 16, "CIRCUIT"},
    {0, 0, 128, 255, 24, 1.0f, 18, 25, "PIXEL"},
    {106, 50, 200, 200, 18, 1.0f, 30, 19, "GLITCH"},
    // ── ORGANIC ──────────────────────────────────────────────────────────────
    {240, 30, 10, 80, 5, 4.0f, 3, 5, "DREAM"},
    {200, 10, 0, 40, 4, 4.0f, 4, 7, "CLOUDS"},
    {85, 0, 50, 100, 8, 2.0f, 11, 8, "MARBLE"},
    {255, 15, 20, 130, 6, 2.0f, 6, 12, "SILK"},
    {170, 5, 15, 120, 8, 2.0f, 4, 18, "AURORA"},
    // ── PSYCHEDELIC ──────────────────────────────────────────────────────────
    // HYPNO: ACID base — 5x slower, 3x zoomed in, palette-repeating dense ripple
    {42, 100, 60, 255, 24, 0.03f, 4, 3, "HYPNO"},
    {42, 100, 60, 230, 18, 1.0f, 20, 14, "ACID"},
    {42, 0, 0, 200, 10, 1.0f, 8, 11, "RAINBOW"},
    {127, 60, 170, 240, 20, 1.0f, 28, 21, "PRISM"},
    {255, 120, 140, 255, 24, 1.0f, 32, 23, "DISCO"},
    // ── DARK / MOODY ─────────────────────────────────────────────────────────
    {85, 0, 60, 100, 6, 2.0f, 12, 7, "SMOOTHIE"},
    {42, 0, 200, 60, 8, 0.05f, 5, 8, "BLOOD"},
    {35, 0, 0, 50, 4, 4.0f, 7, 9, "DEEP"},
    {0, 0, 180, 90, 6, 2.0f, 9, 6, "SMOKE"},
    {85, 40, 80, 130, 8, 0.1f, 14, 10, "DECAY"},
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

            if (ra)
            {
                const uint8_t ripple = (uint8_t)(((uint16_t)sin8((uint8_t)(ox * rf + oy * (rf >> 1) + tc * 2)) +
                                                  sin8((uint8_t)(ox * (rf >> 1) + oy * rf + tc * 3)) +
                                                  sin8((uint8_t)((ox + oy) * rf + tc))) /
                                                 3);
                v = (uint8_t)(v + scale8(ripple, ra));
            }

            pixels[ox + W * oy] = ColorFromPalette(shiftedPalette,
                                                   v + po, 255, LINEARBLEND);
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
    const int32_t fpX = ((int32_t)halfRange << 8) + (int32_t)(sin16(navPhaseX) * ((long)halfRange << 8) / 32767L);
    const int32_t fpY = ((int32_t)halfRange << 8) + (int32_t)(sin16(navPhaseY) * ((long)halfRange << 8) / 32767L);
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
