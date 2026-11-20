// Settings and schedule <-> JSON, in the shape the HTTP API serves.
#pragma once

#include <ArduinoJson.h>

#include "ep_adherence.h"
#include "ep_events.h"
#include "ep_settings.h"

namespace ep {
namespace json {

// Secrets read back as this when a value is stored, and posting it back means
// "leave the stored value alone". Lets the GUI round-trip a form without ever
// receiving the password.
constexpr const char* kSecretPlaceholder = "__set__";

void settingsToJson(const Settings& s, JsonObject out);
// Merges whatever is present and leaves the rest alone. Returns true if any
// field actually changed.
bool settingsFromJson(JsonObjectConst in, Settings& s);

void scheduleToJson(const Schedule& sch, JsonObject out);
bool scheduleFromJson(JsonObjectConst in, Schedule& sch);

void doseToJson(const Dose& d, JsonObject out);
bool doseFromJson(JsonObjectConst in, Dose& d);

void eventToJson(const Event& e, JsonObject out, const char* text);
void doseRecordToJson(const DoseRecord& r, JsonObject out);

// Copies at most n-1 bytes and always terminates. Returns true if the value
// differed from what was already there.
bool copyString(JsonVariantConst v, char* dst, size_t n);
// Same, but treats kSecretPlaceholder as "no change".
bool copySecret(JsonVariantConst v, char* dst, size_t n);

const char* sleepPolicyToString(SleepPolicy p);
bool sleepPolicyFromString(const char* s, SleepPolicy* out);

}  // namespace json
}  // namespace ep
