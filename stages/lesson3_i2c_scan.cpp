// Lesson 03, Part 1 — Who is on the internal I2C bus?
// Scan with M5Unified's OWN bus object (M5.In_I2C = I2C port 1 on SDA G3 /
// SCL G2) — never Wire.begin(3, 2), which would steal those pins onto port 0.
// Two scans: audio power OFF, then ON (speaker amp stays OFF -> still silent).
// The audio chips should only answer when powered: proof the scan CAN tell
// "present" from "absent" (gotcha 29: an instrument must be able to disagree).
#include <Arduino.h>
#include <M5Unified.h>
#include "board_pins.h"

struct Known { uint8_t addr; const char* name; };
static constexpr Known KNOWN[] = {
  { 0x18, "ES8311 audio codec" }, { 0x32, "RX8130CE RTC" },
  { 0x40, "ES7210 mic ADC" },     { 0x44, "SHT40 temp/humidity" },
  { 0x6E, "M5PM1 power manager" },
};

static const char* nameOf(uint8_t a) {
  for (auto& k : KNOWN) if (k.addr == a) return k.name;
  return "?? not in the docs";
}

static void scan(const char* label) {
  // `= {}` zero-fills the array. A plain `bool found[128];` holds leftover
  // stack garbage, and scanID() only writes 0x08..0x77 (0x00-0x07 and
  // 0x78-0x7F are I2C-reserved; probing 0-7 hangs the S3's I2C).
  bool found[128] = {};
  M5.In_I2C.scanID(found);
  Serial.printf("\n--- scan: %s ---\n", label);
  int n = 0;
  for (int a = 0; a < 128; ++a)
    if (found[a]) { Serial.printf("  0x%02X  %s\n", a, nameOf(a)); ++n; }
  Serial.printf("  %d device(s)\n", n);
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nLesson 03 Part 1: internal I2C scan");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;  // no refresh
  M5.begin(cfg);
  Serial.printf("In_I2C port %d, SDA G%d, SCL G%d\n", (int)M5.In_I2C.getPort(),
                (int)M5.In_I2C.getSDA(), (int)M5.In_I2C.getSCL());

  scan("audio power OFF (G45 LOW)");

  digitalWrite(PIN_AUDIO_PWR_EN, HIGH);  // codec + mic ON; amp (G46) stays OFF
  delay(100);                            // let the chips power up
  scan("audio power ON  (G45 HIGH, speaker amp still OFF)");
  digitalWrite(PIN_AUDIO_PWR_EN, LOW);   // back to silent
}

void loop() { delay(1000); }
