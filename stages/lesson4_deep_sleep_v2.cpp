// Lesson 04, Part 3 — Deep-sleep dashboard v2: every unneeded supply OFF.
//
//  Every wake (timer every 2 min, or any button):
//    read SHT40 + RTC -> decide -> maybe refresh -> supplies off -> deep sleep.
//  Refresh if: button | 10 min since last refresh | T moved >= 0.5 C.
//
//  v2 changes (from the Part 2 power bench):
//   - Grove 5 V boost OFF (M5Unified leaves it on: 24 mA with nothing attached)
//   - LED supply, SD power and panel power OFF while asleep
//     (M5.begin() switches them back on at the next wake)
//   - hourly BATTERY LOG (120 h) for the multi-day on-battery test (option A)
//   - after a BUTTON wake: 30 s serial window — send LOG to dump both logs
//     (plugging USB in doesn't reboot a sleeping board, so this is how to read it)
#include <Arduino.h>
#include <M5Unified.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "board_pins.h"
#include "Sht40.h"

static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";
static constexpr uint32_t WAKE_EVERY_S     = 120;
static constexpr uint32_t REFRESH_EVERY_S  = 600;
static constexpr uint32_t BATT_LOG_EVERY_S = 3600;
static constexpr float    EARLY_DELTA_C    = 0.5f;
static constexpr uint32_t FLASH_WINDOW_MS  = 20000;  // cold boot
static constexpr uint32_t SERVICE_MS       = 30000;  // after a button wake

static constexpr uint32_t INK_BLACK = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED   = 0xBF0000, INK_BLUE  = 0x6440FF;
static constexpr uint64_t BUTTON_MASK =
    (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B) | (1ULL << PIN_BTN_C);

static Sht40 sht(m5::In_I2C);

// ---- state that survives deep sleep ----
struct RefreshEntry { time_t t; float tempC, rh; uint16_t batt_mV; char why; };
struct BattEntry    { time_t t; uint16_t batt_mV; int16_t temp_cC; uint32_t wakes; };
static constexpr uint32_t MAGIC = 0x50415046;  // bumped for v2 layout
static constexpr int REFRESH_LOG = 32, BATT_LOG = 120;
RTC_DATA_ATTR static uint32_t rtcMagic;
RTC_DATA_ATTR static uint32_t wakes, refreshes;
RTC_DATA_ATTR static float    shownT, minT, maxT;
RTC_DATA_ATTR static time_t   lastRefreshAt, lastBattLogAt, bootAt;
RTC_DATA_ATTR static uint16_t lastAwakeMs;
RTC_DATA_ATTR static RefreshEntry refreshLog[REFRESH_LOG];
RTC_DATA_ATTR static BattEntry    battLog[BATT_LOG];
RTC_DATA_ATTR static int refreshCount, battCount;

static float curT = NAN, curRH = NAN;

