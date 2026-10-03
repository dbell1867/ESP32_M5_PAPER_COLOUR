// Lesson 02, Part 5 — verify the simulator on the glass.
// Draws tools/epd_sim.py's test image ONCE (same integer maths), then shows it
// in each epd_mode. Switching mode needs NO redraw: the frame buffer keeps full
// RGB (Part 3) and the ink decision happens at display() time.
//   A / B  -> choose mode (LED 0 previews: fastest=red fast=yellow text=green quality=blue)
//   C      -> refresh in that mode (LED 1 red while busy)
#include <Arduino.h>
#include <M5Unified.h>
#include "board_pins.h"

static constexpr int W = 400, H = 600;

// ---- test image: a direct port of pixel() in tools/epd_sim.py ------------
struct RGB { int r, g, b; };

static RGB hue(int h) {  // h in 0..1535 -> fully saturated
  int seg = h >> 8, f = h & 255;
  switch (seg) {
    case 0:  return { 255, f, 0 };
    case 1:  return { 255 - f, 255, 0 };
    case 2:  return { 0, 255, f };
    case 3:  return { 0, 255 - f, 255 };
    case 4:  return { f, 0, 255 };
    default: return { 255, 0, 255 - f };
  }
}

static constexpr RGB PATCHES[8] = {
  { 128, 128, 128 }, { 192, 192, 192 }, { 64, 64, 64 }, { 200, 130, 90 },
  { 255, 195, 206 }, { 123, 0, 123 },   { 0, 255, 255 }, { 0, 0, 123 },
};

static RGB pixel(int x, int y) {
  if (y < 100) { int v = x * 255 / (W - 1); return { v, v, v }; }  // A grey ramp
  if (y < 200) return hue(x * 1536 / W);                            // B rainbow
  if (y < 300) return PATCHES[x / 50];                              // C patches
  RGB c = hue(x * 1536 / W);                                        // D hue x lightness
  int v = y - 300;
  if (v < 150) {
    int k = 150 - v;  // toward white at the top
    return { c.r + (255 - c.r) * k / 150, c.g + (255 - c.g) * k / 150,
             c.b + (255 - c.b) * k / 150 };
  }
  int k = v - 150;    // toward black at the bottom
  return { c.r * (150 - k) / 150, c.g * (150 - k) / 150, c.b * (150 - k) / 150 };
}

// ---- modes ------------------------------------------------------------------
struct Mode { const char* name; epd_mode_t mode; uint8_t lr, lg, lb; };
static constexpr Mode MODES[] = {
  { "fastest", epd_mode_t::epd_fastest, 255,   0,   0 },
  { "fast",    epd_mode_t::epd_fast,    255, 160,   0 },
  { "text",    epd_mode_t::epd_text,      0, 255,   0 },
  { "quality", epd_mode_t::epd_quality,   0,   0, 255 },
};
static constexpr int N_MODES = sizeof(MODES) / sizeof(MODES[0]);
static int selected = 0, shown = -1;

static void setStatus(bool busy) {
  M5.Led.setColor(1, busy ? 255 : 0, busy ? 0 : 255, 0);
  M5.Led.display();
}

static void preview() {
  const Mode& m = MODES[selected];
  M5.Led.setColor(0, m.lr, m.lg, m.lb);
  M5.Led.display();
  Serial.printf("selected: %s%s\n", m.name, selected == shown ? " (on screen)" : "");
}

// The label is the only thing redrawn per commit. It ALSO matters for a
// reason from Part 1: display() returns early if nothing was drawn since the
// last refresh, so without touching the buffer a mode change would do nothing.
static void drawLabel(const char* name) {
  // Bottom of band D is black, so a black box + white text sits naturally there.
  M5.Display.fillRect(0, H - 28, W, 28, TFT_BLACK);
  M5.Display.setFont(&fonts::FreeSansBold9pt7b);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString(String("epd_mode: ") + name, W / 2, H - 6);
}

static void commit() {
  const Mode& m = MODES[selected];
  drawLabel(m.name);
  M5.Display.setEpdMode(m.mode);
  Serial.printf("refreshing in %s...\n", m.name);
  setStatus(true);
  uint32_t t0 = millis();
  M5.Display.display();
  M5.Display.waitDisplay();
  Serial.printf("%s: display() took %lu ms\n", m.name, (unsigned long)(millis() - t0));
  shown = selected;
  setStatus(false);
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nLesson 02 Part 5: simulator vs glass");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Display.setAutoDisplay(false);
  M5.Led.setBrightness(40);

  // Draw the test image ONCE into the frame buffer (no refresh yet).
  uint32_t d0 = millis();
  M5.Display.startWrite();  // batch the 240,000 pixel writes into one transaction
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      RGB c = pixel(x, y);
      M5.Display.drawPixel(x, y, M5.Display.color888(c.r, c.g, c.b));
    }
  M5.Display.endWrite();    // autoDisplay is off, so this does NOT refresh
  Serial.printf("test image drawn into frame buffer in %lu ms\n",
                (unsigned long)(millis() - d0));

  setStatus(false);
  preview();
  Serial.println("A/B choose mode, C refresh.");
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) { selected = (selected + N_MODES - 1) % N_MODES; preview(); }
  if (M5.BtnB.wasPressed()) { selected = (selected + 1) % N_MODES;           preview(); }
  if (M5.BtnC.wasPressed()) {
    if (selected == shown) Serial.println("C: already on screen - no refresh");
    else                   commit();
  }
  delay(10);
}
