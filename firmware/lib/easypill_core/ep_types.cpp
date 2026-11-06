#include "ep_types.h"

#include <string.h>

namespace ep {

// The switches below deliberately have no `default:` so that adding an
// enumerator breaks the build here instead of silently serialising as
// "unknown" into the JSON API.

const char* toString(EventType t) {
  switch (t) {
    case EventType::Boot:            return "boot";
    case EventType::LidOpened:       return "lidOpened";
    case EventType::LidClosed:       return "lidClosed";
    case EventType::DoseTaken:       return "doseTaken";
    case EventType::DoseMissed:      return "doseMissed";
    case EventType::DoseDue:         return "doseDue";
    case EventType::DoseReminder:    return "doseReminder";
    case EventType::UnexpectedOpen:  return "unexpectedOpen";
    case EventType::WrongDayOpen:    return "wrongDayOpen";
    case EventType::ButtonPress:     return "buttonPress";
    case EventType::ButtonHold:      return "buttonHold";
    case EventType::ChargerIn:       return "chargerIn";
    case EventType::ChargerOut:      return "chargerOut";
    case EventType::BatteryLow:      return "batteryLow";
    case EventType::BatteryCritical: return "batteryCritical";
    case EventType::TimeSynced:      return "timeSynced";
    case EventType::WifiUp:          return "wifiUp";
    case EventType::WifiDown:        return "wifiDown";
    case EventType::SleepEnter:      return "sleepEnter";
    case EventType::SleepExit:       return "sleepExit";
    case EventType::ConfigChanged:   return "configChanged";
    case EventType::SensorFault:     return "sensorFault";
    case EventType::Count:           break;
  }
  return "unknown";
}

const char* toString(DoseState s) {
  switch (s) {
    case DoseState::Pending: return "pending";
    case DoseState::Taken:   return "taken";
    case DoseState::Late:    return "late";
    case DoseState::Early:   return "early";
    case DoseState::Missed:  return "missed";
    case DoseState::Skipped: return "skipped";
  }
  return "unknown";
}

const char* toString(LidMode m) {
  switch (m) {
    case LidMode::Weekday:    return "weekday";
    case LidMode::Sequential: return "sequential";
  }
  return "unknown";
}

bool eventTypeFromString(const char* s, EventType* out) {
  if (!s || !out) return false;
  for (uint8_t i = 0; i < static_cast<uint8_t>(EventType::Count); ++i) {
    EventType t = static_cast<EventType>(i);
    if (strcmp(s, toString(t)) == 0) {
      *out = t;
      return true;
    }
  }
  return false;
}

bool doseStateFromString(const char* s, DoseState* out) {
  if (!s || !out) return false;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(DoseState::Skipped); ++i) {
    DoseState d = static_cast<DoseState>(i);
    if (strcmp(s, toString(d)) == 0) {
      *out = d;
      return true;
    }
  }
  return false;
}

const char* defaultLidName(uint8_t lid, LidMode mode) {
  static const char* const kWeekdays[kLidCount] = {"Monday",   "Tuesday", "Wednesday",
                                                   "Thursday", "Friday",  "Saturday",
                                                   "Sunday"};
  static const char* const kDays[kLidCount] = {"Day 1", "Day 2", "Day 3", "Day 4",
                                               "Day 5", "Day 6", "Day 7"};
  if (lid >= kLidCount) return "unknown";
  return mode == LidMode::Sequential ? kDays[lid] : kWeekdays[lid];
}

bool isTakenState(DoseState s) {
  // Skipped means a human decided the dose did not have to be taken, so the lid
  // was never lifted for it. Everything else here involves an actual open.
  return s == DoseState::Taken || s == DoseState::Late || s == DoseState::Early;
}

}  // namespace ep
