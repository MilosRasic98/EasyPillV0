#include "analog_sense.h"

namespace ep {
namespace {

// Channels 1 and 2 are real divided measurements of the two 5 V inputs, so they
// are the only trustworthy "is a charger attached" signal on the board.
constexpr uint16_t kRailPresentMv = 4000;

constexpr uint8_t kSampleCount = 5;

// adcMv * num / den, in 32-bit: a 2400 mV reading times 175 is 420000, which
// does not fit in 16 bits.
uint16_t scaleMv(uint16_t adcMv, uint16_t num, uint16_t den) {
  if (num == 0 || den == 0) return adcMv;  // unset divider: report what we read
  const uint32_t v = (static_cast<uint32_t>(adcMv) * num) / den;
  return (v > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(v);
}

}  // namespace

void AnalogSense::begin(MuxBus* bus, const HardwareSettings* hw) {
  bus_ = bus;
  hw_  = hw;
  r_   = PowerReading{};
}

void AnalogSense::sampleAll(uint32_t nowMs) {
  if (bus_ == nullptr || hw_ == nullptr) {
    r_.valid = false;
    return;
  }

  uint16_t adc[mux::kSenseStat + 1];
  for (uint8_t ch = mux::kSenseVbat; ch <= mux::kSenseStat; ch++) {
    bus_->select(ch);
    adc[ch] = bus_->readSenseMvMedian(kSampleCount);
  }
  bus_->park();

  r_.vbatMv = scaleMv(adc[mux::kSenseVbat], hw_->vbatDivNum, hw_->vbatDivDen);
  r_.usbMv  = scaleMv(adc[mux::kSenseUsb], hw_->railDivNum, hw_->railDivDen);
  r_.coilMv = scaleMv(adc[mux::kSenseCoil], hw_->railDivNum, hw_->railDivDen);

  // Kept as read at the pin, because with the stock rework both of these clamp
  // through the mux's ESD diode and the number means nothing as a voltage.
  r_.buttonRawMv = adc[mux::kSenseButton];
  r_.statRawMv   = adc[mux::kSenseStat];

  // The button threshold is compared against the divider-corrected value so
  // that the optional 33k/100k rework (buttonDiv 133/100) keeps the same
  // threshold meaning. With the default 1/1 the two are identical. STAT has no
  // divider setting, so it is compared as read.
  const uint16_t buttonMv =
      scaleMv(r_.buttonRawMv, hw_->buttonDivNum, hw_->buttonDivDen);
  r_.buttonPressed = buttonMv >= hw_->buttonThresholdMv;
  r_.statHigh      = r_.statRawMv >= hw_->statThresholdMv;

  r_.usbPresent  = r_.usbMv > kRailPresentMv;
  r_.coilPresent = r_.coilMv > kRailPresentMv;

  // The MCP73831's STAT is push-pull and low means "charging *or* no input",
  // with R37 pulling the Hi-Z done state low as well. Only the rails can tell
  // those apart.
  r_.charging = (r_.usbPresent || r_.coilPresent) && !r_.statHigh;

  r_.batteryPercent = lipoPercent(r_.vbatMv);
  r_.sampledAtMs    = nowMs;
  r_.valid          = true;
}

uint16_t AnalogSense::sampleVbatMv() {
  if (bus_ == nullptr || hw_ == nullptr) return 0;
  bus_->select(mux::kSenseVbat);
  const uint16_t adcMv = bus_->readSenseMvMedian(kSampleCount);
  bus_->park();
  // Deliberately does not touch r_, so reading() stays one coherent snapshot
  // taken by sampleAll().
  return scaleMv(adcMv, hw_->vbatDivNum, hw_->vbatDivDen);
}

uint16_t AnalogSense::rawChannelMv(uint8_t channel, uint8_t samples) {
  if (bus_ == nullptr || channel >= mux::kChannelCount) return 0;
  bus_->select(channel);
  const uint16_t mv = bus_->readSenseMvMedian(samples);
  bus_->park();
  return mv;
}

uint8_t AnalogSense::lipoPercent(uint16_t mv) {
  // Breakpoints of a single-cell LiPo under a light load, descending. The top
  // is deliberately steep (4.2 V is barely above 4.1 V in charge terms) and the
  // knee below 3.6 V is where a cell really does fall off a cliff.
  //
  // The simulator's LIPO_CURVE mirrors this table so that a given voltage reads
  // the same on the bench and in the GUI. Touch one and you must touch the
  // other.
  struct Point {
    uint16_t mv;
    uint8_t  pct;
  };
  static constexpr Point kCurve[] = {
      {4200, 100}, {4100, 90}, {4000, 80}, {3900, 70}, {3800, 60}, {3700, 50},
      {3600, 35},  {3500, 22}, {3400, 12}, {3300, 5},  {3000, 0},
  };
  constexpr uint8_t kPoints = sizeof(kCurve) / sizeof(kCurve[0]);

  if (mv >= kCurve[0].mv) return kCurve[0].pct;
  if (mv <= kCurve[kPoints - 1].mv) return kCurve[kPoints - 1].pct;

  for (uint8_t i = 1; i < kPoints; i++) {
    if (mv < kCurve[i].mv) continue;
    const uint16_t lo = kCurve[i].mv, hi = kCurve[i - 1].mv;
    const uint8_t loPct = kCurve[i].pct, hiPct = kCurve[i - 1].pct;
    const uint32_t span = static_cast<uint32_t>(mv - lo) * (hiPct - loPct);
    return static_cast<uint8_t>(loPct + span / (hi - lo));
  }
  return 0;
}

}  // namespace ep
