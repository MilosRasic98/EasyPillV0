// Host unit tests for easypill_core. No framework, no dependencies: the point
// is that `tests/run_core_tests.sh` works on a laptop with nothing but clang++,
// because the core is the only part of EasyPill that can be verified without
// the board on the desk.
//
// Dates are converted with an independent days-from-civil implementation
// (Howard Hinnant's, public domain) so the weekday arithmetic in ep_timecalc is
// checked against something other than itself.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ep_adherence.h"
#include "ep_events.h"
#include "ep_schedule.h"
#include "ep_settings.h"
#include "ep_timecalc.h"
#include "ep_types.h"

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      ++g_failures;                                                                 \
      printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                             \
  do {                                                                             \
    ++g_checks;                                                                     \
    long long va_ = (long long)(a);                                                 \
    long long vb_ = (long long)(b);                                                 \
    if (va_ != vb_) {                                                               \
      ++g_failures;                                                                 \
      printf("%s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__, __LINE__,  \
             #a, #b, va_, vb_);                                                     \
    }                                                                               \
  } while (0)

#define CHECK_STR(a, b)                                                            \
  do {                                                                             \
    ++g_checks;                                                                     \
    const char* sa_ = (a);                                                          \
    const char* sb_ = (b);                                                          \
    if (strcmp(sa_, sb_) != 0) {                                                    \
      ++g_failures;                                                                 \
      printf("%s:%d: CHECK_STR(%s, %s) failed: \"%s\" != \"%s\"\n", __FILE__,       \
             __LINE__, #a, #b, sa_, sb_);                                           \
    }                                                                               \
  } while (0)

// ---------------------------------------------------------------------------
// Independent calendar, for checking ep_timecalc against real dates.

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t  era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m > 2 ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static uint32_t dayOf(int y, unsigned m, unsigned d) {
  return static_cast<uint32_t>(daysFromCivil(y, m, d));
}

static uint32_t at(int y, unsigned m, unsigned d, unsigned hh, unsigned mm) {
  return dayOf(y, m, d) * ep::kSecPerDay + hh * ep::kSecPerHour + mm * ep::kSecPerMin;
}

// ---------------------------------------------------------------------------

static void testWeekdayMath() {
  // Anchor: if this is wrong, nothing below means anything.
  CHECK_EQ(daysFromCivil(1970, 1, 1), 0);
  CHECK_EQ(dayOf(2024, 2, 29), 19782);   // leap day
  CHECK_EQ(dayOf(2026, 10, 31), 20757);

  // 1970-01-01, a Thursday: this is the fact the two one-liners rest on.
  CHECK_EQ(ep::dayIndex(at(1970, 1, 1, 0, 0)), 0);
  CHECK_EQ(ep::weekdaySun0(at(1970, 1, 1, 12, 0)), 4);
  CHECK_EQ(ep::weekdayMon0(at(1970, 1, 1, 12, 0)), 3);

  // 2000-01-01 was a Saturday.
  CHECK_EQ(ep::weekdaySun0(at(2000, 1, 1, 0, 0)), 6);
  CHECK_EQ(ep::weekdayMon0(at(2000, 1, 1, 0, 0)), 5);

  // 2024-02-29, a Thursday in a leap year.
  CHECK_EQ(ep::weekdaySun0(at(2024, 2, 29, 23, 59)), 4);
  CHECK_EQ(ep::weekdayMon0(at(2024, 2, 29, 23, 59)), 3);

  // 2026-10-31 is a Saturday, 2026-11-01 the Sunday after it.
  CHECK_EQ(ep::weekdaySun0(at(2026, 10, 31, 9, 0)), 6);
  CHECK_EQ(ep::weekdayMon0(at(2026, 10, 31, 9, 0)), 5);
  CHECK_EQ(ep::weekdaySun0(at(2026, 11, 1, 9, 0)), 0);
  CHECK_EQ(ep::weekdayMon0(at(2026, 11, 1, 9, 0)), 6);
  CHECK_EQ(ep::weekdayMon0(at(2026, 11, 2, 9, 0)), 0);  // Monday

  // weekdayMon0FromDay must agree with weekdayMon0 everywhere.
  bool agree = true;
  for (uint32_t day = 20000; day < 20400; ++day) {
    if (ep::weekdayMon0FromDay(day) != ep::weekdayMon0(day * ep::kSecPerDay)) agree = false;
  }
  CHECK(agree);

  // Day/minute decomposition.
  const uint32_t t = at(2026, 10, 31, 14, 35);
  CHECK_EQ(ep::dayIndex(t), 20757);
  CHECK_EQ(ep::minuteOfDay(t), 14 * 60 + 35);
  CHECK_EQ(ep::dayStart(t), 20757u * 86400u);
  CHECK_EQ(ep::minuteOfDay(ep::dayStart(t)), 0);
  CHECK_EQ(ep::localEpochFor(20757, 14 * 60 + 35), t);
  CHECK_EQ(ep::localEpochFor(ep::dayIndex(t), ep::minuteOfDay(t)), t);

  // minuteDelta, including saturation at both ends.
  CHECK_EQ(ep::minuteDelta(t, t + 90 * 60), 90);
  CHECK_EQ(ep::minuteDelta(t + 90 * 60, t), -90);
  CHECK_EQ(ep::minuteDelta(t, t), 0);
  CHECK_EQ(ep::minuteDelta(t, t + 59), 0);  // truncates towards zero
  CHECK_EQ(ep::minuteDelta(0, 0xFFFFFFFFu), 32767);
  CHECK_EQ(ep::minuteDelta(0xFFFFFFFFu, 0), -32767);
}

static void testHhMm() {
  uint16_t m = 0xFFFF;
  CHECK(ep::parseHhMm("8:30", &m));
  CHECK_EQ(m, 510);
  m = 0xFFFF;
  CHECK(ep::parseHhMm("08:30", &m));
  CHECK_EQ(m, 510);
  m = 0xFFFF;
  CHECK(ep::parseHhMm("0830", &m));
  CHECK_EQ(m, 510);
  m = 0xFFFF;
  CHECK(ep::parseHhMm("00:00", &m));
  CHECK_EQ(m, 0);
  m = 0xFFFF;
  CHECK(ep::parseHhMm("23:59", &m));
  CHECK_EQ(m, 1439);
  m = 0xFFFF;
  CHECK(ep::parseHhMm(" 20:00 ", &m));  // the GUI is allowed to be sloppy
  CHECK_EQ(m, 1200);

  // Everything below must be refused, and must leave *out alone.
  const char* bad[] = {"24:00", "8:60", "", "abc", "12:3", "830", "25",
                       "8:",    ":30",  "08:300", "8:3a", "-1:00", "0860"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    uint16_t keep = 777;
    CHECK(!ep::parseHhMm(bad[i], &keep));
    CHECK_EQ(keep, 777);
  }
  uint16_t dummy = 0;
  CHECK(!ep::parseHhMm(nullptr, &dummy));

  char buf[8];
  ep::formatHhMm(0, buf, sizeof(buf));
  CHECK_STR(buf, "00:00");
  ep::formatHhMm(510, buf, sizeof(buf));
  CHECK_STR(buf, "08:30");
  ep::formatHhMm(1439, buf, sizeof(buf));
  CHECK_STR(buf, "23:59");

  // Round-trip every minute of the day.
  bool roundTrips = true;
  for (uint16_t minute = 0; minute < ep::kMinPerDay; ++minute) {
    char s[8];
    ep::formatHhMm(minute, s, sizeof(s));
    uint16_t back = 0xFFFF;
    if (!ep::parseHhMm(s, &back) || back != minute) roundTrips = false;
  }
  CHECK(roundTrips);
}

static void testDefaults() {
  ep::Settings s;
  ep::applyDefaults(s);

  // A fresh device must already be internally consistent.
  CHECK(!ep::validate(s));

  CHECK_EQ(s.version, ep::kSettingsVersion);
  CHECK_STR(s.wifi.ssid, "");
  CHECK_STR(s.wifi.pass, "");
  CHECK_STR(s.wifi.hostname, "easypill");
  CHECK_STR(s.time.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
  CHECK_STR(s.time.ntp1, "pool.ntp.org");
  CHECK_STR(s.time.ntp2, "time.google.com");
  CHECK_EQ(s.time.driftPpm, 0);
  CHECK_EQ(s.time.resyncIntervalSec, 6 * 3600);

  CHECK_EQ((int)s.power.policy, (int)ep::SleepPolicy::Smart);
  CHECK_EQ(s.power.awakeAfterEventSec, 300);
  CHECK_EQ(s.power.wakeIntervalSec, 900);
  CHECK_EQ(s.power.lowBatteryMv, 3500);
  CHECK_EQ(s.power.criticalBatteryMv, 3300);

  CHECK(s.hw.hallClosedIsLow);
  CHECK_EQ(s.hw.hallInvertMask, 0);
  // The V0 sensor row runs opposite to the days (docs/HARDWARE.md 3), so the
  // default is a reversal, not the identity.
  const uint8_t wantMap[ep::kLidCount] = {6, 5, 4, 3, 2, 1, 0};
  for (uint8_t i = 0; i < ep::kLidCount; ++i) CHECK_EQ(s.hw.hallToLid[i], wantMap[i]);
  CHECK_EQ(s.hw.ledChannelRed, 2);
  CHECK_EQ(s.hw.ledChannelGreen, 1);
  CHECK_EQ(s.hw.ledChannelBlue, 0);
  CHECK_EQ(s.hw.buzzerChannel, 3);
  CHECK_EQ(s.hw.buzzerToneHz, 2700);
  CHECK(!s.hw.buzzerSelfDrive);
  CHECK_EQ(s.hw.vbatDivNum, 175);
  CHECK_EQ(s.hw.vbatDivDen, 100);
  CHECK_EQ(s.hw.railDivNum, 175);
  CHECK_EQ(s.hw.railDivDen, 75);

  CHECK_EQ((int)s.schedule.mode, (int)ep::LidMode::Weekday);
  CHECK_EQ(s.schedule.windowBeforeMin, 60);
  CHECK_EQ(s.schedule.windowAfterMin, 120);
  CHECK_EQ(s.schedule.missedGraceMin, 15);

  // Two doses on all seven lids, so the GUI has something to show.
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    CHECK_EQ(s.schedule.lids[lid].doseCount, 2);
    CHECK_EQ(s.schedule.lids[lid].doses[0].minute, 8 * 60);
    CHECK_EQ(s.schedule.lids[lid].doses[1].minute, 20 * 60);
    CHECK(s.schedule.lids[lid].doses[0].enabled);
    CHECK_EQ(s.schedule.lids[lid].doses[0].pillCount, 1);
    CHECK_STR(s.schedule.lids[lid].doses[0].pills[0].name, "Pill");
    CHECK_EQ(ep::enabledDoseCount(s.schedule, lid), 2);
    CHECK(ep::lidHasSchedule(s.schedule, lid));
  }
  CHECK_STR(s.schedule.lids[0].name, "Monday");
  CHECK_STR(s.schedule.lids[6].name, "Sunday");
  CHECK_STR(s.schedule.lids[0].doses[0].label, "Morning");
  CHECK_STR(s.schedule.lids[0].doses[1].label, "Evening");
  CHECK_EQ(ep::totalPillsPerWeek(s.schedule), 14);

  char names[4][ep::kNameLen];
  CHECK_EQ(ep::distinctPillNames(s.schedule, names, 4), 1);
  CHECK_STR(names[0], "Pill");
}

static void testValidateRepairs() {
  ep::Settings s;
  ep::applyDefaults(s);

  // Deliberate damage, roughly in the order a corrupt NVS blob would inflict it.
  s.version                       = 999;
  s.schedule.lids[0].doseCount    = 9;
  s.schedule.lids[0].doses[0].minute    = 5000;
  s.schedule.lids[0].doses[0].pillCount = 9;
  s.schedule.lids[0].doses[0].pills[0].count = 0;
  memset(s.schedule.lids[1].name, 'A', ep::kLabelLen);  // no terminator
  s.schedule.windowAfterMin = 0;
  s.schedule.mode           = static_cast<ep::LidMode>(9);
  for (uint8_t i = 0; i < ep::kLidCount; ++i) s.hw.hallToLid[i] = 0;
  s.hw.ledChannelRed       = 9;
  s.hw.buzzerChannel       = 7;
  s.hw.buzzerToneHz        = 0;
  s.hw.vbatDivDen          = 0;
  s.power.criticalBatteryMv = 3600;  // above lowBatteryMv
  s.power.policy            = static_cast<ep::SleepPolicy>(7);
  s.wifi.hostname[0]        = '\0';
  s.time.tz[0]              = '\0';
  s.time.ntp1[0]            = '\0';
  s.time.driftPpm           = 9999999;
  s.time.resyncIntervalSec  = 5;
  s.notify.dailySummaryMinute = 5000;

  CHECK(ep::validate(s));

  CHECK_EQ(s.version, ep::kSettingsVersion);
  CHECK_EQ(s.schedule.lids[0].doseCount, ep::kMaxDosesPerLid);
  CHECK_EQ(s.schedule.lids[0].doses[0].minute, 1439);
  CHECK_EQ(s.schedule.lids[0].doses[0].pillCount, ep::kMaxPillsPerDose);
  CHECK_EQ(s.schedule.lids[0].doses[0].pills[0].count, 1);
  CHECK_EQ(strlen(s.schedule.lids[1].name), ep::kLabelLen - 1);
  CHECK_EQ(s.schedule.windowAfterMin, 1);
  CHECK_EQ((int)s.schedule.mode, (int)ep::LidMode::Weekday);
  // A mapping that is not a permutation repairs to the board default, not to
  // the identity -- identity would be a silently wrong day order here.
  const uint8_t repaired[ep::kLidCount] = {6, 5, 4, 3, 2, 1, 0};
  for (uint8_t i = 0; i < ep::kLidCount; ++i) CHECK_EQ(s.hw.hallToLid[i], repaired[i]);
  CHECK_EQ(s.hw.ledChannelRed, 2);
  CHECK_EQ(s.hw.ledChannelGreen, 1);
  CHECK_EQ(s.hw.ledChannelBlue, 0);
  CHECK_EQ(s.hw.buzzerChannel, 3);
  CHECK_EQ(s.hw.buzzerToneHz, 2700);
  CHECK_EQ(s.hw.vbatDivNum, 175);
  CHECK_EQ(s.hw.vbatDivDen, 100);
  CHECK(s.power.criticalBatteryMv < s.power.lowBatteryMv);
  CHECK_EQ((int)s.power.policy, (int)ep::SleepPolicy::Smart);
  CHECK_STR(s.wifi.hostname, "easypill");
  CHECK(s.time.tz[0] != '\0');
  CHECK_STR(s.time.ntp1, "time.google.com");  // promoted from the secondary slot
  CHECK(s.time.driftPpm <= 100000);
  CHECK_EQ(s.time.resyncIntervalSec, 600);
  CHECK_EQ(s.notify.dailySummaryMinute, 1439);

  // Idempotent: a validated blob reports no further change.
  CHECK(!ep::validate(s));

  // Duplicate LED channels are as broken as out-of-range ones.
  ep::Settings d;
  ep::applyDefaults(d);
  d.hw.ledChannelGreen = d.hw.ledChannelRed;
  CHECK(ep::validate(d));
  CHECK_EQ(d.hw.ledChannelRed, 2);
  CHECK_EQ(d.hw.ledChannelGreen, 1);

  // A colour on channel 3 chirps instead of glowing: that FET is the buzzer's.
  ep::Settings q;
  ep::applyDefaults(q);
  q.hw.ledChannelRed = 3;
  CHECK(ep::validate(q));
  CHECK_EQ(q.hw.ledChannelRed, 2);
  CHECK_EQ(q.hw.ledChannelGreen, 1);
  CHECK_EQ(q.hw.ledChannelBlue, 0);
  CHECK(!ep::validate(q));

  // And the buzzer cannot sit on an LED gate. The second call is the one that
  // matters: a repair that moved the colours instead would never converge.
  ep::Settings r;
  ep::applyDefaults(r);
  r.hw.buzzerChannel = 0;
  CHECK(ep::validate(r));
  CHECK_EQ(r.hw.buzzerChannel, 3);
  CHECK_EQ(r.hw.ledChannelRed, 2);
  CHECK(!ep::validate(r));

  // A valid hall mapping that is neither the identity nor the compiled default
  // must survive untouched. It has to differ from the default or the check is
  // vacuous -- it would pass even if validate() overwrote the whole array.
  ep::Settings p;
  ep::applyDefaults(p);
  const uint8_t custom[ep::kLidCount] = {3, 1, 4, 0, 6, 2, 5};
  memcpy(p.hw.hallToLid, custom, sizeof(custom));
  CHECK(!ep::validate(p));
  for (uint8_t i = 0; i < ep::kLidCount; ++i) CHECK_EQ(p.hw.hallToLid[i], custom[i]);
}

static void testLidForDay() {
  ep::Settings s;
  ep::applyDefaults(s);

  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 11, 2)), 0);   // Monday
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 10, 31)), 5);  // Saturday
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 11, 1)), 6);   // Sunday
  CHECK(ep::lidActiveOnDay(s.schedule, 5, dayOf(2026, 10, 31)));
  CHECK(!ep::lidActiveOnDay(s.schedule, 6, dayOf(2026, 10, 31)));
  CHECK(!ep::lidActiveOnDay(s.schedule, 99, dayOf(2026, 10, 31)));

  uint32_t day = 0;
  CHECK(ep::nextDayForLid(s.schedule, 0, dayOf(2026, 10, 31), &day));
  CHECK_EQ(day, dayOf(2026, 11, 2));  // the Monday after that Saturday
  CHECK(ep::nextDayForLid(s.schedule, 5, dayOf(2026, 10, 31), &day));
  CHECK_EQ(day, dayOf(2026, 10, 31));  // today already is Saturday
  CHECK(!ep::nextDayForLid(s.schedule, 7, 0, &day));

  // Sequential mode counts from the anchor, in both directions.
  s.schedule.mode                = ep::LidMode::Sequential;
  s.schedule.sequentialAnchorDay = dayOf(2026, 10, 31);
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 10, 31)), 0);
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 11, 3)), 3);
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 11, 7)), 0);
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 10, 30)), 6);
  CHECK_EQ(ep::lidForDay(s.schedule, dayOf(2026, 10, 24)), 0);
}

