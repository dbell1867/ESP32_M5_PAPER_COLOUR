// Lesson 03, Part 2 — An SHT40 driver by hand (M5Unified has none).
// Protocol (Sensirion SHT4x datasheet):
//   write 0xFD (measure T + RH, high precision) -> wait ~10 ms ->
//   read 6 bytes: [T_hi T_lo CRC] [RH_hi RH_lo CRC]
//   CRC-8: polynomial 0x31, init 0xFF   (datasheet example: 0xBE 0xEF -> 0x92)
//   T  = -45 + 175 * raw / 65535   [degC]
//   RH =  -6 + 125 * raw / 65535   [%RH], clamped to 0..100
// Talks through M5.In_I2C (port 1) — the bus M5Unified owns (Part 1).
#include <Arduino.h>
#include <M5Unified.h>
#include "board_pins.h"

static constexpr uint8_t  SHT40_ADDR    = 0x44;
static constexpr uint32_t I2C_FREQ      = 400000;
static constexpr uint8_t  CMD_MEASURE   = 0xFD;  // high precision
static constexpr uint8_t  CMD_SERIAL    = 0x89;  // read 32-bit serial number

// CRC-8 as in the datasheet: shift each byte in, XOR the polynomial
// whenever the top bit falls out.
static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// One I2C "command then read" exchange: write `cmd`, wait, read `len` bytes.
static bool sht40Exchange(uint8_t cmd, uint8_t* buf, size_t len, uint32_t waitMs) {
  auto& bus = M5.In_I2C;
  if (!bus.start(SHT40_ADDR, false, I2C_FREQ)) return false;  // address + WRITE
  bool ok = bus.write(cmd);
  ok &= bus.stop();
  if (!ok) return false;
  delay(waitMs);                                              // sensor is measuring
  if (!bus.start(SHT40_ADDR, true, I2C_FREQ)) return false;   // address + READ
  ok = bus.read(buf, len, true);   // true = NACK the last byte: "that's enough"
  ok &= bus.stop();
  return ok;
}

struct Reading {
  float tempC, rh;
  uint16_t rawT, rawRH;
  bool crcOk;
};

// Returns false on a bus error. `out` is filled in place — like passing a
// mutable object in Python. `&` = "reference": the caller's own variable.
static bool readSHT40(Reading& out) {
  uint8_t b[6];
  if (!sht40Exchange(CMD_MEASURE, b, 6, 10)) return false;  // datasheet max 8.2 ms
  out.crcOk = crc8(&b[0], 2) == b[2] && crc8(&b[3], 2) == b[5];
  out.rawT  = (uint16_t)(b[0] << 8 | b[1]);  // big-endian: high byte first
  out.rawRH = (uint16_t)(b[3] << 8 | b[4]);
  out.tempC = -45.0f + 175.0f * out.rawT / 65535.0f;
  out.rh    = -6.0f + 125.0f * out.rawRH / 65535.0f;
  out.rh    = constrain(out.rh, 0.0f, 100.0f);  // formula can overshoot slightly
  return true;
}

void setup() {
  pinMode(PIN_AUDIO_PWR_EN, OUTPUT);  digitalWrite(PIN_AUDIO_PWR_EN, LOW);
  pinMode(PIN_SPK_EN, OUTPUT);        digitalWrite(PIN_SPK_EN, LOW);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  Serial.println("\nLesson 03 Part 2: SHT40 by hand");

  auto cfg = M5.config();
  cfg.internal_spk  = false;
  cfg.internal_mic  = false;
  cfg.clear_display = false;
  M5.begin(cfg);

  // 1. Prove the CRC code before trusting it to judge real data.
  const uint8_t example[2] = { 0xBE, 0xEF };
  uint8_t c = crc8(example, 2);
  Serial.printf("CRC self-test: crc8(BE EF) = 0x%02X (datasheet: 0x92) -> %s\n",
                c, c == 0x92 ? "PASS" : "FAIL");

  // 2. Identify the chip: its serial number, CRC-checked like any reading.
  uint8_t s[6];
  if (sht40Exchange(CMD_SERIAL, s, 6, 2)) {
    bool ok = crc8(&s[0], 2) == s[2] && crc8(&s[3], 2) == s[5];
    Serial.printf("Serial number: %02X%02X%02X%02X  (CRC %s)\n",
                  s[0], s[1], s[3], s[4], ok ? "ok" : "BAD");
  } else {
    Serial.println("Serial number read FAILED (no ACK)");
  }
}

void loop() {
  Reading r;
  uint32_t t0 = micros();
  bool ok = readSHT40(r);
  uint32_t us = micros() - t0;
  if (ok)
    Serial.printf("T %6.2f C   RH %5.1f %%   raw %5u / %5u   CRC %s   (%lu us)\n",
                  r.tempC, r.rh, r.rawT, r.rawRH, r.crcOk ? "ok" : "BAD",
                  (unsigned long)us);
  else
    Serial.println("read FAILED (bus error)");
  delay(2000);
}
