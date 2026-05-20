#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <FastLED.h>

// ═══════════════════════════════════════════════════════════════ HARDWARE ══
#define W 64
#define H 64
#define BTN_UP   6
#define BTN_DOWN 7

static uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
Adafruit_Protomatter matrix(W, 4, 1, rgbPins, 5, addrPins, 2, 47, 14, true);

// ══════════════════════════════════════════════════════════════ PARAMETERS ══
struct Chan { float nsclx, nscly, tscl, sinscl, mask, steepness; };
Chan channels[3];   // [0]=noise ch0, [1]=noise ch1, [2]=palette
uint32_t time_counter = 0;

// ══════════════════════════════════════════════════════════════════ PIXELS ══
CRGB pixels[W * H];

// ═════════════════════════════════════════════════════════════════ PALETTES ══
const CRGBPalette16 Zebra = CRGBPalette16(
    CRGB::Black,      CRGB::GhostWhite, CRGB::DarkGrey,  CRGB::Black,
    CRGB::GhostWhite, CRGB::DarkGrey,   CRGB::Black,     CRGB::GhostWhite,
    CRGB::Black,      CRGB::DarkGrey,   CRGB::Black,     CRGB::DarkGrey,
    CRGB::Black,      CRGB::DarkGrey,   CRGB::Black,     CRGB::DarkGrey);
const CRGBPalette16 Hello = CRGBPalette16(
    CRGB::Red,        CRGB::Orange,     CRGB::Yellow,    CRGB::DarkGreen,
    CRGB::DarkBlue,   CRGB::Purple,     CRGB::Red,       CRGB::Orange,
    CRGB::Yellow,     CRGB::DarkGreen,  CRGB::DarkBlue,  CRGB::Purple,
    CRGB::Red,        CRGB::Orange,     CRGB::Yellow,    CRGB::DarkGreen);
const CRGBPalette16 Smoothie = CRGBPalette16(
    CRGB::DarkOrchid, CRGB::Olive,      CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta,CRGB::DarkKhaki,  CRGB::RoyalBlue, CRGB::Black,
    CRGB::DarkOrchid, CRGB::Olive,      CRGB::DarkGoldenrod, CRGB::Black,
    CRGB::DarkMagenta,CRGB::DarkKhaki,  CRGB::RoyalBlue, CRGB::Black);
const CRGBPalette16 XGAColors = CRGBPalette16(
    CRGB::Black,      CRGB::Yellow,     CRGB::Magenta,   CRGB::Cyan,
    CRGB::Black,      CRGB::Yellow,     CRGB::Magenta,   CRGB::Cyan,
    CRGB::Black,      CRGB::Yellow,     CRGB::Magenta,   CRGB::Cyan,
    CRGB::Black,      CRGB::Yellow,     CRGB::Magenta,   CRGB::Cyan);
const CRGBPalette16 Arctic = CRGBPalette16(
    CRGB::White,      CRGB::Blue,       CRGB::Red,       CRGB::White,
    CRGB::Blue,       CRGB::Red,        CRGB::DarkBlue,  CRGB::Gold,
    CRGB::DarkBlue,   CRGB::Gold,       CRGB::DarkBlue,  CRGB::Gold,
    CRGB::White,      CRGB::Blue,       CRGB::Red,       CRGB::Gold);
const CRGBPalette16 Italy = CRGBPalette16(
    CRGB::GhostWhite, CRGB::Red,        CRGB::Green,     CRGB::Black,
    CRGB::GhostWhite, CRGB::Red,        CRGB::Green,     CRGB::Black,
    CRGB::WhiteSmoke, CRGB::Blue,       CRGB::WhiteSmoke,CRGB::Blue,
    CRGB::WhiteSmoke, CRGB::Blue,       CRGB::WhiteSmoke,CRGB::Blue);
const CRGBPalette16 HugMeColors = CRGBPalette16(
    CRGB::HotPink,    CRGB::Olive,      CRGB::YellowGreen,CRGB::Black,
    CRGB::DarkSalmon, CRGB::Olive,      CRGB::DarkBlue,  CRGB::Black,
    CRGB::DarkOrchid, CRGB::Olive,      CRGB::DarkGoldenrod,CRGB::Black,
    CRGB::DarkMagenta,CRGB::DarkKhaki,  CRGB::RoyalBlue, CRGB::Black);

const uint8_t NUM_PALETTES = 7;
CRGBPalette16 palettes[NUM_PALETTES] = {
    Zebra, Hello, Smoothie, XGAColors, Arctic, Italy, HugMeColors};
CRGBPalette16 blendedPalette, shiftedPalette;
uint8_t phaseOffset  = 0;
uint8_t hueShiftINDX = 0;

