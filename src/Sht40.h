// SHT40 temperature/humidity driver (Lesson 03 Part 2, extracted as a class).
// Talks through an M5Unified I2C_Class (the internal bus M5Unified owns).
#pragma once
#include <M5Unified.h>

class Sht40 {
public:
  struct Reading {
    float tempC, rh;
    uint16_t rawT, rawRH;
    bool crcOk;
  };

  explicit Sht40(m5::I2C_Class& bus, uint8_t addr = 0x44, uint32_t freq = 400000)
    : _bus(bus), _addr(addr), _freq(freq) {}

  bool read(Reading& out);                  // false = bus error
  bool serialNumber(uint32_t& out);         // false = bus error or bad CRC
  static uint8_t crc8(const uint8_t* data, size_t len);

private:
  bool exchange(uint8_t cmd, uint8_t* buf, size_t len, uint32_t waitMs);
  m5::I2C_Class& _bus;
  uint8_t _addr;
  uint32_t _freq;
};
