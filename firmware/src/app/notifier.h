// Outbound push notifications.
//
// The box pushes out rather than being polled, which is what makes the whole
// thing work on battery: wake on a lid, send, sleep again. ntfy is the default
// because getting it onto an iPad is "install the app, type a topic name" with
// no account and no keys.
#pragma once

#include "app_state.h"

namespace ep {

struct NotifyOutcome {
  bool ok;
  int  httpCode;      // 0 when the request never went out
  char detail[96];
};

void notifyBegin();

// Queues one push. Returns false if it was suppressed (quiet hours, no
// network, no target configured, event type switched off). Sending happens in
// notifyService so a handler never blocks on TLS.
bool notifyPush(const char* title, const char* message, uint8_t priority,
                const char* tags);

// Decides whether this event deserves a push, formats it and queues it.
void notifyEvent(const Event& e);

// Daily "here is how yesterday went" summary.
void notifyDailySummary();

// Call every loop. Drains at most one queued push per call.
void notifyService(uint32_t nowMs);

// Fires a test push immediately and blocks for the result. target is
// "ntfy", "pushover", "webhook" or "all".
NotifyOutcome notifyTest(const char* target);

bool notifyQueueEmpty();

}  // namespace ep