static void testNextPreviousDose() {
  ep::Settings s;
  ep::applyDefaults(s);

  // Sunday 20:30 -> next is Monday 08:00 on lid 0, across the week boundary.
  const uint32_t sundayEvening = at(2026, 11, 1, 20, 30);
  ep::DoseInstance next = ep::nextDose(s.schedule, sundayEvening);
  CHECK(next.valid);
  CHECK_EQ(next.lid, 0);
  CHECK_EQ(next.dose, 0);
  CHECK_EQ(next.localEpoch, at(2026, 11, 2, 8, 0));

  ep::DoseInstance prev = ep::previousDose(s.schedule, sundayEvening);
  CHECK(prev.valid);
  CHECK_EQ(prev.lid, 6);
  CHECK_EQ(prev.dose, 1);
  CHECK_EQ(prev.localEpoch, at(2026, 11, 1, 20, 0));

  // Exactly on a dose: next is strictly after, previous is at-or-before.
  const uint32_t onDose = at(2026, 11, 1, 20, 0);
  CHECK_EQ(ep::nextDose(s.schedule, onDose).localEpoch, at(2026, 11, 2, 8, 0));
  CHECK_EQ(ep::previousDose(s.schedule, onDose).localEpoch, onDose);

  // An empty schedule has no answer either way.
  ep::Settings e;
  ep::applyDefaults(e);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) e.schedule.lids[lid].doseCount = 0;
  CHECK(!ep::nextDose(e.schedule, sundayEvening).valid);
  CHECK(!ep::previousDose(e.schedule, sundayEvening).valid);
  CHECK(!ep::lidHasSchedule(e.schedule, 3));
  CHECK_EQ(ep::totalPillsPerWeek(e.schedule), 0);

  // So does a schedule whose doses are all disabled.
  ep::Settings off;
  ep::applyDefaults(off);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    for (uint8_t d = 0; d < off.schedule.lids[lid].doseCount; ++d) {
      off.schedule.lids[lid].doses[d].enabled = false;
    }
  }
  CHECK(!ep::nextDose(off.schedule, sundayEvening).valid);
  CHECK_EQ(ep::enabledDoseCount(off.schedule, 0), 0);
}

