// Shared vocabulary for the EasyPill core. Pure C++17: nothing in easypill_core
// may include Arduino.h, so all of it builds and tests on the host too.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ep {

constexpr uint8_t kLidCount        = 7;  // one hall sensor per lid
constexpr uint8_t kMaxDosesPerLid  = 4;  // e.g. morning / noon / evening / night
constexpr uint8_t kMaxPillsPerDose = 4;

constexpr size_t kNameLen  = 24;
constexpr size_t kDoseLen  = 16;
constexpr size_t kLabelLen = 16;

// How the seven lids relate to the calendar.
enum class LidMode : uint8_t {
  // Lid 0 is Monday ... lid 6 is Sunday, every week. The default, and the only
  // mode in which "did they open the right lid?" is a meaningful question.
  Weekday = 0,
  // Lid 0 is day 0 of a rolling 7-day tray starting at a chosen date. Useful if
  // the box is refilled on an arbitrary day.
  Sequential = 1,
};

enum class DoseState : uint8_t {
  Pending = 0,  // in the future, or inside its window and not yet opened
  Taken   = 1,  // opened inside the on-time window
  Late    = 2,  // opened after the window closed but before it was swept
  Early   = 3,  // opened before the window opened
  Missed  = 4,  // window closed plus grace, never opened
  Skipped = 5,  // a human marked it skipped from the GUI
};

enum class EventType : uint8_t {
  Boot = 0,
  LidOpened,
  LidClosed,
  DoseTaken,
  DoseMissed,
  DoseDue,
  DoseReminder,
  UnexpectedOpen,   // a lid with nothing scheduled anywhere near now
  WrongDayOpen,     // a lid opened on a day that is not its day
  ButtonPress,
  ButtonHold,
  ChargerIn,
  ChargerOut,
  BatteryLow,
  BatteryCritical,
  TimeSynced,
  WifiUp,
  WifiDown,
  SleepEnter,
  SleepExit,
  ConfigChanged,
  SensorFault,      // the channel-7 ground reference did not read low
  Count,
};

// Stable machine-readable names; these go into the JSON API, so do not reword
// them casually. Returns "unknown" for out-of-range values.
const char* toString(EventType t);
const char* toString(DoseState s);
const char* toString(LidMode m);

bool eventTypeFromString(const char* s, EventType* out);
bool doseStateFromString(const char* s, DoseState* out);

// "Monday" ... "Sunday" for weekday mode, "Day 1" ... "Day 7" for sequential.
const char* defaultLidName(uint8_t lid, LidMode mode);

// True for the states that mean "the lid actually got opened for this dose".
bool isTakenState(DoseState s);

}  // namespace ep
