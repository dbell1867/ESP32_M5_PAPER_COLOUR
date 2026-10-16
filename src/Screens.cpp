#include "Screens.h"
#include <M5Unified.h>

namespace {
auto& D = M5.Display;
constexpr int W = 400, H = 600;

const char* DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                       "Saturday" };
const char* MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
                         "August", "September", "October", "November", "December" };

// The GFX fonts are plain ASCII (no degree sign): draw text + a small ring.
// (x, y) is the TOP-CENTRE of the whole thing.
void drawTemp(const char* num, int x, int y, const GFXfont* font, uint8_t size,
              uint32_t ink, int ringR, const char* unit = nullptr) {
  D.setFont(font);
  D.setTextSize(size);
  D.setTextColor(ink);
  D.setTextDatum(top_left);
  int tw = D.textWidth(num), gap = ringR / 2 + 2;
  int uw = unit ? gap + D.textWidth(unit) : 0;
  int x0 = x - (tw + gap + 2 * ringR + uw) / 2;
  D.drawString(num, x0, y);
  int cx = x0 + tw + gap + ringR, cy = y + ringR + size * 2;
  D.fillCircle(cx, cy, ringR, ink);
  D.fillCircle(cx, cy, max(1, ringR / 2), INK_WHITE);
  if (unit) D.drawString(unit, cx + ringR + gap, y);
  D.setTextSize(1);
}

uint32_t tempInk(float c) { return c < 18 ? INK_BLUE : (c > 25 ? INK_RED : INK_BLACK); }

// ---- weather icons, drawn from circles and lines, in a box of size s centred on (cx, cy)
// Yellow is the palest ink: on white it barely shows, so yellow shapes get a black
// outline — drawn like the cloud's: the shape a little bigger in black, then yellow on top.
// A straight bar from radius r0 to r1 along angle a, half-width h: two filled triangles.
// Not drawWideLine: that anti-aliases, and the blended edge pixels (grey, olive) come out
// GREEN on the panel (Lesson 02) — here every pixel is exactly one ink.
void ray(int cx, int cy, float a, float r0, float r1, float h, uint32_t ink) {
  float c = cosf(a), s = sinf(a);
  float x0 = cx + c * r0, y0 = cy + s * r0, x1 = cx + c * r1, y1 = cy + s * r1;
  float px = -s * h, py = c * h;          // perpendicular offset
  D.fillTriangle(x0 + px, y0 + py, x1 + px, y1 + py, x1 - px, y1 - py, ink);
  D.fillTriangle(x0 + px, y0 + py, x1 - px, y1 - py, x0 - px, y0 - py, ink);
}

void sun(int cx, int cy, int r) {
  float h = max(1.5f, r / 6.0f);          // ray half-width
  int g = max(1, r / 8);                  // outline: 1 px small, 2 px big (keeps the ray gap)
  for (int pass = 0; pass < 2; ++pass) {
    uint32_t ink = pass ? INK_YELLOW : INK_BLACK;
    int e = pass ? 0 : g;
    // A diagonal outline is a staircase and looks about 1 px heavier than a straight one,
    // so the four straight rays get 1 px more outline on their SIDES (not the ends:
    // that keeps the gap to the disc).
    for (int i = 0; i < 8; ++i) {
      float side = e + ((pass == 0 && i % 2 == 0) ? 1 : 0);
      ray(cx, cy, i * PI / 4, r * 1.3f - e, r * 1.8f + e, h + side, ink);
    }
    D.fillCircle(cx, cy, r + e, ink);
  }
}

