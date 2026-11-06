#include "ep_events.h"

#include <string.h>

namespace ep {
namespace {

// Returned by at() for an index that does not exist. Type Count stringifies as
// "unknown", so a bad index shows up in the GUI instead of looking like a boot.
Event g_outOfRange;

}  // namespace

void EventLog::push(const Event& e) {
  buf_[head_] = e;
  head_       = (head_ + 1) % kCapacity;
  if (count_ < kCapacity) ++count_;
}

void EventLog::push(EventType type, uint32_t localEpoch, uint32_t uptimeMs,
                    bool timeVerified, uint8_t lid, int16_t detail) {
  Event e;
  memset(&e, 0, sizeof(e));
  e.localEpoch   = localEpoch;
  e.uptimeMs     = uptimeMs;
  e.type         = type;
  e.lid          = lid;
  e.detail       = detail;
  e.timeVerified = timeVerified;
  push(e);
}

const Event& EventLog::at(size_t i) const {
  if (i >= count_) {
    g_outOfRange      = Event{};
    g_outOfRange.type = EventType::Count;
    g_outOfRange.lid  = kNoLid;
    return g_outOfRange;
  }
  // head_ points at the next slot to write, so the one before it is newest.
  return buf_[(head_ + kCapacity - 1 - i) % kCapacity];
}

void EventLog::clear() {
  count_ = 0;
  head_  = 0;
}

size_t EventLog::backfillTimes(uint32_t nowLocal, uint32_t nowUptimeMs) {
  size_t filled = 0;
  for (size_t i = 0; i < count_; ++i) {
    Event& e = buf_[(head_ + kCapacity - 1 - i) % kCapacity];
    if (e.localEpoch != 0) continue;
    if (e.fromEarlierBoot) continue;  // its uptime has no meaning in this epoch
    // Read as a SIGNED interval. A millis() rollover at 49 days still comes out
    // as a small positive number, but an event stamped by a previous power
    // cycle reads negative -- millis() restarts at every power-on, so the two
    // uptimes are not on the same clock. Leaving localEpoch 0 is the honest
    // answer; the alternative is an event dated 49.7 days in the past.
    const int32_t elapsedMs = static_cast<int32_t>(nowUptimeMs - e.uptimeMs);
    if (elapsedMs < 0) continue;  // not from this boot
    const uint32_t elapsedSec = static_cast<uint32_t>(elapsedMs) / 1000u;
    if (elapsedSec > nowLocal) continue;  // would land before 1970; leave it for later
    e.localEpoch = nowLocal - elapsedSec;
    // timeVerified stays false: this is a reconstruction, and the GUI marks
    // such events as approximate.
    ++filled;
  }
  return filled;
}

const Event* EventLog::findLatest(EventType type) const {
  for (size_t i = 0; i < count_; ++i) {
    const Event& e = buf_[(head_ + kCapacity - 1 - i) % kCapacity];
    if (e.type == type) return &e;
  }
  return nullptr;
}

}  // namespace ep
