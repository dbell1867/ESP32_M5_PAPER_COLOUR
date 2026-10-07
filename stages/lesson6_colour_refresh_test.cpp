// Lesson 02, Part 2b — BUSY timeline: measure where a refresh's time goes.
// An interrupt timestamps every edge on the panel's BUSY pin while M5GFX's
// display() runs. We only LISTEN to the pin; the driver still owns it.
//   C (top-centre) -> redraw (showing the previous measurement) + measure
#include <Arduino.h>
#include <M5Unified.h>
#include "soc/gpio_reg.h"  // GPIO_IN_REG: read pin levels directly in the ISR
#include "board_pins.h"

// ---- ISR side: tiny, IRAM-resident, touches only volatile data ----------
static constexpr int MAX_EDGES = 32;
static volatile uint32_t edgeUs[MAX_EDGES];
static volatile uint8_t  edgeLevel[MAX_EDGES];
static volatile int      nEdges = 0;

static void IRAM_ATTR onBusyEdge() {
  int n = nEdges;
  if (n < MAX_EDGES) {
    edgeUs[n]    = micros();
    // Raw register read: safe in an ISR (digitalRead may not be IRAM-resident).
    edgeLevel[n] = (REG_READ(GPIO_IN_REG) >> PIN_EPD_BUSY) & 1;
    nEdges = n + 1;
  }
}

// ---- App side ----------------------------------------------------------
static int    runs = 0;
static String lastReport = "(no measurement yet)";

// Lesson 06 experiment: does refresh time depend on the colours shown?
// Each press of C cycles: 0 all WHITE, 1 all BLACK, 2 six solid ink bands.
static int pattern = 0;
static const char* PATTERN_NAME[] = { "all WHITE", "all BLACK", "six ink bands" };

static void measureRefresh() {
  ++runs;
  const int W = M5.Display.width(), H = M5.Display.height();
  int p = pattern;
  pattern = (pattern + 1) % 3;
  if (p == 0) M5.Display.fillScreen(TFT_WHITE);
  else if (p == 1) M5.Display.fillScreen(TFT_BLACK);
  else {
    static constexpr uint32_t INK[6] = { 0x000000, 0xFFFFFF, 0xFFF338, 0xBF0000, 0x6440FF, 0x438A1C };
    for (int i = 0; i < 6; ++i) M5.Display.fillRect(0, i * H / 6, W, H / 6 + 1, INK[i]);
  }
  Serial.printf("pattern: %s\n", PATTERN_NAME[p]);

  // Arm, then hand control to the driver for the whole refresh.
  nEdges = 0;
  int startLevel = digitalRead(PIN_EPD_BUSY);
  uint32_t t0 = micros();
  M5.Display.display();
  M5.Display.waitDisplay();
  uint32_t tEnd = micros();

  // Build the timeline: each LOW period is one busy phase of the panel.
  static const char* phase[] = { "POWER_ON", "REFRESH", "POWER_OFF", "?", "?", "?" };
  String r;
  char line[80];
  int n = nEdges, busyIdx = 0;
  uint32_t lowStart = 0;
  bool firstLow = true;
  snprintf(line, sizeof line, "BUSY level at start: %d\n", startLevel);
  r += line;
  for (int i = 0; i < n; ++i) {
    uint32_t t = edgeUs[i] - t0;
    if (edgeLevel[i] == 0) {             // went LOW: panel started working
      lowStart = t;
      if (firstLow) {
        snprintf(line, sizeof line, "transfer   : %6lu ms before 1st BUSY\n",
                 (unsigned long)(t / 1000));
        r += line;
        firstLow = false;
      }
    } else {                             // went HIGH: panel finished
      snprintf(line, sizeof line, "%-10s : %6lu ms busy (at %lu)\n",
               phase[busyIdx < 6 ? busyIdx : 5],
               (unsigned long)((t - lowStart) / 1000),
               (unsigned long)(lowStart / 1000));
      r += line;
      ++busyIdx;
    }
  }
  uint32_t total = (tEnd - t0) / 1000;
  snprintf(line, sizeof line, "edges: %d   total: %lu ms\n", n, (unsigned long)total);
  r += line;

  Serial.printf("\n--- refresh #%d ---\n%s", runs, r.c_str());
  lastReport = r;
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nLesson 02: BUSY timeline. Press C to refresh + measure.");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);

  // Listen on BUSY. attachInterrupt doesn't change the pin's mode — the
  // driver configured it as an input and still reads it normally.
  attachInterrupt(digitalPinToInterrupt(PIN_EPD_BUSY), onBusyEdge, CHANGE);

  M5.Led.setBrightness(40);
  M5.Led.setColor(1, 0, 255, 0);
  M5.Led.display();
}

void loop() {
  M5.update();
  if (M5.BtnC.wasPressed()) {
    M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
    measureRefresh();
    M5.Led.setColor(1, 0, 255, 0);  M5.Led.display();
  }
  delay(10);
}
