// The rolling event log behind the GUI's activity feed.
#pragma once

#include "ep_types.h"

namespace ep {

struct Event {
  // 0 means "the clock was not trustworthy when this happened". The app
  // back-fills these once the first NTP sync lands; until then the GUI shows
  // them relative to uptime. The firmware never invents a wall-clock time.
  uint32_t  localEpoch;
  uint32_t  uptimeMs;
  EventType type;
  uint8_t   lid;     // 0xFF when the event is not about a particular lid
  int16_t   detail;  // minutes late, millivolts, RSSI - depends on type
  bool      timeVerified;
  // Set by whoever reloads the log from flash. Such an event's uptimeMs was
  // measured against a millis() that no longer exists, so it can never be
  // back-filled: see backfillTimes().
  bool      fromEarlierBoot;
};

// Persisted as raw structs and read back by size, so a new field has to fit in
// the padding or the stored layout moves under the loader.
static_assert(sizeof(Event) == 16, "Event is persisted as a raw struct");

constexpr uint8_t kNoLid = 0xFF;

class EventLog {
 public:
  static constexpr size_t kCapacity = 64;

  void push(const Event& e);
  void push(EventType type, uint32_t localEpoch, uint32_t uptimeMs, bool timeVerified,
            uint8_t lid = kNoLid, int16_t detail = 0);

  size_t size() const { return count_; }
  // Index 0 is the newest event.
  const Event& at(size_t i) const;

  void clear();

  // Once the clock is known, give the events that happened before the first
  // sync a real timestamp by working backwards from their uptime. Returns how
  // many were back-filled. Events carried over from an earlier power cycle keep
  // localEpoch 0 forever, because there is nothing to work backwards from.
  size_t backfillTimes(uint32_t nowLocal, uint32_t nowUptimeMs);

  // Newest event of this type, or null.
  const Event* findLatest(EventType type) const;

 private:
  Event  buf_[kCapacity];
  size_t count_ = 0;
  size_t head_  = 0;  // index of the next slot to write
};

}  // namespace ep
