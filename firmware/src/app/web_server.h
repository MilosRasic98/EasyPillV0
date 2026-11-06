// The GUI and the JSON API, on the built-in synchronous WebServer.
//
// Synchronous on purpose. ESPAsyncWebServer would give us server-sent events,
// but it drags in AsyncTCP and its own crop of watchdog problems, and the GUI
// polls /api/state once a second anyway - which for watching a pill lid open is
// indistinguishable from a push. Zero extra dependencies is worth more here.
//
// Handlers run inside loop() via webService(), so they can touch ep::g freely.
// Keep them short: while a handler runs, nothing is scanning the hall sensors.
#pragma once

#include "app_state.h"

namespace ep {

// Registers all routes and starts listening on port 80.
void webBegin();

// Services at most one client. Call every loop.
void webService(uint32_t nowMs);

// True when a browser has polled recently, which the sleep logic uses to stay
// awake while somebody is actually looking at the GUI.
bool webClientActive(uint32_t nowMs);

uint32_t webLastRequestMs();

}  // namespace ep
