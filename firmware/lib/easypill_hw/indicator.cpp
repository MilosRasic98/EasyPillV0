#include "indicator.h"

namespace ep {
namespace {

// Nominal amber is red at full with green at roughly 60 %. The 470 R series
// resistors make this LED red-biased in practice, so expect to trim the green
// up once a real LED is wired to CTL1.
constexpr uint8_t kAmberGreen = 150;

constexpr uint32_t kPulsePeriodMs = 1000;  // ~1 Hz triangular ramp
constexpr uint16_t kDefaultToneHz = 2700;

// Triangular ramp 0..255 and back, integer only: a sine would pull in libm for
// no visible benefit at 255 steps.
uint8_t triangle(uint32_t elapsedMs, uint32_t periodMs) {
  const uint32_t t    = elapsedMs % periodMs;
  const uint32_t up   = periodMs / 2;
  const uint32_t down = periodMs - up;
  if (t < up) return static_cast<uint8_t>((t * 255u) / up);
  return static_cast<uint8_t>(((periodMs - t) * 255u) / down);
}

void patternColor(Pattern p, uint32_t elapsedMs, const uint8_t solid[3],
                  uint8_t out[3]) {
  out[0] = out[1] = out[2] = 0;
  switch (p) {
    case Pattern::Off:
      break;
    case Pattern::Solid:
      out[0] = solid[0];
      out[1] = solid[1];
      out[2] = solid[2];
      break;
    case Pattern::PulseGreen:
      out[1] = triangle(elapsedMs, kPulsePeriodMs);
      break;
    case Pattern::PulseAmber: {
      const uint8_t ramp = triangle(elapsedMs, kPulsePeriodMs);
      out[0] = ramp;
      out[1] = static_cast<uint8_t>((static_cast<uint16_t>(ramp) * kAmberGreen) / 255u);
      break;
    }
    case Pattern::PulseRed:
      out[0] = triangle(elapsedMs, kPulsePeriodMs);
      break;
    case Pattern::BlinkBlue:
      out[2] = (elapsedMs % 1000u) < 250u ? 255 : 0;
      break;
    case Pattern::DoubleBlinkGreen: {
      const uint32_t t = elapsedMs % 960u;  // 120 on, 120 off, 120 on, 600 off
      out[1] = (t < 120u || (t >= 240u && t < 360u)) ? 255 : 0;
      break;
    }
    case Pattern::SpinRgb: {
      // One pass only, with a dark gap between slots, so the user can say which
      // colour came first - that answer is the whole purpose of identifyLed.
      const uint32_t slot = elapsedMs / kIdentifySlotMs;
      if (slot < 3u && (elapsedMs % kIdentifySlotMs) < kIdentityOnMs) out[slot] = 255;
      break;
    }
  }
}

}  // namespace

void Indicator::begin(MuxBus* bus, const HardwareSettings* hw) {
  bus_ = bus;
  hw_  = hw;
  rgb_[0] = rgb_[1] = rgb_[2] = 0;
  pattern_        = Pattern::Off;
  patternStartMs_ = 0;
  patternEndMs_   = 0;
  slotIndex_      = 0;
  holdingBus_     = false;
  busLocked_      = false;
  holdChannel_    = 0xFF;
  holdFreqHz_     = 0;
  holdDuty_       = 0;
  holdUntilUs_    = micros();
  beepOnMs_       = 0;
  beepGapMs_      = 0;
  beepsLeft_      = 0;
  beepPhaseEndMs_ = 0;
  beepOn_         = false;
}

void Indicator::setColor(uint8_t r, uint8_t g, uint8_t b) {
  rgb_[0] = r;
  rgb_[1] = g;
  rgb_[2] = b;
  slotIndex_ = 0;

  // Let go rather than keep holding the channel the old colour was on; the next
  // service() reacquires with the new one.
  if (holdingBus_) yieldBus();

  // A bare setColor() is meant to light the LED, so it implies Solid. A timed
  // pattern already running keeps running; it just gets the new Solid colour
  // for when it ends.
  const bool any = (r | g | b) != 0;
  if (pattern_ == Pattern::Off && any) {
    pattern_        = Pattern::Solid;
    patternStartMs_ = millis();
    patternEndMs_   = 0;
  } else if (pattern_ == Pattern::Solid && !any) {
    pattern_      = Pattern::Off;
    patternEndMs_ = 0;
  }
}

void Indicator::playPattern(Pattern p, uint32_t durationMs) {
  if (holdingBus_) yieldBus();

  pattern_        = p;
  patternStartMs_ = millis();
  patternEndMs_   = (durationMs > 0) ? (patternStartMs_ + durationMs) : 0;
  if (durationMs > 0 && patternEndMs_ == 0) patternEndMs_ = 1;  // 0 means "forever"
  slotIndex_ = 0;

  if (p == Pattern::Off) {
    rgb_[0] = rgb_[1] = rgb_[2] = 0;
    patternEndMs_ = 0;
  }
}

void Indicator::beep(uint16_t onMs, uint8_t repeats, uint16_t gapMs) {
  if (repeats == 0) repeats = 1;
  beepOnMs_  = onMs;
  beepGapMs_ = gapMs;
  beepsLeft_ = repeats;
  beepOn_    = false;
  // 0 means "no phase scheduled", so the next service() starts the first beep.
  beepPhaseEndMs_ = 0;
}

void Indicator::stop() {
  pattern_        = Pattern::Off;
  patternStartMs_ = 0;
  patternEndMs_   = 0;
  rgb_[0] = rgb_[1] = rgb_[2] = 0;
  slotIndex_ = 0;

  beepOnMs_       = 0;
  beepGapMs_      = 0;
  beepsLeft_      = 0;
  beepPhaseEndMs_ = 0;
  beepOn_         = false;

  yieldBus();  // detaches LEDC, drives SIG_OUT low as a plain output, parks
}

bool Indicator::busy() const {
  // Still busy while it holds the bus, even with nothing left to show. The
  // caller stops calling service() the moment this goes false, so going false
  // with a channel still driven would leave a colour burning and nothing left
  // running to turn it off. One more service() finds an off pattern, hands the
  // bus back, and then this answers false.
  if (holdingBus_ || busLocked_) return true;
  if (beepsLeft_ > 0 || beepOn_) return true;
  if (pattern_ == Pattern::Off) return false;
  if (patternEndMs_ == 0) return true;  // runs until stop()
  return static_cast<int32_t>(millis() - patternEndMs_) < 0;
}

void Indicator::yieldBus() {
  if (bus_ == nullptr) return;
  bus_->detachPwm();
  bus_->park();
  holdingBus_  = false;
  busLocked_   = false;
  holdChannel_ = 0xFF;
  holdFreqHz_  = 0;
  holdDuty_    = 0;
}

bool Indicator::holdsBus() const {
  // Bounded by wall time, not by being serviced: this answers false once the
  // span is up even if service() is never called again, so an indicator that
  // stops being serviced cannot lock the bus away from the hall scan.
  return busLocked_ && !holdLapsed();
}

bool Indicator::holdLapsed() const {
  return static_cast<int32_t>(micros() - holdUntilUs_) >= 0;
}

void Indicator::grabChannel(uint8_t channel, uint32_t freqHz, uint32_t duty,
                            uint32_t spanUs) {
  // Rebuild only what is not already right. ledcSetup() restarts the LEDC
  // counter, and rebuilding on every pass would restart it faster than one
  // period - at the buzzer's 2.7 kHz that is 370 us against a loop pass of a few
  // microseconds, so the duty would never reach a cycle boundary and the buzzer
  // would sit silent. Whatever let go last may have left LEDC configured for
  // something else, though, so the frequency is part of the test.
  if (!holdingBus_ || holdChannel_ != channel || holdFreqHz_ != freqHz ||
      (freqHz != 0) != bus_->pwmAttached()) {
    bus_->detachPwm();      // the LED's 20 kHz is inaudible on a magnetic buzzer
    bus_->select(channel);  // drops SIG_OUT before the address moves
    if (freqHz != 0) bus_->attachPwm(freqHz, kPwmBits);
    holdChannel_ = channel;
    holdFreqHz_  = freqHz;
    holdingBus_  = true;
  }
  holdDuty_ = duty;
  if (spanUs != 0) holdUntilUs_ = micros() + spanUs;
  busLocked_ = true;
  reassertHold();
}

void Indicator::holdBuzzer(uint32_t spanUs) {
  const uint8_t channel =
      hw_->buzzerChannel < mux::kChannelCount ? hw_->buzzerChannel : mux::kOutBuzzer;
  if (hw_->buzzerSelfDrive) {
    // A self-oscillating piezo makes its own tone and only wants DC, so no LEDC
    // at all; any non-zero duty means "hold SIG_OUT high" on that path.
    grabChannel(channel, 0, 1, spanUs);
  } else {
    const uint16_t hz = hw_->buzzerToneHz > 0 ? hw_->buzzerToneHz : kDefaultToneHz;
    grabChannel(channel, hz, 1u << (kPwmBits - 1), spanUs);  // 50 % square wave
  }
}

void Indicator::reassertHold() {
  if (!holdingBus_) return;
  // The whole defence against a foreign select(): restate the address on every
  // pass. A channel that still matches costs three digitalWrite()s and changes
  // nothing on the pads; one that does not goes back through select(), which
  // drops SIG_OUT first and so cannot walk a high across the four gates on the
  // way back.
  if (bus_->channel() != holdChannel_) {
    bus_->select(holdChannel_);
  } else {
    bus_->reassertAddress();
  }
  // Restating the duty is one queued LEDC write of the value the channel already
  // has: the timer keeps running and the tone keeps its phase.
  if (holdFreqHz_ == 0) {
    bus_->setSigOut(holdDuty_ != 0);
  } else {
    bus_->writePwm(holdDuty_);
  }
}

void Indicator::service(uint32_t nowMs) {
  if (bus_ == nullptr || hw_ == nullptr) return;

  // A finished pattern goes back to off but does not cancel queued beeps; only
  // stop() does that.
  if (pattern_ != Pattern::Off && patternEndMs_ != 0 &&
      static_cast<int32_t>(nowMs - patternEndMs_) >= 0) {
    pattern_      = Pattern::Off;
    patternEndMs_ = 0;
    rgb_[0] = rgb_[1] = rgb_[2] = 0;
  }

  // ---- Beeps first: a chirp matters more than a colour and it wants the bus
  // ---- for its whole on-phase, so no colour slot can interleave.
  if (beepsLeft_ > 0 || beepOn_) {
    const bool phaseDue = (beepPhaseEndMs_ == 0) ||
                          static_cast<int32_t>(nowMs - beepPhaseEndMs_) >= 0;
    if (phaseDue) {
      if (beepOn_) {
        yieldBus();
        beepOn_         = false;
        beepPhaseEndMs_ = (beepsLeft_ > 0) ? (nowMs + beepGapMs_) : 0;
      } else if (beepsLeft_ > 0) {
        beepsLeft_--;
        beepOn_         = true;
        beepPhaseEndMs_ = nowMs + (beepOnMs_ > 0 ? beepOnMs_ : 1u);
        holdBuzzer(kMaxHoldUs);
      }
      // Keep 0 reserved as the "nothing scheduled" sentinel across the wrap.
      if (beepPhaseEndMs_ == 0 && (beepOn_ || beepsLeft_ > 0)) beepPhaseEndMs_ = 1;
    } else if (beepOn_) {
      // Mid on-phase, which is the whole point of a beep: the tone has to keep
      // driving, so restate the channel and the duty every pass rather than
      // assume nothing moved the address since the last one. The hold still
      // lapses every kMaxHoldUs, and that pass is the caller's window to scan;
      // if the caller does not take it the tone never breaks at all, because
      // grabChannel() rebuilds nothing that is already right.
      if (busLocked_ && holdLapsed()) {
        busLocked_ = false;
      } else {
        holdBuzzer(busLocked_ ? 0 : kMaxHoldUs);
      }
    }
    if (beepOn_ || beepsLeft_ > 0) return;
  }

  uint8_t colour[3];
  patternColor(pattern_, nowMs - patternStartMs_, rgb_, colour);

  uint8_t duty[3];
  uint8_t activeCount = 0;
  uint8_t onlyActive  = 0;
  for (uint8_t i = 0; i < 3; i++) {
    duty[i] = static_cast<uint8_t>(
        (static_cast<uint16_t>(colour[i]) * hw_->ledBrightness) / 255u);
    if (duty[i] != 0) {
      activeCount++;
      onlyActive = i;
    }
  }

  if (activeCount == 0) {
    if (holdingBus_) yieldBus();
    return;
  }

  // Which channel this slot belongs to. One active colour needs no multiplexing,
  // so every slot is the same channel and the duty simply stays put; a mixed
  // colour rotates, and re-selecting between slots is what drops SIG_OUT low in
  // between, so no two gates are ever driven together.
  uint8_t idx = onlyActive;
  if (activeCount > 1) {
    idx = slotIndex_ % 3;
    for (uint8_t i = 0; i < 3; i++) {
      if (duty[idx] != 0) break;
      idx = static_cast<uint8_t>((idx + 1) % 3);
    }
  }

  if (busLocked_ && holdLapsed()) {
    // The slot is up. Say so for this one pass and no more - that gap is what
    // the caller's hall scan runs in. A multiplexed slot ends dark so one colour
    // cannot burn on through the scan and skew the mix; a single colour is left
    // driving, because there is nothing to skew and whoever takes the bus next
    // drops SIG_OUT on its way in.
    busLocked_ = false;
    if (activeCount > 1) {
      bus_->writePwm(0);
      slotIndex_ = static_cast<uint8_t>((idx + 1) % 3);
    }
    return;
  }

  grabChannel(channelForColor(idx), kPwmFreqHz, duty[idx], busLocked_ ? 0 : kSlotUs);
}

uint8_t Indicator::channelForColor(uint8_t idx) const {
  if (hw_ == nullptr) return mux::kPark;
  uint8_t channel;
  switch (idx) {
    case 0:  channel = hw_->ledChannelRed;   break;
    case 1:  channel = hw_->ledChannelGreen; break;
    case 2:  channel = hw_->ledChannelBlue;  break;
    default: return mux::kPark;
  }
  // A garbage setting lands on the parking channel, where nothing is connected,
  // rather than on the buzzer.
  return channel < mux::kChannelCount ? channel : mux::kPark;
}

}  // namespace ep
