#include "Weather.h"
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "isrg_roots.h"

namespace Weather {

bool fetch(float lat, float lon, WxData& out, String& err) {
  char url[320];
  // ~830 bytes back: today's code/max/min/rain + 7 hourly entries from the current hour.
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&timezone=Europe%%2FLondon&timeformat=unixtime&forecast_days=1"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&hourly=weather_code,temperature_2m,precipitation_probability,is_day"
           "&forecast_hours=%d",
           lat, lon, WxData::HOURS);

  NetworkClientSecure tls;
  tls.setCACert(ISRG_ROOTS_PEM);       // verify the server — never setInsecure()
  tls.setTimeout(10);                  // seconds
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(tls, url)) { err = "bad URL"; return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    err = code < 0 ? String("TLS/connect: ") + http.errorToString(code) : String("HTTP ") + code;
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, body);
  if (je) { err = String("JSON: ") + je.c_str(); return false; }

  JsonObject d = doc["daily"], h = doc["hourly"];
  JsonArray ht = h["time"];
  if (d.isNull() || h.isNull() || ht.size() < (size_t)WxData::HOURS) {
    err = "JSON: missing fields";
    return false;
  }
  WxData w = {};
  w.dayCode    = d["weather_code"][0] | 0;
  w.dayMax_c10 = (int16_t)lroundf((float)(d["temperature_2m_max"][0] | 0.0f) * 10);
  w.dayMin_c10 = (int16_t)lroundf((float)(d["temperature_2m_min"][0] | 0.0f) * 10);
  w.dayRain    = d["precipitation_probability_max"][0] | 0;
  w.hour0      = ht[0] | 0;
  for (int i = 0; i < WxData::HOURS; ++i) {
    w.code[i]     = h["weather_code"][i] | 0;
    w.temp_c10[i] = (int16_t)lroundf((float)(h["temperature_2m"][i] | 0.0f) * 10);
    w.rain[i]     = h["precipitation_probability"][i] | 0;
    if ((int)(h["is_day"][i] | 1)) w.dayMask |= 1 << i;
  }
  w.fetchedAt = (uint32_t)time(nullptr);
  out = w;
  return true;
}

const char* describe(uint8_t c) {
  if (c == 0) return "clear";
  if (c <= 2) return "partly cloudy";
  if (c == 3) return "overcast";
  if (c == 45 || c == 48) return "fog";
  if (c >= 51 && c <= 57) return "drizzle";
  if (c >= 61 && c <= 67) return "rain";
  if (c >= 71 && c <= 77) return "snow";
  if (c >= 80 && c <= 82) return "rain showers";
  if (c == 85 || c == 86) return "snow showers";
  if (c >= 95) return "thunderstorm";
  return "?";
}

}  // namespace Weather
