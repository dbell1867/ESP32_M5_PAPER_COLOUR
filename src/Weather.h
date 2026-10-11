// Weather from Open-Meteo (Lesson 08) — free, no API key. Fetch + parse only:
// storage, scheduling and drawing live in main.cpp. Needs Wi-Fi up and the clock right
// (HTTPS checks certificate dates).
#pragma once
#include <Arduino.h>

struct __attribute__((packed)) WxData {
  uint32_t fetchedAt;        // UTC epoch of the fetch (0 = no data)
  uint8_t  dayCode;          // today: WMO weather code
  int16_t  dayMax_c10;       // today: max / min temperature, tenths of a degree C
  int16_t  dayMin_c10;
  uint8_t  dayRain;          // today: max precipitation probability, %
  uint32_t hour0;            // UTC epoch of the first hourly entry (the current hour)
  static constexpr int HOURS = 7;   // current hour + 6: "next 5 hours" lasts until the next fetch
  uint8_t  code[HOURS];
  int16_t  temp_c10[HOURS];
  uint8_t  rain[HOURS];      // %
  uint8_t  dayMask;          // bit i: hour i is daytime (sun vs moon icon)
};

namespace Weather {
// Fetch today + the next HOURS hours for lat/lon. On failure returns false and puts a
// short reason in `err` ("TLS/connect", "HTTP 503", "JSON: ...", ...).
bool fetch(float lat, float lon, WxData& out, String& err);
// Short English description of a WMO weather code ("overcast", "rain showers", ...).
const char* describe(uint8_t code);
}
