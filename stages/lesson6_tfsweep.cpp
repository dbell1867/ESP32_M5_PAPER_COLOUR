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
//
//  v3 — battery protection (Lesson 04 Part 7):
//   - hardware backstop: the PMIC's low-voltage cut-off (BATT_LVP) raised from
//     its 2.50 V default to 3.10 V, written + read back every wake
//   - < 3.50 V on two wakes in a row (and no USB): LOW mode — red "charge me"
//     banner, no early refreshes, scheduled refresh every 30 min
//   - < 3.30 V on two wakes in a row: draw "Battery empty" ONCE, then power
//     off with NO timer (only USB / power button wake it); e-paper keeps the
//     message at zero power
//   - test hook: in a service window send "SIMV 3.40" to simulate an on-battery
//     voltage (persists in NVS); "SIMV 0" clears it
#include <Arduino.h>
// SD.h BEFORE M5Unified.h: M5GFX decides at include time whether to compile its
// file-reading support (drawJpgFile(SD, ...)) by checking which FS headers it has
// already seen. Included after, it fails with "abstract type DataWrapperT<fs::SDFS>".
#include <SPI.h>
#include <SD.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "nvs.h"            // nvs_get_stats (refresh-log space check)
#include "soc/gpio_reg.h"   // GPIO_IN_REG for the ISR-safe BUSY read
#include "driver/gpio.h"    // light-sleep wake on BUSY
#include "esp_sleep.h"
#include "board_pins.h"
#include "Sht40.h"

static constexpr const char* TZ_LONDON = "GMT0BST,M3.5.0/1,M10.5.0";
static constexpr uint32_t WAKE_EVERY_S     = 120;
static constexpr uint32_t REFRESH_EVERY_S  = 600;
static constexpr uint32_t BATT_LOG_EVERY_S = 3600;
static constexpr float    EARLY_DELTA_C    = 0.5f;
static constexpr uint32_t FLASH_WINDOW_MS  = 20000;
static constexpr uint32_t SERVICE_MS       = 30000;

// Battery protection (LiPo, measured at the start of a wake = light load)
static constexpr float    LOW_V               = 3.50f;  // ~last 10 %
static constexpr float    EMPTY_V             = 3.30f;  // shut down while there's margin
static constexpr uint8_t  STREAK              = 2;      // consecutive wakes to confirm
static constexpr uint32_t REFRESH_EVERY_LOW_S = 1800;   // low battery: every 30 min

static constexpr uint32_t INK_BLACK = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED   = 0xBF0000, INK_BLUE  = 0x6440FF;

// M5PM1 registers (datasheet; sequence copied from M5Stack's M5PM1 library)
static constexpr uint8_t PM_PWR_SRC  = 0x04;  // bit0 = 5VIN (USB) valid
static constexpr uint8_t PM_WAKE_SRC = 0x05;  // write-0-to-clear flags
static constexpr uint8_t PM_BATT_LVP = 0x08;  // cut-off = 2.0 V + reg x 7.81 mV
static constexpr uint8_t LVP_3V10    = 0x8D;  // 141 -> 2.0 + 1.101 = 3.101 V (default 0x40 = 2.50 V)
static constexpr uint8_t PM_SYS_CMD  = 0x0C;  // 0xA0 key | 0x01 = power off
static constexpr uint8_t PM_TIM_CNT0 = 0x38;  // 4 bytes, seconds, LSB first
static constexpr uint8_t PM_TIM_CFG  = 0x3C;  // 0x08 ARM | ACTION (0b011 = power on)
static constexpr uint8_t PM_TIM_KEY  = 0x3D;  // write 0xA5 to (re)load
static constexpr uint8_t PM_RTC_MEM  = 0xA0;  // 32 bytes kept across ESP32 power-off
enum : uint8_t { WAKE_TIM = 1 << 0, WAKE_VIN = 1 << 1, WAKE_PWRBTN = 1 << 2 };

static Sht40 sht(m5::In_I2C);

// ---- refresh timing (Lesson 06 Part 3): Lesson 02's BUSY-pin listener ----
// Splits every refresh into "sending data" (until BUSY first goes LOW) and the
// panel's own busy phases (power-on, refresh, power-off).
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

// ---- light sleep while the panel refreshes (Lesson 06) ----
// The driver's busy-wait loop calls this hook (tools/patch_m5gfx.py) instead of
// delay(10). The panel does all the work during its 15-35 s refresh; the ESP32 only
// waits for BUSY to go HIGH — so on battery it light-sleeps until then (or 1 s, so the
// driver's 60 s timeout still counts). On USB it keeps the old delay: light sleep
// would stall the USB serial port, and there's no battery to save.
static bool     g_lightSleepWait = false;  // decided at boot (battery, or LS 1 test switch)
static bool     g_usedLightSleep = false;  // any light sleep during the latest refresh
static uint32_t g_lightSleeps = 0;         // light-sleep entries (diagnostic)

