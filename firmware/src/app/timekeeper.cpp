// Clock discipline without an RTC. See docs/HARDWARE.md section 8.
#include "timekeeper.h"

#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "net_wifi.h"
#include "store.h"

namespace ep {
namespace {

// Same floor as app_state.cpp: 2026-01-01. Anything below it is a chip that has
// just been reset, not a clock.

// A measurement needs a decent lever arm to mean anything: over ten minutes the
// quantisation of whole-second timestamps is already down in the single-digit
// ppm, and anything claiming more than 10 % error is a bad sync, not a bad
// oscillator.
constexpr int64_t kMinDriftSpanSec = 600;
constexpr int64_t kMaxDriftPpm     = 100000;

// Retry floor after a request that nobody answered. Without it a dead NTP server
// would have us re-initialising SNTP every pass of the loop.
constexpr uint32_t kSyncRetryMs = 60000;

// Written from the SNTP task, read from loop(). One flag and two words, each
// written once per sync, so the handover needs no lock.
volatile bool     syncPending = false;
volatile uint32_t syncAtMs    = 0;

// What our own clock thought the time was when the request went out, and when
// that was. The notification callback fires *after* lwIP has already stepped the
// clock, so the pre-sync reading has to be taken here instead.
uint32_t preSyncUtc = 0;
uint32_t preSyncMs  = 0;

uint32_t nextEpochPersistMs = 0;
uint32_t nextTzRefreshMs    = 0;

// RTC slow memory survives deep sleep, which is the only way the drift estimate
// can span the hours this box spends asleep - and sleep is exactly where the RC
// oscillator does its drifting. Cleared on a real power-on, which is correct:
// the reference has to be a time we actually measured this battery cycle.
RTC_DATA_ATTR uint32_t rtcRefSyncUtc = 0;
RTC_DATA_ATTR uint32_t rtcSyncTotal  = 0;

void onSntpSync(struct timeval* tv) {
  // Runs in the SNTP task. Record and get out; the work happens in timeService.
  (void)tv;
  syncAtMs    = millis();
  syncPending = true;
}

void applySync(uint32_t nowMs) {
  syncPending = false;

  const uint32_t trueUtc = (uint32_t)::time(nullptr);
  if (!plausibleUtc(trueUtc)) return;  // an answer that cannot be true

  // Consume the pre-sync sample: it belongs to exactly one request. lwIP's SNTP
  // client polls again on its own schedule and fires this notification each
  // time, and such an answer has no sample of its own - the stale one is a
  // reading of a clock that the previous sync already stepped, so measuring
  // against it invents a drift of (off-time / sync interval). The implausible
  // answer above returns before this on purpose: that request was never
  // answered usefully and the retry re-takes the sample.
  const uint32_t sampleUtc = preSyncUtc;
  const uint32_t sampleMs  = preSyncMs;
  preSyncUtc = 0;
  preSyncMs  = 0;

  // Where our own clock had got to by the time the answer landed. millis() runs
  // off the crystal-backed timer, so using it to carry the pre-sync reading
  // forward over the request round trip costs nothing we can measure.
  const uint32_t rtcAtAnswer = sampleUtc + (((uint32_t)syncAtMs - sampleMs) + 500u) / 1000u;

  const bool firstThisBoot = (g.time.syncCount == 0);
  g.time.syncCount++;
  g.time.lastSyncUtc = trueUtc;
  g.time.tzOffsetSec = timeTzOffsetSec();

  if (firstThisBoot) {
    g.time.verified = true;
    const size_t filled = g.events.backfillTimes(g.nowLocal(), millis());
    if (filled != 0) Serial.printf("[time] back-filled %u events\n", (unsigned)filled);
  }

  // This is the first moment the box can say what the local time is, so it is
  // also the first moment it can say which doses predate it. Doing it on every
  // sync rather than only the first keeps the floor out of the future when the
  // clock is stepped backwards.
  g.bootFloorFromClock(g.nowLocal());

  // Drift against the last sync we have a true time for. That reference may come
  // from before a deep sleep, so this fires on the first sync after a wake too -
  // which is the case worth measuring.
  if (plausibleUtc(rtcRefSyncUtc) && plausibleUtc(sampleUtc)) {
    const int64_t ntpElapsed = (int64_t)trueUtc - (int64_t)rtcRefSyncUtc;
    const int64_t rtcElapsed = (int64_t)rtcAtAnswer - (int64_t)rtcRefSyncUtc;
    if (ntpElapsed >= kMinDriftSpanSec) {
      const int64_t ppm = ((rtcElapsed - ntpElapsed) * 1000000) / ntpElapsed;
      if (ppm > -kMaxDriftPpm && ppm < kMaxDriftPpm) {
        int32_t next = (int32_t)ppm;
        // Light smoothing: one sync's worth of whole-second rounding should not
        // throw the estimate around.
        if (g.settings.time.driftPpm != 0) next = (g.settings.time.driftPpm + next) / 2;
        g.settings.time.driftPpm   = next;
        g.time.measuredDriftPpm    = next;
        g.markSettingsDirty();
        Serial.printf("[time] drift %ld ppm over %lld s\n", (long)next, (long long)ntpElapsed);
      } else {
        Serial.printf("[time] rejecting absurd drift %lld ppm\n", (long long)ppm);
      }
    }
  }

  rtcRefSyncUtc = trueUtc;
  rtcSyncTotal++;

  g.logEvent(EventType::TimeSynced, kNoLid, (int16_t)g.settings.time.driftPpm);
  storeSaveLastEpoch(trueUtc);
  // Floor the interval: a zero in settings would otherwise re-initialise SNTP on
  // every pass of the loop, and ten minutes is already far more often than this
  // oscillator needs.
  uint32_t interval = g.settings.time.resyncIntervalSec;
  if (interval < 600) interval = 600;
  g.time.nextSyncDueMs = nowMs + interval * 1000u;

  char stamp[24];
  timeFormat(g.nowLocal(), stamp, sizeof stamp);
  Serial.printf("[time] synced: %s (sync %lu this boot, %lu since power-on)\n", stamp,
                (unsigned long)g.time.syncCount, (unsigned long)rtcSyncTotal);
}

}  // namespace

void timeBegin() {
  timeApplyTz(g.settings.time.tz);

  // Step the clock rather than slew it: from a stale NVS seed the offset can be
  // hours, and smooth mode would take days to walk that off.
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  sntp_set_time_sync_notification_cb(onSntpSync);

  // A deep-sleep wake comes back with the clock still running: the RTC timer
  // carried it through the sleep (docs/HARDWARE.md section 8), so what the chip
  // already has is strictly better than the NVS seed, which is the epoch at
  // sleep *entry*. Stepping back to the seed would rewind the clock by the whole
  // sleep, on every wake, forever. Only step when the chip is not holding a time
  // at all - a real power-on, or a reset that lost the RTC domain - or when the
  // seed is somehow ahead of it. Tested against the clock rather than the wake
  // reason because lastWakeReason() reports a panic or WDT reset as PowerOn, and
  // those keep the RTC domain.
  const uint32_t seed    = storeLoadLastEpoch();
  const uint32_t carried = (uint32_t)::time(nullptr);
  const bool     stepped = plausibleUtc(seed) &&
                           (!plausibleUtc(carried) || seed > carried);
  if (stepped) {
    struct timeval tv {};
    tv.tv_sec  = (time_t)seed;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }

  g.time.verified         = false;
  g.time.syncCount        = 0;
  g.time.lastSyncUtc      = 0;
  g.time.measuredDriftPpm = g.settings.time.driftPpm;
  g.time.nextSyncDueMs    = 0;  // sync the moment there is a network
  g.bootUtc               = g.nowUtc();

  char stamp[24];
  timeFormat(g.nowLocal(), stamp, sizeof stamp);
  const char* how = stepped                       ? "seeded from NVS"
                    : plausibleUtc(carried)       ? "carried through sleep"
                                                  : "no clock yet";
  Serial.printf("[time] %s: %s (unverified), drift %ld ppm\n", how, stamp,
                (long)g.settings.time.driftPpm);
}

void timeApplyTz(const char* tz) {
  setenv("TZ", (tz != nullptr && tz[0] != '\0') ? tz : "UTC0", 1);
  tzset();
  g.time.tzOffsetSec = timeTzOffsetSec();
}

void timeRequestSync() {
  if (!netConnected()) return;

  preSyncUtc = (uint32_t)::time(nullptr);
  preSyncMs  = millis();

  const char* s1 = g.settings.time.ntp1[0] ? g.settings.time.ntp1 : "pool.ntp.org";
  const char* s2 = g.settings.time.ntp2[0] ? g.settings.time.ntp2 : nullptr;
  const char* tz = g.settings.time.tz[0] ? g.settings.time.tz : "UTC0";

  // lwIP's poll-mode client re-polls on CONFIG_LWIP_SNTP_UPDATE_DELAY, which is
  // three hours in this SDK and therefore shorter than resyncIntervalSec. Those
  // answers arrive with no pre-sync sample and still push nextSyncDueMs out, so
  // left alone they would starve timeService() of every sync it asks for and
  // drift would never be measured again. Push its cadence past ours instead.
  // Set before sntp_init(), which configTzTime() does below.
  uint32_t pollSec = g.settings.time.resyncIntervalSec;
  if (pollSec < 600) pollSec = 600;  // same floor as applySync()
  sntp_set_sync_interval(pollSec * 1000u + 60000u);

  // configTzTime stops any running SNTP client first, so calling it again is the
  // documented way to re-arm a request. It also does the setenv/tzset for us.
  configTzTime(tz, s1, s2);

  g.time.nextSyncDueMs = millis() + kSyncRetryMs;
}

bool timeSyncBlocking(uint32_t timeoutMs) {
  if (!netConnected()) return false;
  timeRequestSync();

  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    if (syncPending) {
      applySync(millis());
      return true;
    }
    delay(20);
  }
  return false;
}

