#include "ep_settings.h"

#include <stdio.h>
#include <string.h>

#include "ep_timecalc.h"

namespace ep {
namespace {

// easypill_core must stay free of Arduino.h, so it cannot include
// include/board_pins.h. The hardware-derived numbers below are copied from
// there and from docs/HARDWARE.md; if the board is reworked, that document
// changes first, then board_pins.h, then these.
constexpr uint8_t  kDefaultLedChannelRed   = 2;    // U5 ch2 -> Q1 -> CTL1 pin 2
constexpr uint8_t  kDefaultLedChannelGreen = 1;    // U5 ch1 -> Q2 -> CTL1 pin 3
constexpr uint8_t  kDefaultLedChannelBlue  = 0;    // U5 ch0 -> Q3 -> CTL1 pin 4
constexpr uint8_t  kDefaultBuzzerChannel   = 3;    // U5 ch3 -> Q4 -> buzzer U6
constexpr uint8_t  kMaxOutputChannel       = 3;    // U5 drives four FETs, 0..3
// Only 0..2 reach CTL1. Channel 3 is Q4, which has the buzzer on it, so a
// colour there would PWM the buzzer instead of lighting the LED.
constexpr uint8_t  kMaxLedChannel          = 2;
constexpr uint16_t kDefaultBuzzerToneHz    = 2700;
// Hall sensor index -> lid index, measured on the V0 board one sensor at a time
// (docs/HARDWARE.md 3). The sensor row runs opposite to the days: sensor 0 is
// Sunday, sensor 6 is Monday. This is mechanical, not electrical -- the mux
// channel order in board_pins.h is unaffected -- so it changes if the PCB is
// mounted differently, and tools/mapdays.py re-derives it over HTTP.
constexpr uint8_t  kDefaultHallToLid[kLidCount] = {6, 5, 4, 3, 2, 1, 0};
constexpr uint16_t kVbatDivNum = 175, kVbatDivDen = 100;  // 75k/100k divider
constexpr uint16_t kRailDivNum = 175, kRailDivDen = 75;   // 100k/75k divider
constexpr uint16_t kButtonDivNum = 1, kButtonDivDen = 1;  // clamped, not divided

constexpr uint16_t kDefaultMorningMinute = 8 * 60;
constexpr uint16_t kDefaultEveningMinute = 20 * 60;

static_assert(sizeof(bool) == 1, "validate() normalises bool bytes in place");

void setStr(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  memset(dst, 0, n);
  if (!src) return;
  size_t len = strlen(src);
  if (len > n - 1) len = n - 1;
  memcpy(dst, src, len);
}

// A string arriving from NVS or from a half-written flash page can be missing
// its terminator, and every reader here uses strlen/snprintf.
bool fixTerm(char* buf, size_t n) {
  if (!buf || n == 0) return false;
  if (memchr(buf, '\0', n) != nullptr) return false;
  buf[n - 1] = '\0';
  return true;
}

// Settings come back as a raw blob, so a bool byte can hold a value that is
// neither 0 nor 1. Reading such a bool directly is undefined, hence memcpy.
bool fixBool(bool& b) {
  unsigned char raw;
  memcpy(&raw, &b, 1);
  if (raw <= 1) return false;
  raw = 1;
  memcpy(&b, &raw, 1);
  return true;
}

template <typename T>
bool clampTo(T& v, T lo, T hi) {
  if (v < lo) {
    v = lo;
    return true;
  }
  if (v > hi) {
    v = hi;
    return true;
  }
  return false;
}

void defaultDose(Dose& d, uint16_t minute, const char* label) {
  memset(&d, 0, sizeof(d));
  d.enabled   = true;
  d.minute    = minute;
  d.pillCount = 1;
  setStr(d.label, kLabelLen, label);
  // A placeholder pill, so a fresh device shows a populated schedule instead of
  // seven empty days that look like a bug.
  setStr(d.pills[0].name, kNameLen, "Pill");
  d.pills[0].count = 1;
}

// Keeps the dose slot that applyDoseToLids just wrote inside the limits the
// rest of the core assumes, without running validate() over the whole tree.
void sanitiseDose(Dose& d) {
  if (d.minute >= kMinPerDay) d.minute = kMinPerDay - 1;
  if (d.pillCount > kMaxPillsPerDose) d.pillCount = kMaxPillsPerDose;
  fixTerm(d.label, kLabelLen);
  fixBool(d.enabled);
  for (uint8_t p = 0; p < d.pillCount; ++p) {
    fixTerm(d.pills[p].name, kNameLen);
    fixTerm(d.pills[p].dose, kDoseLen);
    if (d.pills[p].count == 0) d.pills[p].count = 1;
  }
}

}  // namespace

void applyDefaults(Settings& s) {
  // One memset up front: the struct is blobbed into NVS whole, so the slots
  // past doseCount/pillCount have to be deterministic too.
  memset(&s, 0, sizeof(s));

  s.version = kSettingsVersion;

  setStr(s.ui.deviceName, sizeof(s.ui.deviceName), "EasyPill");
  s.ui.patientName[0] = '\0';  // the relatives fill this in; never guess a name
  s.ui.showAdvanced   = false;

  // Empty credentials on purpose: the AP fallback is the way in on first boot.
  s.wifi.ssid[0] = '\0';
  s.wifi.pass[0] = '\0';
  setStr(s.wifi.hostname, sizeof(s.wifi.hostname), "easypill");
  s.wifi.apFallback     = true;
  s.wifi.apPassword[0]  = '\0';

  setStr(s.time.ntp1, sizeof(s.time.ntp1), "pool.ntp.org");
  setStr(s.time.ntp2, sizeof(s.time.ntp2), "time.google.com");
  setStr(s.time.tz, sizeof(s.time.tz), "CET-1CEST,M3.5.0,M10.5.0/3");
  s.time.driftPpm = 0;  // unknown until two syncs have been compared
  // There is no RTC and RTC_SLOW_CLK is the internal RC oscillator, so the
  // clock has to be pulled back every few hours. docs/HARDWARE.md section 8.
  s.time.resyncIntervalSec = 6 * kSecPerHour;

  s.notify.ntfyEnabled = false;
  setStr(s.notify.ntfyServer, sizeof(s.notify.ntfyServer), "https://ntfy.sh");
  s.notify.pushoverEnabled = false;
  s.notify.webhookEnabled  = false;
  // Transports are off, but the per-event choices are already sensible so that
  // filling in one ntfy topic is the whole setup.
  s.notify.onLidOpen        = true;
  s.notify.onDoseTaken      = true;
  s.notify.onDoseMissed     = true;
  s.notify.onDoseDue        = false;
  s.notify.onUnexpectedOpen = true;
  s.notify.onLowBattery     = true;
  s.notify.onBoot           = false;
  s.notify.onDailySummary   = false;
  s.notify.dailySummaryMinute = 21 * 60;
  s.notify.quietFromMinute    = 0;  // equal values mean no quiet hours
  s.notify.quietToMinute      = 0;

  s.power.policy             = SleepPolicy::Smart;
  s.power.awakeAfterEventSec = 300;
  s.power.wakeIntervalSec    = 900;
  s.power.lowBatteryMv       = 3500;
  s.power.criticalBatteryMv  = 3300;  // drop the latch here, not at the cell's 2.5 V
  s.power.buzzerOnReminder   = true;
  s.power.ledOnReminder      = true;

  s.hw.hallClosedIsLow = true;  // AH1912 pulls low with the magnet over it
  s.hw.hallInvertMask  = 0;
  for (uint8_t i = 0; i < kLidCount; ++i) s.hw.hallToLid[i] = kDefaultHallToLid[i];
  s.hw.ledChannelRed   = kDefaultLedChannelRed;
  s.hw.ledChannelGreen = kDefaultLedChannelGreen;
  s.hw.ledChannelBlue  = kDefaultLedChannelBlue;
  s.hw.ledBrightness   = 120;
  s.hw.buzzerChannel   = kDefaultBuzzerChannel;
  s.hw.buzzerToneHz    = kDefaultBuzzerToneHz;
  s.hw.buzzerSelfDrive = false;  // assume a magnetic buzzer; it needs the tone
  s.hw.vbatDivNum      = kVbatDivNum;
  s.hw.vbatDivDen      = kVbatDivDen;
  s.hw.railDivNum      = kRailDivNum;
  s.hw.railDivDen      = kRailDivDen;
  s.hw.buttonDivNum    = kButtonDivNum;
  s.hw.buttonDivDen    = kButtonDivDen;
  s.hw.buttonThresholdMv = 1500;
  s.hw.statThresholdMv   = 1500;
  s.hw.hallDebounceMs    = 60;
  s.hw.buttonDebounceMs  = 80;

  s.schedule.mode                = LidMode::Weekday;
  s.schedule.sequentialAnchorDay = 0;
  s.schedule.windowBeforeMin     = 60;
  s.schedule.windowAfterMin      = 120;
  s.schedule.missedGraceMin      = 15;
  s.schedule.remindBeforeMin     = 0;
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    LidSchedule& ls = s.schedule.lids[lid];
    setStr(ls.name, kLabelLen, defaultLidName(lid, s.schedule.mode));
    ls.doseCount = 2;
    defaultDose(ls.doses[0], kDefaultMorningMinute, "Morning");
    defaultDose(ls.doses[1], kDefaultEveningMinute, "Evening");
  }
}