static void testDoseInstances() {
  ep::Settings s;
  ep::applyDefaults(s);

  const uint32_t from = at(2026, 10, 31, 0, 0);
  const uint32_t to   = at(2026, 11, 2, 0, 0);  // two whole days

  ep::DoseInstance buf[8];
  size_t n = ep::doseInstancesInRange(s.schedule, from, to, buf, 8);
  CHECK_EQ(n, 4);
  CHECK_EQ(buf[0].localEpoch, at(2026, 10, 31, 8, 0));
  CHECK_EQ(buf[1].localEpoch, at(2026, 10, 31, 20, 0));
  CHECK_EQ(buf[2].localEpoch, at(2026, 11, 1, 8, 0));
  CHECK_EQ(buf[3].localEpoch, at(2026, 11, 1, 20, 0));
  CHECK_EQ(buf[0].lid, 5);  // Saturday
  CHECK_EQ(buf[2].lid, 6);  // Sunday
  bool ascending = true;
  for (size_t i = 1; i < n; ++i) {
    if (buf[i].localEpoch <= buf[i - 1].localEpoch) ascending = false;
  }
  CHECK(ascending);

  // The cap is honoured and reported, not exceeded.
  ep::DoseInstance small[3];
  CHECK_EQ(ep::doseInstancesInRange(s.schedule, from, to, small, 3), 3);
  CHECK_EQ(small[2].localEpoch, at(2026, 11, 1, 8, 0));
  CHECK_EQ(ep::doseInstancesInRange(s.schedule, from, to, buf, 0), 0);
  CHECK_EQ(ep::doseInstancesInRange(s.schedule, to, from, buf, 8), 0);  // reversed range
  CHECK_EQ(ep::doseInstancesInRange(s.schedule, from, from, buf, 8), 0);

  // The half-open range excludes the end instant.
  CHECK_EQ(ep::doseInstancesInRange(s.schedule, at(2026, 10, 31, 8, 0),
                                    at(2026, 10, 31, 20, 0), buf, 8),
           1);

  // Doses are stored in GUI order; the instance list comes out sorted.
  ep::Settings u;
  ep::applyDefaults(u);
  u.schedule.lids[5].doses[0].minute = 20 * 60;
  u.schedule.lids[5].doses[1].minute = 8 * 60;
  n = ep::doseInstancesForLidOnDay(u.schedule, 5, dayOf(2026, 10, 31), buf, 8);
  CHECK_EQ(n, 2);
  CHECK_EQ(buf[0].localEpoch, at(2026, 10, 31, 8, 0));
  CHECK_EQ(buf[0].dose, 1);  // index 1 holds the earlier time now
  CHECK_EQ(buf[1].dose, 0);

  // A lid's own instances exist on every day, whether or not it is that lid's
  // day: that is what lets matchOpen recognise a wrong-compartment open.
  CHECK_EQ(ep::doseInstancesForLidOnDay(s.schedule, 0, dayOf(2026, 10, 31), buf, 8), 2);
  CHECK_EQ(ep::doseInstancesForLidOnDay(s.schedule, 9, dayOf(2026, 10, 31), buf, 8), 0);
}

