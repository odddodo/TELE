#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <FastLED.h>

// ─── hardware ─────────────────────────────────────────────────────────────────
#define W        64
#define H        64
#define BTN_UP    6
#define BTN_DOWN  7

static uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ─── state ────────────────────────────────────────────────────────────────────
struct Chan { float nsclx, nscly, tscl, sinscl, mask, steepness; };
static Chan     channels[3];
static CRGB     pixels[W * H];
static uint32_t time_counter = 0;

static uint8_t       phaseOffset   = 0;
static uint8_t       hueShiftINDX  = 0;
static CRGBPalette16 blendedPalette;
static CRGBPalette16 shiftedPalette;

// ─── palettes ─────────────────────────────────────────────────────────────────
// blend index: 0=Zebra  42=Hello  85=Smoothie  127=XGA  170=Arctic  213=Italy  255=HugMe
static const CRGBPalette16 Zebra = CRGBPalette16(
    CRGB::Black,       CRGB::GhostWhite,  CRGB::DarkGrey,    CRGB::Black,
    CRGB::GhostWhite,  CRGB::DarkGrey,    CRGB::Black,       CRGB::GhostWhite,
    CRGB::Black,       CRGB::DarkGrey,    CRGB::Black,       CRGB::DarkGrey,
    CRGB::Black,       CRGB::DarkGrey,    CRGB::Black,       CRGB::DarkGrey);
static const CRGBPalette16 Hello = CRGBPalette16(
    CRGB::Red,         CRGB::Orange,      CRGB::Yellow,      CRGB::DarkGreen,
    CRGB::DarkBlue,    CRGB::Purple,      CRGB::Red,         CRGB::Orange,
    CRGB::Yellow,      CRGB::DarkGreen,   CRGB::DarkBlue,    CRGB::Purple,
    CRGB::Red,         CRGB::Orange,      CRGB::Yellow,      CRGB::DarkGreen);
static const CRGBPalette16 Smoothie = CRGBPalette16(
    CRGB::DarkOrchid,  CRGB::Olive,       CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki,   CRGB::RoyalBlue,   CRGB::Black,
    CRGB::DarkOrchid,  CRGB::Olive,       CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki,   CRGB::RoyalBlue,   CRGB::Black);
static const CRGBPalette16 XGAColors = CRGBPalette16(
    CRGB::Black,       CRGB::Yellow,      CRGB::Magenta,     CRGB::Cyan,
    CRGB::Black,       CRGB::Yellow,      CRGB::Magenta,     CRGB::Cyan,
    CRGB::Black,       CRGB::Yellow,      CRGB::Magenta,     CRGB::Cyan,
    CRGB::Black,       CRGB::Yellow,      CRGB::Magenta,     CRGB::Cyan);
static const CRGBPalette16 Arctic = CRGBPalette16(
    CRGB::White,       CRGB::Blue,        CRGB::Red,         CRGB::White,
    CRGB::Blue,        CRGB::Red,         CRGB::DarkBlue,    CRGB::Gold,
    CRGB::DarkBlue,    CRGB::Gold,        CRGB::DarkBlue,    CRGB::Gold,
    CRGB::White,       CRGB::Blue,        CRGB::Red,         CRGB::Gold);
static const CRGBPalette16 Italy = CRGBPalette16(
    CRGB::GhostWhite,  CRGB::Red,         CRGB::Green,       CRGB::Black,
    CRGB::GhostWhite,  CRGB::Red,         CRGB::Green,       CRGB::Black,
    CRGB::WhiteSmoke,  CRGB::Blue,        CRGB::WhiteSmoke,  CRGB::Blue,
    CRGB::WhiteSmoke,  CRGB::Blue,        CRGB::WhiteSmoke,  CRGB::Blue);
static const CRGBPalette16 HugMeColors = CRGBPalette16(
    CRGB::HotPink,     CRGB::Olive,       CRGB::YellowGreen, CRGB::Black,
    CRGB::DarkSalmon,  CRGB::Olive,       CRGB::DarkBlue,    CRGB::Black,
    CRGB::DarkOrchid,  CRGB::Olive,       CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta, CRGB::DarkKhaki,   CRGB::RoyalBlue,   CRGB::Black);