// A crescent: the disc (cx, cy, r) minus a circle shifted up-right. Drawn pixel by pixel
// so the outline can follow the inner curve too (circles alone would leave a black arc
// outside the moon).
void moon(int cx, int cy, int r) {
  int g = max(1, r / 8);
  float ox = cx + r * 0.45f, oy = cy - r * 0.3f, rc = r * 0.85f;
  for (int y = cy - r - g; y <= cy + r + g; ++y)
    for (int x = cx - r - g; x <= cx + r + g; ++x) {
      float d1 = hypotf(x - cx, y - cy), d2 = hypotf(x - ox, y - oy);
      if (d1 <= r && d2 > rc)            D.drawPixel(x, y, INK_YELLOW);
      else if (d1 <= r + g && d2 > rc - g) D.drawPixel(x, y, INK_BLACK);
    }
}

void cloud(int cx, int cy, int w) {     // white with a black outline
  const float c[3][3] = { { -0.24f, 0.06f, 0.21f }, { 0.04f, -0.10f, 0.29f }, { 0.27f, 0.07f, 0.19f } };
  int t = max(2, w / 22);
  for (int pass = 0; pass < 2; ++pass) {
    uint32_t ink = pass ? INK_WHITE : INK_BLACK;
    int g = pass ? 0 : t;
    for (auto& k : c) D.fillCircle(cx + k[0] * w, cy + k[1] * w, k[2] * w + g, ink);
    D.fillRect(cx - 0.24f * w, cy + 0.06f * w - g, 0.51f * w, 0.21f * w + 2 * g, ink);
  }
}

void drops(int cx, int cy, int w, int n) {
  int t = max(2, w / 18);
  for (int i = 0; i < n; ++i) {
    int x = cx + (i - (n - 1) / 2.0f) * w * 0.22f;
    D.drawWideLine(x + w * 0.05f, cy, x - w * 0.05f, cy + w * 0.2f, t, INK_BLUE);
  }
}

void flakes(int cx, int cy, int w, int n) {
  int r = max(3, w / 14);
  for (int i = 0; i < n; ++i) {
    int x = cx + (i - (n - 1) / 2.0f) * w * 0.24f, y = cy + w * 0.08f;
    for (int k = 0; k < 3; ++k) {
      float a = k * PI / 3;
      D.drawWideLine(x - cosf(a) * r, y - sinf(a) * r, x + cosf(a) * r, y + sinf(a) * r,
                     max(1, w / 30), INK_BLUE);
    }
  }
}

void bolt(int cx, int cy, int w) {
  int x = cx, y = cy, s = w * 0.12f;
  auto shape = [&](int dx, int dy, uint32_t ink) {
    D.fillTriangle(x + s + dx, y + dy, x - s + dx, y + 1.3f * s + dy, x + 0.2f * s + dx, y + 1.3f * s + dy, ink);
    D.fillTriangle(x + 0.4f * s + dx, y + 1.1f * s + dy, x - 0.9f * s + dx, y + 2.6f * s + dy,
                   x - 0.1f * s + dx, y + 1.1f * s + dy, ink);
  };
  static const int8_t OFF[8][2] = { {-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1} };
  for (auto& o : OFF) shape(o[0], o[1], INK_BLACK);   // outline: the bolt shifted 1 px all round
  shape(0, 0, INK_YELLOW);
}

void wxIcon(uint8_t code, bool day, int cx, int cy, int s) {
  const int r = s * 0.22f;
  if (code == 0) { day ? sun(cx, cy, r) : moon(cx, cy, r * 1.2f); return; }
  if (code == 45 || code == 48) {                         // fog
    int h = max(2, s / 14);                               // half-height (no anti-aliasing)
    for (int i = -1; i <= 1; ++i) {
      int x0 = cx - s * 0.38f + (i & 1) * s * 0.08f, x1 = cx + s * 0.38f;
      D.fillRoundRect(x0 - h, cy + i * s * 0.2f - h, x1 - x0 + 2 * h, 2 * h + 1, h, INK_BLACK);
    }
    return;
  }
  bool precip = code >= 51;
  int ccy = precip ? cy - s * 0.14f : cy + s * 0.04f;     // cloud higher if something falls
  if (code <= 2) {                                        // sun/moon behind a cloud
    if (day) sun(cx - s * 0.16f, cy - s * 0.14f, r * 0.85f);
    else     moon(cx - s * 0.16f, cy - s * 0.16f, r);
    cloud(cx + s * 0.08f, cy + s * 0.12f, s * 0.78f);
    return;
  }
  cloud(cx, ccy, s * 0.9f);
  int py = ccy + s * 0.24f;
  if (code >= 95) bolt(cx, py - s * 0.02f, s);
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86) flakes(cx, py + s * 0.05f, s, 3);
  else if (code >= 51 && code <= 57) drops(cx, py, s, 2);                 // drizzle
  else if (code >= 61) drops(cx, py, s, (code == 61 || code == 80) ? 2 : 3);  // rain, showers
}