static void testBulkEdits() {
  ep::Settings s;
  ep::applyDefaults(s);

  ep::Dose noon;
  memset(&noon, 0, sizeof(noon));
  noon.enabled   = true;
  noon.minute    = 12 * 60 + 15;
  noon.pillCount = 1;
  memcpy(noon.label, "Noon", 5);
  memcpy(noon.pills[0].name, "Ramipril", 9);
  noon.pills[0].count = 2;

  // Overwrite slot 0 on all seven lids.
  CHECK_EQ(ep::applyDoseToLids(s.schedule, 127, 0, noon), 7);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    CHECK_EQ(s.schedule.lids[lid].doses[0].minute, 12 * 60 + 15);
    CHECK_EQ(s.schedule.lids[lid].doseCount, 2);
  }

  // doseIndex 255 appends.
  CHECK_EQ(ep::applyDoseToLids(s.schedule, 127, 0xFF, noon), 7);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    CHECK_EQ(s.schedule.lids[lid].doseCount, 3);
    CHECK_EQ(s.schedule.lids[lid].doses[2].minute, 12 * 60 + 15);
  }
  CHECK_EQ(ep::applyDoseToLids(s.schedule, 127, 0xFF, noon), 7);  // fills slot 3
  CHECK_EQ(s.schedule.lids[0].doseCount, 4);
  CHECK_EQ(ep::applyDoseToLids(s.schedule, 127, 0xFF, noon), 0);  // now full
  CHECK_EQ(ep::applyDoseToLids(s.schedule, 127, 9, noon), 0);     // slot out of range

  // Two pill names are now in use.
  char names[4][ep::kNameLen];
  CHECK_EQ(ep::distinctPillNames(s.schedule, names, 4), 2);
  CHECK_EQ(ep::distinctPillNames(s.schedule, names, 1), 1);  // capped

  // A mask touches only the lids it names.
  ep::Settings m;
  ep::applyDefaults(m);
  CHECK_EQ(ep::applyDoseToLids(m.schedule, 0x05, 0, noon), 2);
  CHECK_EQ(m.schedule.lids[0].doses[0].minute, 12 * 60 + 15);
  CHECK_EQ(m.schedule.lids[1].doses[0].minute, 8 * 60);
  CHECK_EQ(m.schedule.lids[2].doses[0].minute, 12 * 60 + 15);

  // Writing past the end grows doseCount and leaves no undefined slot behind.
  ep::Settings g;
  ep::applyDefaults(g);
  g.schedule.lids[3].doseCount = 0;
  CHECK_EQ(ep::applyDoseToLids(g.schedule, 1u << 3, 1, noon), 1);
  CHECK_EQ(g.schedule.lids[3].doseCount, 2);
  CHECK(!g.schedule.lids[3].doses[0].enabled);
  CHECK(g.schedule.lids[3].doses[1].enabled);

  // A dose arriving from the API gets clamped on the way in.
  ep::Settings c;
  ep::applyDefaults(c);
  ep::Dose wild = noon;
  wild.minute    = 9999;
  wild.pillCount = 99;
  CHECK_EQ(ep::applyDoseToLids(c.schedule, 1, 0, wild), 1);
  CHECK_EQ(c.schedule.lids[0].doses[0].minute, 1439);
  CHECK_EQ(c.schedule.lids[0].doses[0].pillCount, ep::kMaxPillsPerDose);

  // Removal closes the gap.
  ep::Settings r;
  ep::applyDefaults(r);
  CHECK_EQ(ep::removeDoseFromLids(r.schedule, 127, 0), 7);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    CHECK_EQ(r.schedule.lids[lid].doseCount, 1);
    CHECK_EQ(r.schedule.lids[lid].doses[0].minute, 20 * 60);
    CHECK_STR(r.schedule.lids[lid].doses[0].label, "Evening");
  }
  CHECK_EQ(ep::removeDoseFromLids(r.schedule, 127, 5), 0);  // nothing in that slot
  CHECK_EQ(ep::removeDoseFromLids(r.schedule, 127, 0xFF), 7);
  CHECK_EQ(r.schedule.lids[0].doseCount, 0);
  CHECK_EQ(ep::removeDoseFromLids(r.schedule, 127, 0xFF), 0);  // already empty
}

