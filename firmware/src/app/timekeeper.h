// Clock discipline without an RTC.
//
// There is no battery-backed clock on this board, so the strategy is: restore
// an approximate epoch from NVS at boot, mark it untrusted, sync SNTP the
// moment Wi-Fi comes up, and measure how fast the internal RC oscillator is
// running so the error can be corrected between syncs.
// See docs/HARDWARE.md section 8.
#pragma once

#include "app_state.h"

namespace ep {

// Applies the POSIX TZ string and seeds the clock from NVS. Does not use the
// network. Safe before Wi-Fi exists.
void timeBegin();

// Applies a new POSIX TZ string at runtime and refreshes the cached offset.
void timeApplyTz(const char* tz);

// Kicks off an SNTP request. Non-blocking; completion shows up in
// g.time.verified and g.time.lastSyncUtc.
void timeRequestSync();

// Blocks up to timeoutMs for a sync. Used at boot and by the GUI's sync
// button, not in the hot loop.
bool timeSyncBlocking(uint32_t timeoutMs);

// Call every loop. Fires a resync when one is due, updates the drift estimate
// after a successful sync, and persists the epoch every few minutes.
void timeService(uint32_t nowMs);

// Seconds east of UTC, including DST, for the current moment.
int32_t timeTzOffsetSec();

uint32_t timeNowUtc();
uint32_t timeNowLocal();

// "Sun 2026-11-01 08:30". Writes at most n bytes.
void timeFormat(uint32_t localEpoch, char* out, size_t n);

// Scale a requested sleep duration by the measured drift so the chip actually
// sleeps the wall-clock time we asked for.
uint32_t timeCorrectSleepSeconds(uint32_t requestedSeconds);

}  // namespace ep