static void drawScreen(float battV) {
  const int W = M5.Display.width();
  static const char* DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday" };
  static const char* MONTHS[] = { "January", "February", "March", "April", "May",
                                  "June", "July", "August", "September",
                                  "October", "November", "December" };
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[64];

  M5.Display.fillScreen(INK_WHITE);
  M5.Display.setTextColor(INK_BLACK);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextDatum(top_center);
  snprintf(buf, sizeof buf, "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);
  M5.Display.drawString(buf, W / 2, 24);

  const int bx = 20, by = 80, bw = W - 40, bh = 300;
  M5.Display.drawRoundRect(bx, by, bw, bh, 18, INK_BLACK);
  M5.Display.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 17, INK_BLACK);
  uint32_t tInk = curT < 18 ? INK_BLUE : (curT > 25 ? INK_RED : INK_BLACK);

  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(tInk);
  M5.Display.setTextDatum(top_left);
  snprintf(buf, sizeof buf, "%.1f", curT);
  const int tw = M5.Display.textWidth(buf), cw = M5.Display.textWidth("C");
  const int degR = 9, gap = 6;
  const int x0 = (W - (tw + gap + 2 * degR + gap + cw)) / 2, y0 = by + 50;
  M5.Display.drawString(buf, x0, y0);
  M5.Display.fillCircle(x0 + tw + gap + degR, y0 + 8 + degR, degR, tInk);
  M5.Display.fillCircle(x0 + tw + gap + degR, y0 + 8 + degR, degR / 2, INK_WHITE);
  M5.Display.drawString("C", x0 + tw + gap + 2 * degR + gap, y0);
  M5.Display.setTextSize(1);

  M5.Display.setTextColor(INK_BLACK);
  M5.Display.setTextDatum(top_center);
  snprintf(buf, sizeof buf, "%.0f %% RH", curRH);
  M5.Display.drawString(buf, W / 2, by + 190);

  M5.Display.setFont(&fonts::FreeSans12pt7b);
  snprintf(buf, sizeof buf, "min %.1f  -  max %.1f", minT, maxT);
  M5.Display.drawString(buf, W / 2, by + bh + 22);

  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  strftime(buf, sizeof buf, "Updated %H:%M %Z", &lt);
  M5.Display.drawString(buf, W / 2, by + bh + 64);

  M5.Display.setFont(&fonts::FreeSans9pt7b);
  float days = (now - bootAt) / 86400.0f;
  snprintf(buf, sizeof buf, "batt %.2f V  -  up %.1f d  -  wake #%lu",
           battV, days, (unsigned long)wakes);
  M5.Display.drawString(buf, W / 2, by + bh + 104);
  snprintf(buf, sizeof buf, "refresh #%lu  -  last wake %u ms",
           (unsigned long)refreshes + 1, lastAwakeMs);
  M5.Display.drawString(buf, W / 2, by + bh + 128);
}

static void dumpLogs() {
  char loc[24];
  struct tm lt;
  int n = min(battCount, BATT_LOG);
  Serial.println("# battery log (hourly)");
  Serial.println("n,utc_epoch,local,batt_mV,temp_c,wakes");
  for (int i = battCount - n; i < battCount; ++i) {
    const BattEntry& e = battLog[i % BATT_LOG];
    localtime_r(&e.t, &lt);  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%d,%ld,%s,%u,%.2f,%lu\n", i + 1, (long)e.t, loc, e.batt_mV,
                  e.temp_cC / 100.0f, (unsigned long)e.wakes);
  }
  n = min(refreshCount, REFRESH_LOG);
  Serial.println("# refresh log");
  Serial.println("n,utc_epoch,local,temp_c,rh,batt_mV,why");
  for (int i = refreshCount - n; i < refreshCount; ++i) {
    const RefreshEntry& e = refreshLog[i % REFRESH_LOG];
    localtime_r(&e.t, &lt);  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%d,%ld,%s,%.2f,%.1f,%u,%c\n", i + 1, (long)e.t, loc, e.tempC,
                  e.rh, e.batt_mV, e.why);
  }
  Serial.printf("# wakes %lu, refreshes %lu, last wake awake %u ms\n",
                (unsigned long)wakes, (unsigned long)refreshes, lastAwakeMs);
}

// Listen for "LOG" on serial for `ms` (cold-boot flash window / service window).
static void serialWindow(uint32_t ms) {
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < ms) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') { if (line == "LOG") dumpLogs(); line = ""; }
      else if (c != '\r') line += c;
    }
    delay(10);
  }
}

