// Battery, charger rails, button and charge status, all through the SIG_IN mux.
#pragma once

#include <Arduino.h>

#include "ep_settings.h"
#include "mux_bus.h"

namespace ep {

struct PowerReading {
  bool     valid;
  uint16_t vbatMv;        // after the 75k/100k divider correction
  uint16_t usbMv;         // after the 100k/75k divider correction
  uint16_t coilMv;
  uint16_t buttonRawMv;   // at the ADC pin; clamped, threshold only
  uint16_t statRawMv;
  bool     buttonPressed;
  bool     statHigh;
  bool     usbPresent;
  bool     coilPresent;
  bool     charging;      // a rail is present and STAT says not finished
  uint8_t  batteryPercent;
  uint32_t sampledAtMs;
};

class AnalogSense {
 public:
  void begin(MuxBus* bus, const HardwareSettings* hw);

  // Takes the bus for roughly 2 ms: five channels, median of five each.
  void sampleAll(uint32_t nowMs);

  // Just the battery, for the low-voltage watchdog. Much cheaper.
  uint16_t sampleVbatMv();

  const PowerReading& reading() const { return r_; }

  // Raw ADC millivolts on one channel, no divider applied. For bring-up.
  uint16_t rawChannelMv(uint8_t channel, uint8_t samples = 5);

  // Single-cell LiPo discharge curve, lightly smoothed. Deliberately
  // pessimistic at the top so a freshly-charged pack does not sit at 100 %
  // for an hour.
  static uint8_t lipoPercent(uint16_t mv);

 private:
  MuxBus*                 bus_ = nullptr;
  const HardwareSettings* hw_  = nullptr;
  PowerReading            r_{};
};

}  // namespace ep
