// Lesson 04, Part 1 — Deep-sleep dashboard.
// The ESP32 sleeps between short wakes; the e-paper keeps the picture for free.
//
//  Every wake (timer every 2 min, or any button):
//    read SHT40 + RTC -> decide -> maybe refresh -> deep sleep again.
//  Refresh if: button pressed | 10 min since last refresh | T moved >= 0.5 C.
//  Otherwise the display is never touched (no refresh = no 16 s, no power).
//
//  Deep sleep = REBOOT on wake: setup() runs every time, normal globals reset.
//  State that must survive lives in RTC memory (RTC_DATA_ATTR).
//
//  Flash window: after a cold boot / reset the board stays awake ~20 s (the
//  first refresh + a pause) so uploads work (gotcha 23). Timer/button wakes
//  are short. Recovery: hold the side button ~3 s for download mode.
#include <Arduino.h>
#include <M5Unified.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "board_pins.h"
#include "Sht40.h"

static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";
static constexpr uint32_t WAKE_EVERY_S     = 120;   // sample interval
static constexpr uint32_t REFRESH_EVERY_S  = 600;   // scheduled refresh
static constexpr float    EARLY_DELTA_C    = 0.5f;
static constexpr uint32_t FLASH_WINDOW_MS  = 20000; // cold boot only

static constexpr uint32_t INK_BLACK = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED   = 0xBF0000, INK_BLUE  = 0x6440FF;

// The three buttons: external pull-ups, active LOW, all RTC-capable (<= G21),
// so EXT1 "any low" can wake on any of them with no internal pull-ups needed.
static constexpr uint64_t BUTTON_MASK =
    (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B) | (1ULL << PIN_BTN_C);

static Sht40 sht(m5::In_I2C);  // m5::In_I2C, not M5.In_I2C (Lesson 03 Part 4)

// ---- state that survives deep sleep (RTC slow memory, ~8 KB on the S3) ----
struct LogEntry { time_t t; float tempC, rh; uint16_t batt_mV, awakeMs; char why; };
static constexpr uint32_t MAGIC = 0x50415045;  // "PAPE": is RTC memory valid?
static constexpr int LOG_SIZE = 64;
RTC_DATA_ATTR static uint32_t rtcMagic;
RTC_DATA_ATTR static uint32_t wakes, refreshes;
RTC_DATA_ATTR static float    shownT, minT, maxT;
RTC_DATA_ATTR static time_t   lastRefreshAt;
RTC_DATA_ATTR static uint16_t lastAwakeMs;
RTC_DATA_ATTR static LogEntry logBuf[LOG_SIZE];
RTC_DATA_ATTR static int      logCount;

static float curT = NAN, curRH = NAN;

static const char* causeName(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_EXT1:  return "button";
    default:                     return "power-on/reset";
  }
}

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
  M5.Display.fillCircle(x0 + tw + gap + degR, y0 + 8 + degR, degR, tInk);   // degree
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
  snprintf(buf, sizeof buf, "wake #%lu  -  refresh #%lu  -  batt %.2f V",
           (unsigned long)wakes, (unsigned long)refreshes + 1, battV);
  M5.Display.drawString(buf, W / 2, by + bh + 104);
  snprintf(buf, sizeof buf, "last wake awake %u ms", lastAwakeMs);
  M5.Display.drawString(buf, W / 2, by + bh + 128);
}

static void dumpLog() {
  int n = min(logCount, LOG_SIZE);
  Serial.println("n,utc_epoch,local,temp_c,rh,batt_mV,awake_ms,why");
  for (int i = logCount - n; i < logCount; ++i) {
    const LogEntry& e = logBuf[i % LOG_SIZE];
    struct tm lt;  localtime_r(&e.t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%d,%ld,%s,%.2f,%.1f,%u,%u,%c\n", i + 1, (long)e.t, loc, e.tempC,
                  e.rh, e.batt_mV, e.awakeMs, e.why);
  }
  Serial.printf("# %d entries (%d kept)\n", logCount, n);
}

