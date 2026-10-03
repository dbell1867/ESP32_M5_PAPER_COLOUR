// Stage 3c — Buttons + LED: the e-paper interaction pattern.
//   LED = instant feedback (microseconds)   e-paper = results (~16 s)
//   A (left-top, corner)  -> previous colour   } LED 0 previews it instantly
//   B (left-top, second)  -> next colour       }
//   C (top-centre)        -> COMMIT: redraw the screen once in that colour
//   LED 1 = status: green = ready, red = refreshing (buttons ignored)
#include <Arduino.h>
#include <M5Unified.h>
#include "board_pins.h"

// Each colour has two RGB values, because an LED and e-ink are different media:
//  - ink: the panel's exact palette value (from M5GFX's epd_palette[]) so the
//    screen maps 1:1 onto a real ink, no dithering
//  - led: a vivid value for an emitting LED ("blue ink" 100,64,255 looks
//    purple on an LED; LED "black" is simply off)
struct Choice { const char* name; uint8_t ir, ig, ib; uint8_t lr, lg, lb; };
static constexpr Choice CHOICES[] = {
  { "BLACK",    0,   0,   0,     0,   0,   0 },
  { "WHITE",  255, 255, 255,   255, 255, 255 },
  { "YELLOW", 255, 243,  56,   255, 160,   0 },
  { "RED",    191,   0,   0,   255,   0,   0 },
  { "BLUE",   100,  64, 255,     0,   0, 255 },
  { "GREEN",   67, 138,  28,     0, 255,   0 },
};
static constexpr int N_CHOICES = sizeof(CHOICES) / sizeof(CHOICES[0]);

static constexpr int LED_PREVIEW = 0;
static constexpr int LED_STATUS  = 1;

static int selected  = 3;   // start on RED
static int shown     = -1;  // what the e-paper currently shows (-1 = swatches)
static int commits   = 0;

static void showPreview() {
  const Choice& c = CHOICES[selected];
  M5.Led.setColor(LED_PREVIEW, c.lr, c.lg, c.lb);
  M5.Led.display();
  Serial.printf("selected: %s%s\n", c.name, selected == shown ? " (already on screen)" : "");
}

static void setStatus(bool busy) {
  if (busy) M5.Led.setColor(LED_STATUS, 255, 0, 0);
  else      M5.Led.setColor(LED_STATUS, 0, 255, 0);
  M5.Led.display();
}

// Draw the whole screen into the PSRAM frame buffer, then ONE refresh.
static void commit() {
  const Choice& c = CHOICES[selected];
  const int W = M5.Display.width(), H = M5.Display.height();
  uint32_t ink = M5.Display.color888(c.ir, c.ig, c.ib);
  bool light = (selected == 1 || selected == 2);  // WHITE, YELLOW
  uint32_t label = light ? TFT_BLACK : TFT_WHITE;

  ++commits;
  M5.Display.fillScreen(TFT_WHITE);

  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("You chose", W / 2, 14);

  // Big colour panel with its name centred on it.
  const int px = 20, py = 64, pw = W - 40, ph = H - 64 - 90;
  M5.Display.fillRoundRect(px, py, pw, ph, 16, ink);
  M5.Display.drawRoundRect(px, py, pw, ph, 16, TFT_BLACK);  // edge for WHITE
  M5.Display.setTextColor(label);
  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(c.name, W / 2, py + ph / 2);

  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString("commit #" + String(commits), W / 2, H - 46);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawString("A / B choose  -  C commit", W / 2, H - 16);

  Serial.printf("commit #%d: %s - refreshing...\n", commits, c.name);
  setStatus(true);
  uint32_t t0 = millis();
  M5.Display.display();      // BLOCKS for the whole refresh (waits on BUSY)
  M5.Display.waitDisplay();
  Serial.printf("refresh done in %lu ms\n", (unsigned long)(millis() - t0));
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
  Serial.println("\nPaperColor Stage 3c: buttons + LED");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;  // keep whatever is on the glass until a commit
  M5.begin(cfg);

  M5.Display.setAutoDisplay(false);                // one refresh per commit
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);  // flat palette colours

  M5.Led.setBrightness(40);  // these LEDs are glaring at full power
  Serial.printf("LEDs: %d\n", (int)M5.Led.getCount());
  setStatus(false);
  showPreview();
}

void loop() {
  M5.update();  // poll + debounce all buttons; must run every loop

  if (M5.BtnA.wasPressed()) {
    selected = (selected + N_CHOICES - 1) % N_CHOICES;  // wrap backwards
    showPreview();
  }
  if (M5.BtnB.wasPressed()) {
    selected = (selected + 1) % N_CHOICES;              // wrap forwards
    showPreview();
  }
  if (M5.BtnC.wasPressed()) {
    if (selected == shown) Serial.println("C: already on screen - no refresh");
    else                   commit();
  }
  delay(10);
}
