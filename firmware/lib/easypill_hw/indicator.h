// RGB LED and buzzer, both of which live behind the same mux as the sensors.
//
// Holding a colour on means holding the mux address, which means no hall reads.
// So the indicator is cooperative, and holdsBus() is how it cooperates:
// service() takes the address lines for one slot and says so, and the caller
// leaves the bus alone until the slot ends. A slot is 2 ms, and a scan with the
// park either side of it costs around 350 us, so the LED keeps roughly 85 % duty
// while sensing continues.
//
// Nothing in here blocks. A slot is a micros() deadline, not a
// delayMicroseconds(), so it spans as many loop passes as it takes and the LED
// stays lit while the web server answers a request. What does NOT work, and was
// the shape this file used to assume, is service() followed immediately by
// yieldBus(): that parks the address microseconds after taking it, which tears
// a beep down long before one period of its 2.7 kHz tone has elapsed and lights
// a single-colour pattern for the duration of one function call. Deleting the
// yieldBus() is not enough either - the next hall scan's own select() moves the
// address 250 us later. The caller has to be told to wait, which is holdsBus(),
// and service() has to keep restating what it is driving, which it now does.
//
// In normal operation the LED is off - this is supposed to look like a dumb
// box - and the indicator only takes the bus for brief announcements.
#pragma once

#include <Arduino.h>

#include "ep_settings.h"
#include "mux_bus.h"

namespace ep {

enum class Pattern : uint8_t {
  Off = 0,
  Solid,            // whatever setColor last set
  PulseGreen,       // dose taken
  PulseAmber,       // dose due
  PulseRed,         // dose missed / battery critical
  BlinkBlue,        // Wi-Fi / setup mode
  DoubleBlinkGreen, // acknowledged
  SpinRgb,          // one-shot identify ramp: red, then green, then blue, once
};

// SpinRgb lights colour slot 0, then 1, then 2, for kIdentifySlotMs each with a
// dark gap between them, and then stays dark. The gap and the one-shot are the
// whole point: /api/action identifyLed tells the user which slot they are
// looking at, and a seamless loop makes "which came first" unanswerable.
constexpr uint32_t kIdentifySlotMs = 1500;
constexpr uint32_t kIdentityOnMs   = 1200;
constexpr uint32_t kIdentifyMs     = kIdentifySlotMs * 3;

class Indicator {
 public:
  void begin(MuxBus* bus, const HardwareSettings* hw);

  // 0..255 per channel. Takes effect on the next service() call. A single
  // non-zero channel is driven statically, which costs no CPU and no bus time
  // beyond the initial select; mixed colours time-multiplex.
  void setColor(uint8_t r, uint8_t g, uint8_t b);

  // Run a pattern for durationMs, then return to off. durationMs == 0 runs
  // until stop() or another call.
  void playPattern(Pattern p, uint32_t durationMs);

  // Queue a beep sequence. Honours buzzerSelfDrive and buzzerToneHz.
  void beep(uint16_t onMs, uint8_t repeats = 1, uint16_t gapMs = 120);

  // Drop everything, release the bus, park the mux.
  void stop();

  // True while the indicator still wants bus time.
  bool busy() const;

  // Drives at most one slot, then returns. Must be called from the main loop.
  // Safe to call when idle (does nothing and holds no bus).
  void service(uint32_t nowMs);

  // Hand the address lines back without cancelling the pattern, so the caller
  // can scan. service() reacquires on its next slot. Only legal while
  // holdsBus() is false.
  void yieldBus();

  // True while the indicator must keep the address lines to finish what it is
  // doing: a beep on-phase, or a colour slot in progress. The caller must not
  // scan halls, sample analog, or call yieldBus() while this is true - MUX_A/B/C
  // are shared, so any of those moves the channel out from under whatever is
  // being driven, and the symptom is a silent buzzer or an LED at a few percent
  // of its duty with nothing in the system to report it.
  //
  // It goes false by itself at the end of every slot and at least every
  // kMaxHoldUs, whether or not service() is being called, so a caller that
  // honours it can never be starved of the bus.
  bool holdsBus() const;

  static constexpr uint32_t kSlotUs       = 2000;
  static constexpr uint16_t kPwmFreqHz    = 20000;  // above audio, for the LED
  static constexpr uint8_t  kPwmBits      = 8;

  // The longest one hold can last. A colour slot is kSlotUs; a beep on-phase is
  // whatever was asked for, and POST /api/action beep allows up to 5000 ms, so a
  // long chirp is sliced into holds of this length with a scan window between
  // them. 250 ms is longer than every beep the firmware itself asks for (60, 120
  // and 150 ms), so no real reminder is ever cut in half, and short enough that
  // a test beep from the GUI cannot stop the box watching the lids for five
  // seconds. The slice costs the tone one park-and-rebuild, which is around
  // 300 us out of 250 ms.
  static constexpr uint32_t kMaxHoldUs    = 250000;

 private:
  uint8_t channelForColor(uint8_t idx) const;

  // Take, or keep, one output channel with `duty` on SIG_OUT. freqHz is the LEDC
  // frequency to configure if the channel has to be built; 0 means DC, with no
  // LEDC at all, for a self-oscillating piezo. spanUs starts a fresh hold of
  // that length, and 0 keeps the deadline the hold already has - a refresh in
  // the middle of a slot must not postpone the caller's turn.
  void grabChannel(uint8_t channel, uint32_t freqHz, uint32_t duty, uint32_t spanUs);

  // The buzzer channel, tone and duty that the settings currently describe.
  void holdBuzzer(uint32_t spanUs);

  // Restate the address and the drive level, rebuilding nothing.
  void reassertHold();

  // Has this hold used up its span? Deliberately on micros(), not on a call
  // count: the loop's pass rate changes with Wi-Fi and web traffic.
  bool holdLapsed() const;

  MuxBus*                 bus_ = nullptr;
  const HardwareSettings* hw_  = nullptr;

  uint8_t  rgb_[3]{};
  Pattern  pattern_       = Pattern::Off;
  uint32_t patternStartMs_ = 0;
  uint32_t patternEndMs_   = 0;
  uint8_t  slotIndex_      = 0;
  bool     holdingBus_     = false;  // the channel is built and ours
  bool     busLocked_      = false;  // ... and the caller must stay off it
  uint8_t  holdChannel_    = 0xFF;   // 0xFF: nothing held
  uint32_t holdFreqHz_     = 0;      // 0: DC on SIG_OUT, no LEDC
  uint32_t holdDuty_       = 0;
  uint32_t holdUntilUs_    = 0;

  uint16_t beepOnMs_   = 0;
  uint16_t beepGapMs_  = 0;
  uint8_t  beepsLeft_  = 0;
  uint32_t beepPhaseEndMs_ = 0;
  bool     beepOn_     = false;
};

}  // namespace ep