void battery(int x, int y, int pct, bool usb) {        // 46 x 22 body + nub, right edge at x
  const int bw = 46, bh = 22;
  int bx = x - bw - 4;
  D.fillRect(bx, y, bw, bh, INK_BLACK);
  D.fillRect(bx + 2, y + 2, bw - 4, bh - 4, INK_WHITE);
  D.fillRect(bx + bw, y + 6, 4, bh - 12, INK_BLACK);
  uint32_t ink = pct > 50 ? INK_GREEN : (pct > 20 ? INK_YELLOW : INK_RED);
  int fw = (bw - 8) * pct / 100;
  if (fw > 0) D.fillRect(bx + 4, y + 4, fw, bh - 8, ink);
  if (usb) {                                           // a small bolt: charging from USB
    int cx = bx + bw / 2, cy = y + 3;
    D.fillTriangle(cx + 4, cy, cx - 5, cy + 9, cx + 1, cy + 9, INK_BLACK);
    D.fillTriangle(cx + 1, cy + 7, cx - 3, cy + 16, cx + 6, cy + 7, INK_BLACK);
  }
}
}  // namespace

int batteryPercent(float v) {
  static const float V[] = { 3.30f, 3.60f, 3.69f, 3.73f, 3.77f, 3.80f, 3.84f, 3.87f, 3.91f,
                             3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f };
  static const int P[] = { 0, 5, 10, 15, 20, 30, 40, 50, 60, 70, 75, 80, 85, 90, 95, 100 };
  if (v <= V[0]) return 0;
  for (int i = 1; i < 16; ++i)
    if (v < V[i]) return P[i - 1] + (int)((P[i] - P[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]));
  return 100;
}

