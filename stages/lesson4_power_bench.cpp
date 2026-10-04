// Lesson 04, Part 2 — Power bench: find what draws 24 mA in "deep sleep".
// Holds ONE power state at a time so a slow USB meter can read it (gotcha 20).
// Each step switches one more supply off (cumulative), so the DIFFERENCE
// between neighbouring steps is that supply's cost.
//
// v2 (after run 1 found Grove 5 V = 24 mA, and awake == asleep on the meter):
//   0 AWAKE           CPU running, everything on                (reference)
//   1 AWAKE -GROVE    + Grove 5 V boost off. DECISIVE: ~0 here means the
//                       board runs from its battery while on USB and the
//                       meter can't see it; 20-40 mA means it runs from USB.
//   2 SLEEP           ESP32 deep sleep (Grove still off)
//   3 -LEDs           + RGB LED supply off           (PMIC LDO)
//   4 -SD             + SD card power off             (PMIC GPIO3)
//   5 -PANEL          + e-paper power off             (PMIC GPIO0)
//   6 PMIC OFF        whole board off via the power chip (M5.Power.powerOff)
//
// Each step: announce on screen (one refresh, full brightness of activity),
// THEN settle into the state. Press any button to go to the next step.
// Sleep steps wake ONLY on a button (plus a 30 min safety timer -> step 0).
#include <Arduino.h>
#include <M5Unified.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "board_pins.h"

static constexpr uint64_t BUTTON_MASK =
    (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B) | (1ULL << PIN_BTN_C);
static constexpr uint64_t SAFETY_TIMER_US = 30ULL * 60 * 1000000;

RTC_DATA_ATTR static int step = 0;

static const char* STEP_NAME[] = {
  "AWAKE (reference)", "AWAKE, Grove 5V off", "DEEP SLEEP (Grove off)",
  "+ LEDs supply off", "+ SD power off", "+ panel power off", "PMIC power OFF",
};
static constexpr int N_STEPS = 7;

static void waitButtonsReleased() {
  uint32_t t0 = millis();
  while ((!digitalRead(PIN_BTN_A) || !digitalRead(PIN_BTN_B) || !digitalRead(PIN_BTN_C))
         && millis() - t0 < 3000) delay(10);
  delay(50);
}

static void announce() {
  auto& pm = M5.Power.M5pm1;
  const int W = M5.Display.width();
  char buf[80];
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextDatum(top_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString("POWER BENCH", W / 2, 20);
  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  snprintf(buf, sizeof buf, "Step %d of %d", step, N_STEPS - 1);
  M5.Display.drawString(buf, W / 2, 90);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString(STEP_NAME[step], W / 2, 160);

  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.setTextDatum(top_left);
  int y = 240;
  snprintf(buf, sizeof buf, "Battery  %.2f V", M5.Power.getBatteryVoltage() / 1000.0f);
  M5.Display.drawString(buf, 30, y); y += 34;
  snprintf(buf, sizeof buf, "USB in   %.2f V", pm.getVBUSVoltage() / 1000.0f);
  M5.Display.drawString(buf, 30, y); y += 34;
  // NB: pm.isCharging() is a permanent stub in M5Unified ("the PM1 register map
  // exposes no charging status register") — it can only say "no", so it is
  // NOT shown. An instrument that can't disagree isn't evidence (gotcha 29).
  snprintf(buf, sizeof buf, "Grove 5V was %s", pm.getExtOutput() ? "ON" : "off");
  M5.Display.drawString(buf, 30, y); y += 50;

  M5.Display.setFont(&fonts::FreeSansBold12pt7b);
  M5.Display.drawString("When the red LED goes out,", 30, y); y += 30;
  M5.Display.drawString("wait 20 s, read the meter,", 30, y); y += 30;
  M5.Display.drawString("then press any button.", 30, y);

  Serial.printf("\nSTEP %d: %s  | batt %.2f V  vbus %.2f V  grove5V %d\n",
                step, STEP_NAME[step], M5.Power.getBatteryVoltage() / 1000.0f,
                pm.getVBUSVoltage() / 1000.0f, (int)pm.getExtOutput());

  M5.Led.setColor(1, 255, 0, 0);  M5.Led.display();
  M5.Display.display();
  M5.Display.waitDisplay();
  M5.Led.setAllColor(0, 0, 0);    M5.Led.display();
}

[[noreturn]] static void settleAndSleep() {
  auto& pm = M5.Power.M5pm1;
  waitButtonsReleased();
  M5.Display.sleep();                     // panel controller DEEP_SLEEP command
  M5.Display.waitDisplay();

  pm.setExtOutput(false);                                      // Grove 5 V (step >= 2)
  if (step >= 3) pm.setLDOOutput(false);                       // LED supply
  if (step >= 4) pm.setGPIOOutput(m5::M5PM1_Class::gpio3, false);  // SD power
  if (step >= 5) pm.setGPIOOutput(m5::M5PM1_Class::gpio0, false);  // panel power
  Serial.printf("step %d: settled, sleeping\n", step);
  Serial.flush();

  if (step >= 6) {                        // the whole board, via the power chip
    M5.Power.powerOff();                  // returns only if the PMIC refused
    Serial.println("powerOff() returned - PMIC did not cut power");
  }

  gpio_hold_en((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_en((gpio_num_t)PIN_SPK_EN);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext1_wakeup(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(SAFETY_TIMER_US);
  esp_deep_sleep_start();
}

void setup() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_AUDIO_PWR_EN);
  gpio_hold_dis((gpio_num_t)PIN_SPK_EN);
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);

  auto cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT1)       step = step + 1 < N_STEPS ? step + 1 : 0;
  else                                      step = 0;  // cold boot or safety timer

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);
  M5.Display.setAutoDisplay(false);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Led.setBrightness(40);

  announce();

  // Steps 0 and 1 stay AWAKE (idle loop) until a button; then advance.
  while (step <= 1) {
    if (step == 1) M5.Power.M5pm1.setExtOutput(false);   // Grove 5 V off, awake
    waitButtonsReleased();
    Serial.printf("step %d: awake idle; press any button for step %d\n", step, step + 1);
    while (digitalRead(PIN_BTN_A) && digitalRead(PIN_BTN_B) && digitalRead(PIN_BTN_C))
      delay(10);
    ++step;                               // no sleep in between, so advance here
    announce();
  }
  settleAndSleep();
}

void loop() {}