bool validate(Settings& s) {
  bool ch = false;

  // Only obvious garbage gets stamped. A lower-but-plausible version is left
  // visible so the store layer can decide whether to migrate or re-default it.
  if (s.version == 0 || s.version > kSettingsVersion) {
    s.version = kSettingsVersion;
    ch = true;
  }

  ch |= fixTerm(s.ui.deviceName, sizeof(s.ui.deviceName));
  ch |= fixTerm(s.ui.patientName, sizeof(s.ui.patientName));
  ch |= fixBool(s.ui.showAdvanced);
  if (s.ui.deviceName[0] == '\0') {
    setStr(s.ui.deviceName, sizeof(s.ui.deviceName), "EasyPill");
    ch = true;
  }

  ch |= fixTerm(s.wifi.ssid, sizeof(s.wifi.ssid));
  ch |= fixTerm(s.wifi.pass, sizeof(s.wifi.pass));
  ch |= fixTerm(s.wifi.hostname, sizeof(s.wifi.hostname));
  ch |= fixTerm(s.wifi.apPassword, sizeof(s.wifi.apPassword));
  ch |= fixBool(s.wifi.apFallback);
  if (s.wifi.hostname[0] == '\0') {
    setStr(s.wifi.hostname, sizeof(s.wifi.hostname), "easypill");
    ch = true;  // an empty hostname takes mDNS down with it
  }

  ch |= fixTerm(s.time.ntp1, sizeof(s.time.ntp1));
  ch |= fixTerm(s.time.ntp2, sizeof(s.time.ntp2));
  ch |= fixTerm(s.time.tz, sizeof(s.time.tz));
  if (s.time.tz[0] == '\0') {
    setStr(s.time.tz, sizeof(s.time.tz), "CET-1CEST,M3.5.0,M10.5.0/3");
    ch = true;  // an empty TZ silently turns local epoch into UTC
  }
  if (s.time.ntp1[0] == '\0' && s.time.ntp2[0] != '\0') {
    // Keep the primary slot filled: the sync path tries ntp1 first.
    setStr(s.time.ntp1, sizeof(s.time.ntp1), s.time.ntp2);
    s.time.ntp2[0] = '\0';
    ch = true;
  }
  if (s.time.ntp1[0] == '\0') {
    setStr(s.time.ntp1, sizeof(s.time.ntp1), "pool.ntp.org");
    ch = true;  // without a server the clock never becomes verified at all
  }
  // The RC oscillator is specified at a few per cent. Anything past 10 % is a
  // bad measurement, and it would be applied to every sleep duration.
  ch |= clampTo<int32_t>(s.time.driftPpm, -100000, 100000);
  // Floor stops us hammering a public NTP pool; ceiling keeps drift bounded.
  ch |= clampTo<uint32_t>(s.time.resyncIntervalSec, 10 * kSecPerMin, 24 * kSecPerHour);

  ch |= fixTerm(s.notify.ntfyServer, sizeof(s.notify.ntfyServer));
  ch |= fixTerm(s.notify.ntfyTopic, sizeof(s.notify.ntfyTopic));
  ch |= fixTerm(s.notify.ntfyToken, sizeof(s.notify.ntfyToken));
  ch |= fixTerm(s.notify.pushoverToken, sizeof(s.notify.pushoverToken));
  ch |= fixTerm(s.notify.pushoverUser, sizeof(s.notify.pushoverUser));
  ch |= fixTerm(s.notify.webhookUrl, sizeof(s.notify.webhookUrl));
  ch |= fixBool(s.notify.ntfyEnabled);
  ch |= fixBool(s.notify.pushoverEnabled);
  ch |= fixBool(s.notify.webhookEnabled);
  ch |= fixBool(s.notify.onLidOpen);
  ch |= fixBool(s.notify.onDoseTaken);
  ch |= fixBool(s.notify.onDoseMissed);
  ch |= fixBool(s.notify.onDoseDue);
  ch |= fixBool(s.notify.onUnexpectedOpen);
  ch |= fixBool(s.notify.onLowBattery);
  ch |= fixBool(s.notify.onBoot);
  ch |= fixBool(s.notify.onDailySummary);
  if (s.notify.ntfyEnabled && s.notify.ntfyServer[0] == '\0') {
    setStr(s.notify.ntfyServer, sizeof(s.notify.ntfyServer), "https://ntfy.sh");
    ch = true;
  }
  ch |= clampTo<uint16_t>(s.notify.dailySummaryMinute, 0, kMinPerDay - 1);
  ch |= clampTo<uint16_t>(s.notify.quietFromMinute, 0, kMinPerDay - 1);
  ch |= clampTo<uint16_t>(s.notify.quietToMinute, 0, kMinPerDay - 1);

  if (static_cast<uint8_t>(s.power.policy) > static_cast<uint8_t>(SleepPolicy::Aggressive)) {
    s.power.policy = SleepPolicy::Smart;
    ch = true;
  }
  ch |= fixBool(s.power.buzzerOnReminder);
  ch |= fixBool(s.power.ledOnReminder);
  ch |= clampTo<uint16_t>(s.power.awakeAfterEventSec, 10, 3600);
  // Below half a minute the chip spends more energy waking than sleeping.
  ch |= clampTo<uint16_t>(s.power.wakeIntervalSec, 30, 21600);
  ch |= clampTo<uint16_t>(s.power.lowBatteryMv, 3000, 4200);
  ch |= clampTo<uint16_t>(s.power.criticalBatteryMv, 2800, 4200);
  if (s.power.criticalBatteryMv >= s.power.lowBatteryMv) {
    // Critical has to trip first or the warning never fires. lowBatteryMv is
    // already >= 3000 here, so this cannot underflow past the 2800 floor.
    s.power.criticalBatteryMv = static_cast<uint16_t>(s.power.lowBatteryMv - 100);
    ch = true;
  }

  ch |= fixBool(s.hw.hallClosedIsLow);
  ch |= fixBool(s.hw.buzzerSelfDrive);
  bool permutation = true;
  uint8_t seen = 0;
  for (uint8_t i = 0; i < kLidCount; ++i) {
    uint8_t lid = s.hw.hallToLid[i];
    if (lid >= kLidCount || (seen & (1u << lid))) {
      permutation = false;
      break;
    }
    seen = static_cast<uint8_t>(seen | (1u << lid));
  }
  if (!permutation) {
    // Anything but a bijection means some lid is unreachable and another is
    // reported twice. Repair to this board's real wiring rather than to
    // straight-through: identity is not neutral here, it is simply wrong (see
    // kDefaultHallToLid), and a box that silently reports Monday's lid as
    // Sunday's is worse than one that refuses to start.
    for (uint8_t i = 0; i < kLidCount; ++i) s.hw.hallToLid[i] = kDefaultHallToLid[i];
    ch = true;
  }
  // The buzzer gate is fixed by the board -- Q4 is the only FET with a buzzer
  // on it -- so the only legal channel is the last one. Bounding it before the
  // colours keeps the two sets disjoint by construction, which is why there is
  // no cross-field repair below: resetting the colours to 2/1/0 could not move
  // a buzzer parked on an LED gate, so such a repair would never converge.
  ch |= clampTo<uint8_t>(s.hw.buzzerChannel,
                         static_cast<uint8_t>(kMaxLedChannel + 1), kMaxOutputChannel);
  if (s.hw.ledChannelRed > kMaxLedChannel || s.hw.ledChannelGreen > kMaxLedChannel ||
      s.hw.ledChannelBlue > kMaxLedChannel ||
      s.hw.ledChannelRed == s.hw.ledChannelGreen ||
      s.hw.ledChannelRed == s.hw.ledChannelBlue ||
      s.hw.ledChannelGreen == s.hw.ledChannelBlue) {
    s.hw.ledChannelRed   = kDefaultLedChannelRed;
    s.hw.ledChannelGreen = kDefaultLedChannelGreen;
    s.hw.ledChannelBlue  = kDefaultLedChannelBlue;
    ch = true;
  }
  if (s.hw.buzzerToneHz < 100 || s.hw.buzzerToneHz > 20000) {
    s.hw.buzzerToneHz = kDefaultBuzzerToneHz;  // LEDC needs a real frequency
    ch = true;
  }
  // Every divider is used as a multiply-then-divide, so a zero denominator is a
  // division by zero in the analog path.
  if (s.hw.vbatDivNum == 0 || s.hw.vbatDivDen == 0) {
    s.hw.vbatDivNum = kVbatDivNum;
    s.hw.vbatDivDen = kVbatDivDen;
    ch = true;
  }
  if (s.hw.railDivNum == 0 || s.hw.railDivDen == 0) {
    s.hw.railDivNum = kRailDivNum;
    s.hw.railDivDen = kRailDivDen;
    ch = true;
  }
  if (s.hw.buttonDivNum == 0 || s.hw.buttonDivDen == 0) {
    s.hw.buttonDivNum = kButtonDivNum;
    s.hw.buttonDivDen = kButtonDivDen;
    ch = true;
  }
  ch |= clampTo<uint16_t>(s.hw.buttonThresholdMv, 100, 5000);
  ch |= clampTo<uint16_t>(s.hw.statThresholdMv, 100, 5000);
  ch |= clampTo<uint16_t>(s.hw.hallDebounceMs, 5, 2000);
  ch |= clampTo<uint16_t>(s.hw.buttonDebounceMs, 5, 5000);

  if (static_cast<uint8_t>(s.schedule.mode) > static_cast<uint8_t>(LidMode::Sequential)) {
    s.schedule.mode = LidMode::Weekday;
    ch = true;
  }
  // matchOpen only looks one day either side of an open, so each half of the
  // window has to stay under a day. The tail it actually uses is
  // windowAfterMin + missedGraceMin: set both of those near 24 h and an open
  // more than a day late no longer finds its dose, which is a window nobody
  // sane asks for -- nothing would ever be missed inside it either.
  ch |= clampTo<uint16_t>(s.schedule.windowBeforeMin, 0, kMinPerDay - 1);
  ch |= clampTo<uint16_t>(s.schedule.windowAfterMin, 1, kMinPerDay - 1);
  ch |= clampTo<uint16_t>(s.schedule.missedGraceMin, 0, kMinPerDay - 1);
  ch |= clampTo<uint16_t>(s.schedule.remindBeforeMin, 0, kMinPerDay - 1);

  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    LidSchedule& ls = s.schedule.lids[lid];
    ch |= fixTerm(ls.name, kLabelLen);
    if (ls.name[0] == '\0') {
      setStr(ls.name, kLabelLen, defaultLidName(lid, s.schedule.mode));
      ch = true;
    }
    if (ls.doseCount > kMaxDosesPerLid) {
      ls.doseCount = kMaxDosesPerLid;
      ch = true;
    }
    // Slots past doseCount are not in use; leaving them alone keeps validate()
    // from reporting a change on every single call.
    for (uint8_t d = 0; d < ls.doseCount; ++d) {
      Dose& dose = ls.doses[d];
      ch |= fixBool(dose.enabled);
      ch |= fixTerm(dose.label, kLabelLen);
      if (dose.minute >= kMinPerDay) {
        dose.minute = kMinPerDay - 1;
        ch = true;
      }
      if (dose.pillCount > kMaxPillsPerDose) {
        dose.pillCount = kMaxPillsPerDose;
        ch = true;
      }
      for (uint8_t p = 0; p < dose.pillCount; ++p) {
        ch |= fixTerm(dose.pills[p].name, kNameLen);
        ch |= fixTerm(dose.pills[p].dose, kDoseLen);
        if (dose.pills[p].count == 0) {
          dose.pills[p].count = 1;  // a pill nobody has to take is not a pill
          ch = true;
        }
      }
    }
  }

  return ch;
}

