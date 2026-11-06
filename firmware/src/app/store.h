// Persistence. Settings live in NVS as one blob; the event log and the dose
// records live in LittleFS so they survive a settings change and can be
// rewritten cheaply.
#pragma once

#include "app_state.h"

namespace ep {

// Mounts LittleFS, formatting it on first use. Returns false if the mount
// failed even after a format, in which case the device still runs but keeps no
// history across reboots.
bool storeBegin();

// Loads settings, or writes defaults and returns false when there is nothing
// stored yet (or the stored version does not match kSettingsVersion).
bool storeLoadSettings(Settings& s);
bool storeSaveSettings(const Settings& s);

bool storeLoadEvents(EventLog& log);
bool storeSaveEvents(const EventLog& log);

bool storeLoadAdherence(AdherenceLog& log);
bool storeSaveAdherence(const AdherenceLog& log);

// Last known epoch, so a cold boot without Wi-Fi starts somewhere sensible.
// The clock is still marked unverified until SNTP answers.
uint32_t storeLoadLastEpoch();
void     storeSaveLastEpoch(uint32_t utc);

// Clears NVS and LittleFS.
void storeFactoryReset();

size_t storeFsUsedBytes();
size_t storeFsTotalBytes();

// Called from the main loop: writes anything marked dirty, but no more often
// than once every few seconds, so a burst of GUI edits does not thrash flash.
void storeService(uint32_t nowMs);

}  // namespace ep
