// The soft power latch, and deep sleep.
//
// The user has to hold the button down until firmware asserts POWER_LATCH;
// release before that and the board drops dead. So assertEarly() is the first
// statement in setup(), before Serial, before anything.
#pragma once

#include <Arduino.h>

#include "board_pins.h"

namespace ep {

enum class WakeReason : uint8_t {
  PowerOn = 0,   // cold boot or reset
  Timer,         // the scheduled wake
  WakeUpPin,     // a lid, the button, or a charger
  Other,
};

class PowerLatch {
 public:
  // POWER_LATCH high, immediately, plus the pad hold so a panic or WDT reset
  // cannot drop it mid-reboot. Call this first in setup().
  static void assertEarly();

  // Arm the pad holds that keep POWER_LATCH high through deep sleep. On the C3
  // GPIO10 is a plain digital pad and needs both gpio_hold_en() and
  // gpio_deep_sleep_hold_en(); on the S3 GPIO9 is an RTC pad and the first
  // would do. Both are called on both, which is harmless.
  //
  // Not harmless, and the reason this is documented here: the second call is a
  // single global bit, not a per-pad opt-in. It freezes EVERY digital pad at the
  // level it is driving, so whatever the mux address and SIG_OUT hold when the
  // chip sleeps, they keep holding - see MuxBus::parkForSleep().
  static void holdThroughSleep();

  // Let go of the pad hold, e.g. to deliberately power down.
  static void releaseHold();

  // Drop the latch. On battery this is the last thing the firmware ever does.
  // With USB plugged in it does nothing visible, because the XIAO's own
  // regulator keeps 3V3 alive.
  static void powerOff();

  static bool isAsserted();
};

// WAKE_UP is the hardware OR of all seven halls, the button and the charger.
bool wakeUpLineHigh();

WakeReason lastWakeReason();
const char* toString(WakeReason r);

// Deep sleep for `seconds`, optionally arming the WAKE_UP pin as a wake source.
//
// WAKE_UP is level triggered, so arming it while it is already high wakes the
// chip instantly and forever. Pass armWakePin = true and this function will
// check the line and silently fall back to a timer-only sleep if it is high -
// which is the right behaviour when a lid has been left open.
//
// Does not return.
void enterDeepSleep(uint32_t seconds, bool armWakePin);

}  // namespace ep
