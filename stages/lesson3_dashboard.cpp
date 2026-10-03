// Lesson 03, Part 4 — E-paper dashboard + the cold-refresh experiment.
//  Screen: date, big temperature (ink by band), humidity, min/max since boot,
//          "Updated HH:MM BST" (not a clock — e-paper is mostly stale), and the
//          previous refresh's measured BUSY REFRESH time.
//  Refresh: every 10 min; early if T moved >= 0.5 C (but >= 2 min apart);
//           button C forces one.
//  Experiment: every refresh is logged in RAM (time, T, RH, REFRESH ms, total
//              ms) — works unplugged, e.g. in the fridge. Serial "LOG" dumps it
//              as CSV; "SET YYYY-MM-DD HH:MM:SS" (UTC) sets the RTC.
#include <Arduino.h>
#include <M5Unified.h>
#include <time.h>
#include <sys/time.h>
#include "soc/gpio_reg.h"
#include "board_pins.h"
#include "Sht40.h"

static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";
static constexpr uint32_t SAMPLE_MS        = 10UL * 1000;       // read sensor
static constexpr uint32_t REFRESH_EVERY_MS = 10UL * 60 * 1000;  // scheduled
static constexpr uint32_t MIN_GAP_MS       = 2UL * 60 * 1000;   // early-refresh floor
static constexpr float    EARLY_DELTA_C    = 0.5f;

// Exact palette inks (Lessons 01/02): pass through every epd_mode untouched.
static constexpr uint32_t INK_BLACK = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED   = 0xBF0000, INK_BLUE  = 0x6440FF;

// NOT `sht(M5.In_I2C)`: M5.In_I2C is a reference *member* of the global M5
// object, filled in only when M5 is constructed. Globals in different .cpp
// files are constructed in an unspecified order (the "static initialization
// order fiasco"), so at this point M5's member could still read 0 -> crash on
// the first I2C call. m5::In_I2C is the bus object itself: its ADDRESS is
// fixed at link time, so binding a reference to it here is always safe.
static Sht40 sht(m5::In_I2C);

// ---- BUSY timing (Lesson 02's ISR, reduced to "longest LOW period") ----
static constexpr int MAX_EDGES = 32;
static volatile uint32_t edgeUs[MAX_EDGES];
static volatile uint8_t  edgeLevel[MAX_EDGES];
static volatile int      nEdges = 0;
static void IRAM_ATTR onBusyEdge() {
  int n = nEdges;
  if (n < MAX_EDGES) {
    edgeUs[n] = micros();
    edgeLevel[n] = (REG_READ(GPIO_IN_REG) >> PIN_EPD_BUSY) & 1;
    nEdges = n + 1;
  }
}
static uint32_t longestBusyMs() {
  uint32_t best = 0, lowStart = 0;
  bool low = false;
  for (int i = 0; i < nEdges; ++i) {
    if (edgeLevel[i] == 0) { lowStart = edgeUs[i]; low = true; }
    else if (low) { best = max(best, (edgeUs[i] - lowStart) / 1000); low = false; }
  }
  return best;
}

// ---- refresh log, kept in RAM (no USB needed) ----
struct LogEntry { time_t t; float tempC, rh; uint16_t refreshMs, totalMs; char why; };
static constexpr int LOG_SIZE = 200;   // a few KB; oldest overwritten when full
static LogEntry logBuf[LOG_SIZE];
static int logCount = 0;               // total ever written

// ---- state ----
static float curT = NAN, curRH = NAN, minT = NAN, maxT = NAN, shownT = NAN;
static uint32_t lastRefreshMs = 0, lastRefreshBusy = 0;
static bool everRefreshed = false;

static void syncSystemClock() {  // Part 3: on the RTC's second edge, then fix TZ
  int s0 = M5.Rtc.getTime().seconds;
  uint32_t t0 = millis();
  while (M5.Rtc.getTime().seconds == s0 && millis() - t0 < 1100) delay(1);
  M5.Rtc.setSystemTimeFromRtc();
  setenv("TZ", TZ_LONDON, 1);
  tzset();
}

static bool timeValid() { return time(nullptr) > 1700000000; }  // after Nov 2023

static void sample() {
  Sht40::Reading r;
  if (!sht.read(r) || !r.crcOk) { Serial.println("SHT40 read failed"); return; }
  curT = r.tempC;  curRH = r.rh;
  if (isnan(minT) || curT < minT) minT = curT;
  if (isnan(maxT) || curT > maxT) maxT = curT;
}

// Free fonts are 7-bit ASCII: there is no degree glyph, so draw a small ring.
static void drawDegree(int x, int y, int r, uint32_t ink) {
  M5.Display.fillCircle(x + r, y + r, r, ink);
  M5.Display.fillCircle(x + r, y + r, r / 2, INK_WHITE);
}

