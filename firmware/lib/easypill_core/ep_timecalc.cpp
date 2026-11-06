#include "ep_timecalc.h"

#include <stdio.h>

namespace ep {

uint32_t dayIndex(uint32_t localEpoch) { return localEpoch / kSecPerDay; }

uint32_t dayStart(uint32_t localEpoch) { return dayIndex(localEpoch) * kSecPerDay; }

uint16_t minuteOfDay(uint32_t localEpoch) {
  return static_cast<uint16_t>((localEpoch % kSecPerDay) / kSecPerMin);
}

// Day 0 (1970-01-01) was a Thursday: +4 puts it at index 4 counting from
// Sunday, +3 at index 3 counting from Monday. Verified against known dates in
// tests/core_tests.cpp rather than taken on trust.
uint8_t weekdaySun0(uint32_t localEpoch) {
  return static_cast<uint8_t>((dayIndex(localEpoch) + 4) % 7);
}

uint8_t weekdayMon0(uint32_t localEpoch) {
  return static_cast<uint8_t>((dayIndex(localEpoch) + 3) % 7);
}

uint8_t weekdayMon0FromDay(uint32_t dayIdx) {
  return static_cast<uint8_t>((dayIdx + 3) % 7);
}

uint32_t localEpochFor(uint32_t dayIdx, uint16_t minuteOfDay) {
  return dayIdx * kSecPerDay + static_cast<uint32_t>(minuteOfDay) * kSecPerMin;
}

void formatHhMm(uint16_t minuteOfDay, char* out, size_t n) {
  if (!out || n == 0) return;
  if (minuteOfDay >= kMinPerDay) minuteOfDay = kMinPerDay - 1;
  snprintf(out, n, "%02u:%02u", static_cast<unsigned>(minuteOfDay / 60),
           static_cast<unsigned>(minuteOfDay % 60));
}

bool parseHhMm(const char* s, uint16_t* minuteOfDay) {
  if (!s || !minuteOfDay) return false;

  while (*s == ' ' || *s == '\t') ++s;

  // Collect the digits before the separator: one or two for "8:30"/"08:30",
  // four for the colon-less "0830" that a bare numeric keypad produces.
  uint16_t digits[4];
  size_t nd = 0;
  while (nd < 4 && *s >= '0' && *s <= '9') {
    digits[nd++] = static_cast<uint16_t>(*s - '0');
    ++s;
  }

  uint16_t hour = 0, minute = 0;
  if (*s == ':') {
    if (nd < 1 || nd > 2) return false;  // ":30" and "0830:" are not times
    hour = nd == 1 ? digits[0] : static_cast<uint16_t>(digits[0] * 10 + digits[1]);
    ++s;
    if (!(s[0] >= '0' && s[0] <= '9') || !(s[1] >= '0' && s[1] <= '9')) return false;
    minute = static_cast<uint16_t>((s[0] - '0') * 10 + (s[1] - '0'));
    s += 2;
  } else if (nd == 4) {
    hour   = static_cast<uint16_t>(digits[0] * 10 + digits[1]);
    minute = static_cast<uint16_t>(digits[2] * 10 + digits[3]);
  } else {
    return false;
  }

  while (*s == ' ' || *s == '\t') ++s;
  if (*s != '\0') return false;  // trailing junk is a typo, not a time
  if (hour > 23 || minute > 59) return false;

  *minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

int16_t minuteDelta(uint32_t fromLocal, uint32_t toLocal) {
  // Widen first: the two epochs can be a decade apart and the difference has to
  // be signed, so neither uint32 nor int32 arithmetic is safe here.
  int64_t minutes = (static_cast<int64_t>(toLocal) - static_cast<int64_t>(fromLocal)) /
                    static_cast<int64_t>(kSecPerMin);
  if (minutes > 32767) return 32767;
  if (minutes < -32767) return -32767;
  return static_cast<int16_t>(minutes);
}

}  // namespace ep