static void testMatchOpen() {
  ep::Settings s;
  ep::applyDefaults(s);
  const uint32_t saturday = dayOf(2026, 10, 31);  // lid 5

  // Right lid, right time.
  ep::MatchResult m = ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 8, 5));
  CHECK(m.matched);
  CHECK(!m.wrongDay);
  CHECK_EQ(m.scheduledLocal, at(2026, 10, 31, 8, 0));
  CHECK_EQ(m.dose, 0);
  CHECK_EQ(m.deltaMin, 5);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Taken);

  // State boundaries: Taken spans [D - 15 min, D + windowAfter/2].
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 7, 50)).state,
           (int)ep::DoseState::Taken);
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 7, 45)).state,
           (int)ep::DoseState::Taken);  // exactly 15 minutes early
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 7, 44)).state,
           (int)ep::DoseState::Early);
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 9, 0)).state,
           (int)ep::DoseState::Taken);  // half the after-window
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 9, 1)).state,
           (int)ep::DoseState::Late);
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 0)).state,
           (int)ep::DoseState::Late);  // the last minute of the window

  // Window edges are inclusive, one second past them is not a match. The upper
  // edge is windowAfter + missedGraceMin -- 08:00 + 120 + 15 -- which is
  // exactly where sweepMissed takes over, so no open can fall between the two.
  CHECK(ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 7, 0)).matched);
  CHECK(!ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 7, 0) - 1).matched);
  CHECK(ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 0)).matched);
  CHECK(ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 14)).matched);
  CHECK_EQ((int)ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 14)).state,
           (int)ep::DoseState::Late);
  CHECK(ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 15)).matched);
  CHECK(!ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 10, 15) + 1).matched);
  // A grace of zero closes the window at windowAfter again.
  ep::Settings g;
  ep::applyDefaults(g);
  g.schedule.missedGraceMin = 0;
  CHECK(ep::matchOpen(g.schedule, 5, at(2026, 10, 31, 10, 0)).matched);
  CHECK(!ep::matchOpen(g.schedule, 5, at(2026, 10, 31, 10, 0) + 1).matched);

  // 03:00 is nowhere near either dose.
  m = ep::matchOpen(s.schedule, 5, at(2026, 10, 31, 3, 0));
  CHECK(!m.matched);
  CHECK(!m.wrongDay);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Pending);

  // Right time, wrong compartment: Sunday's lid opened on Saturday.
  m = ep::matchOpen(s.schedule, 6, at(2026, 10, 31, 8, 5));
  CHECK(m.matched);
  CHECK(m.wrongDay);
  CHECK_EQ(m.scheduledLocal, at(2026, 10, 31, 8, 0));

  // A lid that does not exist matches nothing.
  CHECK(!ep::matchOpen(s.schedule, 7, at(2026, 10, 31, 8, 5)).matched);

  // Across midnight: a 23:50 dose opened at 00:10 the next day.
  ep::Settings n;
  ep::applyDefaults(n);
  n.schedule.lids[5].doseCount         = 1;
  n.schedule.lids[5].doses[0].minute   = 23 * 60 + 50;
  m = ep::matchOpen(n.schedule, 5, at(2026, 11, 1, 0, 10));
  CHECK(m.matched);
  CHECK_EQ(m.scheduledLocal, at(2026, 10, 31, 23, 50));
  CHECK_EQ(m.deltaMin, 20);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Taken);
  CHECK(m.wrongDay);  // the open itself landed on Sunday, which is lid 6's day

  // And the mirror image: a 00:10 dose opened at 23:50 the day before. The
  // on-time window is asymmetric by design (15 minutes early, half the
  // after-window late), so 20 minutes early is Early, not Taken.
  ep::Settings b;
  ep::applyDefaults(b);
  b.schedule.lids[6].doseCount       = 1;
  b.schedule.lids[6].doses[0].minute = 10;
  m = ep::matchOpen(b.schedule, 6, at(2026, 10, 31, 23, 50));
  CHECK(m.matched);
  CHECK_EQ(m.scheduledLocal, at(2026, 11, 1, 0, 10));
  CHECK_EQ(m.deltaMin, -20);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Early);
  CHECK(m.wrongDay);  // opened on Saturday, which is lid 5's day

  // Dead centre between two doses: the earlier one wins.
  ep::Settings t;
  ep::applyDefaults(t);
  t.schedule.windowBeforeMin         = 60;
  t.schedule.windowAfterMin          = 60;
  t.schedule.lids[5].doseCount       = 2;
  t.schedule.lids[5].doses[0].minute = 10 * 60;
  t.schedule.lids[5].doses[1].minute = 12 * 60;
  m = ep::matchOpen(t.schedule, 5, at(2026, 10, 31, 11, 0));
  CHECK(m.matched);
  CHECK_EQ(m.dose, 0);
  CHECK_EQ(m.scheduledLocal, at(2026, 10, 31, 10, 0));
  CHECK_EQ((int)m.state, (int)ep::DoseState::Late);
  // A minute later the second dose is strictly nearer.
  m = ep::matchOpen(t.schedule, 5, at(2026, 10, 31, 11, 1));
  CHECK_EQ(m.dose, 1);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Early);

  (void)saturday;
}