extern "C" void m5gfx_ed2208_busy_wait_hook(int pin) {
  if (!g_lightSleepWait || pin < 0) { delay(10); return; }
  gpio_num_t busy = (gpio_num_t)pin;
  gpio_intr_disable(busy);                       // our CHANGE ISR can't run asleep anyway
  gpio_wakeup_enable(busy, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(1000000);        // <= 1 s per nap: timeout still works
  Serial.flush();
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(busy);
  gpio_set_intr_type(busy, GPIO_INTR_ANYEDGE);   // back to the timing ISR
  gpio_intr_enable(busy);
  g_usedLightSleep = true;
  ++g_lightSleeps;
  // The ISR missed the rising edge while we slept: record it here (within ~1 ms).
  int n = nEdges;
  if (gpio_get_level(busy) && n > 0 && n < MAX_EDGES && edgeLevel[n - 1] == 0) {
    edgeUs[n] = micros();
    edgeLevel[n] = 1;
    nEdges = n + 1;
  }
}

static uint16_t g_lastRefreshMs = 0;   // panel REFRESH phase of the latest refresh
static uint16_t g_lastTransferMs = 0;  // SPI transfer before BUSY first went LOW
static uint8_t  g_wakeSrc = 0;         // PMIC WAKE_SRC of this boot (for the refresh log)

static void timedDisplay(const char* what) {
  nEdges = 0;
  g_usedLightSleep = false;
  uint32_t t0 = micros();
  M5.Display.display();
  M5.Display.waitDisplay();
  uint32_t t1 = micros();
  uint32_t transfer = 0, lowStart = 0, phase[4] = {};
  int np = 0;
  bool low = false;
  for (int i = 0; i < nEdges; ++i) {
    if (edgeLevel[i] == 0) {
      if (!low && np == 0) transfer = edgeUs[i] - t0;
      lowStart = edgeUs[i]; low = true;
    } else if (low) {
      if (np < 4) phase[np++] = (edgeUs[i] - lowStart) / 1000;
      low = false;
    }
  }
  uint32_t longest = max(max(phase[0], phase[1]), phase[2]);   // = REFRESH normally
  g_lastRefreshMs = (uint16_t)min<uint32_t>(longest, 65535);
  g_lastTransferMs = (uint16_t)min<uint32_t>(transfer / 1000, 65535);
  Serial.printf("refresh timing [%s]: total %lu ms, transfer %lu ms, busy phases %lu / %lu / %lu ms "
                "(power-on / REFRESH / power-off)%s\n", what, (unsigned long)((t1 - t0) / 1000),
                (unsigned long)(transfer / 1000), (unsigned long)phase[0],
                (unsigned long)phase[1], (unsigned long)phase[2],
                g_usedLightSleep ? " [light sleep]" : "");
}

// ---- state that survives ESP32 power-off: <= 32 bytes, in the PMIC ----
struct __attribute__((packed)) State {
  uint16_t magic;
  uint32_t wakes;
  uint16_t refreshes;                             // 65535 ~ 16 months at 136/day
  uint32_t lastRefreshAt, lastBattLogAt, bootAt;  // UTC epoch seconds
  int16_t  shownT_cC, minT_cC, maxT_cC;           // centi-degrees C
  uint16_t lastAwakeMs;
  uint8_t  armRetries, fallbacks;                 // saturating diagnostics
  uint8_t  lowStreak, emptyStreak;                // consecutive low / empty wakes
};
static_assert(sizeof(State) <= 32, "State must fit the M5PM1's 32-byte RTC_MEM");
static constexpr uint16_t MAGIC = 0x4C31;  // "L1"
static State st;

// ---- hourly battery log in NVS flash ----
struct BattEntry { uint32_t t; uint16_t batt_mV; int16_t temp_cC; uint16_t refreshMs; };
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

static void drawScreen(float battV, bool lowBatt) {
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
  M5.Display.setTextSize(1);   // never inherit a size from earlier drawing (TFSWEEP bug)
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
  Preferences sp;                             // last NTP sync (Lesson 05)
  sp.begin("sync", true);
  uint32_t lastSync = sp.isKey("last") ? sp.getUInt("last") : 0;
  sp.end();
  char synced[24];
  if (!lastSync) snprintf(synced, sizeof synced, "not synced");
  else {
    long h = (now - (time_t)lastSync) / 3600;
    if (h < 1) snprintf(synced, sizeof synced, "synced <1 h ago");
    else if (h < 48) snprintf(synced, sizeof synced, "synced %ld h ago", h);
    else snprintf(synced, sizeof synced, "synced %ld d ago", h / 24);
  }
  snprintf(buf, sizeof buf, "batt %.2f V  -  up %.1f d  -  %s",
           battV, (now - st.bootAt) / 86400.0f, synced);
  M5.Display.drawString(buf, W / 2, by + bh + 104);
  snprintf(buf, sizeof buf, "wake #%lu - refresh #%lu - %u ms - arm r%u f%u",
           (unsigned long)st.wakes, (unsigned long)st.refreshes + 1, st.lastAwakeMs,
           st.armRetries, st.fallbacks);
  M5.Display.drawString(buf, W / 2, by + bh + 128);

  if (lowBatt) {                              // red banner across the bottom
    const int H = M5.Display.height();
    M5.Display.fillRect(0, H - 52, W, 52, INK_RED);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(INK_WHITE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("LOW BATTERY - please charge", W / 2, H - 26);
  }
}

// Shown ONCE when the battery is empty; e-paper then keeps it at zero power.
static void drawEmptyScreen(float battV) {
  const int W = M5.Display.width(), H = M5.Display.height();
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[64];
  M5.Display.fillScreen(INK_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.fillRoundRect(20, 120, W - 40, 220, 18, INK_RED);
  M5.Display.setTextColor(INK_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.drawString("Battery empty", W / 2, 200);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString("Please charge", W / 2, 270);
  M5.Display.setTextColor(INK_BLACK);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.drawString("Connect USB-C. The display", W / 2, 400);
  M5.Display.drawString("restarts by itself when charging.", W / 2, 430);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  strftime(buf, sizeof buf, "Shut down %a %d %b %H:%M %Z", &lt);
  M5.Display.drawString(buf, W / 2, H - 70);
  snprintf(buf, sizeof buf, "battery %.2f V (shutdown below %.2f V)", battV, EMPTY_V);
  M5.Display.drawString(buf, W / 2, H - 45);
}

// ---- refresh log (Lesson 06): context for every refresh, to find what makes the
// panel's REFRESH switch between ~14.4 s and ~26.7 s. 16-byte records grouped 16 per
// NVS blob (NVS stores each key with ~32 B overhead, so one key per record would
// fill the 20 KB partition); 12 blobs = the last 192 refreshes (~1.3 days).
struct __attribute__((packed)) RefreshRec {
  uint32_t t;            // UTC epoch
  uint16_t refreshMs;    // panel REFRESH phase
  uint16_t transferMs;   // SPI transfer
  uint16_t batt_mV;
  int16_t  temp_cC;
  uint16_t minsSincePrev;
  uint8_t  wakeSrc;      // PMIC WAKE_SRC bits of the boot it happened in
  uint8_t  flags;        // bit0 USB present, bits1-3 kind (0 dash, 1 photo B, 2 photo A, 3 redraw, 4 empty),
                         // bit4 the ESP32 light-slept through the refresh
};
static_assert(sizeof(RefreshRec) == 16, "keep records 16 bytes");
static constexpr int RREC_PER_BLOB = 16, RBLOBS = 12;

static void logRefresh(uint8_t kind) {
  if (!g_lastRefreshMs) return;
  Preferences rl;
  rl.begin("rlog", false);
  uint32_t n = rl.getUInt("n", 0);
  uint32_t prevT = rl.getUInt("lastt", 0);
  uint32_t now = (uint32_t)time(nullptr);
  uint8_t src = 0;
  pmRead(0x04, &src, 1);                                  // PWR_SRC bit0 = USB
  RefreshRec r = { now, g_lastRefreshMs, g_lastTransferMs,
                   (uint16_t)M5.Power.getBatteryVoltage(),
                   (int16_t)(isnan(curT) ? 0 : lroundf(curT * 100)),
                   (uint16_t)(prevT && now > prevT ? min<uint32_t>((now - prevT) / 60, 65535) : 65535),
                   g_wakeSrc, (uint8_t)((src & 1) | (kind << 1) | (g_usedLightSleep ? 0x10 : 0)) };
  char key[8];
  uint32_t blob = (n / RREC_PER_BLOB) % RBLOBS, slot = n % RREC_PER_BLOB;
  snprintf(key, sizeof key, "r%u", (unsigned)blob);
  RefreshRec buf[RREC_PER_BLOB] = {};
  if (slot) rl.getBytes(key, buf, sizeof buf);            // continue the current blob
  buf[slot] = r;
  rl.putBytes(key, buf, sizeof buf);
  rl.putUInt("n", n + 1);
  rl.putUInt("lastt", now);
  rl.end();
}

static void dumpRefreshLog() {
  static const char* KIND[] = { "dash", "photoB", "photoA", "redraw", "empty", "?", "?", "?" };
  Preferences rl;
  rl.begin("rlog", true);
  uint32_t n = rl.getUInt("n", 0);
  // The blob being filled has already overwritten the oldest one, so once the ring has
  // wrapped only (RBLOBS - 1) full blobs + the current partial one are valid.
  uint32_t kept = min<uint32_t>(n, (RBLOBS - 1) * RREC_PER_BLOB + n % RREC_PER_BLOB);
  Serial.println("# refresh log");
  Serial.println("n,utc_epoch,local,refresh_ms,transfer_ms,batt_mV,temp_c,mins_since_prev,wake_src,usb,kind,light_sleep");
  RefreshRec buf[RREC_PER_BLOB];
  int loaded = -1;
  for (uint32_t i = n - kept; i < n; ++i) {
    int blob = (i / RREC_PER_BLOB) % RBLOBS;
    if (blob != loaded) {
      char key[8];
      snprintf(key, sizeof key, "r%d", blob);
      if (rl.getBytes(key, buf, sizeof buf) != sizeof buf) { loaded = -1; continue; }
      loaded = blob;
    }
    const RefreshRec& r = buf[i % RREC_PER_BLOB];
    time_t t = r.t;
    struct tm lt;  localtime_r(&t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%lu,%lu,%s,%u,%u,%u,%.2f,%u,0x%02X,%u,%s,%u\n", (unsigned long)i + 1,
                  (unsigned long)r.t, loc, r.refreshMs, r.transferMs, r.batt_mV,
                  r.temp_cC / 100.0f, r.minsSincePrev, r.wakeSrc, r.flags & 1,
                  KIND[(r.flags >> 1) & 7], (r.flags >> 4) & 1);
  }
  rl.end();
  nvs_stats_t st;
  if (nvs_get_stats(nullptr, &st) == ESP_OK)
    Serial.printf("# NVS entries: used %u, free %u, total %u\n", (unsigned)st.used_entries,
                  (unsigned)st.free_entries, (unsigned)st.total_entries);
  Serial.println("# end refresh log");
}

static void battLogAppend(uint32_t t, uint16_t mV, int16_t cC) {
  // refreshMs: the panel's REFRESH time at the most recent refresh — so a change
  // like 14.4 s -> 26.7 s (Lesson 06) shows up in the log with its date.
  BattEntry e = { t, mV, cC, prefs.isKey("lastref") ? prefs.getUShort("lastref") : (uint16_t)0 };
  uint32_t n = prefs.getUInt("n", 0);
  char key[8];
  snprintf(key, sizeof key, "b%u", (unsigned)(n % BATT_LOG));
  prefs.putBytes(key, &e, sizeof e);
  prefs.putUInt("n", n + 1);
}

static void dumpSyncLog();   // defined further down; declared here so LOG can call it

static void dumpLog() {
  uint32_t n = prefs.getUInt("n", 0), kept = min<uint32_t>(n, BATT_LOG);
  Serial.println("# battery log (hourly, NVS)");
  Serial.println("n,utc_epoch,local,batt_mV,temp_c,refresh_ms");
  for (uint32_t i = n - kept; i < n; ++i) {
    char key[8];
    snprintf(key, sizeof key, "b%u", (unsigned)(i % BATT_LOG));
    BattEntry e;
    if (prefs.getBytes(key, &e, sizeof e) != sizeof e) continue;
    time_t t = e.t;
    struct tm lt;  localtime_r(&t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%lu,%lu,%s,%u,%.2f,%u\n", (unsigned long)i + 1, (unsigned long)e.t, loc,
                  e.batt_mV, e.temp_cC / 100.0f, e.refreshMs);
  }
  Serial.printf("# wakes %lu, refreshes %lu, last wake awake %u ms\n",
                (unsigned long)st.wakes, (unsigned long)st.refreshes, st.lastAwakeMs);
  Serial.printf("# timer arm retries %u, fallbacks to deep sleep %u, power-off failures %lu\n",
                st.armRetries, st.fallbacks, (unsigned long)prefs.getUInt("offfail", 0));
  uint8_t lvp = 0;
  pmRead(PM_BATT_LVP, &lvp, 1);
  Serial.printf("# battery protection: PMIC cut-off 0x%02X (%.3f V), low streak %u, "
                "empty streak %u, simulated V %.2f\n", lvp, 2.0f + lvp * 0.00781f,
                st.lowStreak, st.emptyStreak, prefs.isKey("simv") ? prefs.getFloat("simv", 0) : 0.0f);
  dumpSyncLog();
}

// ---- Wi-Fi (Lesson 05) ----
// Credentials live in their own NVS namespace "wifi" — NOT in the source, and
// not wiped by the dashboard's fresh-start prefs.clear() on the "dash" namespace.
// The board never prints the password back, only its length.
static void wifiScan() {
  Serial.println("scanning (2.4 GHz)...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks();                  // blocks a few seconds
  Serial.printf("%d network(s):\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %-32s  %4d dBm  ch %2d  %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);                          // radio off: it's the hungriest thing here
}

static void wifiCommand(const String& line) {
  Preferences w;
  w.begin("wifi", false);
  if (line.startsWith("SSID ")) {
    w.putString("ssid", line.substring(5));
    Serial.printf("SSID stored: \"%s\"\n", line.substring(5).c_str());
  } else if (line.startsWith("PASS ")) {
    w.putString("pass", line.substring(5));
    Serial.printf("password stored (%u characters)\n", (unsigned)(line.length() - 5));
  } else if (line == "WIFI?") {
    String ssid = w.isKey("ssid") ? w.getString("ssid") : "";
    String pass = w.isKey("pass") ? w.getString("pass") : "";
    Serial.printf("wifi: SSID \"%s\", password %u characters\n", ssid.c_str(),
                  (unsigned)pass.length());
  } else if (line == "WIFI-") {
    w.clear();
    Serial.println("wifi credentials erased");
  }
  w.end();
}

// ---- NTP sync (Lesson 05 Part 3) ----
struct SyncEntry { uint32_t t; int32_t offsetMs; uint16_t connectMs, sntpMs; int8_t rssi; };
static constexpr int SYNC_LOG = 30;
static volatile bool sntpDone = false;
static void onSntpSync(struct timeval*) { sntpDone = true; }

static const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "network not found (check the SSID / 2.4 GHz)";
    case WL_CONNECT_FAILED: return "connection failed (wrong password?)";
    case WL_CONNECTION_LOST:return "connection lost";
    case WL_DISCONNECTED:   return "disconnected (timeout, or wrong password)";
    default:                return "not connected";
  }
}

// Connect, fetch network time, MEASURE how far the RTC had drifted, correct the
// RTC on a second boundary, radio off. Returns true on success; prints each stage.
static bool ntpSync() {
  Preferences w;
  w.begin("wifi", true);
  String ssid = w.isKey("ssid") ? w.getString("ssid") : "";
  String pass = w.isKey("pass") ? w.getString("pass") : "";
  w.end();
  if (!ssid.length()) { Serial.println("sync: no Wi-Fi credentials stored"); return false; }

  // 1. Align the system clock to the RTC's next second tick and remember a
  //    microsecond timestamp, so "what the RTC says now" is known to ~1 ms.
  int s0 = M5.Rtc.getTime().seconds;
  uint32_t w0 = millis();
  while (M5.Rtc.getTime().seconds == s0 && millis() - w0 < 1100) delay(1);
  M5.Rtc.setSystemTimeFromRtc();
  time_t  rtcBase  = time(nullptr);
  int64_t monoBase = esp_timer_get_time();

  // 2. Connect.
  uint32_t t0 = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(50);
  uint16_t connectMs = millis() - t0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("sync: Wi-Fi \"%s\" failed after %u ms: %s\n", ssid.c_str(), connectMs,
                  wifiStatusName(WiFi.status()));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  int8_t rssi = WiFi.RSSI();
  Serial.printf("sync: connected to \"%s\" in %u ms (%d dBm)\n", ssid.c_str(), connectMs, rssi);

  // 3. SNTP.
  uint32_t t1 = millis();
  sntpDone = false;
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(TZ_LONDON, "uk.pool.ntp.org", "pool.ntp.org", "time.google.com");
  while (!sntpDone && millis() - t1 < 10000) delay(20);
  uint16_t sntpMs = millis() - t1;
  bool ok = sntpDone;
  int32_t offsetMs = 0;
  if (ok) {
    // 4. Drift: network time minus what the RTC would read now.
    int64_t mono = esp_timer_get_time();
    timeval tv;
    gettimeofday(&tv, nullptr);
    double rtcNow = rtcBase + (mono - monoBase) / 1e6;
    double ntpNow = tv.tv_sec + tv.tv_usec / 1e6;
    offsetMs = (int32_t)lround((ntpNow - rtcNow) * 1000);

    // 5. Write the RTC on a second boundary (it stores whole seconds).
    gettimeofday(&tv, nullptr);
    delayMicroseconds(1000000 - tv.tv_usec);
    time_t sec = time(nullptr);
    struct tm utc;
    gmtime_r(&sec, &utc);
    M5.Rtc.setDateTime(&utc);
    Serial.printf("sync: SNTP in %u ms; RTC was %+ld ms off (%s); RTC set to %04d-%02d-%02d "
                  "%02d:%02d:%02d UTC\n", sntpMs, (long)offsetMs,
                  offsetMs >= 0 ? "behind" : "ahead", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  } else {
    Serial.printf("sync: no NTP answer within %u ms\n", sntpMs);
  }
  esp_sntp_stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);                        // radio off again
  setenv("TZ", TZ_LONDON, 1);
  tzset();

  if (ok) {                                   // history in its own NVS namespace
    Preferences sp;
    sp.begin("sync", false);
    uint32_t n = sp.getUInt("n", 0);
    SyncEntry e = { (uint32_t)time(nullptr), offsetMs, connectMs, sntpMs, rssi };
    char key[8];
    snprintf(key, sizeof key, "s%u", (unsigned)(n % SYNC_LOG));
    sp.putBytes(key, &e, sizeof e);
    sp.putUInt("n", n + 1);
    sp.putUInt("last", e.t);
    sp.end();
  }
  return ok;
}

static void dumpSyncLog() {
  Preferences sp;
  sp.begin("sync", true);
  uint32_t n = sp.getUInt("n", 0), kept = min<uint32_t>(n, SYNC_LOG);
  Serial.println("# NTP sync log");
  Serial.println("n,utc_epoch,local,offset_ms,connect_ms,sntp_ms,rssi_dbm");
  for (uint32_t i = n - kept; i < n; ++i) {
    char key[8];
    snprintf(key, sizeof key, "s%u", (unsigned)(i % SYNC_LOG));
    SyncEntry e;
    if (sp.getBytes(key, &e, sizeof e) != sizeof e) continue;
    time_t t = e.t;
    struct tm lt;  localtime_r(&t, &lt);
    char loc[24];  strftime(loc, sizeof loc, "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("%lu,%lu,%s,%ld,%u,%u,%d\n", (unsigned long)i + 1, (unsigned long)e.t, loc,
                  (long)e.offsetMs, e.connectMs, e.sntpMs, e.rssi);
  }
  sp.end();
}

// ---- Photos from the SD card (Lesson 06) ----
// The card sits on the e-paper's SPI bus; M5GFX owns Arduino's GLOBAL `SPI`
// (G47), so mount with that. Its power comes from the PMIC (on after M5.begin,
// switched off again before sleep).
static bool sdMount() {
  static constexpr uint32_t speeds[] = { 20000000, 10000000, 4000000 };
  for (int attempt = 0; attempt < 2; ++attempt) {     // a just-powered card can be slow
    for (uint32_t s : speeds) {
      if (SD.begin(PIN_SD_CS, SPI, s)) return true;
      SD.end();
    }
    delay(300);
  }
  Serial.println("photo: SD mount failed (card missing or not FAT32?)");
  return false;
}

static void listPhotos() {
  if (!sdMount()) return;
  File dir = SD.open("/photos");
  Serial.println("# /photos on the SD card");
  for (File e = dir ? dir.openNextFile() : File(); e; e = dir.openNextFile())
    Serial.printf("  %-24s %8lu\n", e.name(), (unsigned long)e.size());
  dir.close();
  SD.end();
}

// Route B (default): our own Floyd-Steinberg version, packed 4-bit ink codes,
// drawn in the EXACT palette colours so epd_fastest passes them through untouched.
// Route A: the 400x600 JPEG, decoded here and dithered by the DRIVER (epd_quality).
static bool showPhoto(const String& name, bool routeA) {
  if (!sdMount()) return false;
  String path = "/photos/" + name + (routeA ? ".jpg" : ".epd4");
  if (!SD.exists(path)) {
    Serial.printf("photo: %s not found (try PHOTOS)\n", path.c_str());
    SD.end();
    return false;
  }
  uint32_t t0 = millis();
  M5.Display.fillScreen(INK_WHITE);
  if (routeA) {
    M5.Display.drawJpgFile(SD, path.c_str(), 0, 0);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
  } else {
    // Ink codes -> the driver's own palette RGB (Panel_ED2208 epd_palette[]).
    static constexpr uint32_t INK_RGB[8] = { 0x000000, 0xFFFFFF, 0xFFF338, 0xBF0000,
                                             0xFFFFFF, 0x6440FF, 0x438A1C, 0xFFFFFF };
    File f = SD.open(path);
    if (!f || f.size() != 120000) {
      Serial.printf("photo: %s is %lu bytes, expected 120000\n", path.c_str(),
                    (unsigned long)(f ? f.size() : 0));
      if (f) f.close();
      SD.end();
      return false;
    }
    uint8_t row[200];                    // 400 px = 200 bytes, 2 px per byte
    M5.Display.startWrite();
    for (int y = 0; y < 600; ++y) {
      f.read(row, sizeof row);
      for (int x = 0; x < 400; ++x) {
        uint8_t ink = (x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4);
        M5.Display.drawPixel(x, y, INK_RGB[ink & 7]);
      }
    }
    M5.Display.endWrite();
    f.close();
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  }
  SD.end();
  uint32_t t1 = millis();
  M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
  timedDisplay(routeA ? "photo A" : "photo B");
  if (g_lastRefreshMs) prefs.putUShort("lastref", g_lastRefreshMs);
  logRefresh(routeA ? 2 : 1);
  M5.Led.setAllColor(0, 0, 0);    M5.Led.display();
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);   // dashboard default
  Serial.printf("photo: %s shown via route %c (load %lu ms, refresh %lu ms)\n", path.c_str(),
                routeA ? 'A' : 'B', (unsigned long)(t1 - t0), (unsigned long)(millis() - t1));
  prefs.putUChar("hold", 1);           // photo-frame mode until the power button
  return true;
}

// Every documented M5PM1 register, read-only (Lesson 06: the panel's refresh doubled and
// running the factory firmware fixed it — the power chip is the only part that keeps its
// state across everything, so record it in the "fast" state and diff if it ever recurs).
static void pmDump() {
  static const uint8_t ranges[][2] = { {0x00,0x0C}, {0x10,0x19}, {0x20,0x2A}, {0x30,0x35},
                                       {0x38,0x3D}, {0x40,0x45}, {0x48,0x4A}, {0x50,0x50},
                                       {0x53,0x53}, {0xA0,0xBF} };
  Serial.println("# M5PM1 register dump (reg=value)");
  for (auto& r : ranges) {
    for (int reg = r[0]; reg <= r[1]; ++reg) {
      uint8_t v = 0;
      bool ok = pmRead((uint8_t)reg, &v, 1);
      Serial.printf("%02X=%s%02X%s", reg, ok ? "" : "??", v, (reg == r[1] || (reg & 7) == 7) ? "\n" : " ");
    }
  }
  Serial.println("# end PMDUMP");
}

// Lesson 06 experiment: the factory firmware drives PM1 GPIO4 ("SD_DET_EN") as a
// push-pull output, HIGH. Our firmware never touches it, and with HOLD_CFG = 0 every
// standby power-off resets it. Does it change the panel's refresh time?
static void sdDetEn(bool on) {
  // (GPIO4's function register 0x17 already reads 00 = plain GPIO — nothing to change.)
  uint8_t mode = 0, out = 0, drv = 0;
  pmRead(0x10, &mode, 1); pmRead(0x11, &out, 1); pmRead(0x13, &drv, 1);
  if (on) {
    mode |= 1 << 4; drv &= ~(1 << 4); out |= 1 << 4;         // output, push-pull, HIGH
    pmWrite(0x13, &drv, 1); pmWrite(0x11, &out, 1); pmWrite(0x10, &mode, 1);
  } else {
    mode &= ~(1 << 4); out &= ~(1 << 4);                     // back to input
    pmWrite(0x10, &mode, 1); pmWrite(0x11, &out, 1);
  }
  uint8_t m2 = 0, o2 = 0, d2 = 0;
  pmRead(0x10, &m2, 1); pmRead(0x11, &o2, 1); pmRead(0x13, &d2, 1);
  Serial.printf("SD_DET_EN (PM1 GPIO4) %s: GPIO_MODE 0x%02X  GPIO_OUT 0x%02X  GPIO_DRV 0x%02X\n",
                on ? "ON" : "OFF", m2, o2, d2);
}

// Lesson 06 experiment: does the panel pick its refresh program by temperature?
// UC81xx-family controllers (the ED2208's init matches that family) take a forced
// temperature: CCSET 0xE0 bit1 (TSFIX) = use TSSET 0xE5 (signed degC) instead of the
// internal sensor. M5GFX never sends these, so the controller uses its own sensor.
// The setting lasts until the controller loses power (every standby power-off).
static constexpr int PIN_EPD_CS = 44;   // from M5GFX's PaperColor board setup
static void epdCmd(uint8_t cmd, int data = -1) {
  auto bus = M5.Display.getPanel()->getBus();
  bus->beginTransaction();
  digitalWrite(PIN_EPD_CS, LOW);
  bus->writeCommand(cmd, 8);
  if (data >= 0) bus->writeData((uint8_t)data, 8);
  bus->wait();
  digitalWrite(PIN_EPD_CS, HIGH);
  bus->endTransaction();
}
static void forceTemp(int degC) {          // INT16_MIN = back to the internal sensor
  if (degC == INT16_MIN) { epdCmd(0xE0, 0x00); return; }
  epdCmd(0xE0, 0x02);
  epdCmd(0xE5, (uint8_t)(int8_t)degC);
}
static void tempSweep() {
  static const int steps[] = { INT16_MIN, 25, 21, 18, 12, 5, 30, 40, INT16_MIN };
  for (int t : steps) {
    if (t == INT16_MIN) Serial.printf("tsweep: internal sensor (SHT40 %.2f C)\n", curT);
    else                Serial.printf("tsweep: forcing %d C\n", t);
    forceTemp(t);
    drawScreen(M5.Power.getBatteryVoltage() / 1000.0f, false);
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    char what[24];
    if (t == INT16_MIN) snprintf(what, sizeof what, "internal");
    else                snprintf(what, sizeof what, "forced %d C", t);
    timedDisplay(what);
  }
}

// Forced temperature BEFORE the controller's first refresh after a reset (the
// in-wake TSWEEP showed the refresh time is fixed for the whole wake). One step per
// boot: pulse the panel's RST, M5.begin re-inits it, force, refresh, store, restart.
static const int TF_STEPS[] = { INT16_MIN, 25, 12, 40, 0, INT16_MIN };
static constexpr int TF_N = sizeof TF_STEPS / sizeof TF_STEPS[0];

static void tforceDump() {
  Preferences p; p.begin("tfs", true);
  Serial.println("# forced-temperature sweep (one controller reset per step)");
  for (int i = 0; i < TF_N; ++i) {
    char k[6]; snprintf(k, sizeof k, "r%d", i);
    if (TF_STEPS[i] == INT16_MIN) Serial.printf("step %d internal : ", i);
    else                          Serial.printf("step %d %3d C    : ", i, TF_STEPS[i]);
    Serial.printf("REFRESH %u ms\n", p.getUShort(k, 0));
  }
  p.end();
}

// One sweep step per boot; restarts until the last step, which prints the results.
static void tforceStep() {
  Preferences p; p.begin("tfs", false);
  int i = p.getInt("i", -1);
  if (i < 0 || i >= TF_N) { p.end(); return; }
  int t = TF_STEPS[i];
  if (t != INT16_MIN) forceTemp(t);
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(20, 40);
  if (t == INT16_MIN) M5.Display.printf("Sweep step %d: internal", i);
  else                M5.Display.printf("Sweep step %d: %d C", i, t);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  char what[24];
  snprintf(what, sizeof what, t == INT16_MIN ? "step %d internal" : "step %d forced", i);
  timedDisplay(what);
  M5.Display.setTextSize(1);   // don't leave x3 behind for the dashboard
  char k[6]; snprintf(k, sizeof k, "r%d", i);
  p.putUShort(k, g_lastRefreshMs);
  p.putInt("i", i + 1);
  p.end();
  if (i + 1 < TF_N) { Serial.flush(); delay(200); ESP.restart(); }
  Preferences q; q.begin("tfs", false); q.putInt("i", -1); q.end();
  tforceDump();
}

static void serialWindow(uint32_t ms) {
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < ms) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        float v;
        if (line == "LOG") dumpLog();
        else if (sscanf(line.c_str(), "SIMV %f", &v) == 1) {   // test hook
          prefs.putFloat("simv", v);
          Serial.printf("simulated battery voltage: %.2f V%s\n", v, v > 0 ? "" : " (off)");
        }
        else if (line == "SCAN") wifiScan();
        else if (line == "SYNC") ntpSync();
        else if (line == "SYNCLOG") dumpSyncLog();
        else if (line == "PHOTOS") listPhotos();
        else if (line == "PMDUMP") pmDump();
        else if (line == "RLOG") dumpRefreshLog();
        else if (line == "TSWEEP") tempSweep();
        else if (line == "LS 1" || line == "LS 0" || line == "LS?") {
          Preferences p; p.begin("ls", false);
          if (line != "LS?") {
            p.putBool("force", line == "LS 1");
            if (line == "LS 1") g_lightSleepWait = true;   // test now, not just from next boot
          }
          Serial.printf("light-sleep wait: force %s; this boot %s, %lu naps so far\n",
                        p.getBool("force", false) ? "ON" : "off", g_lightSleepWait ? "ON" : "off",
                        (unsigned long)g_lightSleeps);
          p.end();
        }
        else if (line == "TFSWEEP") {             // start the per-reset sweep
          Preferences p; p.begin("tfs", false); p.clear(); p.putInt("i", 0); p.end();
          Serial.println("tfsweep: starting, one restart per step");
          Serial.flush(); delay(200); ESP.restart();
        }
        else if (line == "TFRES") tforceDump();
        else if (line == "SDDET 1") sdDetEn(true);
        else if (line == "SDDET 0") sdDetEn(false);
        else if (line == "REDRAW") {                          // timed dashboard refresh now
          drawScreen(M5.Power.getBatteryVoltage() / 1000.0f, false);
          M5.Display.setEpdMode(epd_mode_t::epd_fastest);
          timedDisplay("redraw");
          logRefresh(3);
        }
        else if (line.startsWith("SHOW ")) {                  // SHOW <name> [A]
          String arg = line.substring(5);
          arg.trim();
          bool routeA = arg.endsWith(" A");
          if (routeA) arg = arg.substring(0, arg.length() - 2);
          showPhoto(arg, routeA);
        }
        else if (line == "SYNCDUE") {                         // test hook: make it due now
          Preferences sp;
          sp.begin("sync", false);
          sp.putUInt("last", 0);
          sp.putUInt("try", 0);
          sp.end();
          Serial.println("sync marked due: the next wake will sync");
        }
        else if (line.startsWith("SSID ") || line.startsWith("PASS ") ||
                 line == "WIFI?" || line == "WIFI-") wifiCommand(line);
        if (line.length()) t0 = millis();         // any command: keep the window open
        line = "";
      }
      else if (c != '\r') line += c;
    }
    // Blink LED 0 blue the whole time the window is open: "awake and listening".
    // (A steady LED was easy to miss, and a long silent wait invited a second
    // button press — which ends photo mode.)
    static uint32_t lastBlink = 0;
    static bool blinkOn = false;
    if (millis() - lastBlink >= 400) {
      lastBlink = millis();
      blinkOn = !blinkOn;
      M5.Led.setColor(0, 0, 0, blinkOn ? 255 : 0);
      M5.Led.display();
    }
    delay(10);
  }
  M5.Led.setColor(0, 0, 0, 0);                    // window closed
  M5.Led.display();
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

