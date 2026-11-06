// The one owner of MUX_A/MUX_B/MUX_C.
//
// Those three lines address all three XS3A4051s at once (hall input, analog
// input, output drive), so nothing else in the firmware is allowed to touch
// them. Everything that needs a channel goes through here.
//
// Two rules, both enforced in select():
//   * SIG_OUT is driven low before the address changes, so the sweep cannot
//     walk a logic high across the four FET gates and flash the LED. With LEDC
//     on the pin that means dropping the duty AND waiting out one PWM period,
//     because ledcWrite() only queues a duty and the hardware loads it at the
//     next cycle boundary.
//   * Channel 7 is the parking spot: a 100k to GND on the hall mux, nothing
//     connected on the output mux. Deep sleep is the single exception - see
//     parkForSleep().
#pragma once

#include <Arduino.h>

#include "board_pins.h"

namespace ep {

class MuxBus {
 public:
  // Address lines to outputs, SIG_OUT low, HALL_IN input, SIG_IN to ADC with
  // 11 dB attenuation, then park. Safe to call more than once.
  void begin();

  // Change the selected channel. Drives SIG_OUT low first and waits
  // kSettleUs afterwards. A repeat of the current channel still settles.
  void select(uint8_t channel);

  uint8_t channel() const { return channel_; }

  // Channel 7 with SIG_OUT low. The resting state between operations.
  void park();

  // The resting state for deep sleep only, and the one place channel 7 is the
  // wrong answer. POWER_LATCH has to survive the sleep, which on the C3 takes
  // gpio_deep_sleep_hold_en() (GPIO10 is a plain digital pad), and that is a
  // single global bit: it freezes EVERY digital pad at the level it is driving.
  // Parked on channel 7, MUX_A/B/C are then held high into the 100k pull-downs
  // R13/R14/R15 for the whole sleep - 3 x 33 uA = ~99 uA, which is more than
  // the chip itself draws asleep. Channel 0 holds all three low instead and
  // costs nothing; it is also what the pull-downs give at reset, so the wake
  // path finds the address where it expects it. Safe at any channel, because
  // SIG_OUT is driven low here and is frozen low too, so no FET gate can be
  // energised. Call it last, immediately before esp_deep_sleep_start().
  void parkForSleep();

  // Re-drive MUX_A/B/C with the channel that is already selected, leaving
  // SIG_OUT and the LEDC timer alone. Electrically a no-op in the normal case,
  // which is the point: the indicator calls it on every service() pass while it
  // holds the bus, so an address the firmware did not move - a glitch, a reset
  // peripheral, anything that would otherwise leave the LED dark or the buzzer
  // silent with nothing to report it - is corrected within one loop pass. Use
  // select() to actually change channel; that is the only call that honours
  // rule one.
  void reassertAddress();

  // SIG_OUT is the output mux's common pin, so this energises whichever FET
  // gate the current address selects. Only meaningful on channels 0..3.
  void setSigOut(bool high);
  bool sigOut() const { return sigOutHigh_; }

  // Digital state of HALL_IN at the current channel.
  bool readHall();

  // One eFuse-calibrated reading of SIG_IN in millivolts, at the ADC pin.
  // The caller applies the divider; see ep::divider.
  uint16_t readSenseMv();

  // Median of `samples` readings. `samples` is forced odd and clamped to 9.
  // The divider nodes carry 100 nF and are permanently connected, so they are
  // already settled - this is about ADC noise, not about RC.
  uint16_t readSenseMvMedian(uint8_t samples);

  // Confidence check: channel 7 of the hall mux is a 100k to GND, so it must
  // read low. If it does not, the address lines or the mux are not working and
  // every hall reading is meaningless.
  //
  // The converse does not follow, and nothing is allowed to claim it does.
  // Channel 7 is 0b111, so a line stuck high still produces address 7 and this
  // still passes; a line stuck low - an open pad, held down by R13/R14/R15 -
  // turns the 7 we write into 6, 5 or 3, and with a magnet on that lid the
  // aliased hall reads low as well. A pass means U20 is powered and addressable.
  // It does not mean MUX_A/B/C move independently; only a channel-to-channel
  // comparison can show that.
  bool groundReferenceOk();

  // The LED and the buzzer need PWM on SIG_OUT while the address is held. The
  // indicator borrows the pin through these; they must be balanced.
  void attachPwm(uint32_t freqHz, uint8_t resolutionBits);
  void writePwm(uint32_t duty);
  void detachPwm();
  bool pwmAttached() const { return pwmAttached_; }

  // Settling allowance after an address change. The XS3A4051 switches in tens
  // of nanoseconds and there is no capacitance in the hall signal path, so this
  // is generous on purpose - a full 7-channel scan still costs under 250 us.
  static constexpr uint32_t kSettleUs = 25;

  // LEDC channel used for buzzer tone and LED brightness.
  static constexpr uint8_t kPwmChannel = 0;

  // Where parkForSleep() leaves the address. It lives here rather than beside
  // mux::kPark in board_pins.h because it is not a fact about the board, it is
  // a fact about how this driver puts the pads to bed.
  static constexpr uint8_t kSleepParkChannel = 0;

 private:
  void writeAddress(uint8_t channel);

  uint8_t channel_     = 0xFF;
  bool    sigOutHigh_  = false;
  bool    pwmAttached_ = false;
  bool    begun_       = false;
};

}  // namespace ep
