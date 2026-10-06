// Lesson 04, Part 6 — L1 STANDBY dashboard v2: verified timer arming.
//
// The vendor schematic's power modes: L2 "DeepSleep" keeps the main 3.3 V
// converter (ESP32, sensors...) powered — that is what v2 did, and it drained
// ~35 mA. L1 "Standby" leaves only the power chip (M5PM1) + RTC alive (vendor
// figure 92.53 uA). The M5PM1's own timer powers the board back on.
//
//  Every power-on (timer every 2 min, power button, or USB plug-in):
//    read SHT40 + RTC -> decide -> maybe refresh -> save state -> arm the
//    PMIC timer -> PMIC power-off. The ESP32 is fully OFF in between, so
//    RTC_DATA_ATTR memory is gone. State lives in:
//      - the M5PM1's 32-byte RTC_MEM (0xA0-0xBF), kept while the PMIC has power
//      - NVS flash for the hourly battery log (24 writes/day: fine for wear)
//  Power button / USB plug-in -> refresh + 30 s service window (send LOG).
//  Reset after a flash (not a PMIC power-on) -> fresh state + 20 s flash window.
//
//  v2 (after the first 24 h test stopped waking at ~03:00 with the timer unarmed):
//   - every PMIC write is checked and the timer is READ BACK before power-off
//     (up to 3 attempts); never power off without a confirmed timer
//   - if arming fails: ESP32 deep sleep for this cycle instead (costs more power
//     once, never leaves the board dead)
//   - counters for retries / fallbacks / failed power-offs, reported by LOG
//   - a deep-sleep wake (reset reason DEEPSLEEP) continues the run; v1 treated
//     it as a dev boot and would have wiped state + log
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <time.h>
#include "board_pins.h"
#include "Sht40.h"

static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";
static constexpr uint32_t WAKE_EVERY_S     = 120;
static constexpr uint32_t REFRESH_EVERY_S  = 600;
static constexpr uint32_t BATT_LOG_EVERY_S = 3600;
static constexpr float    EARLY_DELTA_C    = 0.5f;
static constexpr uint32_t FLASH_WINDOW_MS  = 20000;
static constexpr uint32_t SERVICE_MS       = 30000;

static constexpr uint32_t INK_BLACK = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED   = 0xBF0000, INK_BLUE  = 0x6440FF;

// M5PM1 registers (datasheet; sequence copied from M5Stack's M5PM1 library)
static constexpr uint8_t PM_WAKE_SRC = 0x05;  // write-0-to-clear flags
static constexpr uint8_t PM_SYS_CMD  = 0x0C;  // 0xA0 key | 0x01 = power off
static constexpr uint8_t PM_TIM_CNT0 = 0x38;  // 4 bytes, seconds, LSB first
static constexpr uint8_t PM_TIM_CFG  = 0x3C;  // 0x08 ARM | ACTION (0b011 = power on)
static constexpr uint8_t PM_TIM_KEY  = 0x3D;  // write 0xA5 to (re)load
static constexpr uint8_t PM_RTC_MEM  = 0xA0;  // 32 bytes kept across ESP32 power-off
enum : uint8_t { WAKE_TIM = 1 << 0, WAKE_VIN = 1 << 1, WAKE_PWRBTN = 1 << 2 };

static Sht40 sht(m5::In_I2C);

// ---- state that survives ESP32 power-off: <= 32 bytes, in the PMIC ----
struct __attribute__((packed)) State {
  uint16_t magic;
  uint32_t wakes, refreshes;
  uint32_t lastRefreshAt, lastBattLogAt, bootAt;  // UTC epoch seconds
  int16_t  shownT_cC, minT_cC, maxT_cC;           // centi-degrees C
  uint16_t lastAwakeMs;
  uint8_t  armRetries, fallbacks;                 // saturating diagnostics
};
static_assert(sizeof(State) <= 32, "State must fit the M5PM1's 32-byte RTC_MEM");
static constexpr uint16_t MAGIC = 0x4C31;  // "L1"
static State st;

// ---- hourly battery log in NVS flash ----
struct BattEntry { uint32_t t; uint16_t batt_mV; int16_t temp_cC; };
static constexpr int BATT_LOG = 120;
static Preferences prefs;

static float curT = NAN, curRH = NAN;

static inline void bump(uint8_t& c) { if (c < 255) ++c; }