[[noreturn]] static void goToSleep() {
  uint32_t t0 = millis();
  while ((!digitalRead(PIN_BTN_A) || !digitalRead(PIN_BTN_B) || !digitalRead(PIN_BTN_C))
         && millis() - t0 < 3000) delay(10);

  lastAwakeMs = (uint16_t)min<uint32_t>(millis(), 65535);
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Display.sleep();                       // panel controller DEEP_SLEEP
  M5.Display.waitDisplay();

  // Every supply we don't need while asleep (Part 2 power bench):
  auto& pm = M5.Power.M5pm1;
  pm.setExtOutput(false);                            // Grove 5 V boost (24 mA!)
  pm.setLDOOutput(false);                            // RGB LED supply
  pm.setGPIOOutput(m5::M5PM1_Class::gpio3, false);   // SD card power
  pm.setGPIOOutput(m5::M5PM1_Class::gpio0, false);   // e-paper power (image stays)

  gpio_hold_en((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_en((gpio_num_t)PIN_SPK_EN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_timer_wakeup((uint64_t)WAKE_EVERY_S * 1000000ULL);
  esp_sleep_enable_ext1_wakeup(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_dis((gpio_num_t)PIN_SPK_EN);
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  auto cause = esp_sleep_get_wakeup_cause();
  bool coldBoot = (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT1);
  bool buttonWake = (cause == ESP_SLEEP_WAKEUP_EXT1);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Power.M5pm1.setExtOutput(false);  // Grove 5 V off while awake too
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Led.setBrightness(40);

  M5.Rtc.setSystemTimeFromRtc();       // RX8130 = truth (ESP sleep clock drifts)
  setenv("TZ", TZ_LONDON, 1);
  tzset();
  time_t now = time(nullptr);

  if (coldBoot || rtcMagic != MAGIC) {
    rtcMagic = MAGIC;
    wakes = refreshes = 0;
    minT = maxT = shownT = NAN;
    lastRefreshAt = lastBattLogAt = 0;
    bootAt = now;
    lastAwakeMs = 0;
    refreshCount = battCount = 0;
  }
  ++wakes;

  // Measure FIRST, before any refresh: battery voltage under the same light load.
  float battV = M5.Power.getBatteryVoltage() / 1000.0f;
  Sht40::Reading r;
  if (sht.read(r) && r.crcOk) { curT = r.tempC; curRH = r.rh; }
  if (!isnan(curT)) {
    if (isnan(minT) || curT < minT) minT = curT;
    if (isnan(maxT) || curT > maxT) maxT = curT;
  }

  if (coldBoot || now - lastBattLogAt >= (time_t)BATT_LOG_EVERY_S) {
    battLog[battCount % BATT_LOG] = { now, (uint16_t)(battV * 1000),
                                      (int16_t)lroundf(curT * 100), wakes };
    ++battCount;
    lastBattLogAt = now;
  }

  char why = 0;
  if (coldBoot)                                              why = 'B';
  else if (buttonWake)                                       why = 'C';
  else if (now - lastRefreshAt >= (time_t)REFRESH_EVERY_S)   why = 'S';
  else if (fabsf(curT - shownT) >= EARLY_DELTA_C)            why = 'E';

  if (why) {
    drawScreen(battV);
    M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
    M5.Display.display();
    M5.Display.waitDisplay();
    M5.Led.setAllColor(0, 0, 0);    M5.Led.display();
    ++refreshes;
    shownT = curT;
    lastRefreshAt = now;
    refreshLog[refreshCount % REFRESH_LOG] = { now, curT, curRH,
                                               (uint16_t)(battV * 1000), why };
    ++refreshCount;
  }

  Serial.printf("wake #%lu (%s): T %.2f RH %.1f batt %.2f V %s\n",
                (unsigned long)wakes, coldBoot ? "cold" : buttonWake ? "button" : "timer",
                curT, curRH, battV, why ? "refreshed" : "");
  if (coldBoot) {
    Serial.println("cold boot: flash window; send LOG to dump");
    serialWindow(FLASH_WINDOW_MS);
  } else if (buttonWake) {
    M5.Led.setColor(0, 0, 0, 255);  M5.Led.display();   // blue = service window
    Serial.println("button wake: 30 s service window; send LOG to dump");
    serialWindow(SERVICE_MS);
  }
  goToSleep();
}

void loop() {}
