// Reading the seven lid sensors through the shared mux, with debounce.
#pragma once

#include <Arduino.h>

#include "ep_settings.h"
#include "mux_bus.h"

namespace ep {

struct HallState {
  uint8_t rawMask;      // bit n: hall sensor n output is high
  uint8_t openMask;     // bit n: lid n is open, after polarity and debounce
  uint8_t changedMask;  // bits of openMask that changed on the last scan
  bool    gndRefOk;     // channel 7 read low, so the mux is alive
  uint32_t lastScanMs;
};

class HallScanner {
 public:
  void begin(MuxBus* bus, const HardwareSettings* hw);

  // One pass over channels 0..6 plus the ground reference. Costs roughly
  // 250 us, so it is cheap enough to call every few milliseconds from the main
  // loop; debouncing is done on wall time, not on call count.
  void scan(uint32_t nowMs);

  const HallState& state() const { return state_; }

  // Debounced, polarity-corrected, and mapped through hallToLid.
  bool lidOpen(uint8_t lid) const;

  // When the lid last changed to its current state, in millis().
  uint32_t lidChangedAtMs(uint8_t lid) const;

  // Raw sensor read, bypassing debounce and polarity. For the bring-up tool.
  bool rawHall(uint8_t sensor);

  // Call with every lid shut. Returns the raw mask it saw, and sets
  // hallClosedIsLow / hallInvertMask on the settings object to match, so a
  // tray wired either way ends up reading correctly.
  //
  // A mask that is neither all-low nor all-high means the sensors disagree. On
  // an intact tray that is a lid left open, a missing magnet or a dead sensor -
  // not a polarity to learn - and resolving it into an invert mask would teach
  // the box that a shut lid reads OPEN, which no self-check can catch because
  // the mask is derived from the very reading it would check. So that case is
  // only resolved when force is true; otherwise nothing is written, the debounce
  // state is left alone, and the caller is expected to refuse and say why. When
  // mixed is not null it reports the condition either way.
  uint8_t calibrateClosed(HardwareSettings* hw, uint8_t passes = 8,
                          bool force = false, bool* mixed = nullptr);

  // Reset the debounce state, e.g. straight after waking from deep sleep where
  // millis() has restarted.
  void resetDebounce(uint32_t nowMs);

 private:
  bool sensorSaysOpen(uint8_t sensor, bool rawHigh) const;

  MuxBus*                 bus_ = nullptr;
  const HardwareSettings* hw_  = nullptr;
  HallState               state_{};

  uint8_t  candidateMask_ = 0;
  uint32_t candidateSince_[kLidCount]{};
  uint32_t changedAtMs_[kLidCount]{};
};

}  // namespace ep