static const uint8_t NUM_PALETTES = 7;
static const CRGBPalette16 palettes[NUM_PALETTES] = {
    Zebra, Hello, Smoothie, XGAColors, Arctic, Italy, HugMeColors
};

// ─── convolution kernels ──────────────────────────────────────────────────────
static float embossKernel[3][3] = {{-2,-1, 0},{-1, 1, 1},{ 0, 1, 2}};
static float blurKernel[3][3]   = {{1/9.f,1/9.f,1/9.f},
                                   {1/9.f,1/9.f,1/9.f},
                                   {1/9.f,1/9.f,1/9.f}};

// ─── presets ──────────────────────────────────────────────────────────────────
// ch[0]  warped noise :  nsclx, nscly, tscl, sinscl, mask, steepness
// ch[1]  plain noise  :  nsclx, nscly, tscl, sinscl, mask, steepness
// ch[2]  palette      :  {0, 0, 0, hueShift, palBlend, phaseOffset}
// embossAmt / blurAmt :  0=skip, 1-255=uniform blend amount

struct Preset {
    Chan    ch[3];
    uint8_t embossAmt, blurAmt;
    const char *name;
};

// P(name, x0,y0,t0,s0,m0,k0,  x1,y1,t1,s1,m1,k1,  hs,bm,ph,  ea,ba)
#define P(n,x0,y0,t0,s0,m0,k0,x1,y1,t1,s1,m1,k1,hs,bm,ph,ea,ba) \
    {{{x0,y0,t0,s0,m0,k0},{x1,y1,t1,s1,m1,k1},{0,0,0,hs,bm,ph}},ea,ba,n}

