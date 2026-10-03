// Stage 3b — microSD PROBE. Two questions:
//  1. Does G1 (top-centre button) ALSO act as card-detect? Watch it raw.
//  2. Does the slot work? Mount (fastest-first speed ladder), report the card,
//     list the root (READ-ONLY — see note in probeSD()).
// Mount runs at boot and again whenever button A (G10) is pressed, so you can
// insert/remove the card and retry. The e-paper is initialised but NOT refreshed.
#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include "board_pins.h"

// No SPIClass of our own: M5GFX already started Arduino's GLOBAL `SPI` object
// on this bus (M5GFX common.inl: SPI.begin(...); handle = SPI.bus()). A second
// SPIClass(FSPI) = two owners of one bus -> "addApbChangeCallback(): duplicate"
// and a hung mount. Share the global one, exactly as M5Stack's examples do.

static void probeSD() {
  Serial.println("\n--- SD probe ---");
  Serial.printf("G1 level now: %d\n", digitalRead(PIN_BTN_C));

  // Fastest-first: stop at the first clock that works (gotcha 25d).
  static constexpr uint32_t speeds[] = { 20000000, 10000000, 4000000, 1000000, 400000 };
  uint32_t hz = 0;
  for (uint32_t s : speeds) {
    if (SD.begin(PIN_SD_CS, SPI, s)) { hz = s; break; }
    SD.end();
  }
  if (!hz) {
    Serial.println("Mount FAILED at every speed (no card, unformatted/exFAT, or no power)");
    return;
  }

  const char* types[] = { "NONE", "MMC", "SD", "SDHC", "UNKNOWN" };
  uint8_t t = SD.cardType();
  // uint64_t: a >4 GB size doesn't fit in 32 bits (gotcha 36).
  Serial.printf("Mounted at %lu kHz  type:%s  card:%llu MB  fs used:%llu / %llu MB\n",
                (unsigned long)(hz / 1000), types[t > 4 ? 4 : t],
                SD.cardSize() >> 20, SD.usedBytes() >> 20, SD.totalBytes() >> 20);

  // READ-ONLY: the card in the slot is someone's Raspberry Pi boot card.
  // (The write -> read back -> delete round-trip passed on 2026-10-02.)

  Serial.println("root directory:");
  File root = SD.open("/");
  int n = 0;
  for (File e = root.openNextFile(); e && n < 20; e = root.openNextFile(), ++n)
    Serial.printf("  %s%s  %lu\n", e.name(), e.isDirectory() ? "/" : "",
                  (unsigned long)e.size());
  if (n == 0) Serial.println("  (empty)");
  root.close();

  SD.end();  // unmount so the card can be pulled safely between probes
  Serial.println("unmounted — safe to remove");
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);
  Serial.println("\nPaperColor Stage 3b: microSD probe");

  // Display FIRST: M5.begin() powers the panel AND the SD slot via the PMIC,
  // and the panel must own the SPI bus before SPIClass touches it (gotcha 25c).
  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;  // no refresh: the swatches stay on the glass
  M5.begin(cfg);

  probeSD();
  Serial.println("\nWatching G1. Insert/remove the card; press A (G10) to re-probe.");
}

void loop() {
  M5.update();  // M5Unified's debounced button state
  if (M5.BtnA.wasPressed()) probeSD();

  // Raw G1 watch: does inserting/removing a card move it?
  static int g1 = -1;
  int v = digitalRead(PIN_BTN_C);
  if (v != g1) {
    Serial.printf("[%7.3f s] G1 = %d\n", millis() / 1000.0, v);
    g1 = v;
  }
  delay(5);
}
