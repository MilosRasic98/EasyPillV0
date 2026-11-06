// Matching lid-open events to scheduled doses, and keeping the record.
//
// The device cannot see which pill came out of the box, only that a particular
// day's lid was lifted at a particular moment. So the whole of adherence is:
// pick the dose instance that open most plausibly belongs to, and notice the
// instances that nobody ever opened.
#pragma once

#include "ep_schedule.h"

namespace ep {

struct DoseRecord {
  uint32_t  scheduledLocal;  // identifies the instance together with lid+dose
  uint32_t  actualLocal;     // 0 while nothing has opened it
  uint8_t   lid;
  uint8_t   dose;
  DoseState state;
  int16_t   deltaMin;        // actualLocal - scheduledLocal, in minutes
  // 1 while sweepMissed has marked this instance Missed but has not yet handed
  // it to a caller to notify. The sense is deliberate: a record read back from
  // an adherence file written before this field existed sees the old padding
  // byte, which is 0, i.e. "already reported", so an old file cannot replay
  // pushes the family has already had.
  uint8_t   unreportedMiss;
};

// The record array is written to flash as raw structs and read back by size, so
// a field added past this point must stay inside the existing padding or the
// stored layout changes under the loader's feet.
static_assert(sizeof(DoseRecord) == 16, "DoseRecord is persisted as a raw struct");

struct MatchResult {
  bool      matched;
  uint32_t  scheduledLocal;
  uint8_t   dose;
  int16_t   deltaMin;
  DoseState state;      // Taken / Late / Early when matched
  bool      wrongDay;   // the lid was opened on a day that is not its day
};

// Which dose instance does an open of `lid` at `openLocal` belong to?
//
// Considers the lid's instances on the previous, current and next day so an
// open just after midnight still finds a 23:30 dose. Picks the nearest
// candidate inside [D - windowBefore, D + windowAfter + missedGrace]; ties go
// to the earlier instance. The grace period is part of the window on purpose:
// its upper edge is the instant sweepMissed takes over, so an open can never
// fall between the two and be reported unexpected and then missed. Sets
// wrongDay when the lid is not the active lid for that day, which is the
// interesting signal: right time, wrong compartment.
MatchResult matchOpen(const Schedule& sch, uint8_t lid, uint32_t openLocal);

// Fixed-capacity ring of dose records, newest at the end. Sized to hold a
// fortnight of four-a-day dosing.
class AdherenceLog {
 public:
  static constexpr size_t kCapacity = 128;

  void clear();

  size_t size() const { return count_; }
  // Index 0 is the oldest retained record.
  const DoseRecord& at(size_t i) const;
  DoseRecord& at(size_t i);

  DoseRecord* find(uint32_t scheduledLocal, uint8_t lid, uint8_t dose);
  // Never returns null: evicts the oldest record when full.
  DoseRecord* findOrCreate(uint32_t scheduledLocal, uint8_t lid, uint8_t dose);

  // Record a lid open. Fills *outMatch (may be null) and updates or creates the
  // matching record. An open that matches nothing is not recorded here at all;
  // the caller logs it as UnexpectedOpen.
  void recordOpen(const Schedule& sch, uint8_t lid, uint32_t openLocal,
                  MatchResult* outMatch);

  // Let a human override from the GUI.
  bool markState(uint32_t scheduledLocal, uint8_t lid, uint8_t dose, DoseState state,
                 uint32_t nowLocal);

  // Create records for, and mark as Missed, every instance whose window closed
  // more than missedGraceMin ago and that was never opened. Looks back
  // `lookbackDays`. Newly-missed records are written to out[] so the caller can
  // fire one notification each. Returns how many went missing.
  //
  // floorLocal is the local epoch at which this device most recently *cold*
  // booted. An instance before that was never the box's business -- it was not
  // powered -- so it is skipped entirely instead of pushing a miss nobody can
  // act on. A deep-sleep wake must not move the floor: the box sleeps intending
  // to wake for a dose, so a dose that falls while it sleeps is a real miss.
  size_t sweepMissed(const Schedule& sch, uint32_t nowLocal, uint16_t lookbackDays,
                     DoseRecord** out, size_t maxOut, uint32_t floorLocal = 0);

  // 0..100 over the trailing `days`, counting only instances whose window has
  // closed, grace included -- the same deadline sweepMissed uses. Returns 100
  // when nothing was due (nothing to fail). floorLocal as in sweepMissed().
  uint8_t adherencePercent(const Schedule& sch, uint32_t nowLocal, uint16_t days,
                           uint32_t floorLocal = 0) const;

  // Counts for the GUI header, over the local day containing nowLocal.
  struct DayStats {
    uint8_t due;
    uint8_t taken;
    uint8_t missed;
    uint8_t pending;
  };
  // floorLocal as in sweepMissed(): an instance below it is not counted as due
  // at all, so a box powered up at noon does not list the morning dose.
  DayStats today(const Schedule& sch, uint32_t nowLocal, uint32_t floorLocal = 0) const;

 private:
  DoseRecord records_[kCapacity];
  size_t     count_ = 0;
};

}  // namespace ep
