// The field names and enum spellings in here are the HTTP API, which the host
// simulator and the browser GUI also implement. Changing one means changing
// three programs, so don't - add instead.
#include "ep_json.h"

#include <limits>
#include <string.h>

#include "ep_timecalc.h"

namespace ep {
namespace json {
namespace {

// Absent and wrong-typed are the same answer here: "leave it alone". The GUI
// posts partial objects and half-filled forms, and a field it did not send must
// never be read as a zero.
bool numeric(JsonVariantConst v) {
  return v.is<long long>() || v.is<double>() || v.is<bool>();
}

// Clamping on the way in, not just in ep::validate, so a posted 70000 lands as
// 65535 rather than wrapping to 4464 - a wrap looks legal to the validator.
template <typename T>
bool setNum(JsonVariantConst v, T& dst) {
  if (!numeric(v)) return false;
  long long raw = v.as<long long>();
  const long long lo = static_cast<long long>(std::numeric_limits<T>::min());
  const long long hi = static_cast<long long>(std::numeric_limits<T>::max());
  if (raw < lo) raw = lo;
  if (raw > hi) raw = hi;
  const T value = static_cast<T>(raw);
  if (value == dst) return false;
  dst = value;
  return true;
}

bool setBool(JsonVariantConst v, bool& dst) {
  if (!numeric(v)) return false;
  const bool value = v.as<bool>();
  if (value == dst) return false;
  dst = value;
  return true;
}

void putSecret(JsonObject out, const char* key, const char* stored) {
  out[key] = (stored && stored[0] != '\0') ? kSecretPlaceholder : "";
}

// The API fixes these two spellings; the simulator and the GUI compare
// against them literally.
const char* lidModeWire(LidMode m) {
  return m == LidMode::Sequential ? "sequential" : "weekday";
}

bool lidModeFromWire(const char* s, LidMode* out) {
  if (!s) return false;
  if (strcmp(s, "weekday") == 0) {
    *out = LidMode::Weekday;
    return true;
  }
  if (strcmp(s, "sequential") == 0) {
    *out = LidMode::Sequential;
    return true;
  }
  return false;
}

// Field-by-field rather than memcmp: both structs carry padding that NVS blobs
// and freshly memset copies disagree about, and a phantom difference would
// mean a flash write on every POST.
bool pillEquals(const Pill& a, const Pill& b) {
  return a.count == b.count && strncmp(a.name, b.name, kNameLen) == 0 &&
         strncmp(a.dose, b.dose, kDoseLen) == 0;
}

bool doseEquals(const Dose& a, const Dose& b) {
  if (a.enabled != b.enabled || a.minute != b.minute || a.pillCount != b.pillCount) {
    return false;
  }
  if (strncmp(a.label, b.label, kLabelLen) != 0) return false;
  const uint8_t n = a.pillCount <= kMaxPillsPerDose ? a.pillCount : kMaxPillsPerDose;
  for (uint8_t i = 0; i < n; ++i) {
    if (!pillEquals(a.pills[i], b.pills[i])) return false;
  }
  return true;
}

}  // namespace

bool copyString(JsonVariantConst v, char* dst, size_t n) {
  if (!dst || n == 0) return false;
  const char* src = v.as<const char*>();
  if (!src) return false;  // absent, null, or not a string
  size_t len = strlen(src);
  if (len > n - 1) len = n - 1;
  if (strlen(dst) == len && memcmp(dst, src, len) == 0) return false;
  memcpy(dst, src, len);
  dst[len] = '\0';
  return true;
}

bool copySecret(JsonVariantConst v, char* dst, size_t n) {
  const char* src = v.as<const char*>();
  if (!src) return false;
  // The GUI never receives the stored secret, so it round-trips the
  // placeholder. Treating it as a value would overwrite the password with it.
  if (strcmp(src, kSecretPlaceholder) == 0) return false;
  return copyString(v, dst, n);
}

const char* sleepPolicyToString(SleepPolicy p) {
  switch (p) {
    case SleepPolicy::AlwaysOn:   return "alwaysOn";
    case SleepPolicy::Aggressive: return "aggressive";
    case SleepPolicy::Smart:      return "smart";
  }
  return "smart";
}

bool sleepPolicyFromString(const char* s, SleepPolicy* out) {
  if (!s || !out) return false;
  if (strcmp(s, "alwaysOn") == 0) {
    *out = SleepPolicy::AlwaysOn;
    return true;
  }
  if (strcmp(s, "smart") == 0) {
    *out = SleepPolicy::Smart;
    return true;
  }
  if (strcmp(s, "aggressive") == 0) {
    *out = SleepPolicy::Aggressive;
    return true;
  }
  return false;
}

void settingsToJson(const Settings& s, JsonObject out) {
  JsonObject ui = out["ui"].to<JsonObject>();
  ui["deviceName"]   = s.ui.deviceName;
  ui["patientName"]  = s.ui.patientName;
  ui["showAdvanced"] = s.ui.showAdvanced;

  JsonObject wifi = out["wifi"].to<JsonObject>();
  wifi["ssid"] = s.wifi.ssid;
  putSecret(wifi, "pass", s.wifi.pass);
  wifi["hostname"]   = s.wifi.hostname;
  wifi["apFallback"] = s.wifi.apFallback;
  putSecret(wifi, "apPassword", s.wifi.apPassword);

  JsonObject tm = out["time"].to<JsonObject>();
  tm["ntp1"] = s.time.ntp1;
  tm["ntp2"] = s.time.ntp2;
  tm["tz"]   = s.time.tz;
  // Reported so the GUI can show how badly the RC oscillator is drifting; it is
  // measured, not configured, but it round-trips like everything else.
  tm["driftPpm"]          = s.time.driftPpm;
  tm["resyncIntervalSec"] = s.time.resyncIntervalSec;

  JsonObject nt = out["notify"].to<JsonObject>();
  nt["ntfyEnabled"] = s.notify.ntfyEnabled;
  nt["ntfyServer"]  = s.notify.ntfyServer;
  nt["ntfyTopic"]   = s.notify.ntfyTopic;
  putSecret(nt, "ntfyToken", s.notify.ntfyToken);
  nt["pushoverEnabled"] = s.notify.pushoverEnabled;
  putSecret(nt, "pushoverToken", s.notify.pushoverToken);
  putSecret(nt, "pushoverUser", s.notify.pushoverUser);
  nt["webhookEnabled"]     = s.notify.webhookEnabled;
  nt["webhookUrl"]         = s.notify.webhookUrl;
  nt["onLidOpen"]          = s.notify.onLidOpen;
  nt["onDoseTaken"]        = s.notify.onDoseTaken;
  nt["onDoseMissed"]       = s.notify.onDoseMissed;
  nt["onDoseDue"]          = s.notify.onDoseDue;
  nt["onUnexpectedOpen"]   = s.notify.onUnexpectedOpen;
  nt["onLowBattery"]       = s.notify.onLowBattery;
  nt["onBoot"]             = s.notify.onBoot;
  nt["onDailySummary"]     = s.notify.onDailySummary;
  nt["dailySummaryMinute"] = s.notify.dailySummaryMinute;
  nt["quietFromMinute"]    = s.notify.quietFromMinute;
  nt["quietToMinute"]      = s.notify.quietToMinute;

  JsonObject pw = out["power"].to<JsonObject>();
  pw["policy"]             = sleepPolicyToString(s.power.policy);
  pw["awakeAfterEventSec"] = s.power.awakeAfterEventSec;
  pw["wakeIntervalSec"]    = s.power.wakeIntervalSec;
  pw["lowBatteryMv"]       = s.power.lowBatteryMv;
  pw["criticalBatteryMv"]  = s.power.criticalBatteryMv;
  pw["buzzerOnReminder"]   = s.power.buzzerOnReminder;
  pw["ledOnReminder"]      = s.power.ledOnReminder;

  JsonObject hw = out["hw"].to<JsonObject>();
  hw["hallClosedIsLow"] = s.hw.hallClosedIsLow;
  hw["hallInvertMask"]  = s.hw.hallInvertMask;
  JsonArray toLid = hw["hallToLid"].to<JsonArray>();
  for (uint8_t i = 0; i < kLidCount; ++i) toLid.add(s.hw.hallToLid[i]);
  hw["ledChannelRed"]     = s.hw.ledChannelRed;
  hw["ledChannelGreen"]   = s.hw.ledChannelGreen;
  hw["ledChannelBlue"]    = s.hw.ledChannelBlue;
  hw["ledBrightness"]     = s.hw.ledBrightness;
  hw["buzzerChannel"]     = s.hw.buzzerChannel;
  hw["buzzerToneHz"]      = s.hw.buzzerToneHz;
  hw["buzzerSelfDrive"]   = s.hw.buzzerSelfDrive;
  hw["vbatDivNum"]        = s.hw.vbatDivNum;
  hw["vbatDivDen"]        = s.hw.vbatDivDen;
  hw["railDivNum"]        = s.hw.railDivNum;
  hw["railDivDen"]        = s.hw.railDivDen;
  hw["buttonDivNum"]      = s.hw.buttonDivNum;
  hw["buttonDivDen"]      = s.hw.buttonDivDen;
  hw["buttonThresholdMv"] = s.hw.buttonThresholdMv;
  hw["statThresholdMv"]   = s.hw.statThresholdMv;
  hw["hallDebounceMs"]    = s.hw.hallDebounceMs;
  hw["buttonDebounceMs"]  = s.hw.buttonDebounceMs;
}

bool settingsFromJson(JsonObjectConst in, Settings& s) {
  bool ch = false;

  JsonObjectConst ui = in["ui"];
  if (!ui.isNull()) {
    ch |= copyString(ui["deviceName"], s.ui.deviceName, sizeof(s.ui.deviceName));
    ch |= copyString(ui["patientName"], s.ui.patientName, sizeof(s.ui.patientName));
    ch |= setBool(ui["showAdvanced"], s.ui.showAdvanced);
  }

  JsonObjectConst wifi = in["wifi"];
  if (!wifi.isNull()) {
    ch |= copyString(wifi["ssid"], s.wifi.ssid, sizeof(s.wifi.ssid));
    ch |= copySecret(wifi["pass"], s.wifi.pass, sizeof(s.wifi.pass));
    ch |= copyString(wifi["hostname"], s.wifi.hostname, sizeof(s.wifi.hostname));
    ch |= setBool(wifi["apFallback"], s.wifi.apFallback);
    ch |= copySecret(wifi["apPassword"], s.wifi.apPassword, sizeof(s.wifi.apPassword));
  }

  JsonObjectConst tm = in["time"];
  if (!tm.isNull()) {
    ch |= copyString(tm["ntp1"], s.time.ntp1, sizeof(s.time.ntp1));
    ch |= copyString(tm["ntp2"], s.time.ntp2, sizeof(s.time.ntp2));
    ch |= copyString(tm["tz"], s.time.tz, sizeof(s.time.tz));
    ch |= setNum(tm["driftPpm"], s.time.driftPpm);
    ch |= setNum(tm["resyncIntervalSec"], s.time.resyncIntervalSec);
  }

  JsonObjectConst nt = in["notify"];
  if (!nt.isNull()) {
    ch |= setBool(nt["ntfyEnabled"], s.notify.ntfyEnabled);
    ch |= copyString(nt["ntfyServer"], s.notify.ntfyServer, sizeof(s.notify.ntfyServer));
    ch |= copyString(nt["ntfyTopic"], s.notify.ntfyTopic, sizeof(s.notify.ntfyTopic));
    ch |= copySecret(nt["ntfyToken"], s.notify.ntfyToken, sizeof(s.notify.ntfyToken));
    ch |= setBool(nt["pushoverEnabled"], s.notify.pushoverEnabled);
    ch |= copySecret(nt["pushoverToken"], s.notify.pushoverToken,
                     sizeof(s.notify.pushoverToken));
    ch |= copySecret(nt["pushoverUser"], s.notify.pushoverUser,
                     sizeof(s.notify.pushoverUser));
    ch |= setBool(nt["webhookEnabled"], s.notify.webhookEnabled);
    ch |= copyString(nt["webhookUrl"], s.notify.webhookUrl, sizeof(s.notify.webhookUrl));
    ch |= setBool(nt["onLidOpen"], s.notify.onLidOpen);
    ch |= setBool(nt["onDoseTaken"], s.notify.onDoseTaken);
    ch |= setBool(nt["onDoseMissed"], s.notify.onDoseMissed);
    ch |= setBool(nt["onDoseDue"], s.notify.onDoseDue);
    ch |= setBool(nt["onUnexpectedOpen"], s.notify.onUnexpectedOpen);
    ch |= setBool(nt["onLowBattery"], s.notify.onLowBattery);
    ch |= setBool(nt["onBoot"], s.notify.onBoot);
    ch |= setBool(nt["onDailySummary"], s.notify.onDailySummary);
    ch |= setNum(nt["dailySummaryMinute"], s.notify.dailySummaryMinute);
    ch |= setNum(nt["quietFromMinute"], s.notify.quietFromMinute);
    ch |= setNum(nt["quietToMinute"], s.notify.quietToMinute);
  }

  JsonObjectConst pw = in["power"];
  if (!pw.isNull()) {
    SleepPolicy policy;
    if (sleepPolicyFromString(pw["policy"].as<const char*>(), &policy) &&
        policy != s.power.policy) {
      s.power.policy = policy;
      ch = true;
    }
    ch |= setNum(pw["awakeAfterEventSec"], s.power.awakeAfterEventSec);
    ch |= setNum(pw["wakeIntervalSec"], s.power.wakeIntervalSec);
    ch |= setNum(pw["lowBatteryMv"], s.power.lowBatteryMv);
    ch |= setNum(pw["criticalBatteryMv"], s.power.criticalBatteryMv);
    ch |= setBool(pw["buzzerOnReminder"], s.power.buzzerOnReminder);
    ch |= setBool(pw["ledOnReminder"], s.power.ledOnReminder);
  }

  JsonObjectConst hw = in["hw"];
  if (!hw.isNull()) {
    ch |= setBool(hw["hallClosedIsLow"], s.hw.hallClosedIsLow);
    ch |= setNum(hw["hallInvertMask"], s.hw.hallInvertMask);
    JsonArrayConst toLid = hw["hallToLid"];
    if (!toLid.isNull()) {
      // A short or long array is the GUI's bug, not the user's: take what fits
      // and let ep::validate repair anything that is not a permutation.
      uint8_t i = 0;
      for (JsonVariantConst v : toLid) {
        if (i >= kLidCount) break;
        ch |= setNum(v, s.hw.hallToLid[i]);
        ++i;
      }
    }
    ch |= setNum(hw["ledChannelRed"], s.hw.ledChannelRed);
    ch |= setNum(hw["ledChannelGreen"], s.hw.ledChannelGreen);
    ch |= setNum(hw["ledChannelBlue"], s.hw.ledChannelBlue);
    ch |= setNum(hw["ledBrightness"], s.hw.ledBrightness);
    ch |= setNum(hw["buzzerChannel"], s.hw.buzzerChannel);
    ch |= setNum(hw["buzzerToneHz"], s.hw.buzzerToneHz);
    ch |= setBool(hw["buzzerSelfDrive"], s.hw.buzzerSelfDrive);
    ch |= setNum(hw["vbatDivNum"], s.hw.vbatDivNum);
    ch |= setNum(hw["vbatDivDen"], s.hw.vbatDivDen);
    ch |= setNum(hw["railDivNum"], s.hw.railDivNum);
    ch |= setNum(hw["railDivDen"], s.hw.railDivDen);
    ch |= setNum(hw["buttonDivNum"], s.hw.buttonDivNum);
    ch |= setNum(hw["buttonDivDen"], s.hw.buttonDivDen);
    ch |= setNum(hw["buttonThresholdMv"], s.hw.buttonThresholdMv);
    ch |= setNum(hw["statThresholdMv"], s.hw.statThresholdMv);
    ch |= setNum(hw["hallDebounceMs"], s.hw.hallDebounceMs);
    ch |= setNum(hw["buttonDebounceMs"], s.hw.buttonDebounceMs);
  }

  return ch;
}

void scheduleToJson(const Schedule& sch, JsonObject out) {
  out["mode"]                = lidModeWire(sch.mode);
  out["sequentialAnchorDay"] = sch.sequentialAnchorDay;
  out["windowBeforeMin"]     = sch.windowBeforeMin;
  out["windowAfterMin"]      = sch.windowAfterMin;
  out["missedGraceMin"]      = sch.missedGraceMin;
  out["remindBeforeMin"]     = sch.remindBeforeMin;

  JsonArray lids = out["lids"].to<JsonArray>();
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    const LidSchedule& ls = sch.lids[lid];
    JsonObject lo = lids.add<JsonObject>();
    lo["lid"]  = lid;
    lo["name"] = ls.name;
    JsonArray doses = lo["doses"].to<JsonArray>();
    const uint8_t n = ls.doseCount <= kMaxDosesPerLid ? ls.doseCount : kMaxDosesPerLid;
    for (uint8_t d = 0; d < n; ++d) doseToJson(ls.doses[d], doses.add<JsonObject>());
  }
}

bool scheduleFromJson(JsonObjectConst in, Schedule& sch) {
  bool ch = false;

  LidMode mode;
  if (lidModeFromWire(in["mode"].as<const char*>(), &mode) && mode != sch.mode) {
    sch.mode = mode;
    ch = true;
  }
  ch |= setNum(in["sequentialAnchorDay"], sch.sequentialAnchorDay);
  ch |= setNum(in["windowBeforeMin"], sch.windowBeforeMin);
  ch |= setNum(in["windowAfterMin"], sch.windowAfterMin);
  ch |= setNum(in["missedGraceMin"], sch.missedGraceMin);
  ch |= setNum(in["remindBeforeMin"], sch.remindBeforeMin);

  JsonArrayConst lids = in["lids"];
  if (lids.isNull()) return ch;

  for (JsonObjectConst lo : lids) {
    // Each entry says which compartment it is. The array does not have to be
    // complete or in order, so an entry without "lid" cannot be placed.
    if (!numeric(lo["lid"])) continue;
    const long long idx = lo["lid"].as<long long>();
    if (idx < 0 || idx >= kLidCount) continue;
    LidSchedule& ls = sch.lids[idx];

    ch |= copyString(lo["name"], ls.name, sizeof(ls.name));

    JsonArrayConst doses = lo["doses"];
    if (doses.isNull()) continue;  // name-only rename, leave the doses alone

    // The doses array replaces this lid's list outright: there is no way to
    // express "delete dose 2" by merging.
    uint8_t n = 0;
    for (JsonObjectConst dj : doses) {
      if (n >= kMaxDosesPerLid) break;
      Dose fresh;
      memset(&fresh, 0, sizeof(fresh));
      fresh.enabled = true;  // posting a dose means wanting it, unless it says not
      doseFromJson(dj, fresh);
      if (!doseEquals(fresh, ls.doses[n])) {
        ls.doses[n] = fresh;
        ch = true;
      }
      ++n;
    }
    if (ls.doseCount != n) {
      ls.doseCount = n;
      ch = true;
    }
  }
  return ch;
}

void doseToJson(const Dose& d, JsonObject out) {
  out["enabled"] = d.enabled;
  // Both spellings go out: "time" for the form field, "minute" for arithmetic.
  char hhmm[6];
  formatHhMm(d.minute, hhmm, sizeof(hhmm));
  out["time"]   = hhmm;
  out["minute"] = d.minute;
  out["label"]  = d.label;

  JsonArray pills = out["pills"].to<JsonArray>();
  const uint8_t n = d.pillCount <= kMaxPillsPerDose ? d.pillCount : kMaxPillsPerDose;
  for (uint8_t i = 0; i < n; ++i) {
    JsonObject po = pills.add<JsonObject>();
    po["name"]  = d.pills[i].name;
    po["dose"]  = d.pills[i].dose;
    po["count"] = d.pills[i].count;
  }
}

bool doseFromJson(JsonObjectConst in, Dose& d) {
  bool ch = setBool(in["enabled"], d.enabled);

  // "time" wins when both are present. The GUI edits the string
  // and sends "minute" back only because it got it from us.
  uint16_t minute   = d.minute;
  bool haveMinute   = false;
  const char* hhmm  = in["time"].as<const char*>();
  if (hhmm && parseHhMm(hhmm, &minute)) {
    haveMinute = true;
  } else if (numeric(in["minute"])) {
    long long raw = in["minute"].as<long long>();
    if (raw < 0) raw = 0;
    if (raw > kMinPerDay - 1) raw = kMinPerDay - 1;
    minute     = static_cast<uint16_t>(raw);
    haveMinute = true;
  }
  if (haveMinute && minute != d.minute) {
    d.minute = minute;
    ch       = true;
  }

  ch |= copyString(in["label"], d.label, sizeof(d.label));

  JsonArrayConst pills = in["pills"];
  if (!pills.isNull()) {
    uint8_t n = 0;
    for (JsonObjectConst pj : pills) {
      if (n >= kMaxPillsPerDose) break;
      Pill p;
      memset(&p, 0, sizeof(p));
      copyString(pj["name"], p.name, sizeof(p.name));
      copyString(pj["dose"], p.dose, sizeof(p.dose));
      p.count = 1;
      setNum(pj["count"], p.count);
      if (p.count == 0) p.count = 1;  // "zero of this pill" is not a dose
      if (!pillEquals(p, d.pills[n])) {
        d.pills[n] = p;
        ch         = true;
      }
      ++n;
    }
    if (d.pillCount != n) {
      d.pillCount = n;
      ch          = true;
    }
  }
  return ch;
}

void eventToJson(const Event& e, JsonObject out, const char* text) {
  // "at" is 0 for anything logged before the first NTP answer; the GUI falls
  // back to uptimeMs for those instead of printing 1970.
  out["at"]           = e.localEpoch;
  out["uptimeMs"]     = e.uptimeMs;
  out["type"]         = toString(e.type);
  out["lid"]          = e.lid;  // kNoLid (255) when it is not about one lid
  out["detail"]       = e.detail;
  out["timeVerified"] = e.timeVerified;
  out["text"]         = text ? text : "";
}

void doseRecordToJson(const DoseRecord& r, JsonObject out) {
  out["scheduledAt"] = r.scheduledLocal;
  out["actualAt"]    = r.actualLocal;
  out["lid"]         = r.lid;
  out["dose"]        = r.dose;
  out["state"]       = toString(r.state);
  out["deltaMin"]    = r.deltaMin;
}

}  // namespace json
}  // namespace ep