void drawDashboard(const DashInfo& d) {
  char buf[48];
  struct tm lt;
  localtime_r(&d.now, &lt);
  D.fillScreen(INK_WHITE);
  D.setTextSize(1);

  // ---- date
  D.setTextColor(INK_BLACK);
  D.setTextDatum(top_center);
  snprintf(buf, sizeof buf, "%s %d %s", DAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);
  D.setFont(&fonts::FreeSansBold18pt7b);
  if (D.textWidth(buf) > W - 24) D.setFont(&fonts::FreeSansBold12pt7b);   // "Wednesday 30 September"
  D.drawString(buf, W / 2, 16);

  // ---- room
  const int ry = 62, rh = 186;
  D.drawRoundRect(14, ry, W - 28, rh, 16, INK_BLACK);
  D.drawRoundRect(15, ry + 1, W - 30, rh - 2, 15, INK_BLACK);
  D.setFont(&fonts::FreeSans9pt7b);
  D.setTextDatum(top_left);
  D.setTextColor(INK_BLACK);
  D.drawString("INDOORS", 30, ry + 10);
  if (isnan(d.roomC)) {
    D.setFont(&fonts::FreeSansBold18pt7b);
    D.setTextDatum(top_center);
    D.drawString("sensor error", W / 2, ry + 70);
  } else {
    snprintf(buf, sizeof buf, "%.1f", d.roomC);
    drawTemp(buf, W / 2, ry + 26, &fonts::FreeSansBold24pt7b, 2, tempInk(d.roomC), 10, "C");
    snprintf(buf, sizeof buf, "%.0f %% RH", d.roomRH);
    D.setFont(&fonts::FreeSansBold18pt7b);
    D.setTextColor(INK_BLACK);
    D.setTextDatum(top_center);
    D.drawString(buf, W / 2, ry + 128);
  }

  // ---- weather
  const int wy = 262;
  D.setFont(&fonts::FreeSans9pt7b);
  D.setTextDatum(top_left);
  D.setTextColor(INK_BLACK);
  D.drawString("TODAY", 22, wy);
  struct tm ft;
  time_t fetched = d.wx.fetchedAt;
  localtime_r(&fetched, &ft);
  bool today = d.haveWx && ft.tm_yday == lt.tm_yday && ft.tm_year == lt.tm_year;
  if (!d.haveWx) {
    D.setFont(&fonts::FreeSans12pt7b);
    D.setTextDatum(top_center);
    D.drawString("weather not available yet", W / 2, wy + 60);
  } else {
    if (today) {
      wxIcon(d.wx.dayCode, true, 72, wy + 62, 96);
      D.setFont(&fonts::FreeSansBold12pt7b);
      D.setTextDatum(top_left);
      D.setTextColor(INK_BLACK);
      String desc = Weather::describe(d.wx.dayCode);
      desc[0] = toupper(desc[0]);
      D.drawString(desc, 140, wy + 22);
      snprintf(buf, sizeof buf, "%.0f", d.wx.dayMax_c10 / 10.0f);
      drawTemp(buf, 175, wy + 54, &fonts::FreeSansBold18pt7b, 1, INK_RED, 5);
      snprintf(buf, sizeof buf, "%.0f", d.wx.dayMin_c10 / 10.0f);
      drawTemp(buf, 255, wy + 54, &fonts::FreeSansBold18pt7b, 1, INK_BLUE, 5);
      D.setFont(&fonts::FreeSans12pt7b);
      D.setTextColor(d.wx.dayRain >= 30 ? INK_BLUE : INK_BLACK);
      D.setTextDatum(top_left);
      snprintf(buf, sizeof buf, "rain %u %%", d.wx.dayRain);
      D.drawString(buf, 140, wy + 96);
    }
    // ---- next 5 hours: the hourly entries after the current hour
    const int hy = 396, cw = (W - 20) / 5;
    D.drawFastHLine(14, hy - 10, W - 28, INK_BLACK);
    int shown = 0;
    for (int i = 0; i < WxData::HOURS && shown < 5; ++i) {
      time_t ht = d.wx.hour0 + 3600 * i;
      if (ht <= d.now) continue;                        // past or current hour
      int cx = 10 + cw / 2 + cw * shown++;
      struct tm hl;
      localtime_r(&ht, &hl);
      snprintf(buf, sizeof buf, "%02d:00", hl.tm_hour);
      D.setFont(&fonts::FreeSansBold9pt7b);
      D.setTextColor(INK_BLACK);
      D.setTextDatum(top_center);
      D.drawString(buf, cx, hy);
      wxIcon(d.wx.code[i], d.wx.dayMask >> i & 1, cx, hy + 50, 58);
      snprintf(buf, sizeof buf, "%.0f", d.wx.temp_c10[i] / 10.0f);
      drawTemp(buf, cx, hy + 86, &fonts::FreeSansBold12pt7b, 1, INK_BLACK, 4);
      if (d.wx.rain[i]) {
        snprintf(buf, sizeof buf, "%u%%", d.wx.rain[i]);
        D.setFont(&fonts::FreeSans9pt7b);
        D.setTextColor(d.wx.rain[i] >= 30 ? INK_BLUE : INK_BLACK);
        D.setTextDatum(top_center);
        D.drawString(buf, cx, hy + 114);
      }
    }
    if (!shown) {
      D.setFont(&fonts::FreeSans12pt7b);
      D.setTextColor(INK_BLACK);
      D.setTextDatum(top_center);
      D.drawString("hourly forecast out of date", W / 2, hy + 50);
    }
  }

  // ---- footer: updated + battery (or the low-battery banner above it)
  D.drawFastHLine(14, 538, W - 28, INK_BLACK);
  if (d.lowBatt) {
    D.fillRect(0, 542, W, 22, INK_RED);
    D.setFont(&fonts::FreeSansBold9pt7b);
    D.setTextColor(INK_WHITE);
    D.setTextDatum(middle_center);
    D.drawString("LOW BATTERY - please charge", W / 2, 553);
  }
  D.setFont(&fonts::FreeSansBold12pt7b);
  D.setTextColor(INK_BLACK);
  D.setTextDatum(top_left);
  strftime(buf, sizeof buf, "Updated %H:%M", &lt);
  D.drawString(buf, 20, 570);
  if (d.storageLow) {                                    // more urgent than a stale forecast
    D.setFont(&fonts::FreeSansBold9pt7b);
    D.setTextColor(INK_RED);
    D.drawString("STORAGE FULL", 176, 574);
    D.setTextColor(INK_BLACK);
  } else if (d.haveWx && (d.now - fetched) > 3 * 3600) { // say so if the forecast is stale
    D.setFont(&fonts::FreeSans9pt7b);
    strftime(buf, sizeof buf, "(weather %H:%M)", &ft);
    D.drawString(buf, 176, 574);
  }
  int pct = batteryPercent(d.battV);
  battery(W - 16, 568, pct, d.usb);
  D.setFont(&fonts::FreeSansBold12pt7b);
  D.setTextDatum(top_right);
  snprintf(buf, sizeof buf, "%d%%", pct);
  D.drawString(buf, W - 16 - 54 - 6, 570);
}

