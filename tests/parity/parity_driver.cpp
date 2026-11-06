// C++ half of the firmware/simulator parity harness.
//
// Prints a deterministic report of what easypill_core decides for a grid of
// scheduler questions. tests/parity/parity_py.py prints the same report using
// sim/easypill_sim.py, and run_parity.sh diffs the two. Any divergence between
// the box and the thing the author demonstrates on his desk shows up here as a
// diff line instead of as a surprise on the bench.
//
// Keep the two report writers in step: same questions, same order, same
// formatting. The point is that `diff` is the whole assertion.
#include <cstdio>
#include <cstring>

#include "ep_adherence.h"
#include "ep_schedule.h"
#include "ep_settings.h"
#include "ep_timecalc.h"

using namespace ep;

namespace {

// A fixed day so the report does not change with the wall clock. 20757 is
// 2026-10-31, a Saturday, which puts a weekday/weekend boundary in range.
constexpr uint32_t kDay = 20757;

const char* stateName(DoseState s) { return toString(s); }

void buildSchedule(Schedule& sch, LidMode mode, uint16_t before, uint16_t after,
                   uint16_t grace) {
  Settings s;
  applyDefaults(s);
  sch                  = s.schedule;
  sch.mode             = mode;
  sch.windowBeforeMin  = before;
  sch.windowAfterMin   = after;
  sch.missedGraceMin   = grace;
  sch.sequentialAnchorDay = kDay;
}

void reportMatch() {
  const uint16_t windows[][3] = {{60, 120, 15}, {0, 0, 0}, {30, 60, 0}, {120, 240, 60}};
  const LidMode  modes[]      = {LidMode::Weekday, LidMode::Sequential};

  for (size_t m = 0; m < 2; ++m) {
    for (size_t w = 0; w < 4; ++w) {
      Schedule sch;
      buildSchedule(sch, modes[m], windows[w][0], windows[w][1], windows[w][2]);
      for (uint8_t lid = 0; lid < kLidCount; ++lid) {
        // Walk the whole day in 10-minute steps plus the two dose instants and
        // one second either side of every window edge, which is where an
        // off-by-one between the two implementations actually lives.
        for (int32_t min = -60; min <= 1500; min += 10) {
          const uint32_t open = localEpochFor(kDay, 0) + (uint32_t)(int32_t)(min * 60);
          const MatchResult r = matchOpen(sch, lid, open);
          printf("match mode=%d w=%u/%u/%u lid=%u open=%ld -> m=%d sched=%lu dose=%u "
                 "delta=%d state=%s wrong=%d\n",
                 (int)m, (unsigned)windows[w][0], (unsigned)windows[w][1],
                 (unsigned)windows[w][2], (unsigned)lid, (long)min, r.matched ? 1 : 0,
                 (unsigned long)r.scheduledLocal, (unsigned)r.dose, (int)r.deltaMin,
                 stateName(r.state), r.wrongDay ? 1 : 0);
        }
      }
    }
  }
}

void reportNextDose() {
  for (size_t m = 0; m < 2; ++m) {
    Schedule sch;
    buildSchedule(sch, m == 0 ? LidMode::Weekday : LidMode::Sequential, 60, 120, 15);
    for (int32_t min = 0; min < 1440; min += 7) {
      const uint32_t now = localEpochFor(kDay, 0) + (uint32_t)(min * 60);
      const DoseInstance n = nextDose(sch, now);
      const DoseInstance p = previousDose(sch, now);
      printf("next mode=%d now=%ld -> n(%d,%lu,%u,%u) p(%d,%lu,%u,%u)\n", (int)m, (long)min,
             n.valid ? 1 : 0, (unsigned long)n.localEpoch, (unsigned)n.lid, (unsigned)n.dose,
             p.valid ? 1 : 0, (unsigned long)p.localEpoch, (unsigned)p.lid, (unsigned)p.dose);
    }
  }
}

void reportLidMap() {
  for (size_t m = 0; m < 2; ++m) {
    Schedule sch;
    buildSchedule(sch, m == 0 ? LidMode::Weekday : LidMode::Sequential, 60, 120, 15);
    for (uint32_t d = kDay - 3; d <= kDay + 10; ++d) {
      printf("lidday mode=%d day=%lu -> lid=%u", (int)m, (unsigned long)d,
             (unsigned)lidForDay(sch, d));
      for (uint8_t lid = 0; lid < kLidCount; ++lid) {
        printf(" a%u=%d", (unsigned)lid, lidActiveOnDay(sch, lid, d) ? 1 : 0);
      }
      printf("\n");
    }
  }
}

// The three places a dose can be declared failed must agree on the deadline.
void reportSweepAndStats() {
  const uint16_t windows[][3] = {{60, 120, 15}, {60, 120, 0}, {0, 30, 30}};
  for (size_t w = 0; w < 3; ++w) {
    Schedule sch;
    buildSchedule(sch, LidMode::Weekday, windows[w][0], windows[w][1], windows[w][2]);
    for (int32_t min = 0; min <= 1440; min += 5) {
      AdherenceLog log;
      log.clear();
      const uint32_t now = localEpochFor(kDay, 0) + (uint32_t)(min * 60);
      DoseRecord* out[16];
      const size_t n  = log.sweepMissed(sch, now, 2, out, 16);
      const AdherenceLog::DayStats st = log.today(sch, now);
      printf("sweep w=%u/%u/%u now=%ld -> swept=%u due=%u taken=%u missed=%u pending=%u pct=%u\n",
             (unsigned)windows[w][0], (unsigned)windows[w][1], (unsigned)windows[w][2],
             (long)min, (unsigned)n, (unsigned)st.due, (unsigned)st.taken,
             (unsigned)st.missed, (unsigned)st.pending,
             (unsigned)log.adherencePercent(sch, now, 7));
    }
  }
}

// The boot floor: instances before it are not the box's business at all.
void reportFloor() {
  Schedule sch;
  buildSchedule(sch, LidMode::Weekday, 60, 120, 15);
  const uint32_t now = localEpochFor(kDay, 23 * 60);
  for (int32_t floorMin = -1; floorMin <= 1440; floorMin += 15) {
    AdherenceLog log;
    log.clear();
    const uint32_t floor = floorMin < 0 ? 0u : localEpochFor(kDay, (uint16_t)floorMin);
    DoseRecord* out[16];
    const size_t n = log.sweepMissed(sch, now, 2, out, 16, floor);
    const AdherenceLog::DayStats st = log.today(sch, now, floor);
    printf("floor at=%ld -> swept=%u due=%u pending=%u pct=%u\n", (long)floorMin,
           (unsigned)n, (unsigned)st.due, (unsigned)st.pending,
           (unsigned)log.adherencePercent(sch, now, 7, floor));
  }
}

void reportTimeCalc() {
  for (uint32_t d = kDay - 2; d <= kDay + 8; ++d) {
    printf("time day=%lu mon0=%u sun0=%u\n", (unsigned long)d,
           (unsigned)weekdayMon0FromDay(d),
           (unsigned)weekdaySun0(localEpochFor(d, 0)));
  }
  for (uint16_t min = 0; min < 1440; min += 37) {
    char buf[8];
    formatHhMm(min, buf, sizeof buf);
    uint16_t back = 0;
    const bool ok = parseHhMm(buf, &back);
    printf("hhmm %u -> %s -> ok=%d %u\n", (unsigned)min, buf, ok ? 1 : 0, (unsigned)back);
  }
}

}  // namespace

int main() {
  reportLidMap();
  reportTimeCalc();
  reportNextDose();
  reportMatch();
  reportSweepAndStats();
  reportFloor();
  return 0;
}