static void testAdherenceRecords() {
  ep::Settings s;
  ep::applyDefaults(s);
  ep::AdherenceLog log;
  log.clear();

  // An open that matches is recorded; one that does not is left to the caller.
  ep::MatchResult m;
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 8, 5), &m);
  CHECK(m.matched);
  CHECK_EQ(log.size(), 1);
  CHECK_EQ(log.at(0).actualLocal, at(2026, 10, 31, 8, 5));
  CHECK_EQ((int)log.at(0).state, (int)ep::DoseState::Taken);
  CHECK_EQ(log.at(0).deltaMin, 5);

  // A second lift of the same lid must not rewrite the first open.
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 9, 30), &m);
  CHECK_EQ(log.size(), 1);
  CHECK_EQ(log.at(0).actualLocal, at(2026, 10, 31, 8, 5));

  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 3, 0), &m);
  CHECK(!m.matched);
  CHECK_EQ(log.size(), 1);  // unexpected opens are not dose records

  // Out-of-range access is defined, not a fault.
  CHECK_EQ(log.at(99).scheduledLocal, 0);
  CHECK(log.find(12345, 0, 0) == nullptr);

  // Records stay sorted however they arrive.
  log.clear();
  CHECK_EQ(log.size(), 0);
  const uint32_t base = at(2026, 10, 31, 0, 0);
  for (int i = 5; i >= 0; --i) {
    log.findOrCreate(base + static_cast<uint32_t>(i) * 3600, 5, 0);
  }
  CHECK_EQ(log.size(), 6);
  bool sorted = true;
  for (size_t i = 1; i < log.size(); ++i) {
    if (log.at(i).scheduledLocal <= log.at(i - 1).scheduledLocal) sorted = false;
  }
  CHECK(sorted);
  CHECK_EQ(log.at(0).scheduledLocal, base);
  CHECK(log.find(base + 3600, 5, 0) != nullptr);
  CHECK(log.find(base + 3600, 4, 0) == nullptr);  // lid is part of the identity

  // The oldest record is evicted when the array is full.
  log.clear();
  const size_t over = ep::AdherenceLog::kCapacity + 2;
  for (size_t i = 0; i < over; ++i) {
    log.findOrCreate(base + static_cast<uint32_t>(i) * 3600, 5, 0);
  }
  CHECK_EQ(log.size(), ep::AdherenceLog::kCapacity);
  CHECK_EQ(log.at(0).scheduledLocal, base + 2 * 3600);
  CHECK_EQ(log.at(log.size() - 1).scheduledLocal,
           base + static_cast<uint32_t>(over - 1) * 3600);
  CHECK(log.find(base, 5, 0) == nullptr);

  // Human overrides from the GUI.
  log.clear();
  const uint32_t sched = at(2026, 10, 31, 8, 0);
  CHECK(log.markState(sched, 5, 0, ep::DoseState::Taken, at(2026, 10, 31, 8, 20)));
  CHECK_EQ((int)log.at(0).state, (int)ep::DoseState::Taken);
  CHECK_EQ(log.at(0).actualLocal, at(2026, 10, 31, 8, 20));
  CHECK_EQ(log.at(0).deltaMin, 20);
  CHECK(!log.markState(sched, 5, 0, ep::DoseState::Taken, at(2026, 10, 31, 8, 20)));
  CHECK(log.markState(sched, 5, 0, ep::DoseState::Skipped, at(2026, 10, 31, 9, 0)));
  CHECK_EQ(log.at(0).actualLocal, 0);  // nothing was opened for a skipped dose
  CHECK_EQ(log.at(0).deltaMin, 0);

  // An open after a human marked it skipped: reality wins.
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 8, 10), &m);
  CHECK_EQ((int)log.at(0).state, (int)ep::DoseState::Taken);
  CHECK_EQ(log.size(), 1);
}

static void testSweepMissed() {
  ep::Settings s;
  ep::applyDefaults(s);
  ep::AdherenceLog log;
  log.clear();

  // 23:00 on the Saturday: both of that day's doses are long past their
  // window (08:00 + 120 + 15, 20:00 + 120 + 15) and so are the two days before.
  const uint32_t now = at(2026, 10, 31, 23, 0);

  ep::DoseRecord* missed[16];
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16), 6);
  CHECK_EQ(log.size(), 6);
  bool allMissed = true;
  for (size_t i = 0; i < 6; ++i) {
    if (missed[i]->state != ep::DoseState::Missed) allMissed = false;
    if (missed[i]->actualLocal != 0) allMissed = false;
    if (missed[i]->deltaMin != 0) allMissed = false;
  }
  CHECK(allMissed);
  CHECK_EQ(missed[0]->scheduledLocal, at(2026, 10, 29, 8, 0));
  CHECK_EQ(missed[5]->scheduledLocal, at(2026, 10, 31, 20, 0));

  // Exactly one notification each: a second sweep reports nothing new.
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16), 0);
  CHECK_EQ(log.size(), 6);

  // A dose that was taken is never swept.
  log.clear();
  ep::MatchResult m;
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 8, 10), &m);
  CHECK_EQ(log.sweepMissed(s.schedule, now, 0, missed, 16), 1);
  CHECK_EQ(missed[0]->scheduledLocal, at(2026, 10, 31, 20, 0));
  CHECK_EQ((int)log.at(0).state, (int)ep::DoseState::Taken);

  // Nothing is due yet at 08:30, so nothing can be missing.
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, at(2026, 10, 31, 8, 30), 0, missed, 16), 0);
  CHECK_EQ(log.size(), 0);
  // 10:15 is the deadline itself; a second later it has passed.
  CHECK_EQ(log.sweepMissed(s.schedule, at(2026, 10, 31, 10, 15), 0, missed, 16), 0);
  CHECK_EQ(log.sweepMissed(s.schedule, at(2026, 10, 31, 10, 15) + 1, 0, missed, 16), 1);

  // An open inside the grace tail belongs to the dose, so the sweep that runs a
  // minute later must not call the same dose missed. This is the two-alarms
  // bench failure: one unexpectedOpen push and then one doseMissed push, for a
  // dose that was taken.
  log.clear();
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 10, 10), &m);
  CHECK(m.matched);
  CHECK_EQ((int)m.state, (int)ep::DoseState::Late);
  CHECK_EQ(log.size(), 1);
  CHECK_EQ(log.sweepMissed(s.schedule, at(2026, 10, 31, 10, 16), 0, missed, 16), 0);
  CHECK_EQ((int)log.at(0).state, (int)ep::DoseState::Late);

  // A caller with a small out[] still gets every notification, just spread over
  // two calls.
  log.clear();
  ep::DoseRecord* few[4];
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, few, 4), 4);
  CHECK_EQ(log.size(), 6);
  // The two that did not fit are marked but unreported, and that is tracked out
  // of band: deltaMin is published by /api/history, so every record a sweep
  // touched has to read 0 there, not a private marker.
  bool deltasClean = true;
  for (size_t i = 0; i < log.size(); ++i) {
    if (log.at(i).deltaMin != 0) deltasClean = false;
  }
  CHECK(deltasClean);
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, few, 4), 2);
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, few, 4), 0);
  // A human ruling on a dose takes its queued notification with it: the family
  // already knows, so the leftover must not be pushed later.
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, few, 4), 4);
  CHECK(log.markState(at(2026, 10, 31, 20, 0), 5, 1, ep::DoseState::Skipped, now));
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, few, 4), 1);
  CHECK_EQ(few[0]->scheduledLocal, at(2026, 10, 31, 8, 0));

  // Day counts for the GUI header.
  log.clear();
  log.recordOpen(s.schedule, 5, at(2026, 10, 31, 8, 10), &m);
  ep::AdherenceLog::DayStats st = log.today(s.schedule, now);
  CHECK_EQ(st.due, 2);
  CHECK_EQ(st.taken, 1);
  CHECK_EQ(st.missed, 0);
  CHECK_EQ(st.pending, 1);
  log.sweepMissed(s.schedule, now, 0, missed, 16);
  st = log.today(s.schedule, now);
  CHECK_EQ(st.due, 2);
  CHECK_EQ(st.taken, 1);
  CHECK_EQ(st.missed, 1);
  CHECK_EQ(st.pending, 0);
}