static bool pmRead(uint8_t reg, uint8_t* buf, size_t n) {
  return M5.Power.M5pm1.readRegister(reg, buf, n);
}
static bool pmWrite(uint8_t reg, const uint8_t* buf, size_t n) {
  return M5.Power.M5pm1.writeRegister(reg, buf, n);
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
  float minT = st.minT_cC / 100.0f, maxT = st.maxT_cC / 100.0f;

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
  snprintf(buf, sizeof buf, "batt %.2f V  -  up %.1f d  -  wake #%lu  (standby)",
           battV, (now - st.bootAt) / 86400.0f, (unsigned long)st.wakes);
  M5.Display.drawString(buf, W / 2, by + bh + 104);
  snprintf(buf, sizeof buf, "refresh #%lu - last wake %u ms - arm r%u f%u",
           (unsigned long)st.refreshes + 1, st.lastAwakeMs, st.armRetries, st.fallbacks);
  M5.Display.drawString(buf, W / 2, by + bh + 128);
}

static void battLogAppend(uint32_t t, uint16_t mV, int16_t cC) {
  BattEntry e = { t, mV, cC };
  uint32_t n = prefs.getUInt("n", 0);
  char key[8];
  snprintf(key, sizeof key, "b%u", (unsigned)(n % BATT_LOG));
  prefs.putBytes(key, &e, sizeof e);
  prefs.putUInt("n", n + 1);
}

static void dumpLog() {
  uint32_t n = prefs.getUInt("n", 0), kept = min<uint32_t>(n, BATT_LOG);
  Serial.println("# battery log (hourly, NVS)");
  Serial.println("n,utc_epoch,local,batt_mV,temp_c");
  for (uint32_t i = n - kept; i < n; ++i) {
    char key[8];
    snprintf(key, sizeof key, "b%u", (unsigned)(i % BATT_LOG));
    BattEntry e;
    if (prefs.getBytes(key, &e, sizeof e) != sizeof e) continue;
    time_t t = e.t;
    struct tm lt;  localtime_r(&t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%lu,%lu,%s,%u,%.2f\n", (unsigned long)i + 1, (unsigned long)e.t, loc,
                  e.batt_mV, e.temp_cC / 100.0f);
  }
  Serial.printf("# wakes %lu, refreshes %lu, last wake awake %u ms\n",
                (unsigned long)st.wakes, (unsigned long)st.refreshes, st.lastAwakeMs);
  Serial.printf("# timer arm retries %u, fallbacks to deep sleep %u, power-off failures %lu\n",
                st.armRetries, st.fallbacks, (unsigned long)prefs.getUInt("offfail", 0));
}

static void serialWindow(uint32_t ms) {
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < ms) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') { if (line == "LOG") dumpLog(); line = ""; }
      else if (c != '\r') line += c;
    }
    delay(10);
  }
}

// Arm the PMIC's "power on in N seconds" timer and READ IT BACK. v1 ignored the
// write results; one failed write among ~520 left the board off for 8.7 h.
static bool armTimer(uint32_t seconds) {
  const uint8_t cnt[4] = { (uint8_t)seconds, (uint8_t)(seconds >> 8),
                           (uint8_t)(seconds >> 16), (uint8_t)((seconds >> 24) & 0x7F) };
  const uint8_t cfg = 0x08 | 0x03, key = 0xA5;   // ARM | "system power on"; reload
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt) { bump(st.armRetries); delay(20); }
    bool ok = pmWrite(PM_TIM_CNT0, cnt, 4) && pmWrite(PM_TIM_CFG, &cfg, 1) &&
              pmWrite(PM_TIM_KEY, &key, 1);
    uint8_t rc[4] = {}, rcfg = 0;
    ok = ok && pmRead(PM_TIM_CNT0, rc, 4) && pmRead(PM_TIM_CFG, &rcfg, 1);
    uint32_t back = rc[0] | rc[1] << 8 | rc[2] << 16 | (uint32_t)(rc[3] & 0x7F) << 24;
    // The counter may already have ticked down a second or two.
    bool verified = ok && (rcfg & 0x0F) == cfg && back <= seconds && back + 3 >= seconds;
    if (verified) return true;
    Serial.printf("timer arm attempt %d failed: ok %d cfg 0x%02X count %lu\n",
                  attempt + 1, (int)ok, rcfg, (unsigned long)back);
  }
  return false;
}

