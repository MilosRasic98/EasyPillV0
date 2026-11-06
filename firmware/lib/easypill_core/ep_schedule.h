// Turning a Schedule into concrete dose instances on the timeline.
#pragma once

#include "ep_settings.h"

namespace ep {

// A single scheduled dose at a single moment.
struct DoseInstance {
  bool     valid;
  uint32_t localEpoch;
  uint8_t  lid;
  uint8_t  dose;
};

// Which lid is "today"? In Weekday mode this is just weekdayMon0 mapped through
// the schedule; in Sequential mode it counts days from the anchor.
uint8_t lidForDay(const Schedule& sch, uint32_t dayIdx);

// The first day index >= fromDay on which `lid` is the active lid.
bool nextDayForLid(const Schedule& sch, uint8_t lid, uint32_t fromDay, uint32_t* dayIdx);

// True when `lid` is the active lid on `dayIdx`.
bool lidActiveOnDay(const Schedule& sch, uint8_t lid, uint32_t dayIdx);

// Soonest enabled dose strictly after nowLocal. Searches 14 days ahead, which
// covers both modes, and returns valid == false if the schedule is empty.
DoseInstance nextDose(const Schedule& sch, uint32_t nowLocal);

// Most recent enabled dose at or before nowLocal, searching 14 days back.
DoseInstance previousDose(const Schedule& sch, uint32_t nowLocal);

// Every enabled dose instance in [fromLocal, toLocal), ascending. Writes up to
// maxOut entries and returns how many it wrote; the return value is capped at
// maxOut even if more exist.
size_t doseInstancesInRange(const Schedule& sch, uint32_t fromLocal, uint32_t toLocal,
                            DoseInstance* out, size_t maxOut);

// All enabled instances for one lid on one day, ascending.
size_t doseInstancesForLidOnDay(const Schedule& sch, uint8_t lid, uint32_t dayIdx,
                                DoseInstance* out, size_t maxOut);

uint8_t  enabledDoseCount(const Schedule& sch, uint8_t lid);
uint16_t totalPillsPerWeek(const Schedule& sch);

// Does this lid have anything scheduled at all?
bool lidHasSchedule(const Schedule& sch, uint8_t lid);

}  // namespace ep