void timeService(uint32_t nowMs) {
  if (syncPending) applySync(nowMs);

  if (netConnected() && (int32_t)(nowMs - g.time.nextSyncDueMs) >= 0) timeRequestSync();

  // The cached offset is what nowLocal() and the API report, so it has to notice
  // a DST transition without waiting for the next sync six hours later.
  if ((int32_t)(nowMs - nextTzRefreshMs) >= 0) {
    g.time.tzOffsetSec = timeTzOffsetSec();
    nextTzRefreshMs = nowMs + 10000u;
  }

  // A cold boot with no Wi-Fi starts from whatever was last written here, so keep
  // it reasonably fresh. storeSaveLastEpoch() throws away writes under a minute
  // apart, which keeps this cheap.
  if (g.clockUsable() && (int32_t)(nowMs - nextEpochPersistMs) >= 0) {
    storeSaveLastEpoch(g.nowUtc());
    nextEpochPersistMs = nowMs + 300000u;
  }
}

int32_t timeTzOffsetSec() {
  const time_t t = ::time(nullptr);
  struct tm lt {};
  struct tm gt {};
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);

  // Computed fresh from the two broken-down times rather than cached, so a DST
  // transition takes effect the moment it happens.
  int32_t off = (int32_t)(lt.tm_hour - gt.tm_hour) * 3600 +
                (int32_t)(lt.tm_min - gt.tm_min) * 60 + (int32_t)(lt.tm_sec - gt.tm_sec);
  const int dayDiff = lt.tm_yday - gt.tm_yday;
  if (dayDiff == 1 || dayDiff < -1) {
    off += 86400;  // local is a day ahead, or it is 31 Dec / 1 Jan
  } else if (dayDiff == -1 || dayDiff > 1) {
    off -= 86400;
  }
  return off;
}

