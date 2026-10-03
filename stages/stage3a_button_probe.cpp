// Stage 3a — Button PROBE (no display, no M5Unified). Measure before designing:
//  1. Is each pin pulled up externally? (fight it with an internal pull-DOWN)
//  2. What level is "pressed"?  3. Does G1 react to the SD card (card-detect)?
// Prints every edge with a timestamp and how long the pin stayed in each state.
#include <Arduino.h>
#include "board_pins.h"

struct Probe { const char* name; uint8_t pin; int level; uint32_t since; };
static Probe probes[] = {
  { "A  G10", PIN_BTN_A, 0, 0 },
  { "B  G9 ", PIN_BTN_B, 0, 0 },
  { "C  G1 ", PIN_BTN_C, 0, 0 },
};

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);  // longer: we want this report
  Serial.println("\nPaperColor Stage 3a: button probe (don't press anything yet)");

  // Pull-up test: with an internal pull-DOWN (~45k) fighting it, a pin that
  // still reads HIGH must have a stronger EXTERNAL pull-up on the board.
  for (auto& p : probes) {
    pinMode(p.pin, INPUT_PULLDOWN); delay(5);
    int vDown = digitalRead(p.pin);
    pinMode(p.pin, INPUT_PULLUP);   delay(5);
    int vUp = digitalRead(p.pin);
    pinMode(p.pin, INPUT);          delay(5);  // plain input, like M5Unified uses
    p.level = digitalRead(p.pin);
    p.since = millis();
    Serial.printf("%s  pulldown:%d pullup:%d plain:%d  -> %s\n", p.name, vDown,
                  vUp, p.level,
                  vDown ? "EXTERNAL pull-up (idle HIGH)"
                        : (vUp ? "no external pull (floats/follows ours)"
                               : "held LOW externally"));
  }
  Serial.println("Ready: press each button (tap, then hold ~2 s). Insert/remove SD.");
}

void loop() {
  uint32_t now = millis();
  for (auto& p : probes) {
    int v = digitalRead(p.pin);
    if (v != p.level) {
      Serial.printf("[%7.3f s] %s  %d -> %d   (was %d for %lu ms)\n", now / 1000.0,
                    p.name, p.level, v, p.level, (unsigned long)(now - p.since));
      p.level = v;
      p.since = now;
    }
  }
  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 10000) {  // heartbeat: proves we're alive + current levels
    lastBeat = now;
    Serial.printf("[%7.3f s] alive  A=%d B=%d C=%d\n", now / 1000.0,
                  probes[0].level, probes[1].level, probes[2].level);
  }
  delay(1);  // ~1 kHz poll: fine for reading presses (not for timing bounce)
}
