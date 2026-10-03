#include "Sht40.h"

// CRC-8, polynomial 0x31, init 0xFF (datasheet example: 0xBE 0xEF -> 0x92).
uint8_t Sht40::crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// Write `cmd`, wait, read `len` bytes (last one NACKed).
bool Sht40::exchange(uint8_t cmd, uint8_t* buf, size_t len, uint32_t waitMs) {
  if (!_bus.start(_addr, false, _freq)) return false;
  bool ok = _bus.write(cmd);
  ok &= _bus.stop();
  if (!ok) return false;
  delay(waitMs);  // NB: delay(n) is n-1..n ms (1 ms ticks) — keep a tick of margin
  if (!_bus.start(_addr, true, _freq)) return false;
  ok = _bus.read(buf, len, true);
  ok &= _bus.stop();
  return ok;
}

bool Sht40::read(Reading& out) {
  uint8_t b[6];
  if (!exchange(0xFD, b, 6, 10)) return false;  // high precision, max 8.2 ms
  out.crcOk = crc8(&b[0], 2) == b[2] && crc8(&b[3], 2) == b[5];
  out.rawT  = (uint16_t)(b[0] << 8 | b[1]);
  out.rawRH = (uint16_t)(b[3] << 8 | b[4]);
  out.tempC = -45.0f + 175.0f * out.rawT / 65535.0f;
  out.rh    = constrain(-6.0f + 125.0f * out.rawRH / 65535.0f, 0.0f, 100.0f);
  return true;
}

bool Sht40::serialNumber(uint32_t& out) {
  uint8_t s[6];
  if (!exchange(0x89, s, 6, 2)) return false;
  if (crc8(&s[0], 2) != s[2] || crc8(&s[3], 2) != s[5]) return false;
  out = (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[3] << 8 | s[4];
  return true;
}