static const Preset presets[] = {
    // ── LAVA ─────────────────────────────────────────────────────────────────
    P("LV1",  400,400,  1.5f,0.055f,  5, 15,  250,300,  2.0f,0.045f, 10,  5,   8, 42, 20,  60,100),
    P("LV2",  500,500,  2.5f,0.060f,  8, 20,  300,350,  3.0f,0.050f, 12, 10,  10, 42, 40,  80, 80),
    P("LV3",  350,450,  2.0f,0.050f, 10, 60,  200,250,  2.5f,0.045f,  8, 40,  20, 42, 60,  60,120),
    P("MG1",  150,150,  0.8f,0.040f,  3,  8,  100,120,  1.0f,0.035f,  5,  4,   0, 42, 10,  40,140),
    P("MG2",  600,200,  1.5f,0.055f,  5, 12,  150,500,  2.0f,0.045f,  8,  8,  30, 42, 30,  70, 90),
    P("INF",  380,380,  2.2f,0.048f,  6, 18,  230,270,  2.8f,0.040f,  9,  8,   5, 42, 80, 200, 30),
    P("SLG",  420,420,  1.8f,0.035f, 80, 12,  260,300,  2.3f,0.030f, 60,  6,   0, 42,  0, 100, 80),
    P("CND",  400,400,  2.0f,0.052f,  5, 15,  250,300,  2.5f,0.042f, 10,  5,   0, 85,  0,  20,200),
    P("PYR",  400,400, 15.0f,0.065f,  5, 15,  250,300, 18.0f,0.055f, 10,  5,  60, 42,100,  80, 60),
    P("EMB",  450,450,  3.0f,0.070f, 15, 25,  280,320,  3.5f,0.060f, 18, 15,  15,255, 50, 180, 40),
    // ── PLASMA ───────────────────────────────────────────────────────────────
    P("PL1",  180,180, 15.0f,0.080f,  0,120,  140,140, 12.0f,0.070f,  0, 90,  60,128, 60,  80, 80),
    P("PL2",  220,160, 18.0f,0.085f,  0,100,  160,220, 14.0f,0.075f,  0, 80,  90,127, 80, 100, 60),
    P("TSL",  300,300, 40.0f,0.090f,  0,160,  250,250, 35.0f,0.080f,  0,140, 120,127,  0, 120, 20),
    P("SPK",  200,200, 20.0f,0.095f,  0,130,  170,170, 16.0f,0.090f,  0,110, 180,127, 40,  60, 40),
    P("ARC",  180,180, 18.0f,0.085f,  0,200,  140,140, 14.0f,0.075f,  0,180,   0,127,100,  80, 80),
    P("COR",  200,200,  2.0f,0.075f,  0,120,  160,160,  1.5f,0.065f,  0, 90, 100,127, 20,  60,100),
    P("ION",  180,180, 15.0f,0.080f,  0,120,  140,140, 12.0f,0.070f,  0, 90,   0,128, 60,  40,180),
    P("AND",  185,185, 17.0f,0.078f,  0,125,  145,145, 13.0f,0.068f,  0, 85,  60,127,128,  70, 90),
    P("VLT",  180,180, 60.0f,0.080f,  0,120,  140,140, 50.0f,0.070f,  0, 90, 200,127, 40, 100, 30),
    P("CTH",  200,200, 16.0f,0.082f,  0,115,  155,155, 13.0f,0.072f,  0, 95,  30,127, 90, 140, 50),
    // ── ARCTIC ───────────────────────────────────────────────────────────────
    P("AR1", 1200,600,  1.0f,0.030f, 30, 10,  800,1000, 0.8f,0.035f, 20,  8,   0,170,  5,  40,140),
    P("IC1", 1800,300,  1.2f,0.025f, 25,  8,  500,1500, 0.9f,0.028f, 18,  5,   0,170, 20,  50,130),
    P("FRS",  800,800,  1.5f,0.025f, 20, 15,  600,600,  1.2f,0.020f, 15, 10,  30,170, 10,  30,160),
    P("GLC",  200,200,  0.3f,0.025f, 20,  5,  150,150,  0.2f,0.020f, 15,  3,   0,170,  0,  20,180),
    P("TND", 1000,500,  1.0f,0.032f, 28, 10,  700,900,  0.8f,0.028f, 22,  7,  20,213, 30,  45,120),
    P("BLZ", 1200,600, 12.0f,0.030f, 30, 10,  800,1000, 9.0f,0.035f, 20,  8,   0,170, 50,  60, 80),
    P("PRM", 1200,600,  0.6f,0.022f, 40,  8,  800,1000, 0.5f,0.018f, 30,  5,   0,170,  0,  10,240),
    P("AUR",  400,1600, 2.0f,0.040f, 15, 20,  300,1200, 1.5f,0.035f, 12, 15, 100, 85, 40,  40,100),
    P("SNW",  900,900,  1.0f,0.028f, 25,  5,  700,700,  0.8f,0.024f, 18,  3,  10,170, 15,  25,160),
    P("SLT", 1000,500,  3.0f,0.060f, 20,  8,  700,800,  2.5f,0.055f, 15,  5,   0,170, 35,  55,100),
    // ── ORGANIC ──────────────────────────────────────────────────────────────
    P("MSS",  500,500,  1.0f,0.045f, 20, 30,  350,400,  1.2f,0.040f, 15, 20,   0, 85, 10,  60,120),
    P("FNG",  400,400,  2.5f,0.055f, 10,100,  300,350,  2.0f,0.050f,  8, 80,  40, 42, 20, 100, 60),
    P("MYC",  600,600,  1.8f,0.050f, 15, 40,  450,500,  2.2f,0.045f, 12, 30,  20, 85, 30,  50,180),
    P("SWP",  300,300,  0.6f,0.040f, 25, 20,  200,250,  0.5f,0.035f, 18, 12,  30, 85,  5,  40,150),
    P("PET",  450,450,  1.2f,0.035f, 80, 15,  300,350,  1.0f,0.030f, 60, 10,   0, 85,  0,  70,100),
    P("LCH",  800,400,  1.5f,0.050f, 18, 45,  400,900,  1.8f,0.045f, 14, 35,  10, 85, 25,  80, 80),
    P("SPR",  550,550,  2.0f,0.055f, 12, 35,  380,420,  2.5f,0.050f, 10, 25,   0, 85, 40, 160, 40),
    P("ALG",  400,400,  1.5f,0.048f, 15, 25,  280,320,  1.8f,0.042f, 12, 18,   0,213, 20,  50,130),
    P("SOL",  350,350,  0.3f,0.045f, 18, 20,  250,280, 0.25f,0.040f, 14, 12,   0, 85,  8,  30,160),
    P("BRK",  700,700,  2.0f,0.058f, 10, 20,  500,550,  2.5f,0.052f,  8, 15,  10, 85, 35,  90, 60),
    // ── PSYCHEDELIC ──────────────────────────────────────────────────────────
    P("HYP",  800,800,  8.0f,0.060f,  0, 50,  600,600, 10.0f,0.070f,  0, 80, 100,100,100, 100, 60),
    P("ACD",  300,300, 20.0f,0.095f,  0, 80,  200,200, 25.0f,0.090f,  0, 60, 200,255,128,  80, 80),
    P("TRP",  400,400, 12.0f,0.075f,  0,220,  300,300, 15.0f,0.065f,  0,200, 180, 42, 64, 200, 20),
    P("DRM",  250,250,  0.5f,0.060f,  0, 40,  180,180,  0.4f,0.055f,  0, 30,  80,240, 20,  20,160),
    P("VSN",  500,500, 10.0f,0.070f,  0, 60,  350,400,  8.0f,0.065f,  0, 50,  40,170, 80,  30,200),
    P("KAL", 1500,200,  5.0f,0.080f,  0,150,  200,1500, 7.0f,0.075f,  0,130, 150,127, 50, 120, 50),
    P("NEN",  200,200, 14.0f,0.080f,  0,100,  150,150, 11.0f,0.075f,  0, 80, 220, 42,200,  90, 70),
    P("STR",  200,200, 80.0f,0.085f,  0,120,  160,160, 70.0f,0.080f,  0,100, 255,127,  0, 110, 30),
    P("VRT",  350,350,  8.0f,0.070f,  0,240,  280,280,  6.0f,0.065f,  0,220,   0,100, 90, 130, 60),
    P("ETH",  300,300,  4.0f,0.065f,  5, 60,  220,220,  3.5f,0.060f,  5, 50, 128,170, 50,  60,110),
    // ── BONUS ────────────────────────────────────────────────────────────────
    P("ZBR",  500,200,  5.0f,0.090f,  0,200,  200,500,  7.0f,0.080f,  0,180,   0,  0, 30, 160, 40),
    P("MSC",  100,100,  3.0f,0.090f,  0, 30,   80, 80,  4.0f,0.080f,  0, 20,   0,127,  0, 200, 20),
};
#undef P