static void testAdherencePercent() {
  ep::Settings s;
  ep::applyDefaults(s);
  ep::AdherenceLog log;
  log.clear();

  const uint32_t now = at(2026, 10, 31, 23, 0);

  // Nothing recorded at all still counts as six missed doses over three days.
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3), 0);
  CHECK_EQ(log.adherencePercent(s.schedule, now, 0), 100);  // nothing was due

  ep::DoseRecord* missed[16];
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16), 6);
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3), 0);

  // Three of the six taken: 50 %.
  CHECK(log.markState(at(2026, 10, 29, 8, 0), 3, 0, ep::DoseState::Taken, now));
  CHECK(log.markState(at(2026, 10, 29, 20, 0), 3, 1, ep::DoseState::Taken, now));
  CHECK(log.markState(at(2026, 10, 30, 8, 0), 4, 0, ep::DoseState::Taken, now));
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3), 50);

  // An empty schedule has nothing to fail.
  ep::Settings e;
  ep::applyDefaults(e);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) e.schedule.lids[lid].doseCount = 0;
  CHECK_EQ(log.adherencePercent(e.schedule, now, 30), 100);

  // One dose a day, three days in the window: 2 of 3 rounds to 67.
  ep::Settings one;
  ep::applyDefaults(one);
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) one.schedule.lids[lid].doseCount = 1;
  ep::AdherenceLog log2;
  log2.clear();
  CHECK_EQ(log2.sweepMissed(one.schedule, now, 2, missed, 16), 3);
  CHECK_EQ(log2.adherencePercent(one.schedule, now, 3), 0);
  CHECK(log2.markState(at(2026, 10, 29, 8, 0), 3, 0, ep::DoseState::Taken, now));
  CHECK_EQ(log2.adherencePercent(one.schedule, now, 3), 33);
  CHECK(log2.markState(at(2026, 10, 30, 8, 0), 4, 0, ep::DoseState::Taken, now));
  CHECK_EQ(log2.adherencePercent(one.schedule, now, 3), 67);

  // A skipped dose leaves the denominator rather than counting as a failure.
  CHECK(log2.markState(at(2026, 10, 31, 8, 0), 5, 0, ep::DoseState::Skipped, now));
  CHECK_EQ(log2.adherencePercent(one.schedule, now, 3), 100);

  // A dose still inside its window, or inside the grace that follows it, has
  // not failed yet and must not drag the number down. 08:00 + 120 + 15 = 10:15.
  ep::AdherenceLog log3;
  log3.clear();
  CHECK_EQ(log3.adherencePercent(one.schedule, at(2026, 10, 31, 9, 0), 1), 100);
  CHECK_EQ(log3.adherencePercent(one.schedule, at(2026, 10, 31, 10, 15), 1), 100);
  CHECK_EQ(log3.adherencePercent(one.schedule, at(2026, 10, 31, 10, 16), 1), 0);
}

static void testBootFloor() {
  // A box powered up at noon never saw the morning dose, so it must not push a
  // miss for it: floorLocal is the local epoch of the most recent cold boot.
  ep::Settings s;
  ep::applyDefaults(s);
  ep::AdherenceLog log;
  log.clear();

  const uint32_t now      = at(2026, 10, 31, 23, 0);
  const uint32_t bootNoon = at(2026, 10, 31, 12, 0);
  ep::DoseRecord* missed[16];

  // Three days of 08:00 and 20:00 doses are all missed without a floor.
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16), 6);

  // With one, only the doses from the boot onwards were the device's business.
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16, bootNoon), 1);
  CHECK_EQ(log.size(), 1);  // skipped entirely: not even a Pending record
  CHECK_EQ(missed[0]->scheduledLocal, at(2026, 10, 31, 20, 0));

  // The floor's own instant still counts, one second past it does not.
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16, at(2026, 10, 31, 8, 0)), 2);
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16, at(2026, 10, 31, 8, 0) + 1), 1);

  // today() drops what the floor skipped rather than calling it pending.
  log.clear();
  ep::AdherenceLog::DayStats st = log.today(s.schedule, now, bootNoon);
  CHECK_EQ(st.due, 1);
  CHECK_EQ(st.pending, 1);
  CHECK_EQ(st.missed, 0);
  st = log.today(s.schedule, now);
  CHECK_EQ(st.due, 2);
  CHECK_EQ(st.pending, 2);

  // And the percentage counts neither the skipped instances nor their failure.
  CHECK(log.markState(at(2026, 10, 31, 20, 0), 5, 1, ep::DoseState::Taken, now));
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3, bootNoon), 100);
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3), 17);  // one of six
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3, at(2026, 10, 31, 8, 0)), 50);
  CHECK_EQ(log.adherencePercent(s.schedule, now, 3, at(2026, 10, 31, 8, 0) + 1), 100);
  st = log.today(s.schedule, now, bootNoon);
  CHECK_EQ(st.due, 1);
  CHECK_EQ(st.taken, 1);

  // A floor of 0 means "no floor", which is what an unverified clock gives.
  log.clear();
  CHECK_EQ(log.sweepMissed(s.schedule, now, 2, missed, 16, 0), 6);
}

