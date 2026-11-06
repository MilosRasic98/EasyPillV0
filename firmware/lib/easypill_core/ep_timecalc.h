// Calendar arithmetic for the scheduler.
//
// Everything in the core works in *local epoch*: seconds since 1970 counted in
// local wall-clock terms, i.e. UTC epoch plus the current UTC offset. The
// platform layer owns the conversion (it has tzdata and a POSIX TZ string); the
// core stays pure integer maths so it can be unit-tested on the host without
// caring about timezones at all.
//
// The one place this leaks: across a DST transition a dose can land an hour out
// for a single day, because the offset used to build the local epoch changes.
// For pill reminders that is acceptable and it self-corrects the next day.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ep {

constexpr uint32_t kSecPerMin  = 60;
constexpr uint32_t kSecPerHour = 3600;
constexpr uint32_t kSecPerDay  = 86400;
constexpr uint16_t kMinPerDay  = 1440;

// Days since 1970-01-01.
uint32_t dayIndex(uint32_t localEpoch);

// Local epoch of the preceding midnight.
uint32_t dayStart(uint32_t localEpoch);

// 0..1439
uint16_t minuteOfDay(uint32_t localEpoch);

// 1970-01-01 was a Thursday, which is what makes these two one-liners.
uint8_t weekdaySun0(uint32_t localEpoch);  // 0 = Sunday
uint8_t weekdayMon0(uint32_t localEpoch);  // 0 = Monday

uint8_t weekdayMon0FromDay(uint32_t dayIdx);

uint32_t localEpochFor(uint32_t dayIdx, uint16_t minuteOfDay);

// "08:30". Writes at most n bytes including the terminator.
void formatHhMm(uint16_t minuteOfDay, char* out, size_t n);

// Accepts "8:30", "08:30", "0830". Returns false and leaves *out alone on
// anything else, including out-of-range hours or minutes.
bool parseHhMm(const char* s, uint16_t* minuteOfDay);

// Signed minute difference, saturating at +/-32767 so it always fits an int16.
int16_t minuteDelta(uint32_t fromLocal, uint32_t toLocal);

}  // namespace ep