// Battery empty: power off with NO timer — only USB or the power button wake
// the board. The deliberate opposite of powerOffUntil's "never without a timer".
[[noreturn]] static void powerOffForever() {
  M5.Led.setAllColor(0, 0, 0);
  M5.Led.display();
  M5.Display.sleep();
  M5.Display.waitDisplay();
  M5.Power.M5pm1.clearWakeSource();
  uint8_t stop = 0x00, key = 0xA5;
  pmWrite(PM_TIM_CFG, &stop, 1);          // make sure no timer is left armed
  pmWrite(PM_TIM_KEY, &key, 1);
  st.lastAwakeMs = (uint16_t)min<uint32_t>(millis(), 65535);
  pmWrite(PM_RTC_MEM, reinterpret_cast<const uint8_t*>(&st), sizeof st);
  Serial.println("battery empty: power-off with no timer (USB / power button to wake)");
  Serial.flush();
  prefs.end();
  delay(120);
  uint8_t off = 0xA0 | 0x01;
  pmWrite(PM_SYS_CMD, &off, 1);
  delay(2000);
  esp_deep_sleep_start();                 // PMIC refused: sleep with no wake source
}

// Raise the PMIC's own cut-off from 2.50 V to 3.10 V (write + read back).
static void setHardwareCutoff() {
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint8_t v = LVP_3V10, back = 0;
    if (pmWrite(PM_BATT_LVP, &v, 1) && pmRead(PM_BATT_LVP, &back, 1) && back == LVP_3V10) return;
    delay(20);
  }
  Serial.println("warning: could not set the PMIC low-voltage cut-off");
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  {
    Preferences p; p.begin("tfs", true);
    int i = p.isKey("i") ? p.getInt("i", -1) : -1;
    p.end();
    if (i >= 0) {          // sweep step: hardware-reset the panel controller first
      // (was G43 = DC by mistake; with clear_display = false M5GFX never pulses G12 itself)
      pinMode(PIN_EPD_RST, OUTPUT); digitalWrite(PIN_EPD_RST, LOW); delay(20);
      digitalWrite(PIN_EPD_RST, HIGH); delay(50);
      Serial.printf("tfsweep: panel reset pulsed on G%u\n", PIN_EPD_RST);
    }
  }
  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Power.M5pm1.setExtOutput(false);   // Grove 5 V off
  attachInterrupt(digitalPinToInterrupt(PIN_EPD_BUSY), onBusyEdge, CHANGE);
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Led.setBrightness(40);
  tforceStep();            // no-op unless a TFSWEEP is in progress
  {
    uint8_t pwr = 0;
    pmRead(0x04, &pwr, 1);                       // PWR_SRC bit0 = 5 V in (USB)
    Preferences p; p.begin("ls", true);
    bool force = p.isKey("force") && p.getBool("force", false);
    p.end();
    g_lightSleepWait = !(pwr & 1) || force;      // battery only, unless forced for a test
  }

  // Why are we running? A PMIC power-on (timer / button / USB) shows up as a
  // POWERON reset; anything else (reset after flashing, crash) is a dev boot.
  uint8_t wake = 0;   // (copied to g_wakeSrc below for the refresh log)
  pmRead(PM_WAKE_SRC, &wake, 1);
  g_wakeSrc = wake;
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

  // ---- battery protection ----
  setHardwareCutoff();
  uint8_t src = 0;
  pmRead(PM_PWR_SRC, &src, 1);
  bool onUsb = src & 0x01;                // charging: thresholds don't apply
  float simV = prefs.isKey("simv") ? prefs.getFloat("simv", 0) : 0;
  if (simV > 0) { battV = simV; onUsb = false; }   // test hook: pretend on battery
  if (onUsb) { st.lowStreak = st.emptyStreak = 0; }
  else {
    if (battV < LOW_V)   bump(st.lowStreak);   else st.lowStreak = 0;
    if (battV < EMPTY_V) bump(st.emptyStreak); else st.emptyStreak = 0;
  }
  bool lowBatt = st.lowStreak >= STREAK;
  bool empty   = st.emptyStreak >= STREAK;
  if (empty) {
    Serial.printf("wake #%lu: battery %.2f V -> EMPTY\n", (unsigned long)st.wakes, battV);
    if (st.emptyStreak == STREAK) {       // first confirmation: draw the message ONCE
      drawEmptyScreen(battV);
      timedDisplay("empty screen");
      logRefresh(4);
    }
    if (serviceWake) {                    // still allow LOG / SIMV 0 from a button wake
      M5.Led.setColor(0, 0, 0, 255);  M5.Led.display();
      Serial.println("service window (battery empty)");
      serialWindow(SERVICE_MS);
    }
    powerOffForever();
  }

  // ---- automatic NTP sync (Lesson 05 Part 4) ----
  // At most once a day (the RTC drifts ~1 s/day), immediately if the RTC has lost
  // the time, never in low-battery mode (Wi-Fi peaks are the likeliest brownout),
  // and no sooner than 1 h after a failed attempt. Done BEFORE drawing so the
  // screen shows the corrected time.
  {
    Preferences sp;
    sp.begin("sync", false);
    uint32_t last = sp.isKey("last") ? sp.getUInt("last") : 0;
    uint32_t lastTry = sp.isKey("try") ? sp.getUInt("try") : 0;
    bool timeLost = now < 1700000000;                       // RTC invalid / unset
    bool due = timeLost || (time_t)(now - last) >= 86400;
    bool retryOk = timeLost ? (time_t)(now - lastTry) >= 600 : (time_t)(now - lastTry) >= 3600;
    Preferences w;
    w.begin("wifi", true);
    bool haveCreds = w.isKey("ssid");
    w.end();
    if (haveCreds && due && retryOk && !lowBatt) {
      sp.putUInt("try", (uint32_t)now);
      sp.end();
      uint32_t s0 = millis();
      bool ok = ntpSync();
      Serial.printf("auto-sync %s in %lu ms\n", ok ? "OK" : "FAILED", (unsigned long)(millis() - s0));
      now = time(nullptr);                                   // corrected time
    } else {
      sp.end();
    }
  }

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

  // Photo-frame hold (Lesson 06): a photo stays up — no dashboard refreshes — until
  // the power button. Low battery always wins: its warnings matter more.
  bool hold = prefs.isKey("hold") && prefs.getUChar("hold") == 1;
  if (hold && (serviceWake || lowBatt)) { prefs.putUChar("hold", 0); hold = false; }

  char why = 0;
  if (fresh)                                                       why = 'B';
  else if (serviceWake)                                            why = 'P';
  else if (hold)                                                   why = 0;   // keep the photo
  else if (now - (time_t)st.lastRefreshAt >=
           (time_t)(lowBatt ? REFRESH_EVERY_LOW_S : REFRESH_EVERY_S)) why = 'S';
  else if (!lowBatt && (st.shownT_cC == INT16_MIN ||                // no early refreshes
           abs(cC - st.shownT_cC) >= (int)(EARLY_DELTA_C * 100)))   why = 'E'; // when low

  if (why) {
    drawScreen(battV, lowBatt);
    M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
    timedDisplay("dashboard");
    if (g_lastRefreshMs) prefs.putUShort("lastref", g_lastRefreshMs);
    logRefresh(0);
    M5.Led.setAllColor(0, 0, 0);    M5.Led.display();
    ++st.refreshes;
    st.shownT_cC = cC;
    st.lastRefreshAt = (uint32_t)now;
  }

  Serial.printf("wake #%lu (%s, WAKE_SRC 0x%02X, reset %d): T %.2f batt %.2f V%s%s %s\n",
                (unsigned long)st.wakes,
                fresh ? "fresh" : serviceWake ? "button/USB" : timerWake ? "timer" : "other",
                wake, (int)esp_reset_reason(), curT, battV, onUsb ? " (USB)" : "",
                lowBatt ? " LOW" : "", why ? "refreshed" : "");
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