static void testEventLog() {
  ep::EventLog log;
  log.clear();
  CHECK_EQ(log.size(), 0);
  CHECK(log.findLatest(ep::EventType::Boot) == nullptr);

  // Wrap the ring: 70 pushes into 64 slots.
  const size_t pushes = ep::EventLog::kCapacity + 6;
  for (size_t i = 0; i < pushes; ++i) {
    log.push(ep::EventType::LidOpened, 1000 + static_cast<uint32_t>(i), 100,
             true, static_cast<uint8_t>(i % ep::kLidCount),
             static_cast<int16_t>(i));
  }
  CHECK_EQ(log.size(), ep::EventLog::kCapacity);
  CHECK_EQ(log.at(0).detail, static_cast<int16_t>(pushes - 1));  // index 0 is newest
  CHECK_EQ(log.at(ep::EventLog::kCapacity - 1).detail, 6);       // oldest retained
  CHECK_EQ((int)log.at(ep::EventLog::kCapacity).type, (int)ep::EventType::Count);
  CHECK_EQ(log.at(ep::EventLog::kCapacity).lid, ep::kNoLid);

  log.push(ep::EventType::DoseMissed, 2000, 200, true, 3, -5);
  const ep::Event* latest = log.findLatest(ep::EventType::DoseMissed);
  CHECK(latest != nullptr);
  CHECK_EQ(latest->localEpoch, 2000);
  CHECK_EQ(latest->detail, -5);
  CHECK_EQ((int)log.at(0).type, (int)ep::EventType::DoseMissed);
  CHECK_EQ((int)log.at(1).type, (int)ep::EventType::LidOpened);
  CHECK(log.findLatest(ep::EventType::SensorFault) == nullptr);

  log.clear();
  CHECK_EQ(log.size(), 0);

  // Events logged before the first NTP sync carry uptime only, and get a real
  // timestamp afterwards without ever claiming to be verified.
  log.push(ep::EventType::Boot, 0, 1000, false);
  log.push(ep::EventType::LidOpened, 0, 2000, false, 2);
  log.push(ep::EventType::WifiUp, 0, 3000, false);
  log.push(ep::EventType::TimeSynced, 1000000, 10000, true);
  CHECK_EQ(log.at(3).localEpoch, 0);

  CHECK_EQ(log.backfillTimes(1000000, 10000), 3);
  CHECK_EQ(log.at(0).localEpoch, 1000000);  // the verified one is untouched
  CHECK(log.at(0).timeVerified);
  CHECK_EQ(log.at(1).localEpoch, 1000000 - 7);  // (10000 - 3000) / 1000
  CHECK_EQ(log.at(2).localEpoch, 1000000 - 8);
  CHECK_EQ(log.at(3).localEpoch, 1000000 - 9);
  CHECK(!log.at(1).timeVerified);  // approximate, and the GUI must be able to say so
  CHECK_EQ(log.backfillTimes(1000000, 10000), 0);  // idempotent

  // A millis() rollover between the event and the sync still yields the real
  // interval, because the subtraction is unsigned.
  log.clear();
  log.push(ep::EventType::Boot, 0, 0xFFFFF000u, false);
  CHECK_EQ(log.backfillTimes(1000000, 0x00000FA0u), 1);
  CHECK_EQ(log.at(0).localEpoch, 1000000 - 8);  // 0x1FA0 ms apart

  // An uptime that would place the event before 1970 is left for a later call.
  log.clear();
  log.push(ep::EventType::Boot, 0, 0, false);
  CHECK_EQ(log.backfillTimes(10, 100000), 0);
  CHECK_EQ(log.at(0).localEpoch, 0);

  // An uptime from a previous power cycle is not on this boot's clock. Reading
  // it as an interval anyway gave the event a stamp 49.7 days in the past, and
  // the GUI rendered that as "49d ago" in the activity feed.
  log.clear();
  log.push(ep::EventType::SleepEnter, 0, 61000, false);
  CHECK_EQ(log.backfillTimes(1793522400u, 4000), 0);
  CHECK_EQ(log.at(0).localEpoch, 0);

  // The other half of it: an old uptime that happens to look recent. Only the
  // loader knows, so it says so on the event.
  ep::Event carried{};
  carried.type            = ep::EventType::Boot;
  carried.uptimeMs        = 1000;
  carried.lid             = ep::kNoLid;
  carried.fromEarlierBoot = true;
  log.clear();
  log.push(carried);
  CHECK_EQ(log.backfillTimes(1793522400u, 900000), 0);
  CHECK_EQ(log.at(0).localEpoch, 0);
  // The same event logged this boot does back-fill: 899 s of uptime ago.
  carried.fromEarlierBoot = false;
  log.clear();
  log.push(carried);
  CHECK_EQ(log.backfillTimes(1793522400u, 900000), 1);
  CHECK_EQ(log.at(0).localEpoch, 1793522400u - 899);
}

static void testTypeNames() {
  // These strings are the JSON API, so round-trip every one of them.
  bool allRoundTrip = true;
  for (uint8_t i = 0; i < static_cast<uint8_t>(ep::EventType::Count); ++i) {
    const ep::EventType t = static_cast<ep::EventType>(i);
    ep::EventType back = ep::EventType::Count;
    if (!ep::eventTypeFromString(ep::toString(t), &back) || back != t) {
      allRoundTrip = false;
    }
    if (strcmp(ep::toString(t), "unknown") == 0) allRoundTrip = false;
  }
  CHECK(allRoundTrip);
  CHECK_STR(ep::toString(ep::EventType::LidOpened), "lidOpened");
  CHECK_STR(ep::toString(ep::EventType::UnexpectedOpen), "unexpectedOpen");
  CHECK_STR(ep::toString(ep::EventType::WrongDayOpen), "wrongDayOpen");
  CHECK_STR(ep::toString(ep::EventType::Count), "unknown");
  ep::EventType et;
  CHECK(!ep::eventTypeFromString("nonsense", &et));
  CHECK(!ep::eventTypeFromString(nullptr, &et));

  bool statesRoundTrip = true;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(ep::DoseState::Skipped); ++i) {
    const ep::DoseState d = static_cast<ep::DoseState>(i);
    ep::DoseState back = ep::DoseState::Pending;
    if (!ep::doseStateFromString(ep::toString(d), &back) || back != d) {
      statesRoundTrip = false;
    }
  }
  CHECK(statesRoundTrip);
  CHECK_STR(ep::toString(ep::DoseState::Taken), "taken");
  CHECK_STR(ep::toString(ep::DoseState::Skipped), "skipped");
  CHECK_STR(ep::toString(ep::LidMode::Weekday), "weekday");
  CHECK_STR(ep::toString(ep::LidMode::Sequential), "sequential");
  ep::DoseState ds;
  CHECK(!ep::doseStateFromString("Taken", &ds));  // the API is lower case

  CHECK(ep::isTakenState(ep::DoseState::Taken));
  CHECK(ep::isTakenState(ep::DoseState::Late));
  CHECK(ep::isTakenState(ep::DoseState::Early));
  CHECK(!ep::isTakenState(ep::DoseState::Missed));
  CHECK(!ep::isTakenState(ep::DoseState::Skipped));
  CHECK(!ep::isTakenState(ep::DoseState::Pending));

  CHECK_STR(ep::defaultLidName(0, ep::LidMode::Weekday), "Monday");
  CHECK_STR(ep::defaultLidName(6, ep::LidMode::Weekday), "Sunday");
  CHECK_STR(ep::defaultLidName(0, ep::LidMode::Sequential), "Day 1");
  CHECK_STR(ep::defaultLidName(6, ep::LidMode::Sequential), "Day 7");
  CHECK_STR(ep::defaultLidName(7, ep::LidMode::Weekday), "unknown");
}

int main() {
  testWeekdayMath();
  testHhMm();
  testDefaults();
  testValidateRepairs();
  testLidForDay();
  testNextPreviousDose();
  testDoseInstances();
  testBulkEdits();
  testMatchOpen();
  testAdherenceRecords();
  testSweepMissed();
  testAdherencePercent();
  testBootFloor();
  testEventLog();
  testTypeNames();

  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
