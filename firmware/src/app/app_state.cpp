// The one global, plus the handful of helpers everything else leans on.
#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "ep_schedule.h"
#include "ep_timecalc.h"
#include "timekeeper.h"

namespace ep {

AppState g;

namespace {

// Below this the chip is not telling us the time, it is telling us how long ago
// it was reset: 2026-01-01. A clock seeded from NVS is above it, a cold boot
// with nothing stored is not, which is the whole of the distinction clockUsable()
// has to make without asking the network anything.

// The boot floor has to survive deep sleep - a dose that falls while the box is
// asleep is still the box's business, and the box woke up for it - but not a
// power cycle, because the whole point of it is to stop a box that has just been
// plugged in reporting doses from before it was. RTC slow memory draws exactly
// that line: it is carried through a deep-sleep wake and reloaded from the image
// on anything else.
RTC_DATA_ATTR uint32_t rtcBootFloorLocal = 0;

const char* lidNameOf(const Settings& s, uint8_t lid) {
  if (lid >= kLidCount) return "the";
  const char* n = s.schedule.lids[lid].name;
  return n[0] ? n : defaultLidName(lid, s.schedule.mode);
}

// Several event types carry the dose's minute-of-day in `detail` so a sentence
// can name the dose without going back to the schedule, which may have been
// edited since.
void hhmmFromDetail(int16_t detail, char* out, size_t n) {
  if (detail < 0 || detail >= (int16_t)kMinPerDay) {
    snprintf(out, n, "scheduled");
    return;
  }
  formatHhMm((uint16_t)detail, out, n);
}

}  // namespace

uint32_t AppState::nowUtc() const {
  const uint32_t utc = timeNowUtc();
  return plausibleUtc(utc) ? utc : 0;
}

uint32_t AppState::nowLocal() const {
  const uint32_t utc = nowUtc();
  if (utc == 0) return 0;
  return (uint32_t)((int64_t)utc + (int64_t)time.tzOffsetSec);
}

bool AppState::clockUsable() const { return nowUtc() != 0; }

void AppState::logEvent(EventType t, uint8_t lid, int16_t detail) {
  // Until SNTP has answered once the timestamp stays 0 even when the NVS seed
  // looks plausible: backfillTimes() will give these events a real time from
  // their uptime, and that is strictly better than committing to a guess.
  const uint32_t stamp = time.verified ? nowLocal() : 0;
  events.push(t, stamp, millis(), time.verified, lid, detail);
}

void AppState::keepAwakeFor(uint32_t seconds) {
  const uint32_t until = millis() + seconds * 1000u;
  if ((int32_t)(until - stayAwakeUntilMs) > 0) stayAwakeUntilMs = until;
}

void AppState::markSettingsDirty() { settingsDirty = true; }

void AppState::bootFloorBegin() {
  if (!wokeFromDeepSleep()) rtcBootFloorLocal = 0;
  bootFloorLocal = rtcBootFloorLocal;
}

void AppState::bootFloorFromClock(uint32_t localNow) {
  if (localNow == 0) return;
  // Same rule as the simulator's first sync: name the floor the first time we
  // can, and never leave it in the future - a timezone change, or a seeded clock
  // that was running ahead of the truth, can put it there.
  if (bootFloorLocal != 0 && bootFloorLocal <= localNow) return;
  bootFloorLocal    = localNow;
  rtcBootFloorLocal = localNow;
}

bool wokeFromDeepSleep() {
  return g.wakeReason == WakeReason::Timer || g.wakeReason == WakeReason::WakeUpPin;
}

void AppState::describeEvent(const Event& e, char* out, size_t n) const {
  if (out == nullptr || n == 0) return;
  out[0] = '\0';

  const char* lid = lidNameOf(settings, e.lid);
  char when[12];  // "HH:MM", or the word "scheduled" when the detail field is empty

  switch (e.type) {
    case EventType::Boot:
      snprintf(out, n, "Started up (%s)", toString((WakeReason)(uint8_t)e.detail));
      break;
    case EventType::LidOpened:
      snprintf(out, n, "%s lid opened", lid);
      break;
    case EventType::LidClosed:
      snprintf(out, n, "%s lid closed", lid);
      break;
    case EventType::DoseTaken: {
      // detail is the signed minutes between the open and the dose time, so the
      // dose itself can be named by walking back from the event timestamp.
      if (e.localEpoch != 0) {
        const uint32_t scheduled = (uint32_t)((int64_t)e.localEpoch - (int64_t)e.detail * 60);
        formatHhMm(minuteOfDay(scheduled), when, sizeof when);
      } else {
        snprintf(when, sizeof when, "due");
      }
      if (e.detail == 0) {
        snprintf(out, n, "%s dose taken on time (%s)", lid, when);
      } else if (e.detail < 0) {
        snprintf(out, n, "%s dose taken %d min early (%s)", lid, -e.detail, when);
      } else {
        snprintf(out, n, "%s dose taken %d min late (%s)", lid, e.detail, when);
      }
      break;
    }
    case EventType::DoseMissed:
      hhmmFromDetail(e.detail, when, sizeof when);
      snprintf(out, n, "%s %s dose missed", lid, when);
      break;
    case EventType::DoseDue:
      hhmmFromDetail(e.detail, when, sizeof when);
      snprintf(out, n, "%s %s dose due now", lid, when);
      break;
    case EventType::DoseReminder:
      snprintf(out, n, "%s dose in %d min", lid, (int)e.detail);
      break;
    case EventType::UnexpectedOpen:
      snprintf(out, n, "%s lid opened with nothing due", lid);
      break;
    case EventType::WrongDayOpen:
      snprintf(out, n, "%s lid opened on the wrong day", lid);
      break;
    case EventType::ButtonPress:
      snprintf(out, n, "Button pressed");
      break;
    case EventType::ButtonHold:
      snprintf(out, n, "Button held");
      break;
    case EventType::ChargerIn:
      snprintf(out, n, "Charger connected");
      break;
    case EventType::ChargerOut:
      snprintf(out, n, "Charger removed");
      break;
    case EventType::BatteryLow:
      snprintf(out, n, "Battery low, %d mV", (int)e.detail);
      break;
    case EventType::BatteryCritical:
      snprintf(out, n, "Battery critical at %d mV, shutting down", (int)e.detail);
      break;
    case EventType::TimeSynced:
      snprintf(out, n, "Clock synced, drift %d ppm", (int)e.detail);
      break;
    case EventType::WifiUp:
      snprintf(out, n, "Wi-Fi connected, %d dBm", (int)e.detail);
      break;
    case EventType::WifiDown:
      snprintf(out, n, "Wi-Fi lost");
      break;
    case EventType::SleepEnter:
      snprintf(out, n, "Sleeping for %d s", (int)e.detail);
      break;
    case EventType::SleepExit:
      snprintf(out, n, "Woke up");
      break;
    case EventType::ConfigChanged:
      snprintf(out, n, "Settings changed");
      break;
    case EventType::SensorFault:
      snprintf(out, n, "Sensor fault: mux ground reference read high");
      break;
    default:
      snprintf(out, n, "%s", toString(e.type));
      break;
  }
}

}  // namespace ep
