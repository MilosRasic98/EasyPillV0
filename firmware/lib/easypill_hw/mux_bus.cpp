#include "mux_bus.h"

namespace ep {
namespace {

// LEDC is set up once per attach and the frequency matters (20 kHz for the LED,
// ~2.7 kHz for a magnetic buzzer), so setSigOut() has to know the resolution to
// express a static high as a full-scale duty. There is exactly one MuxBus in
// the firmware - it owns the only set of address lines in the system - so file
// scope is the honest place for this rather than widening the header.
uint8_t gPwmBits = 0;

// One full PWM period, rounded up, plus a microsecond of margin. ledcWrite()
// only queues a duty - ledc_set_duty() then ledc_update_duty() - and the
// hardware loads it at the next cycle boundary, so this is how long SIG_OUT can
// still be driving after the duty has been dropped to zero.
uint32_t gPwmPeriodUs = 0;

}  // namespace

void MuxBus::begin() {
  // SIG_OUT first and low: it is the output mux's common pin, so anything else
  // we do to the address lines while it floats could walk a high across the
  // four FET gates. Its own 100k gate pull-downs cover us until this line runs.
  if (pwmAttached_) detachPwm();
  pinMode(pins::kSigOut, OUTPUT);
  digitalWrite(pins::kSigOut, LOW);
  sigOutHigh_ = false;

  pinMode(pins::kMuxA, OUTPUT);
  pinMode(pins::kMuxB, OUTPUT);
  pinMode(pins::kMuxC, OUTPUT);

  // The AH1912 is push-pull, so no pull-up - one would fight the sensor.
  pinMode(pins::kHallIn, INPUT);
  pinMode(pins::kWakeUp, INPUT);

  // VBAT reads 2.40 V at the pin on a full cell, so the 11 dB range (~0..2.5 V)
  // is the only one that fits. analogReadMilliVolts() applies the eFuse
  // calibration for us.
  analogSetPinAttenuation(pins::kSigIn, ADC_11db);
  analogReadResolution(12);

  // Before park(), so the select() inside it does not recurse back into begin().
  begun_   = true;
  channel_ = 0xFF;  // force the first select() to actually write the address
  park();
}

void MuxBus::select(uint8_t channel) {
  if (!begun_) begin();
  channel &= 0x07;

  // Rule one: SIG_OUT low before the address moves. With LEDC holding the pin a
  // digitalWrite() would be swallowed by the GPIO matrix, so the duty goes to
  // zero instead - but that write is only queued, and until the next PWM cycle
  // loads it the pad is still driving the tail of the old pulse. Wait it out,
  // or that high lands on whichever gate the new - or, mid-writeAddress, the
  // intermediate - address selects. The channel itself stays configured, so
  // pwmAttached_ and the tone survive the select().
  if (pwmAttached_) {
    ledcWrite(kPwmChannel, 0);
    delayMicroseconds(gPwmPeriodUs);
  } else {
    digitalWrite(pins::kSigOut, LOW);
  }
  sigOutHigh_ = false;

  writeAddress(channel);
  channel_ = channel;

  // Unconditional, even when the channel is unchanged: callers use select() as
  // the "it is safe to read now" barrier after a PWM write dropped SIG_OUT.
  delayMicroseconds(kSettleUs);
}

void MuxBus::park() { select(mux::kPark); }

void MuxBus::parkForSleep() {
  // detachPwm() first: when the deep-sleep autohold freezes the pads, SIG_OUT
  // has to be a plain GPIO output driven low rather than an LEDC-matrix pad.
  // select() then drops it before the address moves, exactly as everywhere else.
  if (pwmAttached_) detachPwm();
  select(kSleepParkChannel);
}

void MuxBus::reassertAddress() {
  // Nothing to restate before the first select(), and no guessing: 0xFF is the
  // "never selected" marker, not a channel.
  if (!begun_ || channel_ >= mux::kChannelCount) return;
  // Same three levels that are already on the pads, so SIG_OUT does not have to
  // be dropped first - rule one is about changing the address, and this does
  // not change it.
  writeAddress(channel_);
}

void MuxBus::setSigOut(bool high) {
  if (!begun_) begin();
  if (pwmAttached_) {
    const uint32_t full = (gPwmBits >= 32) ? 0xFFFFFFFFu : ((1u << gPwmBits) - 1u);
    ledcWrite(kPwmChannel, high ? full : 0);
  } else {
    digitalWrite(pins::kSigOut, high ? HIGH : LOW);
  }
  sigOutHigh_ = high;
}

bool MuxBus::readHall() {
  return digitalRead(pins::kHallIn) == HIGH;
}

uint16_t MuxBus::readSenseMv() {
  const uint32_t mv = analogReadMilliVolts(pins::kSigIn);
  return (mv > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(mv);
}

uint16_t MuxBus::readSenseMvMedian(uint8_t samples) {
  if (samples == 0) samples = 1;
  if (samples > 9) samples = 9;
  if ((samples & 1) == 0) samples++;  // clamped first, so this stays within 9

  uint16_t buf[9];
  for (uint8_t i = 0; i < samples; i++) {
    const uint16_t v = readSenseMv();
    // Insertion sort on the way in: nine elements, no allocation, no second pass.
    uint8_t j = i;
    while (j > 0 && buf[j - 1] > v) {
      buf[j] = buf[j - 1];
      j--;
    }
    buf[j] = v;
  }
  return buf[samples / 2];
}

bool MuxBus::groundReferenceOk() {
  const uint8_t previous = channel_;
  select(mux::kHallGndRef);
  const bool ok = !readHall();  // R38 is a 100k to GND, so this must read low
  if (previous < mux::kChannelCount) select(previous);
  return ok;
}

void MuxBus::attachPwm(uint32_t freqHz, uint8_t resolutionBits) {
  if (!begun_) begin();
  if (pwmAttached_) return;  // re-timing the channel mid-tone is never wanted

  if (freqHz == 0) freqHz = 1000;
  if (resolutionBits == 0 || resolutionBits > 14) resolutionBits = 8;

  ledcSetup(kPwmChannel, freqHz, resolutionBits);
  ledcWrite(kPwmChannel, 0);  // low before the pin is handed over to LEDC
  ledcAttachPin(pins::kSigOut, kPwmChannel);

  gPwmBits     = resolutionBits;
  gPwmPeriodUs = (1000000u + freqHz - 1u) / freqHz + 1u;
  pwmAttached_ = true;
  sigOutHigh_  = false;
}

void MuxBus::writePwm(uint32_t duty) {
  if (!pwmAttached_) return;
  const uint32_t full = (gPwmBits >= 32) ? 0xFFFFFFFFu : ((1u << gPwmBits) - 1u);
  if (duty > full) duty = full;
  ledcWrite(kPwmChannel, duty);
  sigOutHigh_ = (duty != 0);
}

void MuxBus::detachPwm() {
  if (!pwmAttached_) return;
  ledcWrite(kPwmChannel, 0);
  ledcDetachPin(pins::kSigOut);
  pwmAttached_ = false;

  // The matrix detach leaves the pad wherever it was configured, so restate it:
  // SIG_OUT is a plain output and its resting level is low.
  pinMode(pins::kSigOut, OUTPUT);
  digitalWrite(pins::kSigOut, LOW);
  sigOutHigh_ = false;
}

void MuxBus::writeAddress(uint8_t channel) {
  // XS3A4051 S1/S2/S3 = address bits 0/1/2, so channel = A + 2B + 4C.
  digitalWrite(pins::kMuxA, (channel & 0x01) ? HIGH : LOW);
  digitalWrite(pins::kMuxB, (channel & 0x02) ? HIGH : LOW);
  digitalWrite(pins::kMuxC, (channel & 0x04) ? HIGH : LOW);
}

}  // namespace ep
