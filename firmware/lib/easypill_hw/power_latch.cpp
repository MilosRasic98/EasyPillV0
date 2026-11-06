#include "power_latch.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

namespace ep {
namespace {

// Nothing would ever wake a sleep with no timer and no pin armed, and a box
// that never wakes is indistinguishable from a dead one. Fall back to this.
constexpr uint32_t kFallbackSleepSec = 60;

}  // namespace

void PowerLatch::assertEarly() {
  // The user is still holding the button at this point and lets go the moment
  // they see something happen, so this runs before Serial, before anything.
  pinMode(pins::kPowerLatch, OUTPUT);
  digitalWrite(pins::kPowerLatch, HIGH);
  // Immediately, not later: a panic or WDT reset re-runs setup() from the top
  // and the pad would otherwise float through the reboot, dropping the rail.
  holdThroughSleep();
}

void PowerLatch::holdThroughSleep() {
  gpio_hold_en(static_cast<gpio_num_t>(pins::kPowerLatchGpio));
  // Only the C3's plain digital pad needs this second call; on the S3 the latch
  // is on an RTC pad and the hold above is enough. Harmless on both.
  gpio_deep_sleep_hold_en();
}

void PowerLatch::releaseHold() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(pins::kPowerLatchGpio));
}

void PowerLatch::powerOff() {
  releaseHold();  // the hold would otherwise keep the pad high regardless
  pinMode(pins::kPowerLatch, OUTPUT);
  digitalWrite(pins::kPowerLatch, LOW);
  // On battery the 3V3 rail collapses inside a few milliseconds and nothing
  // after this line runs. With USB in, the XIAO's own regulator keeps us alive,
  // so returning is the correct behaviour rather than spinning forever.
}

bool PowerLatch::isAsserted() {
  // Arduino's OUTPUT keeps the input path enabled, so this reads the real pad
  // level and not a shadow copy of what we last wrote.
  return digitalRead(pins::kPowerLatch) == HIGH;
}

bool wakeUpLineHigh() {
  // Callable before MuxBus::begin() - this gets asked right at boot and right
  // before sleeping, neither of which is guaranteed to be after bus setup.
  pinMode(pins::kWakeUp, INPUT);
  return digitalRead(pins::kWakeUp) == HIGH;
}

WakeReason lastWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return WakeReason::Timer;
    case ESP_SLEEP_WAKEUP_GPIO:  // C3 light/deep-sleep GPIO wake
    case ESP_SLEEP_WAKEUP_EXT0:  // S3 RTC-pad wake
    case ESP_SLEEP_WAKEUP_EXT1:
      return WakeReason::WakeUpPin;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return WakeReason::PowerOn;  // cold boot, or any non-sleep reset
    default:
      return WakeReason::Other;
  }
}

const char* toString(WakeReason r) {
  switch (r) {
    case WakeReason::PowerOn:   return "powerOn";
    case WakeReason::Timer:     return "timer";
    case WakeReason::WakeUpPin: return "wakeUpPin";
    case WakeReason::Other:     return "other";
  }
  return "unknown";
}

void enterDeepSleep(uint32_t seconds, bool armWakePin) {
  // Read the line before arming anything. WAKE_UP is a hardware OR of all seven
  // halls, the button and charger presence, and it is level triggered: arming
  // it while a lid is open or a charger is plugged in wakes the chip instantly,
  // then again, forever. A timer-only sleep is the right answer there - the
  // next timed wake will scan and find the lid still open.
  const bool armPin = armWakePin && !wakeUpLineHigh();

  if (armPin) {
#if defined(ARDUINO_XIAO_ESP32C3)
    esp_deep_sleep_enable_gpio_wakeup(1ULL << pins::kWakeUpGpio,
                                      ESP_GPIO_WAKEUP_GPIO_HIGH);
#else  // ARDUINO_XIAO_ESP32S3
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(pins::kWakeUpGpio), 1);
#endif
  }

  const uint32_t timerSec = (seconds > 0) ? seconds
                            : (armPin ? 0 : kFallbackSleepSec);
  if (timerSec > 0) {
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timerSec) * 1000000ULL);
  }

  // Re-arm the pad hold last. Note what this actually does, because the
  // intuition is backwards: gpio_deep_sleep_hold_en() is one global bit, so
  // SIG_OUT and MUX_A/B/C are NOT released as the chip sleeps - every digital
  // pad keeps driving whatever level it holds at this instant, for the whole
  // sleep. SIG_OUT frozen low is exactly what we want, since no FET gate can be
  // energised at any address. But it also means the caller must have moved the
  // address off the parking channel first: channel 7 is A/B/C all high, which is
  // three times 3.3 V / 100 k through R13/R14/R15 - about 99 uA, more than the
  // chip itself draws asleep. MuxBus::parkForSleep() is that call, and
  // maybeSleep() makes it immediately before this one.
  PowerLatch::holdThroughSleep();
  esp_deep_sleep_start();
}

}  // namespace ep