uint32_t timeNowUtc() { return (uint32_t)::time(nullptr); }

uint32_t timeNowLocal() {
  return (uint32_t)((int64_t)timeNowUtc() + (int64_t)timeTzOffsetSec());
}

void timeFormat(uint32_t localEpoch, char* out, size_t n) {
  if (out == nullptr || n == 0) return;
  if (localEpoch == 0) {
    snprintf(out, n, "time unknown");
    return;
  }
  // The epoch handed in is already local, so gmtime_r is right here: localtime_r
  // would apply the offset a second time.
  const time_t t = (time_t)localEpoch;
  struct tm tmv {};
  gmtime_r(&t, &tmv);
  if (strftime(out, n, "%a %Y-%m-%d %H:%M", &tmv) == 0) out[0] = '\0';
}

uint32_t timeCorrectSleepSeconds(uint32_t requestedSeconds) {
  if (requestedSeconds == 0) return 0;

  int32_t ppm = g.settings.time.driftPpm;
  if (ppm > (int32_t)kMaxDriftPpm) ppm = (int32_t)kMaxDriftPpm;
  if (ppm < -(int32_t)kMaxDriftPpm) ppm = -(int32_t)kMaxDriftPpm;

  // driftPpm is measured as (reported elapsed - real elapsed) / real elapsed, so a
  // positive value means our clock counts 1 + ppm/1e6 seconds per real second.
  // The deep-sleep timer counts in that same biased time base, so a request of S
  // returns after S / (1 + ppm/1e6) real seconds - early, for a fast clock. The
  // correction therefore *multiplies* by (1 + ppm/1e6); dividing by it, which is
  // the intuitive reading, doubles the error instead of cancelling it.
  int64_t corrected = ((int64_t)requestedSeconds * (1000000 + (int64_t)ppm)) / 1000000;

  // Never let a bad estimate strand the device: at worst it wakes 20 % early or
  // 20 % late, which costs a little battery but still wakes.
  const int64_t lo = (int64_t)requestedSeconds * 80 / 100;
  const int64_t hi = (int64_t)requestedSeconds * 120 / 100;
  if (corrected < lo) corrected = lo;
  if (corrected > hi) corrected = hi;
  if (corrected < 1) corrected = 1;
  return (uint32_t)corrected;
}

}  // namespace ep
