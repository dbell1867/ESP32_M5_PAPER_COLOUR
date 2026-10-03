// Lesson 02, Part 3 check — does the e-paper frame buffer really cost
// ~720,000 bytes of PSRAM (400 x 600 x 3)? Measure free memory before and
// after M5.begin(). No refresh: the screen is left as it is.
#include <Arduino.h>
#include <M5Unified.h>
#include "esp_heap_caps.h"
#include "board_pins.h"

struct Mem { size_t psramFree, psramBig, intFree, intBig; };

static Mem snapshot() {
  return {
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
  };
}

static Mem before, after;

static void report() {
  Serial.println("\n=== frame buffer memory check ===");
  Serial.printf("PSRAM total            : %8u\n", (unsigned)ESP.getPsramSize());
  Serial.printf("                 before      after      drop\n");
  Serial.printf("PSRAM free     %9u  %9u  %8d\n", (unsigned)before.psramFree,
                (unsigned)after.psramFree, (int)(before.psramFree - after.psramFree));
  Serial.printf("PSRAM largest  %9u  %9u  %8d\n", (unsigned)before.psramBig,
                (unsigned)after.psramBig, (int)(before.psramBig - after.psramBig));
  Serial.printf("internal free  %9u  %9u  %8d\n", (unsigned)before.intFree,
                (unsigned)after.intFree, (int)(before.intFree - after.intFree));
  Serial.printf("internal large %9u  %9u  %8d\n", (unsigned)before.intBig,
                (unsigned)after.intBig, (int)(before.intBig - after.intBig));
  Serial.printf("predicted frame buffer : %8u  (400 x 600 x 3)\n", 400u * 600u * 3u);
  Serial.printf("display %d x %d, colour depth reported: %d bits\n",
                (int)M5.Display.width(), (int)M5.Display.height(),
                (int)M5.Display.getColorDepth() & 0xFF);
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  before = snapshot();               // measure FIRST, before anything allocates

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;         // no refresh
  M5.begin(cfg);

  after = snapshot();
}

void loop() {
  report();                          // repeat so a late-attaching monitor sees it
  delay(5000);
}
