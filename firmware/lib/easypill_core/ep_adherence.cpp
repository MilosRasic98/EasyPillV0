#include "ep_adherence.h"

#include <string.h>

#include "ep_timecalc.h"

namespace ep {
namespace {

// How much before the scheduled minute still counts as simply "taken". Earlier
// than this and the open is reported as Early, which is the signal that matters
// clinically: pills taken too close together.
constexpr int64_t kOnTimeSlackSec = 15 * 60;

// Handed out by at() for an index that does not exist, so a GUI off-by-one
// cannot fault the device. Single-threaded by design (see app_state.h).
DoseRecord g_outOfRange;

const DoseRecord* lookup(const DoseRecord* recs, size_t count, uint32_t scheduledLocal,
                         uint8_t lid, uint8_t dose) {
  for (size_t i = 0; i < count; ++i) {
    if (recs[i].scheduledLocal > scheduledLocal) break;  // sorted, so we are past it
    if (recs[i].scheduledLocal == scheduledLocal && recs[i].lid == lid &&
        recs[i].dose == dose) {
      return &recs[i];
    }
  }
  return nullptr;
}

// Seconds after which a never-opened instance is declared missed.
uint64_t missDeadline(const Schedule& sch, uint32_t scheduledLocal) {
  return static_cast<uint64_t>(scheduledLocal) +
         (static_cast<uint64_t>(sch.windowAfterMin) + sch.missedGraceMin) * kSecPerMin;
}

}  // namespace

MatchResult matchOpen(const Schedule& sch, uint8_t lid, uint32_t openLocal) {
  MatchResult m;
  memset(&m, 0, sizeof(m));
  m.state = DoseState::Pending;
  if (lid >= kLidCount) return m;

  const uint32_t openDay = dayIndex(openLocal);
  // Set whether or not anything matched: an open of Tuesday's lid on Monday is
  // worth reporting even when it happens nowhere near a dose time. Note that a
  // 23:50 dose opened at 00:10 is therefore wrongDay too, which is correct by
  // this rule and self-explanatory once the matched dose time is shown with it.
  m.wrongDay = !lidActiveOnDay(sch, lid, openDay);

  const int64_t open      = static_cast<int64_t>(openLocal);
  const int64_t beforeSec = static_cast<int64_t>(sch.windowBeforeMin) * 60;
  const int64_t afterSec  = static_cast<int64_t>(sch.windowAfterMin) * 60;
  // The grace period is part of the match window, not a gap before it: an open
  // inside the grace is still this dose, reported Late, and sweepMissed then
  // finds a resolved record and leaves the instance alone. Without it an open
  // between windowAfter and windowAfter + grace is unexpected *and* later
  // missed -- two alarms for a dose that was taken.
  const int64_t graceSec  = static_cast<int64_t>(sch.missedGraceMin) * 60;

  // Yesterday and tomorrow are in play so an open just either side of midnight
  // still finds the dose it belongs to.
  int64_t bestDist = -1;
  for (int dayOff = -1; dayOff <= 1; ++dayOff) {
    if (dayOff < 0 && openDay == 0) continue;
    const uint32_t day = static_cast<uint32_t>(static_cast<int64_t>(openDay) + dayOff);
    DoseInstance inst[kMaxDosesPerLid];
    const size_t n = doseInstancesForLidOnDay(sch, lid, day, inst, kMaxDosesPerLid);
    for (size_t i = 0; i < n; ++i) {
      const int64_t sched = static_cast<int64_t>(inst[i].localEpoch);
      if (open < sched - beforeSec || open > sched + afterSec + graceSec) continue;
      const int64_t dist = open >= sched ? open - sched : sched - open;
      // Strict less-than, and candidates arrive in ascending order, so a tie
      // keeps the earlier instance.
      if (bestDist >= 0 && dist >= bestDist) continue;
      bestDist         = dist;
      m.matched        = true;
      m.scheduledLocal = inst[i].localEpoch;
      m.dose           = inst[i].dose;
    }
  }
  if (!m.matched) return m;

  m.deltaMin = minuteDelta(m.scheduledLocal, openLocal);

  // Taken inside [D - 15 min, D + windowAfter/2]; inside the window but earlier
  // is Early, inside it but later is Late. Half the after-window is the
  // boundary so that "a bit late" and "very late" are distinguishable without
  // another setting.
  const int64_t sched = static_cast<int64_t>(m.scheduledLocal);
  if (open < sched - kOnTimeSlackSec) {
    m.state = DoseState::Early;
  } else if (open > sched + afterSec / 2) {
    m.state = DoseState::Late;
  } else {
    m.state = DoseState::Taken;
  }
  return m;
}

void AdherenceLog::clear() { count_ = 0; }

const DoseRecord& AdherenceLog::at(size_t i) const {
  if (i >= count_) {
    g_outOfRange = DoseRecord{};
    return g_outOfRange;
  }
  return records_[i];
}

DoseRecord& AdherenceLog::at(size_t i) {
  if (i >= count_) {
    g_outOfRange = DoseRecord{};
    return g_outOfRange;
  }
  return records_[i];
}

DoseRecord* AdherenceLog::find(uint32_t scheduledLocal, uint8_t lid, uint8_t dose) {
  return const_cast<DoseRecord*>(lookup(records_, count_, scheduledLocal, lid, dose));
}

DoseRecord* AdherenceLog::findOrCreate(uint32_t scheduledLocal, uint8_t lid, uint8_t dose) {
  DoseRecord* existing = find(scheduledLocal, lid, dose);
  if (existing) return existing;

  if (count_ == kCapacity) {
    // Oldest out first. The GUI only ever renders a fortnight.
    memmove(&records_[0], &records_[1], (kCapacity - 1) * sizeof(DoseRecord));
    --count_;
  }
  // Insert in scheduledLocal order so sweepMissed, adherencePercent and the
  // history endpoint can all scan linearly.
  size_t pos = count_;
  while (pos > 0 && records_[pos - 1].scheduledLocal > scheduledLocal) --pos;
  if (pos < count_) {
    memmove(&records_[pos + 1], &records_[pos], (count_ - pos) * sizeof(DoseRecord));
  }
  ++count_;

  DoseRecord& r = records_[pos];
  memset(&r, 0, sizeof(r));
  r.scheduledLocal = scheduledLocal;
  r.lid            = lid;
  r.dose           = dose;
  r.state          = DoseState::Pending;
  return &r;
}

void AdherenceLog::recordOpen(const Schedule& sch, uint8_t lid, uint32_t openLocal,
                              MatchResult* outMatch) {
  MatchResult m = matchOpen(sch, lid, openLocal);
  if (outMatch) *outMatch = m;
  if (!m.matched) return;  // the caller turns this into UnexpectedOpen

  DoseRecord* r = findOrCreate(m.scheduledLocal, lid, m.dose);
  // The first open is when the pills came out; a second lift of the same lid
  // must not rewrite that time. A Missed or Skipped record does get corrected.
  if (isTakenState(r->state)) return;
  r->actualLocal    = openLocal;
  r->state          = m.state;
  r->deltaMin       = m.deltaMin;
  r->unreportedMiss = 0;
}

bool AdherenceLog::markState(uint32_t scheduledLocal, uint8_t lid, uint8_t dose,
                             DoseState state, uint32_t nowLocal) {
  DoseRecord* r = findOrCreate(scheduledLocal, lid, dose);
  // Somebody is looking at this dose in the GUI, so a queued miss notification
  // would only tell them what they just typed. Not part of `changed`: it is
  // bookkeeping about notifications, not a change to the dose itself.
  r->unreportedMiss = 0;
  bool changed = false;
  if (r->state != state) {
    r->state = state;
    changed  = true;
  }
  if (isTakenState(state)) {
    if (r->actualLocal == 0) {
      // nowLocal is 0 while the clock is unverified; the scheduled time is then
      // the only honest answer available.
      r->actualLocal = nowLocal != 0 ? nowLocal : scheduledLocal;
      changed        = true;
    }
    const int16_t delta = minuteDelta(r->scheduledLocal, r->actualLocal);
    if (r->deltaMin != delta) {
      r->deltaMin = delta;
      changed     = true;
    }
  } else {
    // Nothing was opened for this dose, so there is no actual time to show.
    if (r->actualLocal != 0) {
      r->actualLocal = 0;
      changed        = true;
    }
    if (r->deltaMin != 0) {
      r->deltaMin = 0;
      changed     = true;
    }
  }
  return changed;
}

size_t AdherenceLog::sweepMissed(const Schedule& sch, uint32_t nowLocal,
                                 uint16_t lookbackDays, DoseRecord** out, size_t maxOut,
                                 uint32_t floorLocal) {
  // Pass one: mark. Walking day by day keeps the working set to four instances
  // and visits them in ascending order, which is also the insertion order the
  // record array wants.
  const uint32_t endDay   = dayIndex(nowLocal);
  const uint32_t startDay = endDay >= lookbackDays ? endDay - lookbackDays : 0;
  for (uint32_t day = startDay; day <= endDay; ++day) {
    DoseInstance inst[kMaxDosesPerLid];
    const size_t n =
        doseInstancesForLidOnDay(sch, lidForDay(sch, day), day, inst, kMaxDosesPerLid);
    for (size_t i = 0; i < n; ++i) {
      if (inst[i].localEpoch < floorLocal) continue;  // the box was not powered for it
      if (missDeadline(sch, inst[i].localEpoch) >= nowLocal) continue;
      DoseRecord* r = find(inst[i].localEpoch, inst[i].lid, inst[i].dose);
      if (r && r->state != DoseState::Pending) continue;  // already resolved either way
      if (!r) r = findOrCreate(inst[i].localEpoch, inst[i].lid, inst[i].dose);
      r->state          = DoseState::Missed;
      r->actualLocal    = 0;
      r->deltaMin       = 0;
      r->unreportedMiss = 1;
    }
  }

  // Pass two: hand out pointers. Nothing is inserted here, so they stay valid
  // for the caller. Marks that do not fit in out[] are left in place and
  // reported by a later call, so no notification is ever lost.
  size_t reported = 0;
  for (size_t i = 0; i < count_; ++i) {
    DoseRecord& r = records_[i];
    if (r.state != DoseState::Missed || !r.unreportedMiss) continue;
    if (!out || reported >= maxOut) continue;
    r.unreportedMiss = 0;
    out[reported]    = &r;
    ++reported;
  }
  return reported;
}

uint8_t AdherenceLog::adherencePercent(const Schedule& sch, uint32_t nowLocal,
                                       uint16_t days, uint32_t floorLocal) const {
  const uint64_t span = static_cast<uint64_t>(days) * kSecPerDay;
  const uint32_t from =
      span >= nowLocal ? 0 : static_cast<uint32_t>(nowLocal - span);

  uint32_t due = 0, taken = 0;
  const uint32_t endDay = dayIndex(nowLocal);
  for (uint32_t day = dayIndex(from); day <= endDay; ++day) {
    DoseInstance inst[kMaxDosesPerLid];
    const size_t n =
        doseInstancesForLidOnDay(sch, lidForDay(sch, day), day, inst, kMaxDosesPerLid);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t at = inst[i].localEpoch;
      if (at < from || at >= nowLocal) continue;
      if (at < floorLocal) continue;  // before this boot, so not ours to grade
      // Only instances whose window has fully closed, grace included: a dose
      // still inside its grace period has not failed yet and must not drag the
      // number down.
      if (missDeadline(sch, at) >= nowLocal) continue;
      const DoseRecord* r = lookup(records_, count_, at, inst[i].lid, inst[i].dose);
      // A human-marked Skipped dose was not supposed to be taken, so it leaves
      // the denominator rather than counting as a failure.
      if (r && r->state == DoseState::Skipped) continue;
      ++due;
      if (r && isTakenState(r->state)) ++taken;
    }
  }
  if (due == 0) return 100;  // nothing was due, so nothing was missed
  return static_cast<uint8_t>((taken * 100u + due / 2u) / due);
}

AdherenceLog::DayStats AdherenceLog::today(const Schedule& sch, uint32_t nowLocal,
                                           uint32_t floorLocal) const {
  DayStats st;
  memset(&st, 0, sizeof(st));
  const uint32_t day = dayIndex(nowLocal);
  DoseInstance inst[kMaxDosesPerLid];
  const size_t n =
      doseInstancesForLidOnDay(sch, lidForDay(sch, day), day, inst, kMaxDosesPerLid);
  for (size_t i = 0; i < n; ++i) {
    if (inst[i].localEpoch < floorLocal) continue;  // not ours: before this boot
    if (st.due < 255) ++st.due;
    const DoseRecord* r = lookup(records_, count_, inst[i].localEpoch, inst[i].lid, inst[i].dose);
    if (!r) {
      ++st.pending;
      continue;
    }
    // Skipped counts in `due` but in none of the three buckets, so the three do
    // not always add up to due. The GUI shows skipped separately.
    if (isTakenState(r->state)) {
      ++st.taken;
    } else if (r->state == DoseState::Missed) {
      ++st.missed;
    } else if (r->state == DoseState::Pending) {
      ++st.pending;
    }
  }
  return st;
}

}  // namespace ep