void drawWifiQr(const char* ssid, const char* qrText) {
  D.fillScreen(INK_WHITE);
  D.setTextSize(1);
  D.setTextColor(INK_BLACK);
  D.setTextDatum(top_center);
  D.setFont(&fonts::FreeSansBold18pt7b);
  D.drawString("Wi-Fi", W / 2, 22);
  D.setFont(&fonts::FreeSans12pt7b);
  D.drawString("Scan with your phone's camera", W / 2, 70);
  D.qrcode(qrText, 20, 110, W - 40, 1, true);     // 360 px, white quiet zone included
  D.setFont(&fonts::FreeSansBold12pt7b);
  D.drawString(ssid, W / 2, 490);
  D.setFont(&fonts::FreeSans9pt7b);
  D.drawString("press the power button for the next page", W / 2, 560);
}

void drawIconSheet() {
  static const struct { uint8_t code; bool day; const char* name; } I[] = {
    { 0, true, "clear" },  { 0, false, "clear night" }, { 2, true, "part cloud" },
    { 2, false, "pc night" }, { 3, true, "overcast" }, { 45, true, "fog" },
    { 53, true, "drizzle" }, { 63, true, "rain" }, { 73, true, "snow" }, { 95, true, "thunder" },
  };
  D.fillScreen(INK_WHITE);
  D.setTextSize(1);
  D.setTextColor(INK_BLACK);
  D.setTextDatum(top_center);
  D.setFont(&fonts::FreeSans9pt7b);
  for (int i = 0; i < 10; ++i) {          // two columns: big (96) and small (58), 5 rows each
    int col = i % 2, row = i / 2, x0 = col * 200, y0 = row * 118;
    wxIcon(I[i].code, I[i].day, x0 + 60, y0 + 52, 96);
    wxIcon(I[i].code, I[i].day, x0 + 155, y0 + 52, 58);
    D.drawString(I[i].name, x0 + 100, y0 + 98);
  }
}