[[noreturn]] static void powerOffUntil(uint32_t seconds) {
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Display.sleep();
  M5.Display.waitDisplay();

  M5.Power.M5pm1.clearWakeSource();                       // next wake: fresh flags

  bool armed = armTimer(seconds);
  if (!armed) {                        // never power off without a confirmed timer
    bump(st.fallbacks);
    uint8_t stop = 0x00;
    pmWrite(PM_TIM_CFG, &stop, 1);      // don't leave a half-armed timer behind
  }
  st.lastAwakeMs = (uint16_t)min<uint32_t>(millis(), 65535);
  pmWrite(PM_RTC_MEM, reinterpret_cast<const uint8_t*>(&st), sizeof st);

  if (armed) {
    Serial.printf("power-off for %lu s (awake %u ms, timer verified)\n",
                  (unsigned long)seconds, st.lastAwakeMs);
    Serial.flush();
    prefs.end();
    delay(120);                                           // as the M5PM1 library does
    uint8_t off = 0xA0 | 0x01;
    pmWrite(PM_SYS_CMD, &off, 1);
    delay(2000);                                          // still here? PMIC didn't cut us
    prefs.begin("dash", false);
    prefs.putUInt("offfail", prefs.getUInt("offfail", 0) + 1);
    prefs.end();
    Serial.println("PMIC power-off did not happen - deep sleep instead");
  } else {
    Serial.println("timer could not be armed/verified - deep sleep instead");
    prefs.end();
  }
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Power.M5pm1.setExtOutput(false);   // Grove 5 V off
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Led.setBrightness(40);

  // Why are we running? A PMIC power-on (timer / button / USB) shows up as a
  // POWERON reset; anything else (reset after flashing, crash) is a dev boot.
  uint8_t wake = 0;
  pmRead(PM_WAKE_SRC, &wake, 1);
  // POWERON = the PMIC switched us on; DEEPSLEEP = we fell back to deep sleep
  // last cycle. Both continue the run. Anything else (USB reset after flashing,
  // crash) is a dev boot.
  auto reason = esp_reset_reason();
  bool pmicPowerOn = (reason == ESP_RST_POWERON || reason == ESP_RST_DEEPSLEEP);
  bool timerWake   = pmicPowerOn && (wake & WAKE_TIM) && !(wake & (WAKE_PWRBTN | WAKE_VIN));
  bool serviceWake = pmicPowerOn && (wake & (WAKE_PWRBTN | WAKE_VIN));

  M5.Rtc.setSystemTimeFromRtc();         // RX8130 (on the always-on rail) = truth
  setenv("TZ", TZ_LONDON, 1);
  tzset();
  time_t now = time(nullptr);

  prefs.begin("dash", false);
  pmRead(PM_RTC_MEM, reinterpret_cast<uint8_t*>(&st), sizeof st);
  bool fresh = !pmicPowerOn || st.magic != MAGIC;
  if (fresh) {
    st = {};
    st.magic = MAGIC;
    st.bootAt = now;
    st.shownT_cC = INT16_MIN;
    st.minT_cC = INT16_MAX;
    st.maxT_cC = INT16_MIN;
    prefs.clear();                       // new run: new battery log
  }
  ++st.wakes;

  float battV = M5.Power.getBatteryVoltage() / 1000.0f;   // measure before refreshing
  Sht40::Reading r;
  if (sht.read(r) && r.crcOk) { curT = r.tempC; curRH = r.rh; }
  int16_t cC = isnan(curT) ? 0 : (int16_t)lroundf(curT * 100);
  if (!isnan(curT)) {
    st.minT_cC = min(st.minT_cC, cC);
    st.maxT_cC = max(st.maxT_cC, cC);
  }
  if (fresh || now - (time_t)st.lastBattLogAt >= (time_t)BATT_LOG_EVERY_S) {
    battLogAppend((uint32_t)now, (uint16_t)(battV * 1000), cC);
    st.lastBattLogAt = (uint32_t)now;
  }

  char why = 0;
  if (fresh)                                                       why = 'B';
  else if (serviceWake)                                            why = 'P';
  else if (now - (time_t)st.lastRefreshAt >= (time_t)REFRESH_EVERY_S) why = 'S';
  else if (st.shownT_cC == INT16_MIN ||
           abs(cC - st.shownT_cC) >= (int)(EARLY_DELTA_C * 100))   why = 'E';

  if (why) {
    drawScreen(battV);
    M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
    M5.Display.display();
    M5.Display.waitDisplay();
    M5.Led.setAllColor(0, 0, 0);    M5.Led.display();
    ++st.refreshes;
    st.shownT_cC = cC;
    st.lastRefreshAt = (uint32_t)now;
  }

  Serial.printf("wake #%lu (%s, WAKE_SRC 0x%02X, reset %d): T %.2f batt %.2f V %s\n",
                (unsigned long)st.wakes,
                fresh ? "fresh" : serviceWake ? "button/USB" : timerWake ? "timer" : "other",
                wake, (int)esp_reset_reason(), curT, battV, why ? "refreshed" : "");
  if (fresh && !pmicPowerOn) {
    Serial.println("dev boot: flash window; send LOG to dump");
    serialWindow(FLASH_WINDOW_MS);
  } else if (serviceWake) {
    M5.Led.setColor(0, 0, 0, 255);  M5.Led.display();
    Serial.println("power-button/USB wake: 30 s service window; send LOG to dump");
    serialWindow(SERVICE_MS);
  }
  powerOffUntil(WAKE_EVERY_S);
}

void loop() {}