uint8_t applyDoseToLids(Schedule& sch, uint8_t lidMask, uint8_t doseIndex,
                        const Dose& dose) {
  uint8_t changed = 0;
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    if ((lidMask & (1u << lid)) == 0) continue;
    LidSchedule& ls = sch.lids[lid];
    if (ls.doseCount > kMaxDosesPerLid) ls.doseCount = kMaxDosesPerLid;

    uint8_t slot;
    if (doseIndex == 0xFF) {
      if (ls.doseCount >= kMaxDosesPerLid) continue;  // no room, no change
      slot = ls.doseCount;
    } else if (doseIndex < kMaxDosesPerLid) {
      slot = doseIndex;
    } else {
      continue;
    }

    ls.doses[slot] = dose;
    sanitiseDose(ls.doses[slot]);
    if (slot >= ls.doseCount) {
      // Writing past the end also fills the gap, otherwise doseCount would
      // claim slots that were never set.
      for (uint8_t d = ls.doseCount; d < slot; ++d) memset(&ls.doses[d], 0, sizeof(Dose));
      ls.doseCount = static_cast<uint8_t>(slot + 1);
    }
    ++changed;
  }
  return changed;
}

uint8_t removeDoseFromLids(Schedule& sch, uint8_t lidMask, uint8_t doseIndex) {
  uint8_t changed = 0;
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    if ((lidMask & (1u << lid)) == 0) continue;
    LidSchedule& ls = sch.lids[lid];
    if (ls.doseCount > kMaxDosesPerLid) ls.doseCount = kMaxDosesPerLid;
    if (ls.doseCount == 0) continue;

    // 0xFF mirrors applyDoseToLids' "no particular slot" and gives the API's
    // op:"clear" a single call.
    if (doseIndex == 0xFF) {
      memset(ls.doses, 0, sizeof(ls.doses));
      ls.doseCount = 0;
      ++changed;
      continue;
    }
    if (doseIndex >= ls.doseCount) continue;

    for (uint8_t d = doseIndex; d + 1 < ls.doseCount; ++d) ls.doses[d] = ls.doses[d + 1];
    memset(&ls.doses[ls.doseCount - 1], 0, sizeof(Dose));
    --ls.doseCount;
    ++changed;
  }
  return changed;
}

uint8_t distinctPillNames(const Schedule& sch, char out[][kNameLen], uint8_t maxOut) {
  if (!out || maxOut == 0) return 0;
  uint8_t n = 0;
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    const LidSchedule& ls = sch.lids[lid];
    uint8_t doseCount = ls.doseCount > kMaxDosesPerLid ? kMaxDosesPerLid : ls.doseCount;
    for (uint8_t d = 0; d < doseCount; ++d) {
      // Disabled doses still describe what is in the box, so their pills count.
      const Dose& dose = ls.doses[d];
      uint8_t pillCount = dose.pillCount > kMaxPillsPerDose ? kMaxPillsPerDose : dose.pillCount;
      for (uint8_t p = 0; p < pillCount; ++p) {
        const char* name = dose.pills[p].name;
        if (name[0] == '\0') continue;
        bool known = false;
        for (uint8_t i = 0; i < n; ++i) {
          if (strncmp(out[i], name, kNameLen) == 0) {
            known = true;
            break;
          }
        }
        if (known) continue;
        if (n >= maxOut) return n;  // capped, like doseInstancesInRange
        setStr(out[n], kNameLen, name);
        ++n;
      }
    }
  }
  return n;
}

}  // namespace ep