static const int NUM_PRESETS = (int)(sizeof(presets) / sizeof(presets[0]));
static int currentPreset = 0;

// ─── helpers ──────────────────────────────────────────────────────────────────
static inline uint16_t to565(CRGB c) {
    return ((uint16_t)(c.r & 0xF8) << 8) | ((uint16_t)(c.g & 0xFC) << 3) | (c.b >> 3);
}

// ─── convolution ──────────────────────────────────────────────────────────────
static CRGB convolution(int x, int y, float kernel[3][3]) {
    float r = 0, g = 0, b = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int xl = constrain(x + i - 1, 0, W - 1);
            int yl = constrain(y + j - 1, 0, H - 1);
            int loc = xl + W * yl;
            r += pixels[loc].r * kernel[i][j];
            g += pixels[loc].g * kernel[i][j];
            b += pixels[loc].b * kernel[i][j];
        }
    }
    return CRGB((uint8_t)constrain((int)r, 0, 255),
                (uint8_t)constrain((int)g, 0, 255),
                (uint8_t)constrain((int)b, 0, 255));
}

// ─── palette ──────────────────────────────────────────────────────────────────
void blendMultiplePalettes() {
    Chan &c      = channels[2];
    phaseOffset  = (uint8_t)c.steepness;
    hueShiftINDX = (uint8_t)c.sinscl;
    float pos    = ((uint8_t)c.mask / 255.0f) * (NUM_PALETTES - 1);
    uint8_t a    = (uint8_t)pos;
    uint8_t b    = min(a + 1, NUM_PALETTES - 1);
    uint8_t amt  = (uint8_t)((pos - a) * 255);
    for (int i = 0; i < 16; i++)
        blendedPalette[i] = blend(palettes[a][i], palettes[b][i], amt);
}

