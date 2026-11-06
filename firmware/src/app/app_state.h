// The one global the application shares.
//
// Single-threaded by design: everything runs out of loop(), the web handlers
// included, so there are no locks and no queues. Anything that wants work done
// sets a flag here and the main loop picks it up on the next pass.
#pragma once

#include <Arduino.h>

#include "analog_sense.h"
#include "ep_adherence.h"
#include "ep_events.h"
#include "ep_settings.h"
#include "hall_scanner.h"
#include "indicator.h"
#include "mux_bus.h"
#include "power_latch.h"

namespace ep {

constexpr const char* kFirmwareVersion = "0.1.0";

// When this image was built, from build_stamp.cpp. Not a macro here on purpose:
// __DATE__ expands per translation unit, so a macro would report whenever the
// *including* file was last compiled, which is exactly the bug this replaced.
const char* buildStamp();

// A wall-clock time this box could plausibly be running at: after the firmware
// was written, and not two decades past it.
//
// BOTH bounds matter, and the upper one was learnt the hard way. Resetting an
// ESP32-C3 over EN keeps the RTC domain alive, so time() can come back holding
// whatever the previous firmware left in it. With only a lower bound, any
// garbage that happens to be large sails through: a board on the bench came up
// reporting 2089, decided its clock was usable, and let the scheduler write
// dose records against it. A clock is either verified or plausible-and-flagged;
// it is never just "big".
constexpr uint32_t kMinPlausibleUtc = 1767225600u;  // 2026-01-01
constexpr uint32_t kMaxPlausibleUtc = 2398291200u;  // 2046-01-01

inline bool plausibleUtc(uint32_t utc) {
  return utc >= kMinPlausibleUtc && utc < kMaxPlausibleUtc;
}

enum class NetMode : uint8_t { Off = 0, Connecting, Station, AccessPoint, Failed };

struct NetStatus {
  NetMode  mode;
  char     ssid[33];
  char     ip[16];
  int8_t   rssi;
  bool     mdnsUp;
  uint8_t  failCount;
  uint32_t lastChangeMs;
};

struct TimeStatus {
  // True once SNTP has answered at least once this power cycle. Until then the
  // clock is whatever came out of NVS and events are logged with localEpoch 0.
  bool     verified;
  uint32_t lastSyncUtc;
  int32_t  tzOffsetSec;
  uint32_t nextSyncDueMs;
  // Filled in on the second and later syncs: measured RTC error against NTP.
  int32_t  measuredDriftPpm;
  uint32_t syncCount;
};

struct AppState {
  Settings     settings;
  EventLog     events;
  AdherenceLog adherence;

  MuxBus      bus;
  HallScanner halls;
  AnalogSense analog;
  Indicator   led;

  NetStatus  net;
  TimeStatus time;

  WakeReason wakeReason;
  uint32_t   bootUtc;          // best guess at the epoch when we booted, 0 if unknown
  uint32_t   stayAwakeUntilMs; // the GUI or an event postponing deep sleep

  // The local epoch this box most recently cold booted at, or 0 while no clock
  // has ever been verified. A dose that fell due before it is not ours to
  // judge - nobody was watching the box - so sweepMissed(), today() and
  // adherencePercent() skip it. Without it a battery change at 10:00 pushes
  // "the 08:00 dose was missed" to the family every single time.
  uint32_t   bootFloorLocal;

  // Requests from the web layer, serviced by the main loop.
  bool settingsDirty;
  bool rebootRequested;
  bool powerOffRequested;
  bool factoryResetRequested;
  bool forceTimeSync;

  // --- implemented in app_state.cpp ---

  // Local epoch, or 0 when the clock has never been trustworthy.
  uint32_t nowLocal() const;
  uint32_t nowUtc() const;
  bool     clockUsable() const;

  // Appends to the event log with the right timestamp and verified flag.
  void logEvent(EventType t, uint8_t lid = kNoLid, int16_t detail = 0);

  void keepAwakeFor(uint32_t seconds);
  void markSettingsDirty();

  // Pick up the boot floor that survived deep sleep, or start again from "no
  // floor". Call once in setup(), after wakeReason is known.
  void bootFloorBegin();

  // The clock has just become trustworthy, so the floor can finally be named.
  // Idempotent, and it only ever moves the floor earlier.
  void bootFloorFromClock(uint32_t localNow);

  // Human-readable one-liner for an event, used by the GUI and in pushes.
  // Writes at most n bytes including the terminator.
  void describeEvent(const Event& e, char* out, size_t n) const;
};

extern AppState g;

// True when this boot is a deep-sleep wake rather than a start: the system
// clock, RTC slow memory and every "already fired" guard carried over, and only
// millis() went back to zero.
bool wokeFromDeepSleep();

}  // namespace ep
