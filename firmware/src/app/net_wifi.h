// Wi-Fi, in two modes.
//
// Station if credentials are stored and the AP answers. Otherwise, and only
// when apFallback is set, a setup access point called "<deviceName>-setup" so
// the box can be configured once and then never touched again.
#pragma once

#include "app_state.h"

namespace ep {

struct ScanResult {
  char   ssid[33];
  int8_t rssi;
  bool   secure;
};

void netBegin();

// Non-blocking state machine: retries the station with backoff, falls back to
// the AP, brings mDNS up when an address appears.
void netService(uint32_t nowMs);

bool netConnected();

// True when there is a network to join at all: credentials provisioned through
// the GUI, or the build-time defaults in include/wifi_default.h. Ask this
// rather than testing g.settings.wifi.ssid, which is empty on a board running
// on the built-in pair even though that board has a perfectly good route out.
bool netHaveCredentials();

// Stores new credentials and restarts the connection attempt.
bool netSetCredentials(const char* ssid, const char* pass);

// Blocking scan, a second or two. Returns how many entries it wrote.
size_t netScan(ScanResult* out, size_t maxOut);

// Drops the radio to save power before deep sleep.
void netShutdown();

// There is deliberately no netBusy() here. maybeSleep() in main.cpp spells the
// same conditions out one at a time, each with the reason it blocks sleep, and
// two copies of a gate that subtle would drift apart.

}  // namespace ep