void shiftPalette() {
    for (int i = 0; i < 16; i++) {
        CHSV hsv = rgb2hsv_approximate(blendedPalette[i]);
        hsv.hue += hueShiftINDX;
        hsv2rgb_rainbow(hsv, shiftedPalette[i]);
    }
}

static inline CRGB colorFromPal(uint8_t index) {
    return ColorFromPalette(shiftedPalette, (uint8_t)(index + phaseOffset), 255, LINEARBLEND);
}

// ─── noise frame ──────────────────────────────────────────────────────────────
IRAM_ATTR void generateNoiseFrame() {
    Chan &cx = channels[0];
    Chan &cy = channels[1];
    const uint32_t tz0 = (uint32_t)(time_counter * cx.tscl);
    const uint32_t tz1 = (uint32_t)(time_counter * cy.tscl);

    for (int x = 0; x < W; x++) {
        int16_t wx = sin16(x * (uint16_t)cx.steepness);
        int16_t wy = sin16(x * (uint16_t)cy.steepness);
        for (int y = 0; y < H; y++) {
            int16_t v = (int16_t)inoise16(
                (uint32_t)(x * cx.nsclx + wx),
                (uint32_t)(y * cx.nscly + wy), tz0);
            int16_t k = (int16_t)inoise16(
                (uint32_t)(x * cy.nsclx),
                (uint32_t)(y * cy.nscly), tz1);
            uint8_t v_res = (uint8_t)(v * cx.sinscl);
            uint8_t k_res = (uint8_t)(k * cy.sinscl);
            uint8_t palIdx = (uint8_t)(
                ((uint16_t)(sin8(v_res) * cos8(v_res)) / (1 + (uint8_t)cx.mask) +
                 (uint16_t)(sin8(k_res) * cos8(k_res)) / (1 + (uint8_t)cy.mask)) / 2);
            pixels[x + W * y] = colorFromPal(palIdx);
        }
    }
}

// ─── convolution pass ─────────────────────────────────────────────────────────
void applySoftConvolution(float kernel[3][3], uint8_t blendAmt) {
    if (blendAmt == 0) return;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int loc = x + W * y;
            pixels[loc] = blend(pixels[loc], convolution(x, y, kernel), blendAmt);
        }
}

// ─── preset / buttons ─────────────────────────────────────────────────────────
void loadPreset(int idx) {
    currentPreset = idx;
    const Preset &p = presets[idx];
    channels[0] = p.ch[0];
    channels[1] = p.ch[1];
    channels[2] = p.ch[2];
    Serial.printf("► %s (%d/%d)  emb=%d  blur=%d\n",
                  p.name, idx + 1, NUM_PRESETS, p.embossAmt, p.blurAmt);
}

void checkButtons() {
    static uint32_t lastPress = 0;
    if (millis() - lastPress < 250) return;
    if (!digitalRead(BTN_UP)) {
        loadPreset((currentPreset + 1) % NUM_PRESETS);
        lastPress = millis();
    } else if (!digitalRead(BTN_DOWN)) {
        loadPreset((currentPreset + NUM_PRESETS - 1) % NUM_PRESETS);
        lastPress = millis();
    }
}

// ─── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(BTN_UP,   INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);

    ProtomatterStatus s = matrix.begin();
    Serial.printf("matrix: %d  heap: %u\n", (int)s, ESP.getFreeHeap());
    if (s != PROTOMATTER_OK) for (;;);

    loadPreset(0);
}

void loop() {
    static uint32_t frames = 0;
    static uint32_t lastMs = 0;

    const Preset &p = presets[currentPreset];

    checkButtons();
    generateNoiseFrame();
    applySoftConvolution(embossKernel, p.embossAmt);
    applySoftConvolution(blurKernel,   p.blurAmt);
    blendMultiplePalettes();
    shiftPalette();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            matrix.drawPixel(x, y, to565(pixels[x + W * y]));
    matrix.show();

    time_counter++;
    ++frames;

    const uint32_t now = millis();
    if (now - lastMs >= 2000) {
        Serial.printf("[%s] %.1f fps\n",
                      p.name, 1000.0f * frames / (now - lastMs));
        frames = 0;
        lastMs = now;
    }
}
