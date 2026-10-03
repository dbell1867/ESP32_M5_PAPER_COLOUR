// M5Stack PaperColor (C151) pin map — from docs.m5stack.com/en/products/sku/C151
// and cross-checked against M5Unified's PaperColor code.
#pragma once
#include <cstdint>

// Audio power — both ACTIVE HIGH (M5Unified drives them HIGH to enable).
// We hold them LOW so the codec and speaker amp stay off: no pops, no beeps.
constexpr uint8_t PIN_AUDIO_PWR_EN = 45;  // ES8311 codec + ES7210 mic power
constexpr uint8_t PIN_SPK_EN       = 46;  // AW8737A speaker amplifier enable

// E-paper BUSY: LOW while the panel is working. M5GFX owns it; we only LISTEN.
constexpr uint8_t PIN_EPD_BUSY = 11;

// microSD — shares the e-paper's SPI2 (FSPI) bus; power comes from the PMIC
// (M5GFX switches it on during M5.begin()).
constexpr uint8_t PIN_SPI_SCLK = 15;
constexpr uint8_t PIN_SPI_MOSI = 13;
constexpr uint8_t PIN_SPI_MISO = 14;
constexpr uint8_t PIN_SD_CS    = 47;

// Buttons — probed: all three real, external pull-ups, active LOW.
// NOTE: docs also list G1 as SD card-detect — under test in Stage 3b.
constexpr uint8_t PIN_BTN_A = 10;
constexpr uint8_t PIN_BTN_B = 9;
constexpr uint8_t PIN_BTN_C = 1;