[[noreturn]] static void goToSleep() {
  // Don't sleep with a button still held: EXT1 "any low" would wake at once.
  uint32_t t0 = millis();
  while ((!digitalRead(PIN_BTN_A) || !digitalRead(PIN_BTN_B) || !digitalRead(PIN_BTN_C))
         && millis() - t0 < 3000) delay(10);

  lastAwakeMs = (uint16_t)min<uint32_t>(millis(), 65535);
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Display.sleep();                       // panel DEEP_SLEEP command (0x07 0xA5)
  M5.Display.waitDisplay();

  // G45/G46 aren't RTC pins: in deep sleep they'd float, and a floating
  // speaker-amp enable is a gamble. Latch them LOW through sleep (gotcha 19c).
  gpio_hold_en((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_en((gpio_num_t)PIN_SPK_EN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_timer_wakeup((uint64_t)WAKE_EVERY_S * 1000000ULL);
  esp_sleep_enable_ext1_wakeup(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  Serial.printf("sleeping %lu s (awake %u ms)\n", (unsigned long)WAKE_EVERY_S, lastAwakeMs);
  Serial.flush();
  esp_deep_sleep_start();                   // never returns: next wake = reboot
}

void setup() {
  // Release the holds from the previous sleep, then drive the pins ourselves.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_dis((gpio_num_t)PIN_SPK_EN);
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool coldBoot = (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT1);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  if (coldBoot) {                           // only wait for a monitor on cold boot
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) delay(10);
  }

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;  // NEVER: the screen already shows our last frame
  M5.begin(cfg);
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Led.setBrightness(40);

  // Time: the RX8130 is the truth. The ESP32's own sleep clock runs on an
  // internal RC oscillator and drifts, so re-read the RTC every wake (no edge
  // wait: HH:MM on screen doesn't need sub-second accuracy).
  M5.Rtc.setSystemTimeFromRtc();
  setenv("TZ", TZ_LONDON, 1);
  tzset();

  if (coldBoot || rtcMagic != MAGIC) {      // fresh start: reset the RTC memory
    rtcMagic = MAGIC;
    wakes = refreshes = 0;
    minT = maxT = shownT = NAN;
    lastRefreshAt = 0;
    lastAwakeMs = 0;
    logCount = 0;
  }
  ++wakes;

  Sht40::Reading r;
  if (sht.read(r) && r.crcOk) { curT = r.tempC; curRH = r.rh; }
  if (!isnan(curT)) {
    if (isnan(minT) || curT < minT) minT = curT;
    if (isnan(maxT) || curT > maxT) maxT = curT;
  }
  float battV = M5.Power.getBatteryVoltage() / 1000.0f;

  uint64_t btn = (cause == ESP_SLEEP_WAKEUP_EXT1) ? esp_sleep_get_ext1_wakeup_status() : 0;
  time_t now = time(nullptr);
  char why = 0;
  if (coldBoot)                                              why = 'B';
  else if (btn)                                              why = 'C';
  else if (now - lastRefreshAt >= (time_t)REFRESH_EVERY_S)   why = 'S';
  else if (fabsf(curT - shownT) >= EARLY_DELTA_C)            why = 'E';

  Serial.printf("\nwake #%lu (%s%s): T %.2f RH %.1f batt %.2f V -> %s\n",
                (unsigned long)wakes, causeName(cause),
                btn ? (btn & (1ULL << PIN_BTN_C) ? " C" : btn & (1ULL << PIN_BTN_B) ? " B" : " A") : "",
                curT, curRH, battV, why ? "REFRESH" : "no refresh");

  if (why) {
    drawScreen(battV);
    M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
    M5.Display.display();
    M5.Display.waitDisplay();
    ++refreshes;
    shownT = curT;
    lastRefreshAt = now;
  }

  LogEntry& e = logBuf[logCount % LOG_SIZE];
  e = { now, curT, curRH, (uint16_t)(battV * 1000), lastAwakeMs, why ? why : '-' };
  ++logCount;

  if (coldBoot) {  // flash window + a chance to type LOG
    Serial.println("cold boot: awake for the flash window; send LOG to dump");
    uint32_t t0 = millis();
    String line;
    while (millis() - t0 < FLASH_WINDOW_MS) {
      while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') { if (line == "LOG") dumpLog(); line = ""; }
        else if (c != '\r') line += c;
      }
      delay(10);
    }
  }
  goToSleep();
}

void loop() {}  // never reached: every path ends in deep sleep
