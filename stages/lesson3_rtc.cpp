// Lesson 03, Part 3 — The RX8130CE real-time clock via M5.Rtc.
//  - At boot: dump the RTC's raw registers (they're BCD!) and decode them.
//  - Serial command  "SET YYYY-MM-DD HH:MM:SS"  (UTC) sets the RTC.
//  - Every 10 s: print RTC time as UTC + epoch, and as UK local time.
// Design: the RTC holds UTC; local time (with BST) is computed in software.
#include <Arduino.h>
#include <M5Unified.h>
#include <time.h>
#include "board_pins.h"

static constexpr uint8_t RTC_ADDR = 0x32;
static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";  // POSIX TZ rule

static const char* WDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

// Day of week (0 = Sunday) — Sakamoto's method, no library needed.
static int dayOfWeek(int y, int m, int d) {
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void dumpRaw() {
  uint8_t r[7];
  if (!M5.In_I2C.readRegister(RTC_ADDR, 0x10, r, 7, 400000)) {
    Serial.println("raw read FAILED"); return;
  }
  // BCD: each 4-bit half is one decimal digit, so 0x59 means "59", not 89.
  Serial.printf("raw 0x10..0x16 (sec min hour wday day month year): "
                "%02X %02X %02X %02X %02X %02X %02X\n",
                r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
  Serial.printf("M5 getVoltLow(): %d   (flag register 0x1D = 0x%02X)\n",
                (int)M5.Rtc.getVoltLow(),
                M5.In_I2C.readRegister8(RTC_ADDR, 0x1D, 400000));
}

// Copy the RTC (UTC) into the ESP32's own clock — ONCE (boot / after SET).
// Two M5Unified quirks handled here (RTC_Class.inl:214-223):
//  - it saves the old TZ as a char* from getenv(), then overwrites that same
//    environment entry with "GMT0" and "restores" from the now-stale pointer,
//    and never calls tzset() — so our TZ must be re-applied afterwards;
//  - it zeroes the sub-second part, so calling it repeatedly makes the
//    system clock jump. Hence: sync rarely, then let the system clock run.
static void syncSystemClock() {
  // The RTC only reports WHOLE seconds. Copying mid-second leaves the system
  // clock up to 1 s behind, so wait for the seconds digit to tick over first.
  int s0 = M5.Rtc.getTime().seconds;
  uint32_t t0 = millis();
  while (M5.Rtc.getTime().seconds == s0 && millis() - t0 < 1100) delay(1);
  M5.Rtc.setSystemTimeFromRtc();
  setenv("TZ", TZ_LONDON, 1);
  tzset();
}

static void printNow(const char* tag) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) {  // false = invalid/corrupt registers (validated)
    Serial.printf("[%s] RTC holds no valid date/time\n", tag);
    return;
  }
  timeval tv;
  gettimeofday(&tv, nullptr);      // ESP32 system clock, with microseconds
  time_t now = tv.tv_sec;          // seconds since 1970-01-01 UTC ("epoch")
  struct tm local;
  localtime_r(&now, &local);       // uses TZ -> British time incl. BST
  char loc[40];
  strftime(loc, sizeof loc, "%a %Y-%m-%d %H:%M:%S %Z", &local);
  Serial.printf("[%s] RTC UTC %04d-%02d-%02d %02d:%02d:%02d (%s)  epoch %ld.%03ld  local %s\n",
                tag, dt.date.year, dt.date.month, dt.date.date, dt.time.hours,
                dt.time.minutes, dt.time.seconds, WDAY[dt.date.weekDay % 7],
                (long)now, (long)(tv.tv_usec / 1000), loc);
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { if (c != '\r') line += c; continue; }
    int Y, Mo, D, h, mi, s;
    if (sscanf(line.c_str(), "SET %d-%d-%d %d:%d:%d", &Y, &Mo, &D, &h, &mi, &s) == 6) {
      m5::rtc_datetime_t dt;
      dt.date.year = Y;  dt.date.month = Mo;  dt.date.date = D;
      dt.date.weekDay = dayOfWeek(Y, Mo, D);
      dt.time.hours = h; dt.time.minutes = mi; dt.time.seconds = s;
      M5.Rtc.setDateTime(dt);
      syncSystemClock();
      Serial.printf("SET done: %s\n", line.c_str());
      printNow("after SET");
      dumpRaw();
    } else if (line.length()) {
      Serial.printf("unknown command: %s\n", line.c_str());
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
  Serial.println("\nLesson 03 Part 3: RTC");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);

  syncSystemClock();

  Serial.printf("RTC enabled: %s\n", M5.Rtc.isEnabled() ? "yes" : "NO");
  dumpRaw();
  printNow("boot");
}

void loop() {
  handleSerial();
  static uint32_t last = 0;
  if (millis() - last >= 10000) { last = millis(); printNow("tick"); }
  delay(10);
}