static void drawScreen() {
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

  // Date
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextDatum(top_center);
  if (timeValid()) snprintf(buf, sizeof buf, "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday,
                            MONTHS[lt.tm_mon]);
  else             snprintf(buf, sizeof buf, "time not set");
  M5.Display.drawString(buf, W / 2, 24);

  // Big box: temperature (ink by band) + humidity
  const int bx = 20, by = 80, bw = W - 40, bh = 300;
  M5.Display.drawRoundRect(bx, by, bw, bh, 18, INK_BLACK);
  M5.Display.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 17, INK_BLACK);
  uint32_t tInk = curT < 18 ? INK_BLUE : (curT > 25 ? INK_RED : INK_BLACK);

  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.setTextSize(2);                       // ~48 pt (scaled bitmap)
  M5.Display.setTextColor(tInk);
  M5.Display.setTextDatum(top_left);
  snprintf(buf, sizeof buf, "%.1f", curT);
  const int tw = M5.Display.textWidth(buf), cw = M5.Display.textWidth("C");
  const int degR = 9, gap = 6;
  const int x0 = (W - (tw + gap + 2 * degR + gap + cw)) / 2, y0 = by + 50;
  M5.Display.drawString(buf, x0, y0);
  drawDegree(x0 + tw + gap, y0 + 8, degR, tInk);
  M5.Display.drawString("C", x0 + tw + gap + 2 * degR + gap, y0);
  M5.Display.setTextSize(1);

  M5.Display.setTextColor(INK_BLACK);
  M5.Display.setTextDatum(top_center);
  snprintf(buf, sizeof buf, "%.0f %% RH", curRH);
  M5.Display.drawString(buf, W / 2, by + 190);

  // Min / max since boot
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  snprintf(buf, sizeof buf, "min %.1f  -  max %.1f", minT, maxT);
  M5.Display.drawString(buf, W / 2, by + bh + 22);

  // "Updated" — the honest clock
  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  if (timeValid()) strftime(buf, sizeof buf, "Updated %H:%M %Z", &lt);
  else             snprintf(buf, sizeof buf, "Updated (no time)");
  M5.Display.drawString(buf, W / 2, by + bh + 64);

  // The experiment, on screen
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  if (everRefreshed)
    snprintf(buf, sizeof buf, "last refresh %lu ms  -  #%d",
             (unsigned long)lastRefreshBusy, logCount);
  else
    snprintf(buf, sizeof buf, "first refresh since boot");
  M5.Display.drawString(buf, W / 2, by + bh + 112);
}

static void refresh(char why) {
  drawScreen();
  M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();     // busy
  nEdges = 0;
  uint32_t t0 = millis();
  M5.Display.display();
  M5.Display.waitDisplay();
  uint32_t total = millis() - t0;
  M5.Led.setColor(1, 0, 0, 0);    M5.Led.display();     // off: ambient display

  lastRefreshBusy = longestBusyMs();
  lastRefreshMs = millis();
  shownT = curT;
  everRefreshed = true;

  LogEntry& e = logBuf[logCount % LOG_SIZE];
  e = { time(nullptr), curT, curRH, (uint16_t)lastRefreshBusy, (uint16_t)total, why };
  ++logCount;
  Serial.printf("REFRESH #%d (%c): T %.2f RH %.1f  busy %lu ms  total %lu ms\n",
                logCount, why, curT, curRH, (unsigned long)lastRefreshBusy,
                (unsigned long)total);
}

static void dumpLog() {
  int n = min(logCount, LOG_SIZE), first = logCount - n;
  Serial.println("n,utc_epoch,local,temp_c,rh,refresh_busy_ms,display_total_ms,why");
  for (int i = first; i < logCount; ++i) {
    const LogEntry& e = logBuf[i % LOG_SIZE];
    struct tm lt;  localtime_r(&e.t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%d,%ld,%s,%.2f,%.1f,%u,%u,%c\n", i + 1, (long)e.t, loc, e.tempC,
                  e.rh, e.refreshMs, e.totalMs, e.why);
  }
  Serial.printf("# %d entries (%d kept)\n", logCount, n);
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { if (c != '\r') line += c; continue; }
    int Y, Mo, D, h, mi, s;
    if (line == "LOG") {
      dumpLog();
    } else if (sscanf(line.c_str(), "SET %d-%d-%d %d:%d:%d", &Y, &Mo, &D, &h, &mi, &s) == 6) {
      static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };  // Sakamoto
      int y = Mo < 3 ? Y - 1 : Y;
      m5::rtc_datetime_t dt;
      dt.date.year = Y;  dt.date.month = Mo;  dt.date.date = D;
      dt.date.weekDay = (y + y / 4 - y / 100 + y / 400 + t[Mo - 1] + D) % 7;
      dt.time.hours = h; dt.time.minutes = mi; dt.time.seconds = s;
      M5.Rtc.setDateTime(dt);
      syncSystemClock();
      Serial.printf("SET done: %s\n", line.c_str());
    } else if (line.length()) {
      Serial.printf("unknown command: %s  (try LOG)\n", line.c_str());
    }
    line = "";
  }
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nLesson 03 Part 4: dashboard + cold-refresh experiment");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);  // exact inks only
  M5.Led.setBrightness(40);
  M5.Led.setAllColor(0, 0, 0);  M5.Led.display();

  syncSystemClock();
  attachInterrupt(digitalPinToInterrupt(PIN_EPD_BUSY), onBusyEdge, CHANGE);

  uint32_t sn;
  if (sht.serialNumber(sn)) Serial.printf("SHT40 serial %08lX\n", (unsigned long)sn);
  sample();
  refresh('B');  // boot
}

void loop() {
  M5.update();
  handleSerial();

  static uint32_t lastSample = 0;
  uint32_t now = millis();
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    sample();
    static uint32_t lastPrint = 0;
    if (now - lastPrint >= 60000) {  // one SAMPLE line per minute (self-heating)
      lastPrint = now;
      Serial.printf("SAMPLE t+%lus  T %.2f RH %.1f  (min %.2f max %.2f)\n",
                    (unsigned long)(now / 1000), curT, curRH, minT, maxT);
    }
  }

  uint32_t since = now - lastRefreshMs;
  if (M5.BtnC.wasPressed())                                              refresh('C');
  else if (since >= REFRESH_EVERY_MS)                                    refresh('S');
  else if (since >= MIN_GAP_MS && fabsf(curT - shownT) >= EARLY_DELTA_C) refresh('E');
  delay(10);
}
