// What the panel shows (Lesson 08). Drawing only — no sensors, NVS or Wi-Fi in here:
// main.cpp gathers everything into a DashInfo and calls drawDashboard().
#pragma once
#include <Arduino.h>
#include <time.h>
#include "Weather.h"

// The panel's six inks, EXACT (Lesson 02): anything else gets dithered or mapped.
static constexpr uint32_t INK_BLACK  = 0x000000, INK_WHITE = 0xFFFFFF;
static constexpr uint32_t INK_RED    = 0xBF0000, INK_BLUE  = 0x6440FF;
static constexpr uint32_t INK_YELLOW = 0xFFF338, INK_GREEN = 0x438A1C;

struct DashInfo {
  time_t now;            // for the date and "Updated hh:mm"
  float  roomC, roomRH;  // SHT40 (NAN if the read failed)
  float  battV;
  bool   usb;            // 5 V present (charging)
  bool   lowBatt;        // red banner
  bool   haveWx;
  bool   storageLow;     // NVS nearly full: logs/weather can't be saved
  WxData wx;
};

void drawDashboard(const DashInfo& d);

// The Wi-Fi page: "Scan to join" + the network name + a QR code of `qrText`
// ("WIFI:T:WPA;S:...;P:...;;"). The text contains the password: never log or screenshot it.
void drawWifiQr(const char* ssid, const char* qrText);

// Every weather icon at the dashboard's two sizes, labelled — for reviewing the drawing
// with SHOT ICONS (screenshot only; never refreshed to the panel).
void drawIconSheet();

// Rough state of charge from the resting voltage: a typical single-cell LiPo curve,
// 3.30 V (our "empty" shutdown) = 0 %, 4.20 V = 100 %. Approximate by nature.
int batteryPercent(float v);