// ═════════════════════════════════════════════════════════════════ PRESETS ══
// ch[0] & ch[1] : noise (nsclx/nscly: spatial freq, tscl: speed,
//                        sinscl: palette index density, mask: modulation,
//                        steepness: warp strength)
// ch[2]         : palette (mask→blend index 0-255, steepness→phaseOffset,
//                          sinscl→hueShift amount 0-255)
// embossAmt/blurAmt: fixed blend 0-255 for convolution (0=skip)

struct Preset {
    Chan    ch[3];
    uint8_t embossAmt, blurAmt;
    const char* name;
};

const Preset presets[] = {
  // 0 LAVA
  { .ch = {
      {400, 400, 1.5f, 0.055f,  5, 15},
      {250, 300, 2.0f, 0.045f, 10,  5},
      {  0,   0,   0,      8,  42, 20},
    }, .embossAmt = 60, .blurAmt = 100, .name = "LAVA" },

  // 1 PLASMA
  { .ch = {
      {180, 180,  15, 0.080f,  0, 120},
      {140, 140,  12, 0.070f,  0,  90},
      {  0,   0,   0,     50, 128,  60},
    }, .embossAmt = 80, .blurAmt = 80, .name = "PLASMA" },

  // 2 ARCTIC
  { .ch = {
      {1200, 600, 1.0f, 0.030f, 30, 10},
      { 800,1000, 0.8f, 0.035f, 20,  8},
      {   0,   0,   0,      3, 170,  5},
    }, .embossAmt = 40, .blurAmt = 140, .name = "ARCTIC" },

  // 3 ZEBRA
  { .ch = {
      {500, 200, 5.0f, 0.090f,  0, 200},
      {200, 500, 7.0f, 0.080f,  0, 180},
      {  0,   0,   0,      0,   0,  30},
    }, .embossAmt = 160, .blurAmt = 40, .name = "ZEBRA" },

  // 4 DREAM
  { .ch = {
      {300, 300, 0.5f, 0.030f, 20,  5},
      {200, 200, 0.7f, 0.025f, 15,  3},
      {  0,   0,   0,     30, 240, 10},
    }, .embossAmt = 20, .blurAmt = 180, .name = "DREAM" },

  // 5 HYPNO
  { .ch = {
      {800, 800,  8, 0.060f,  0,  50},
      {600, 600, 10, 0.070f,  0,  80},
      {  0,   0,  0,     80, 100, 100},
    }, .embossAmt = 100, .blurAmt = 60, .name = "HYPNO" },

  // 6 MOSAIC
  { .ch = {
      {100, 100,  3, 0.090f, 0, 30},
      { 80,  80,  4, 0.080f, 0, 20},
      {  0,   0,  0,      0, 127,  0},
    }, .embossAmt = 200, .blurAmt = 20, .name = "MOSAIC" },

  // 7 PSYCHO
  { .ch = {
      {350, 500, 10, 0.070f,  0, 255},
      {500, 350,  8, 0.065f,  0, 200},
      {  0,   0,  0,    100, 213, 180},
    }, .embossAmt = 120, .blurAmt = 80, .name = "PSYCHO" },
};

const int NUM_PRESETS = sizeof(presets) / sizeof(presets[0]);
int     currentPreset = 0;
uint8_t presetEmboss  = 0;
uint8_t presetBlur    = 0;

// ═══════════════════════════════════════════════════════════════ HELPERS ══
static inline uint16_t to565(CRGB c) {
    return ((uint16_t)(c.r & 0xF8) << 8)
         | ((uint16_t)(c.g & 0xFC) << 3)
         |  (c.b >> 3);
}

