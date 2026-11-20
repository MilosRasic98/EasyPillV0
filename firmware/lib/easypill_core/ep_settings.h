// Everything the user can configure, as plain old data.
//
// Deliberately POD with fixed-size arrays and no default member initialisers:
// the whole struct gets blobbed into NVS in one go, and `applyDefaults` is the
// single place that decides what a fresh device looks like.
#pragma once

#include "ep_types.h"

namespace ep {

struct Pill {
  char    name[kNameLen];  // "Metformin"
  char    dose[kDoseLen];  // "500 mg"
  uint8_t count;           // how many of this pill in this dose
};

struct Dose {
  bool     enabled;
  uint16_t minute;             // 0..1439, local wall clock
  char     label[kLabelLen];   // "Morning"
  uint8_t  pillCount;
  Pill     pills[kMaxPillsPerDose];
};

struct LidSchedule {
  char    name[kLabelLen];   // "Monday", or whatever the user renamed it to
  uint8_t doseCount;         // how many entries of doses[] are in use
  Dose    doses[kMaxDosesPerLid];
};

struct Schedule {
  LidMode  mode;
  uint32_t sequentialAnchorDay;  // dayIndex that maps to lid 0 in Sequential mode
  LidSchedule lids[kLidCount];

  // An open at time T counts for a dose at time D when
  //   D - windowBeforeMin <= T <= D + windowAfterMin + missedGraceMin
  // The grace is inside the match window so that the instant an open stops
  // counting is the instant sweepMissed starts: see matchOpen().
  uint16_t windowBeforeMin;
  uint16_t windowAfterMin;
  // How long after the window closes before the dose is declared missed.
  uint16_t missedGraceMin;
  // Nudge this many minutes before a dose. 0 disables the reminder.
  uint16_t remindBeforeMin;
};

struct WifiSettings {
  char ssid[33];
  char pass[65];
  char hostname[32];     // mDNS name, so http://<hostname>.local
  bool apFallback;       // start a setup AP when the station will not join
  char apPassword[65];   // at least 8 chars, or the AP comes up open
};

struct TimeSettings {
  char ntp1[48];
  char ntp2[48];
  char tz[64];           // POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
  // Measured RTC error in parts per million, signed. Learned by comparing
  // elapsed RTC seconds against elapsed NTP seconds across two syncs and used
  // to correct sleep durations.
  int32_t  driftPpm;
  uint32_t resyncIntervalSec;
};

struct NotifySettings {
  // ntfy.sh (or a self-hosted ntfy) is the easy path to a phone or an iPad:
  // install the app, subscribe to a topic, done. No account, no keys.
  bool ntfyEnabled;
  char ntfyServer[64];   // "https://ntfy.sh"
  char ntfyTopic[48];
  char ntfyToken[72];    // optional bearer token for a protected topic

  bool pushoverEnabled;
  char pushoverToken[40];
  char pushoverUser[40];

  // Anything that accepts a JSON POST: Home Assistant, Discord, Slack, n8n.
  bool webhookEnabled;
  char webhookUrl[160];

  // Which events are worth a push.
  bool onLidOpen;
  bool onDoseTaken;
  bool onDoseMissed;
  bool onDoseDue;
  bool onUnexpectedOpen;
  bool onLowBattery;
  bool onBoot;
  bool onDailySummary;
  uint16_t dailySummaryMinute;

  // Skip pushes between these two minute-of-day values. Equal values = off.
  uint16_t quietFromMinute;
  uint16_t quietToMinute;
};

enum class SleepPolicy : uint8_t {
  AlwaysOn   = 0,  // never deep sleep. Right for the bench and for filming.
  Smart      = 1,  // stay awake on USB, sleep on battery between events
  Aggressive = 2,  // sleep as soon as there is nothing to do, even on USB
};

struct PowerSettings {
  SleepPolicy policy;
  uint16_t awakeAfterEventSec;  // keep the GUI reachable this long after an event
  uint16_t wakeIntervalSec;     // timer wake period while asleep
  uint16_t lowBatteryMv;        // warn
  uint16_t criticalBatteryMv;   // drop the latch; 3300 by default, not 2500
  bool     buzzerOnReminder;
  bool     ledOnReminder;
};

struct HardwareSettings {
  // AH1912 pulls its output low in the presence of a magnet, and the magnet is
  // meant to sit over the sensor while the lid is shut -> low means closed.
  // Flip this if the magnet approaches on opening instead.
  bool    hallClosedIsLow;
  uint8_t hallInvertMask;      // per-sensor override, bit n inverts hall n
  uint8_t hallToLid[kLidCount];  // hall sensor index -> lid index

  // Which SIG_OUT mux channel drives which LED colour. Not knowable from the
  // schematic; the bring-up firmware's `l` command identifies them.
  uint8_t ledChannelRed;
  uint8_t ledChannelGreen;
  uint8_t ledChannelBlue;
  uint8_t ledBrightness;       // 0..255, applied to all three

  uint8_t  buzzerChannel;
  uint16_t buzzerToneHz;       // magnetic buzzers need a square wave near resonance
  bool     buzzerSelfDrive;    // true for a self-oscillating piezo: just DC

  // adcMv * numerator / denominator = real millivolts at the source.
  uint16_t vbatDivNum, vbatDivDen;
  uint16_t railDivNum, railDivDen;
  uint16_t buttonDivNum, buttonDivDen;
  uint16_t buttonThresholdMv;  // above this, the button counts as pressed
  uint16_t statThresholdMv;

  uint16_t hallDebounceMs;
  uint16_t buttonDebounceMs;
};

struct UiSettings {
  char deviceName[32];    // shown in the GUI header and in notifications
  char patientName[32];   // "Grandma"
  bool showAdvanced;
};

constexpr uint16_t kSettingsVersion = 1;

struct Settings {
  uint16_t         version;
  UiSettings       ui;
  WifiSettings     wifi;
  TimeSettings     time;
  NotifySettings   notify;
  PowerSettings    power;
  HardwareSettings hw;
  Schedule         schedule;
};

// A brand-new device: empty Wi-Fi, Europe/Belgrade, 08:00 and 20:00 on all
// seven days so the GUI has something to show, notifications off.
void applyDefaults(Settings& s);

// Clamps every field into range and repairs obviously broken data (a doseCount
// past the array, a minute above 1439, a hallToLid that is not a permutation).
// Returns true if it had to change anything, which the caller should treat as
// "re-save me".
bool validate(Settings& s);

// Put the same dose at the same time on several lids at once. `lidMask` bit n
// selects lid n. If `doseIndex` is 0xFF the dose is appended where there is
// room, otherwise that slot is overwritten. Returns the number of lids changed.
uint8_t applyDoseToLids(Schedule& sch, uint8_t lidMask, uint8_t doseIndex,
                        const Dose& dose);

// Remove a dose slot from every selected lid, closing the gap.
uint8_t removeDoseFromLids(Schedule& sch, uint8_t lidMask, uint8_t doseIndex);

// Convenience for the GUI: the number of distinct pill names in the schedule.
uint8_t distinctPillNames(const Schedule& sch, char out[][kNameLen], uint8_t maxOut);

}  // namespace ep
