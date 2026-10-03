// Stage 2 — First light on the 4" Spectra 6 e-paper, via M5Unified/M5GFX.
// Draws the panel's six native colours as swatches, a title, and an
// orientation marker, then ONE refresh (~15-30 s), timed over serial.
#include <Arduino.h>
#include <M5Unified.h>
#include "board_pins.h"

// The panel's six real colours. These are the exact RGB values M5GFX's
// ED2208 driver uses as its palette (Panel_ED2208.inl, epd_palette[]), so
// each swatch maps 1:1 onto a native ink with no colour approximation.
struct Swatch { const char* name; uint8_t r, g, b; };
static constexpr Swatch SWATCHES[] = {
  { "BLACK",    0,   0,   0 },
  { "WHITE",  255, 255, 255 },
  { "YELLOW", 255, 243,  56 },
  { "RED",    191,   0,   0 },
  { "BLUE",   100,  64, 255 },
  { "GREEN",   67, 138,  28 },
};

void setup() {
  // Silence first: audio codec + speaker amp off (active HIGH -> LOW = off).
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);
  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // native USB: never block (gotcha 11)
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nPaperColor Stage 2: first light");

  // M5Unified's settings struct — like a Python dataclass of options.
  auto cfg = M5.config();
  cfg.internal_spk  = false;  // never set up the speaker (stays silent)
  cfg.internal_mic  = false;  // ...or the microphone
  cfg.clear_display = false;  // skip the "clear to white" refresh: on
                              // e-paper that alone costs a 15-30 s cycle
  M5.begin(cfg);              // detects the board, powers the panel via the PMIC

  Serial.printf("Board id: %d, display %d x %d\n", (int)M5.getBoard(),
                (int)M5.Display.width(), (int)M5.Display.height());

  // CRUCIAL on e-paper: by default every drawing call (fillRect, print...)
  // triggers its own full refresh — six swatches would take minutes. Turn
  // that off, draw everything into the frame buffer (in PSRAM), then push
  // it to the glass ONCE with display().
  M5.Display.setAutoDisplay(false);
  // epd_fastest = nearest-colour, no dithering: right for flat palette colours.
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);

  const int W = M5.Display.width();
  const int H = M5.Display.height();
  M5.Display.fillScreen(TFT_WHITE);

  // Title
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("Hello, PaperColor", W / 2, 12);

  // Six swatches in a 2-column x 3-row grid, each with its name.
  const int top = 70, gap = 10, cols = 2, rows = 3;
  const int sw = (W - gap * (cols + 1)) / cols;
  const int sh = (H - top - 60 - gap * (rows + 1)) / rows;
  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  M5.Display.setTextDatum(middle_center);
  for (int i = 0; i < 6; ++i) {
    const Swatch& s = SWATCHES[i];
    int x = gap + (i % cols) * (sw + gap);
    int y = top + gap + (i / cols) * (sh + gap);
    uint32_t c = M5.Display.color888(s.r, s.g, s.b);
    M5.Display.fillRect(x, y, sw, sh, c);
    M5.Display.drawRect(x, y, sw, sh, TFT_BLACK);  // outline (WHITE swatch)
    // Black label on light inks, white label on dark ones.
    bool light = (i == 1 || i == 2);
    M5.Display.setTextColor(light ? TFT_BLACK : TFT_WHITE);
    M5.Display.drawString(s.name, x + sw / 2, y + sh / 2);
  }

  // Orientation marker: a red triangle in the TOP-LEFT corner, plus size.
  M5.Display.fillTriangle(0, 0, 40, 0, 0, 40, M5.Display.color888(191, 0, 0));
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString(String(W) + " x " + String(H) + "  - top-left is red",
                        W / 2, H - 12);

  // One refresh, timed. display() starts it; waitDisplay() blocks on BUSY.
  Serial.println("Refreshing panel (expect 15-30 s of flashing)...");
  uint32_t r0 = millis();
  M5.Display.display();
  M5.Display.waitDisplay();
  Serial.printf("Refresh done in %lu ms\n", (unsigned long)(millis() - r0));
}

void loop() {
  delay(1000);  // nothing to do: the image stays with zero power
}