// ═══════════════════════════════════════════════════ PALETTE FUNCTIONS ══
void blendMultiplePalettes() {
    Chan& c     = channels[2];
    phaseOffset  = (uint8_t)c.steepness;
    hueShiftINDX = (uint8_t)c.sinscl;
    uint8_t idx  = (uint8_t)c.mask;
    float   pos  = (idx / 255.0f) * (NUM_PALETTES - 1);
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

IRAM_ATTR CRGB colorFromPal(uint8_t index) {
    return ColorFromPalette(shiftedPalette, (uint8_t)(index + phaseOffset), 255, LINEARBLEND);
}

// ══════════════════════════════════════════════════════ NOISE FRAME ══
IRAM_ATTR void generateNoiseFrame() {
    Chan& cx = channels[0];
    Chan& cy = channels[1];
    const uint32_t tz0 = (uint32_t)(time_counter * cx.tscl);
    const uint32_t tz1 = (uint32_t)(time_counter * cy.tscl);

    for (int x = 0; x < W; x++) {
        int16_t wx = sin16(x * (uint16_t)cx.steepness);
        int16_t wy = sin16(x * (uint16_t)cy.steepness);

        for (int y = 0; y < H; y++) {
            int16_t v = (int16_t)inoise16(
                (uint32_t)(x * cx.nsclx + wx),
                (uint32_t)(y * cx.nscly + wy),
                tz0);
            int16_t k = (int16_t)inoise16(
                (uint32_t)(x * cy.nsclx),
                (uint32_t)(y * cy.nscly),
                tz1);

            uint8_t vr = (uint8_t)(v * cx.sinscl);
            uint8_t kr = (uint8_t)(k * cy.sinscl);
            uint8_t pi = (uint8_t)((
                (uint16_t)sin8(vr) * cos8(vr) / (1.0f + cx.mask) +
                (uint16_t)sin8(kr) * cos8(kr) / (1.0f + cy.mask)
            ) / 2);

            pixels[x + W * y] = colorFromPal(pi);
        }
    }
}

// ═══════════════════════════════════════════════════════════ CONVOLUTION ══
IRAM_ATTR void applyConvolutions(uint8_t embossAmt, uint8_t blurAmt) {
    if (!embossAmt && !blurAmt) return;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t r[9], g[9], b[9];
            for (int j = 0; j < 3; j++) {
                int row = constrain(y+j-1, 0, H-1) * W;
                for (int i = 0; i < 3; i++) {
                    const CRGB& p = pixels[constrain(x+i-1, 0, W-1) + row];
                    r[j*3+i] = p.r;
                    g[j*3+i] = p.g;
                    b[j*3+i] = p.b;
                }
            }
            int  loc = x + W*y;
            CRGB out = pixels[loc];
            if (blurAmt) {
                uint16_t sr=0, sg=0, sb=0;
                for (int n = 0; n < 9; n++) { sr+=r[n]; sg+=g[n]; sb+=b[n]; }
                out = blend(out, CRGB(sr/9, sg/9, sb/9), blurAmt);
            }
            if (embossAmt) {
                int er = -2*r[0]-r[1]-r[3]+r[4]+r[5]+r[7]+(r[8]<<1);
                int eg = -2*g[0]-g[1]-g[3]+g[4]+g[5]+g[7]+(g[8]<<1);
                int eb = -2*b[0]-b[1]-b[3]+b[4]+b[5]+b[7]+(b[8]<<1);
                out = blend(out, CRGB((uint8_t)constrain(er,0,255),
                                     (uint8_t)constrain(eg,0,255),
                                     (uint8_t)constrain(eb,0,255)), embossAmt);
            }
            pixels[loc] = out;
        }
    }
}

// ═══════════════════════════════════════════════════════════════ PRESET ══
void loadPreset(int idx) {
    memcpy(channels, presets[idx].ch, sizeof(channels));
    presetEmboss = presets[idx].embossAmt;
    presetBlur   = presets[idx].blurAmt;
    Serial.printf("► %s (%d/%d)\n", presets[idx].name, idx + 1, NUM_PRESETS);
}

// ══════════════════════════════════════════════════════════════ BUTTONS ══
void checkButtons() {
    static uint32_t lastPress = 0;
    if (millis() - lastPress < 250) return;
    if (!digitalRead(BTN_UP)) {
        currentPreset = (currentPreset + 1) % NUM_PRESETS;
        loadPreset(currentPreset);
        lastPress = millis();
    } else if (!digitalRead(BTN_DOWN)) {
        currentPreset = (currentPreset + NUM_PRESETS - 1) % NUM_PRESETS;
        loadPreset(currentPreset);
        lastPress = millis();
    }
}

// ═══════════════════════════════════════════════════════════ SETUP/LOOP ══
void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(BTN_UP,   INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);

    ProtomatterStatus s = matrix.begin();
    Serial.printf("matrix: %d  heap: %u  psram: %u\n",
        (int)s, ESP.getFreeHeap(), ESP.getFreePsram());
    if (s != PROTOMATTER_OK) for (;;);

    loadPreset(0);
    blendMultiplePalettes();
    shiftPalette();
    matrix.fillScreen(0);
    matrix.show();
}

void loop() {
    static uint32_t frames = 0;
    static uint32_t lastMs = 0;

    checkButtons();

    generateNoiseFrame();
    applyConvolutions(presetEmboss, presetBlur);
    shiftPalette();
    blendMultiplePalettes();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            matrix.drawPixel(x, y, to565(pixels[x + W * y]));
    matrix.show();

    time_counter++;
    frames++;

    uint32_t now = millis();
    if (now - lastMs >= 2000) {
        Serial.printf("[%s] %.1f fps\n",
            presets[currentPreset].name, 1000.f * frames / (now - lastMs));
        frames = 0;
        lastMs = now;
    }
}
