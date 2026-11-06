#include "hall_scanner.h"

namespace ep {
namespace {

constexpr uint8_t kAllSensors = 0x7F;  // seven sensors, bits 0..6

inline uint8_t maskOf(uint8_t n) { return static_cast<uint8_t>(1u << n); }

// Walk channels 0..6 and return a bit per sensor whose output reads high.
// Leaves the address wherever the last select() put it; the callers park.
uint8_t readRawMask(MuxBus* bus) {
  uint8_t raw = 0;
  for (uint8_t s = mux::kHall0; s <= mux::kHallLast; s++) {
    bus->select(s);
    if (bus->readHall()) raw |= maskOf(s);
  }
  return raw;
}

}  // namespace

void HallScanner::begin(MuxBus* bus, const HardwareSettings* hw) {
  bus_ = bus;
  hw_  = hw;
  state_ = HallState{};
  state_.gndRefOk = true;  // optimistic until the first scan says otherwise
  resetDebounce(millis());
}

void HallScanner::scan(uint32_t nowMs) {
  if (bus_ == nullptr || hw_ == nullptr) return;

  // changedMask only ever describes this scan, so it starts empty every time.
  state_.changedMask = 0;

  const uint8_t raw = readRawMask(bus_);
  state_.gndRefOk = bus_->groundReferenceOk();
  bus_->park();

  state_.rawMask    = raw;
  state_.lastScanMs = nowMs;

  // Sensor space -> lid space. A hallToLid that is not a permutation leaves
  // some lid untouched, which reads as closed; validate() repairs the setting.
  uint8_t target = 0;
  for (uint8_t s = 0; s < kLidCount; s++) {
    if (!sensorSaysOpen(s, (raw & maskOf(s)) != 0)) continue;
    const uint8_t lid = hw_->hallToLid[s];
    if (lid < kLidCount) target |= maskOf(lid);
  }

  // Debounce on wall time, not on call count: the main loop's pass rate changes
  // with Wi-Fi and web traffic, so a counter would mean a different filter
  // depending on how busy the box is.
  for (uint8_t lid = 0; lid < kLidCount; lid++) {
    const uint8_t m = maskOf(lid);
    const bool wanted = (target & m) != 0;

    if (wanted != ((candidateMask_ & m) != 0)) {
      candidateMask_ ^= m;
      candidateSince_[lid] = nowMs;
    }
    if (wanted == ((state_.openMask & m) != 0)) continue;

    if (static_cast<uint32_t>(nowMs - candidateSince_[lid]) >= hw_->hallDebounceMs) {
      state_.openMask ^= m;
      state_.changedMask |= m;
      changedAtMs_[lid] = nowMs;
    }
  }
}

bool HallScanner::lidOpen(uint8_t lid) const {
  if (lid >= kLidCount) return false;
  return (state_.openMask & maskOf(lid)) != 0;
}

uint32_t HallScanner::lidChangedAtMs(uint8_t lid) const {
  if (lid >= kLidCount) return 0;
  return changedAtMs_[lid];
}

bool HallScanner::rawHall(uint8_t sensor) {
  if (bus_ == nullptr || sensor > mux::kHallLast) return false;
  bus_->select(sensor);
  const bool high = bus_->readHall();
  bus_->park();
  return high;
}

uint8_t HallScanner::calibrateClosed(HardwareSettings* hw, uint8_t passes, bool force,
                                     bool* mixed) {
  if (bus_ == nullptr || hw == nullptr) return 0;
  if (passes == 0) passes = 1;

  // AND the passes together: a sensor sitting right on its switching threshold
  // flickers, and a single high reading would otherwise teach us that its
  // closed state is high. Settling on low for a flickering sensor is the
  // conservative answer because the lid really is shut while we do this.
  uint8_t mask = kAllSensors;
  for (uint8_t p = 0; p < passes; p++) {
    mask &= readRawMask(bus_);
    bus_->park();
    // Blocking is fine here: this only ever runs from an explicit operator
    // command, never from the scheduler's loop.
    if (p + 1 < passes) delay(4);
  }
  mask &= kAllSensors;

  const uint8_t highs   = static_cast<uint8_t>(__builtin_popcount(mask));
  const bool    isMixed = (highs != 0) && (highs != kLidCount);
  if (mixed != nullptr) *mixed = isMixed;
  if (isMixed && !force) {
    // The sensors disagree, so there is no polarity here to learn. Leave the
    // settings and the debounce state exactly as they were: an invert bit for
    // the odd ones out would make a shut lid read OPEN for good, every scan
    // would then report that lid open, the family would get an UnexpectedOpen
    // for it, and maybeSleep() would stop arming the GPIO wake because openMask
    // is never zero again.
    return mask;
  }
  if (highs == 0) {
    hw->hallClosedIsLow = true;
    hw->hallInvertMask  = 0;
  } else if (highs == kLidCount) {
    hw->hallClosedIsLow = false;
    hw->hallInvertMask  = 0;
  } else {
    // Mixed, and the caller insisted: mixed wiring or mixed magnet polarity. The
    // majority sets the global sense and the odd ones out get an invert bit, so
    // both halves of a genuinely half-reworked tray still read correctly.
    const bool closedIsLow = (highs * 2) < kLidCount;
    uint8_t invert = 0;
    for (uint8_t s = 0; s < kLidCount; s++) {
      if (((mask & maskOf(s)) != 0) == closedIsLow) invert |= maskOf(s);
    }
    hw->hallClosedIsLow = closedIsLow;
    hw->hallInvertMask  = invert;
  }

  // The caller promised every lid is shut, so adopt that as the debounced truth
  // instead of waiting for the next scan to walk there.
  const uint32_t now = millis();
  state_.rawMask     = mask;
  state_.openMask    = 0;
  state_.changedMask = 0;
  state_.lastScanMs  = now;
  candidateMask_     = 0;
  for (uint8_t lid = 0; lid < kLidCount; lid++) {
    candidateSince_[lid] = now;
    changedAtMs_[lid]    = now;
  }
  return mask;
}

void HallScanner::resetDebounce(uint32_t nowMs) {
  // After deep sleep millis() has restarted, so every stored timestamp is in
  // the future. Re-anchor them and treat the current debounced state as stable.
  candidateMask_     = state_.openMask;
  state_.changedMask = 0;
  for (uint8_t lid = 0; lid < kLidCount; lid++) {
    candidateSince_[lid] = nowMs;
    changedAtMs_[lid]    = nowMs;
  }
}

bool HallScanner::sensorSaysOpen(uint8_t sensor, bool rawHigh) const {
  if (hw_ == nullptr || sensor >= kLidCount) return false;
  const bool inverted = (hw_->hallInvertMask & maskOf(sensor)) != 0;
  const bool closedIsLow = hw_->hallClosedIsLow != inverted;  // XOR
  // Magnet present -> AH1912 output low. With closedIsLow, a high output means
  // no magnet over the sensor, which means the lid is off it: open.
  return rawHigh == closedIsLow;
}

}  // namespace ep
