// EasyPill V1 board pin map.
//
// Derived from Schematic_EaSyPill_V1_2026-09-27.pdf. See docs/HARDWARE.md for
// the reasoning behind every line in here; if the board is reworked, update
// that document first.
//
// The XIAO silkscreen pins D0..D10 map to different GPIOs on the C3 and the S3,
// so everything below goes through the Arduino variant macros and the same
// source builds for both modules.

#pragma once

#include <Arduino.h>

#if !defined(ARDUINO_XIAO_ESP32C3) && !defined(ARDUINO_XIAO_ESP32S3)
#error "EasyPill expects a Seeed XIAO ESP32C3 or ESP32S3 board definition"
#endif

namespace ep {
namespace pins {

// ---- Mux address lines, shared by all three XS3A4051s -----------------------
// MUX_C is U0TXD and SIG_OUT is U0RXD on both modules. Never open Serial0.
constexpr uint8_t kMuxA = D4;  // XS3A4051 S1, address bit 0
constexpr uint8_t kMuxB = D5;  // XS3A4051 S2, address bit 1
constexpr uint8_t kMuxC = D6;  // XS3A4051 S3, address bit 2  == U0TXD

// ---- Mux common pins --------------------------------------------------------
constexpr uint8_t kHallIn = D3;   // U20 Z: 7 hall sensors + GND ref on ch7
constexpr uint8_t kSigIn  = D2;   // U4  Z: analog, ADC1 on both modules
constexpr uint8_t kSigOut = D7;   // U5  Z: drives Q1..Q4 gates  == U0RXD

// ---- Direct lines -----------------------------------------------------------
constexpr uint8_t kWakeUp      = D1;   // hardware OR of all 7 halls + button + charger
constexpr uint8_t kPowerLatch  = D10;  // soft-latch hold, assert HIGH first thing

// ---- Deep-sleep wake GPIO, as a raw number for the esp_sleep_* API ----------
#if defined(ARDUINO_XIAO_ESP32C3)
constexpr int kWakeUpGpio     = 3;    // D1
constexpr int kPowerLatchGpio = 10;   // D10, plain digital pad
constexpr bool kLatchIsRtcPad = false;
#else  // ARDUINO_XIAO_ESP32S3
constexpr int kWakeUpGpio     = 2;    // D1, RTC pad -> ext0 capable
constexpr int kPowerLatchGpio = 9;    // D10, RTC pad
constexpr bool kLatchIsRtcPad = true;
#endif

}  // namespace pins

// ---- Mux channel assignments ------------------------------------------------
// One address drives all three muxes, so these three enums share a number
// space. Channel 7 is the safe parking spot: a 100k to GND on the hall mux and
// nothing at all on the output mux.
namespace mux {

constexpr uint8_t kChannelCount = 8;
constexpr uint8_t kPark = 7;

// U20, HALL_IN. Channels 0..6 are hall sensors 1..7 in order.
constexpr uint8_t kHall0    = 0;
constexpr uint8_t kHallLast = 6;
constexpr uint8_t kHallGndRef = 7;   // 100k to GND via R38

// U4, SIG_IN.
constexpr uint8_t kSenseVbat   = 0;  // x 0.5714  (75k / 100k)
constexpr uint8_t kSenseUsb    = 1;  // x 0.4286  (100k / 75k)
constexpr uint8_t kSenseCoil   = 2;  // x 0.4286  (100k / 75k)
constexpr uint8_t kSenseButton = 3;  // raw VBAT, clamped - threshold only
constexpr uint8_t kSenseStat   = 4;  // MCP73831 STAT, clamped - threshold only

// U5, SIG_OUT. Which colour sits on which CTL1 pin is a setting, not a fact.
constexpr uint8_t kOutQ3Ctl1Pin4 = 0;
constexpr uint8_t kOutQ2Ctl1Pin3 = 1;
constexpr uint8_t kOutQ1Ctl1Pin2 = 2;
constexpr uint8_t kOutBuzzer     = 3;

}  // namespace mux

// ---- Divider ratios, as integer numerator/denominator pairs -----------------
// adc_mV * numerator / denominator = real mV at the source.
namespace divider {
constexpr uint16_t kVbatNum = 175, kVbatDen = 100;  // 0.5714 -> 1.75x
constexpr uint16_t kRailNum = 175, kRailDen = 75;   // 0.4286 -> 2.3333x
// Button/STAT clamp through the mux ESD diode, so 1:1 and read as a threshold.
// After the optional 33k+100k rework, set 133/100 in settings.
constexpr uint16_t kButtonNum = 1, kButtonDen = 1;
}  // namespace divider

}  // namespace ep
