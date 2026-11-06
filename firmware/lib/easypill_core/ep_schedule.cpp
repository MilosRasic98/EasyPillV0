#include "ep_schedule.h"

#include <string.h>

#include "ep_timecalc.h"

namespace ep {
namespace {

constexpr uint32_t kSearchDays = 14;  // both modes repeat every 7 days

uint8_t clampDoseCount(const LidSchedule& ls) {
  return ls.doseCount > kMaxDosesPerLid ? kMaxDosesPerLid : ls.doseCount;
}

}  // namespace

uint8_t lidForDay(const Schedule& sch, uint32_t dayIdx) {
  if (sch.mode == LidMode::Sequential) {
    uint32_t anchor = sch.sequentialAnchorDay;
    if (dayIdx >= anchor) return static_cast<uint8_t>((dayIdx - anchor) % kLidCount);
    // Days before the anchor still have to land on a lid: count backwards
    // through the cycle rather than letting the subtraction wrap.
    uint32_t back = (anchor - dayIdx) % kLidCount;
    return static_cast<uint8_t>(back == 0 ? 0 : kLidCount - back);
  }
  return weekdayMon0FromDay(dayIdx);
}

bool lidActiveOnDay(const Schedule& sch, uint8_t lid, uint32_t dayIdx) {
  if (lid >= kLidCount) return false;
  return lidForDay(sch, dayIdx) == lid;
}

bool nextDayForLid(const Schedule& sch, uint8_t lid, uint32_t fromDay, uint32_t* dayIdx) {
  if (lid >= kLidCount || !dayIdx) return false;
  for (uint32_t d = fromDay; d < fromDay + kLidCount; ++d) {
    if (lidForDay(sch, d) == lid) {
      *dayIdx = d;
      return true;
    }
  }
  return false;  // unreachable while both modes have a 7-day cycle
}

size_t doseInstancesForLidOnDay(const Schedule& sch, uint8_t lid, uint32_t dayIdx,
                                DoseInstance* out, size_t maxOut) {
  // No active-lid test here on purpose: matchOpen needs the instances a lid
  // *would* have on a neighbouring day to recognise a wrong-compartment open.
  // doseInstancesInRange is the one that applies the calendar.
  if (lid >= kLidCount || !out || maxOut == 0) return 0;
  const LidSchedule& ls = sch.lids[lid];
  uint8_t doseCount = clampDoseCount(ls);

  // Doses are stored in the order the GUI wrote them, so collect and sort.
  uint8_t order[kMaxDosesPerLid];
  uint8_t n = 0;
  for (uint8_t d = 0; d < doseCount; ++d) {
    if (!ls.doses[d].enabled) continue;
    uint16_t minute = ls.doses[d].minute;
    if (minute >= kMinPerDay) continue;  // validate() clamps these; be safe anyway
    uint8_t pos = n;
    while (pos > 0 && ls.doses[order[pos - 1]].minute > minute) {
      order[pos] = order[pos - 1];
      --pos;
    }
    order[pos] = d;
    ++n;
  }

  size_t written = 0;
  for (uint8_t i = 0; i < n && written < maxOut; ++i) {
    DoseInstance& inst = out[written];
    inst.valid      = true;
    inst.localEpoch = localEpochFor(dayIdx, ls.doses[order[i]].minute);
    inst.lid        = lid;
    inst.dose       = order[i];
    ++written;
  }
  return written;
}

size_t doseInstancesInRange(const Schedule& sch, uint32_t fromLocal, uint32_t toLocal,
                            DoseInstance* out, size_t maxOut) {
  if (!out || maxOut == 0 || toLocal <= fromLocal) return 0;
  size_t written = 0;
  uint32_t firstDay = dayIndex(fromLocal);
  uint32_t lastDay  = dayIndex(toLocal - 1);
  for (uint32_t d = firstDay; d <= lastDay && written < maxOut; ++d) {
    DoseInstance day[kMaxDosesPerLid];
    size_t n = doseInstancesForLidOnDay(sch, lidForDay(sch, d), d, day, kMaxDosesPerLid);
    for (size_t i = 0; i < n && written < maxOut; ++i) {
      if (day[i].localEpoch < fromLocal || day[i].localEpoch >= toLocal) continue;
      out[written++] = day[i];
    }
  }
  return written;
}

DoseInstance nextDose(const Schedule& sch, uint32_t nowLocal) {
  DoseInstance result;
  memset(&result, 0, sizeof(result));
  uint32_t firstDay = dayIndex(nowLocal);
  for (uint32_t d = firstDay; d <= firstDay + kSearchDays; ++d) {
    DoseInstance day[kMaxDosesPerLid];
    size_t n = doseInstancesForLidOnDay(sch, lidForDay(sch, d), d, day, kMaxDosesPerLid);
    for (size_t i = 0; i < n; ++i) {
      if (day[i].localEpoch > nowLocal) return day[i];
    }
  }
  return result;
}

DoseInstance previousDose(const Schedule& sch, uint32_t nowLocal) {
  DoseInstance result;
  memset(&result, 0, sizeof(result));
  uint32_t lastDay = dayIndex(nowLocal);
  for (uint32_t back = 0; back <= kSearchDays; ++back) {
    if (back > lastDay) break;  // 1970 is as far back as the timeline goes
    uint32_t d = lastDay - back;
    DoseInstance day[kMaxDosesPerLid];
    size_t n = doseInstancesForLidOnDay(sch, lidForDay(sch, d), d, day, kMaxDosesPerLid);
    for (size_t i = n; i > 0; --i) {
      if (day[i - 1].localEpoch <= nowLocal) return day[i - 1];
    }
  }
  return result;
}

uint8_t enabledDoseCount(const Schedule& sch, uint8_t lid) {
  if (lid >= kLidCount) return 0;
  const LidSchedule& ls = sch.lids[lid];
  uint8_t doseCount = clampDoseCount(ls);
  uint8_t n = 0;
  for (uint8_t d = 0; d < doseCount; ++d) {
    if (ls.doses[d].enabled) ++n;
  }
  return n;
}

uint16_t totalPillsPerWeek(const Schedule& sch) {
  // Every lid comes round once per seven days in both modes, so one pass over
  // the seven lids already is a week.
  uint32_t total = 0;
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    const LidSchedule& ls = sch.lids[lid];
    uint8_t doseCount = clampDoseCount(ls);
    for (uint8_t d = 0; d < doseCount; ++d) {
      const Dose& dose = ls.doses[d];
      if (!dose.enabled) continue;
      uint8_t pillCount = dose.pillCount > kMaxPillsPerDose ? kMaxPillsPerDose : dose.pillCount;
      for (uint8_t p = 0; p < pillCount; ++p) total += dose.pills[p].count;
    }
  }
  return total > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(total);
}

bool lidHasSchedule(const Schedule& sch, uint8_t lid) {
  // "Nothing scheduled" is what the GUI renders as todayState "none", so a lid
  // whose only doses are disabled counts as empty.
  return enabledDoseCount(sch, lid) > 0;
}

}  // namespace ep
