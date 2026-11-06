// docs/API.md is the contract, and two other programs are written against the
// same document: the host simulator and the browser GUI. A field renamed here
// is a field renamed in three places.
//
// Responses stream out as HTTP chunks rather than being built in a buffer,
// because /api/history can be over 10 kB and the loop task has an 8 kB stack.
// Nothing in here waits on the network except the three calls main.cpp already
// allows to take their time, all of them things a human just clicked.
#include "web_server.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

#include "ep_json.h"
#include "ep_schedule.h"
#include "ep_timecalc.h"
#include "indicator.h"
#include "net_wifi.h"
#include "notifier.h"
#include "power_latch.h"
#include "store.h"
#include "timekeeper.h"
#include "web_assets.h"

namespace ep {
namespace {

constexpr uint16_t kPort           = 80;
constexpr uint32_t kClientActiveMs = 15000;
constexpr size_t   kStateEvents    = 12;   // docs/API.md: /api/state carries the 12 newest
constexpr size_t   kMaxScanResults = 24;
// Pattern::SpinRgb is a single pass: each colour slot is lit, then dark, so the
// three flashes are separated and the user can say which came first. The timing
// lives beside the pattern in indicator.h so the reply cannot drift from what
// the LED actually does.

WebServer server(kPort);

uint32_t lastRequestMs = 0;
// webService()'s idea of now, so every handler in one pass agrees on the time
// and nothing has to call millis() again.
uint32_t serviceNowMs = 0;

void touchRequest() {
  lastRequestMs = serviceNowMs != 0 ? serviceNowMs : 1;  // 0 means "never asked"
}

bool numberish(JsonVariantConst v) {
  return v.is<long long>() || v.is<double>() || v.is<bool>();
}

// Reads a number out of a request body with a default and hard bounds. Posting
// nonsense must never reach the hardware, so clamping happens here rather than
// being left to whatever the driver does with 70000 ms.
long clampArg(JsonVariantConst v, long fallback, long lo, long hi) {
  long value = numberish(v) ? static_cast<long>(v.as<long long>()) : fallback;
  if (value < lo) value = lo;
  if (value > hi) value = hi;
  return value;
}

// ---- responses --------------------------------------------------------------

// serializeJson() writes a byte at a time and WiFiClient::write(uint8_t) is a
// lwip call per byte, so responses go out through a small buffer as HTTP
// chunks. This also means no response is ever assembled in RAM: a full dose
// history is over 10 kB and the loop task has an 8 kB stack.
class ChunkSink : public Print {
 public:
  size_t write(uint8_t c) override {
    buf_[used_++] = c;
    if (used_ == sizeof(buf_)) flushBuffer();
    return 1;
  }

  size_t write(const uint8_t* data, size_t len) override {
    size_t left = len;
    while (left > 0) {
      const size_t room = sizeof(buf_) - used_;
      const size_t take = left < room ? left : room;
      memcpy(buf_ + used_, data, take);
      used_ += take;
      data += take;
      left -= take;
      if (used_ == sizeof(buf_)) flushBuffer();
    }
    return len;
  }

  // Closes the chunked body. Exactly once per response.
  void finish() {
    flushBuffer();
    server.sendContent("", 0);
  }

 private:
  void flushBuffer() {
    if (used_ == 0) return;
    server.sendContent(reinterpret_cast<const char*>(buf_), used_);
    used_ = 0;
  }

  uint8_t buf_[512];
  size_t  used_ = 0;
};

void sendJson(int code, const JsonDocument& doc) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(code, "application/json", "");
  ChunkSink sink;
  serializeJson(doc, sink);
  sink.finish();
}

// The error shape from docs/API.md, built with snprintf because an error reply
// must work even when a JsonDocument allocation is what went wrong.
//
// Messages must not contain a double quote - nothing here escapes them - so
// they use single quotes, and no caller ever echoes request content back.
void sendErrorMsg(int code, const char* message) {
  char body[192];
  snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

// False means the client has already been answered.
bool readBody(JsonDocument& doc) {
  const String raw = server.arg("plain");
  if (raw.length() == 0) {
    sendErrorMsg(400, "expected a JSON body");
    return false;
  }
  const DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    char message[96];
    snprintf(message, sizeof(message), "could not parse the JSON body: %s", err.c_str());
    sendErrorMsg(400, message);
    return false;
  }
  if (!doc.is<JsonObject>()) {
    sendErrorMsg(400, "the body must be a JSON object");
    return false;
  }
  return true;
}

// ---- the GUI itself, straight out of flash ----------------------------------

void handleAsset() {
  touchRequest();
  const WebAsset* asset = findWebAsset(server.uri().c_str());
  if (!asset) {  // only reachable if a route outlives its table row
    sendErrorMsg(404, "no such asset");
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  // The GUI ships inside the firmware, so a cached copy from a previous flash
  // is a GUI talking to an API it does not match. One extra transfer per reload
  // is much cheaper than that.
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, asset->contentType, reinterpret_cast<PGM_P>(asset->gzip),
                asset->gzipLen);
}

void handleNotFound() {
  touchRequest();
  const String uri = server.uri();
  if (uri.startsWith("/api/")) {
    sendErrorMsg(404, "no such endpoint");
    return;
  }
  // Anything else lands on the GUI, which makes a captive-portal redirect or a
  // half-remembered bookmark do the right thing.
  server.sendHeader("Location", "/");
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

// ---- /api/state helpers -----------------------------------------------------

const char* netModeWire(NetMode m) {
  switch (m) {
    case NetMode::Off:         return "off";
    case NetMode::Connecting:  return "connecting";
    case NetMode::Station:     return "sta";
    case NetMode::AccessPoint: return "ap";
    case NetMode::Failed:      return "failed";
  }
  return "off";
}

const char* resetReasonWire() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "powerOn";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interruptWdt";
    case ESP_RST_TASK_WDT:  return "taskWdt";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepSleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// One lid can hold four doses, and the day card has room for one word, so the
// worst state wins: a missed morning matters more than a taken evening.
const DoseState kStatesWorstFirst[] = {DoseState::Missed, DoseState::Pending, DoseState::Late,
                                       DoseState::Early,  DoseState::Skipped, DoseState::Taken};
constexpr size_t kStateRankCount = sizeof(kStatesWorstFirst) / sizeof(kStatesWorstFirst[0]);

const char* lidTodayState(const Schedule& sch, uint8_t lid, uint32_t nowLocal) {
  if (nowLocal == 0 || !lidActiveOnDay(sch, lid, dayIndex(nowLocal))) return "none";

  DoseInstance inst[kMaxDosesPerLid];
  const size_t n =
      doseInstancesForLidOnDay(sch, lid, dayIndex(nowLocal), inst, kMaxDosesPerLid);
  if (n == 0) return "none";

  size_t best       = kStateRankCount;
  bool   anyWatched = false;
  for (size_t i = 0; i < n; ++i) {
    // The boot floor, same rule the adherence counts use: a dose scheduled
    // before this power cycle was never this box's to judge, so it is not even
    // pending. Without this the day card would sit on "pending" forever, since
    // sweepMissed will not resolve it either.
    if (inst[i].localEpoch < g.bootFloorLocal) continue;
    anyWatched = true;
    const DoseRecord* rec = g.adherence.find(inst[i].localEpoch, lid, inst[i].dose);
    const DoseState   st  = rec ? rec->state : DoseState::Pending;
    for (size_t rank = 0; rank < best; ++rank) {
      if (kStatesWorstFirst[rank] == st) {
        best = rank;
        break;
      }
    }
  }
  if (!anyWatched) return "none";
  return best < kStateRankCount ? toString(kStatesWorstFirst[best]) : "pending";
}

// ep::nextDose() answers for the whole tray; each day card wants its own
// compartment's next moment. Two weeks ahead covers both lid modes.
uint32_t nextAtForLid(const Schedule& sch, uint8_t lid, uint32_t nowLocal) {
  if (nowLocal == 0) return 0;
  const uint32_t today = dayIndex(nowLocal);
  for (uint32_t day = today; day < today + 14; ++day) {
    if (!lidActiveOnDay(sch, lid, day)) continue;
    DoseInstance inst[kMaxDosesPerLid];
    const size_t n = doseInstancesForLidOnDay(sch, lid, day, inst, kMaxDosesPerLid);
    for (size_t i = 0; i < n; ++i) {
      if (inst[i].valid && inst[i].localEpoch > nowLocal) return inst[i].localEpoch;
    }
  }
  return 0;
}

// Adherence percentages walk up to a month of dose instances, which is far too
// much to redo for a poll that arrives every second. The log only changes when
// a lid opens or a human overrides something, so a short-lived snapshot is
// indistinguishable from the real thing.
struct AdherenceSnapshot {
  uint32_t               atMs;
  uint32_t               dayIdx;
  size_t                 records;
  uint8_t                pct7d;
  uint8_t                pct30d;
  AdherenceLog::DayStats stats;
};

AdherenceSnapshot adhCache{};
bool              adhCacheValid = false;
constexpr uint32_t kAdherenceCacheMs = 5000;

void invalidateAdherenceCache() { adhCacheValid = false; }

const AdherenceSnapshot& adherenceSnapshot(uint32_t nowMs, uint32_t nowLocal) {
  const uint32_t today = nowLocal != 0 ? dayIndex(nowLocal) : 0;
  const bool stale = !adhCacheValid || adhCache.dayIdx != today ||
                     adhCache.records != g.adherence.size() ||
                     (uint32_t)(nowMs - adhCache.atMs) >= kAdherenceCacheMs;
  if (stale) {
    const Schedule& sch = g.settings.schedule;
    adhCache.atMs    = nowMs;
    adhCache.dayIdx  = today;
    adhCache.records = g.adherence.size();
    if (nowLocal != 0) {
      // bootFloorLocal keeps a cold boot at 10:00 from reporting the 08:00 dose
      // as missed: nothing scheduled before this device was powered on was ever
      // its business to watch. Every adherence call in the app passes it.
      adhCache.pct7d  = g.adherence.adherencePercent(sch, nowLocal, 7, g.bootFloorLocal);
      adhCache.pct30d = g.adherence.adherencePercent(sch, nowLocal, 30, g.bootFloorLocal);
      adhCache.stats  = g.adherence.today(sch, nowLocal, g.bootFloorLocal);
    } else {
      // Nothing was due, because we do not know when now is.
      adhCache.pct7d  = 100;
      adhCache.pct30d = 100;
      adhCache.stats  = AdherenceLog::DayStats{0, 0, 0, 0};
    }
    adhCacheValid = true;
  }
  return adhCache;
}

void pillsToJson(const Dose& d, JsonArray out) {
  const uint8_t n = d.pillCount <= kMaxPillsPerDose ? d.pillCount : kMaxPillsPerDose;
  for (uint8_t i = 0; i < n; ++i) {
    JsonObject po = out.add<JsonObject>();
    po["name"]  = d.pills[i].name;
    po["dose"]  = d.pills[i].dose;
    po["count"] = d.pills[i].count;
  }
}

// ---- /api/state -------------------------------------------------------------

void handleState() {
  touchRequest();
  const Schedule& sch      = g.settings.schedule;
  const uint32_t  nowLocal = g.nowLocal();
  const uint32_t  today    = nowLocal != 0 ? dayIndex(nowLocal) : 0;

  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"]           = true;
  root["now"]          = nowLocal;
  root["tzOffsetSec"]  = g.time.tzOffsetSec;
  root["timeVerified"] = g.time.verified;
  // The GUI needs this to tell "nobody opened it" from "the box was not even
  // powered then": adherence already skips instances before the floor, so
  // without it the Today list shows a dose as untaken that the counter beside
  // it does not count. 0 means no floor yet, i.e. the clock is unverified.
  root["watchingSince"] = g.bootFloorLocal;
  root["uptimeS"]      = serviceNowMs / 1000;
  root["fw"]           = kFirmwareVersion;
  root["chip"]         = ESP.getChipModel();
  root["simulated"]    = false;

  // One pass over the log instead of seven searches: at(0) is the newest, so
  // the first hit per lid is that lid's last open. localEpoch 0 is a legitimate
  // stored value - "the clock was not trustworthy when this happened", see
  // ep_events.h - so "already found one" has to live apart from the timestamp,
  // or an older open with a real stamp overwrites the newest one and the API
  // names the wrong event. The first sync back-fills these zeroes, so 0 only
  // ever goes out for an open that genuinely has no verified time yet.
  uint32_t lastOpened[kLidCount]   = {0};
  bool     haveLastOpen[kLidCount] = {false};
  for (size_t i = 0; i < g.events.size(); ++i) {
    const Event& e = g.events.at(i);
    if (e.type != EventType::LidOpened || e.lid >= kLidCount) continue;
    if (haveLastOpen[e.lid]) continue;
    haveLastOpen[e.lid] = true;
    lastOpened[e.lid]   = e.localEpoch;
  }

  JsonArray lids = root["lids"].to<JsonArray>();
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    JsonObject lo = lids.add<JsonObject>();
    lo["lid"]          = lid;
    lo["name"]         = sch.lids[lid].name;
    lo["open"]         = g.halls.lidOpen(lid);
    lo["lastOpenedAt"] = lastOpened[lid];
    lo["activeToday"]  = nowLocal != 0 && lidActiveOnDay(sch, lid, today);
    lo["doseCount"]    = enabledDoseCount(sch, lid);
    lo["todayState"]   = lidTodayState(sch, lid, nowLocal);
    lo["nextAt"]       = nextAtForLid(sch, lid, nowLocal);
  }

  // Always the full shape, valid or not, so the GUI never has to guess which
  // keys exist.
  const DoseInstance next = nowLocal != 0 ? nextDose(sch, nowLocal) : DoseInstance{};
  JsonObject         nd   = root["nextDose"].to<JsonObject>();
  const bool         haveNext = next.valid && next.lid < kLidCount &&
                                next.dose < kMaxDosesPerLid;
  nd["valid"] = haveNext;
  nd["at"]    = haveNext ? next.localEpoch : 0;
  nd["inMin"] = haveNext ? minuteDelta(nowLocal, next.localEpoch) : 0;
  nd["lid"]   = haveNext ? next.lid : kNoLid;
  nd["dose"]  = haveNext ? next.dose : 0;
  nd["lidName"] = haveNext ? sch.lids[next.lid].name : "";
  nd["label"]   = haveNext ? sch.lids[next.lid].doses[next.dose].label : "";
  JsonArray pills = nd["pills"].to<JsonArray>();
  if (haveNext) pillsToJson(sch.lids[next.lid].doses[next.dose], pills);

  const PowerReading& p  = g.analog.reading();
  JsonObject          po = root["power"].to<JsonObject>();
  po["vbatMv"]        = p.vbatMv;
  po["percent"]       = p.batteryPercent;
  po["usbMv"]         = p.usbMv;
  po["coilMv"]        = p.coilMv;
  po["usbPresent"]    = p.usbPresent;
  po["coilPresent"]   = p.coilPresent;
  po["charging"]      = p.charging;
  po["buttonPressed"] = p.buttonPressed;
  po["lowBattery"]    = p.valid && p.vbatMv != 0 &&
                        p.vbatMv < g.settings.power.lowBatteryMv;

  JsonObject wo = root["wifi"].to<JsonObject>();
  wo["mode"]     = netModeWire(g.net.mode);
  wo["ssid"]     = g.net.ssid;
  wo["rssi"]     = g.net.rssi;
  wo["ip"]       = g.net.ip;
  wo["hostname"] = g.settings.wifi.hostname;

  const AdherenceSnapshot& adh = adherenceSnapshot(serviceNowMs, nowLocal);
  JsonObject               ao  = root["adherence"].to<JsonObject>();
  ao["pct7d"]        = adh.pct7d;
  ao["pct30d"]       = adh.pct30d;
  ao["dueToday"]     = adh.stats.due;
  ao["takenToday"]   = adh.stats.taken;
  ao["missedToday"]  = adh.stats.missed;
  ao["pendingToday"] = adh.stats.pending;

  const HallState& h  = g.halls.state();
  JsonObject       so = root["sensors"].to<JsonObject>();
  so["rawMask"]    = h.rawMask;
  so["openMask"]   = h.openMask;
  so["gndRefOk"]   = h.gndRefOk;
  so["wakeUpHigh"] = wakeUpLineHigh();

  JsonArray evs = root["events"].to<JsonArray>();
  // describeEvent's longest line is around sixty characters; twelve of these is
  // half of what /api/state is allowed to weigh, so it is worth being tight.
  char         text[80];
  const size_t count = g.events.size() < kStateEvents ? g.events.size() : kStateEvents;
  for (size_t i = 0; i < count; ++i) {
    const Event& e = g.events.at(i);
    g.describeEvent(e, text, sizeof(text));
    json::eventToJson(e, evs.add<JsonObject>(), text);
  }

  sendJson(200, doc);
}

// ---- /api/settings ----------------------------------------------------------

void sendSettings(bool clamped) {
  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"] = true;
  json::settingsToJson(g.settings, root);
  root["changed"] = clamped;
  sendJson(200, doc);
}

void handleSettingsGet() {
  touchRequest();
  sendSettings(false);
}

void handleSettingsPost() {
  touchRequest();
  JsonDocument in;
  if (!readBody(in)) return;

  char tzBefore[sizeof(g.settings.time.tz)];
  snprintf(tzBefore, sizeof(tzBefore), "%s", g.settings.time.tz);

  JsonObjectConst body   = in.as<JsonObjectConst>();
  bool            merged = json::settingsFromJson(body, g.settings);
  // Not in docs/API.md, but the simulator accepts it and a restore-from-backup
  // wants to post the whole tree in one go.
  JsonObjectConst schedule = body["schedule"];
  if (!schedule.isNull()) {
    merged |= json::scheduleFromJson(schedule, g.settings.schedule);
    invalidateAdherenceCache();
  }
  const bool clamped = validate(g.settings);
  if (merged || clamped) {
    // store.cpp decides when to actually write flash; a handler must not.
    g.markSettingsDirty();
    g.logEvent(EventType::ConfigChanged);
  }
  // There is no RTC, so the clock is a POSIX TZ string plus NTP. Applying a new
  // zone here means the next poll already renders in it instead of waiting for
  // a reboot.
  if (strcmp(tzBefore, g.settings.time.tz) != 0) timeApplyTz(g.settings.time.tz);

  sendSettings(clamped);
}

// ---- /api/schedule ----------------------------------------------------------

void sendSchedule(bool clamped) {
  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"] = true;
  json::scheduleToJson(g.settings.schedule, root);
  root["changed"] = clamped;
  sendJson(200, doc);
}

void handleScheduleGet() {
  touchRequest();
  sendSchedule(false);
}

void handleSchedulePost() {
  touchRequest();
  JsonDocument in;
  if (!readBody(in)) return;

  const bool merged  = json::scheduleFromJson(in.as<JsonObjectConst>(), g.settings.schedule);
  const bool clamped = validate(g.settings);
  if (merged || clamped) {
    g.markSettingsDirty();
    g.logEvent(EventType::ConfigChanged);
    // Yesterday's answers were about a schedule that no longer exists.
    invalidateAdherenceCache();
  }
  sendSchedule(clamped);
}

void handleScheduleBulk() {
  touchRequest();
  JsonDocument in;
  if (!readBody(in)) return;
  JsonObjectConst body = in.as<JsonObjectConst>();

  if (!numberish(body["lidMask"])) {
    sendErrorMsg(400, "lidMask must be a number, bit n selecting lid n");
    return;
  }
  // 0xFF means "append into the first free slot", so it is the default rather
  // than an out-of-range value to reject. Anything else out of range is a client
  // bug, and the simulator 400s on it, so this does too.
  const long long rawMask = body["lidMask"].as<long long>();
  if (rawMask < 0 || rawMask > 0x7F) {
    sendErrorMsg(400, "lidMask must be 0..127");
    return;
  }
  const uint8_t mask      = static_cast<uint8_t>(rawMask);
  const uint8_t doseIndex = static_cast<uint8_t>(clampArg(body["doseIndex"], 0xFF, 0, 0xFF));
  const char*   op        = body["op"] | "";
  if (doseIndex != 0xFF && doseIndex >= kMaxDosesPerLid) {
    sendErrorMsg(400, "doseIndex must be 0..3 or 255");
    return;
  }

  Schedule& sch     = g.settings.schedule;
  uint8_t   changed = 0;
  if (strcmp(op, "set") == 0) {
    JsonObjectConst doseJson = body["dose"];
    if (doseJson.isNull()) {
      sendErrorMsg(400, "op 'set' needs a 'dose' object");
      return;
    }
    Dose dose;
    memset(&dose, 0, sizeof(dose));
    dose.enabled = true;
    json::doseFromJson(doseJson, dose);
    changed = applyDoseToLids(sch, mask, doseIndex, dose);
  } else if (strcmp(op, "remove") == 0) {
    // 0xFF only ever means "the first free slot", which is an append. It must
    // not reach removeDoseFromLids, whose 0xFF branch empties the whole lid -
    // the GUI's default slot would wipe a week of prescriptions in one click.
    if (doseIndex == 0xFF) {
      sendErrorMsg(400, "op 'remove' needs doseIndex 0..3; use op 'clear' to wipe a lid");
      return;
    }
    changed = removeDoseFromLids(sch, mask, doseIndex);
  } else if (strcmp(op, "clear") == 0) {
    for (uint8_t lid = 0; lid < kLidCount; ++lid) {
      if ((mask & (1u << lid)) == 0 || sch.lids[lid].doseCount == 0) continue;
      sch.lids[lid].doseCount = 0;
      ++changed;
    }
  } else {
    sendErrorMsg(400, "op must be set, remove or clear");
    return;
  }

  const bool clamped = validate(g.settings);
  if (changed != 0 || clamped) {
    g.markSettingsDirty();
    g.logEvent(EventType::ConfigChanged, kNoLid, static_cast<int16_t>(changed));
    invalidateAdherenceCache();
  }

  JsonDocument doc;
  doc["ok"]      = true;
  doc["changed"] = changed;
  sendJson(200, doc);
}

// ---- /api/events and /api/history -------------------------------------------

unsigned long queryNumber(const char* name, unsigned long fallback) {
  if (!server.hasArg(name)) return fallback;
  const String raw = server.arg(name);
  if (raw.length() == 0) return fallback;
  char*               end   = nullptr;
  const unsigned long value = strtoul(raw.c_str(), &end, 10);
  if (end == raw.c_str()) return fallback;  // not a number at all
  return value;
}

void handleEvents() {
  touchRequest();
  unsigned long limit = queryNumber("limit", 64);
  if (limit == 0) limit = 1;
  if (limit > EventLog::kCapacity) limit = EventLog::kCapacity;
  const uint32_t since = static_cast<uint32_t>(queryNumber("since", 0));

  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"]        = true;
  JsonArray evs     = root["events"].to<JsonArray>();

  char          text[112];
  unsigned long written = 0;
  for (size_t i = 0; i < g.events.size() && written < limit; ++i) {
    const Event& e = g.events.at(i);
    // An event logged before the first sync has localEpoch 0 and cannot be
    // placed on a timeline, so a "since" query has to skip it.
    if (since != 0 && e.localEpoch < since) continue;
    g.describeEvent(e, text, sizeof(text));
    json::eventToJson(e, evs.add<JsonObject>(), text);
    ++written;
  }
  sendJson(200, doc);
}

void handleHistory() {
  touchRequest();
  unsigned long days = queryNumber("days", 14);
  if (days == 0) days = 1;
  if (days > 365) days = 365;

  const uint32_t nowLocal = g.nowLocal();
  uint32_t       cutoff   = 0;
  if (nowLocal != 0) {
    const uint32_t span = static_cast<uint32_t>(days) * kSecPerDay;
    cutoff              = nowLocal > span ? nowLocal - span : 0;
  }

  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"]        = true;
  JsonArray recs    = root["records"].to<JsonArray>();
  // Oldest first: the adherence grid reads left to right.
  for (size_t i = 0; i < g.adherence.size(); ++i) {
    const DoseRecord& r = g.adherence.at(i);
    if (r.scheduledLocal < cutoff) continue;
    json::doseRecordToJson(r, recs.add<JsonObject>());
  }
  sendJson(200, doc);
}

// ---- /api/action ------------------------------------------------------------

void handleAction() {
  touchRequest();
  JsonDocument in;
  if (!readBody(in)) return;
  JsonObjectConst body   = in.as<JsonObjectConst>();
  const char*     action = body["action"] | "";

  JsonDocument doc;
  JsonObject   root   = doc.to<JsonObject>();
  root["ok"]          = true;
  JsonObject   result = root["result"].to<JsonObject>();

  if (strcmp(action, "beep") == 0) {
    const long onMs    = clampArg(body["onMs"], 120, 10, 5000);
    const long repeats = clampArg(body["repeats"], 1, 1, 20);
    g.led.beep(static_cast<uint16_t>(onMs), static_cast<uint8_t>(repeats));
    result["onMs"]    = onMs;
    result["repeats"] = repeats;
    result["toneHz"]  = g.settings.hw.buzzerToneHz;

  } else if (strcmp(action, "led") == 0) {
    const uint8_t r  = static_cast<uint8_t>(clampArg(body["r"], 0, 0, 255));
    const uint8_t gr = static_cast<uint8_t>(clampArg(body["g"], 0, 0, 255));
    const uint8_t b  = static_cast<uint8_t>(clampArg(body["b"], 0, 0, 255));
    const long    ms = clampArg(body["ms"], 500, 0, 60000);
    g.led.setColor(r, gr, b);
    g.led.playPattern(ms == 0 ? Pattern::Off : Pattern::Solid,
                      static_cast<uint32_t>(ms));
    result["r"]  = r;
    result["g"]  = gr;
    result["b"]  = b;
    result["ms"] = ms;

  } else if (strcmp(action, "identifyLed") == 0) {
    // Fire and forget: the pattern runs out of the main loop, so the reply goes
    // back now and the browser is not holding a socket open for 4.5 s.
    g.led.playPattern(Pattern::SpinRgb, kIdentifyMs);
    // The pattern drives colour slot 0, then 1, then 2, and each slot goes to
    // the mux channel the settings currently claim. So "channels" is the slot
    // order and "configured" is the claim being tested: whichever colour the
    // user actually sees first belongs to configured.red.
    JsonArray channels = result["channels"].to<JsonArray>();
    channels.add(0);
    channels.add(1);
    channels.add(2);
    result["msEach"] = kIdentifySlotMs;
    result["ms"]     = kIdentifyMs;
    JsonObject configured = result["configured"].to<JsonObject>();
    configured["red"]   = g.settings.hw.ledChannelRed;
    configured["green"] = g.settings.hw.ledChannelGreen;
    configured["blue"]  = g.settings.hw.ledChannelBlue;

  } else if (strcmp(action, "testNotify") == 0) {
    const char* target = body["target"] | "all";
    if (strcmp(target, "ntfy") != 0 && strcmp(target, "pushover") != 0 &&
        strcmp(target, "webhook") != 0 && strcmp(target, "all") != 0) {
      sendErrorMsg(400, "target must be ntfy, pushover, webhook or all");
      return;
    }
    // Blocking on purpose: the GUI asked for an HTTP status, and there is no
    // status to report until the push has been through TLS. One of the three
    // calls main.cpp allows to take its time.
    const NotifyOutcome outcome = notifyTest(target);
    result["target"]   = target;
    result["sent"]     = outcome.ok;
    result["httpCode"] = outcome.httpCode;
    result["detail"]   = outcome.detail;

  } else if (strcmp(action, "syncTime") == 0) {
    // There is no RTC, so this button is the user's only way to say "the clock
    // looks wrong, go and check". It blocks - the third of the three calls
    // main.cpp allows to - because an answer of "requested" tells nobody
    // whether the clock is right now. A failure leaves the loop to keep trying.
    //
    // before/after go out as local epochs like every other timestamp in the API
    // (docs/API.md), which is what the simulator already reports; lastSyncUtc is
    // the one UTC field and says so in its name. correctionSec stays a
    // UTC-to-UTC delta, so a DST change landing inside the sync window cannot be
    // mistaken for an hour of drift - which means the offset has to be captured
    // before the blocking call, because applySync() refreshes it.
    const uint32_t beforeUtc   = g.nowUtc();
    const int32_t  beforeTzOff = g.time.tzOffsetSec;
    const bool     synced      = timeSyncBlocking(4000);
    if (!synced) g.forceTimeSync = true;
    const uint32_t afterUtc = g.nowUtc();
    result["synced"]        = synced;
    result["before"]        = beforeUtc != 0
                                  ? (uint32_t)((int64_t)beforeUtc + (int64_t)beforeTzOff)
                                  : 0u;
    result["after"]         = g.nowLocal();
    // Meaningless when the clock had never been set, which is the common case
    // on a cold boot without Wi-Fi.
    result["correctionSec"] = beforeUtc != 0 ? static_cast<int32_t>(afterUtc - beforeUtc) : 0;
    result["verified"]      = g.time.verified;
    result["lastSyncUtc"]   = g.time.lastSyncUtc;
    result["syncCount"]     = g.time.syncCount;
    result["driftPpm"]      = g.time.measuredDriftPpm;

  } else if (strcmp(action, "markTaken") == 0 || strcmp(action, "markSkipped") == 0) {
    if (!numberish(body["scheduledAt"]) || !numberish(body["lid"])) {
      sendErrorMsg(400, "markTaken and markSkipped need scheduledAt, lid and dose");
      return;
    }
    // markState creates the record it is told about, so a nonsense timestamp
    // would become a nonsense row in the adherence log forever.
    const long long scheduledAt = body["scheduledAt"].as<long long>();
    const long      lid         = body["lid"].as<long>();
    const long      dose        = body["dose"] | 0L;
    if (scheduledAt <= 0 || scheduledAt > 4102444800LL) {
      sendErrorMsg(400, "scheduledAt must be a local epoch");
      return;
    }
    if (lid < 0 || lid >= kLidCount || dose < 0 || dose >= kMaxDosesPerLid) {
      sendErrorMsg(400, "lid must be 0..6 and dose 0..3");
      return;
    }
    const DoseState state =
        strcmp(action, "markTaken") == 0 ? DoseState::Taken : DoseState::Skipped;
    const uint32_t scheduledLocal = static_cast<uint32_t>(scheduledAt);
    const bool     updated =
        g.adherence.markState(scheduledLocal, static_cast<uint8_t>(lid),
                              static_cast<uint8_t>(dose), state, g.nowLocal());
    if (updated) invalidateAdherenceCache();
    const DoseRecord* rec = g.adherence.find(scheduledLocal, static_cast<uint8_t>(lid),
                                             static_cast<uint8_t>(dose));
    if (rec != nullptr) json::doseRecordToJson(*rec, result);
    result["updated"] = updated;

  } else if (strcmp(action, "clearEvents") == 0) {
    g.events.clear();
    result["cleared"] = "events";

  } else if (strcmp(action, "clearHistory") == 0) {
    g.adherence.clear();
    invalidateAdherenceCache();
    result["cleared"] = "history";

  } else if (strcmp(action, "factoryReset") == 0) {
    if (!body["confirm"].as<bool>()) {
      sendErrorMsg(400, "factoryReset needs confirm: true");
      return;
    }
    // Flags only: the loop does the deed, which is the only way this reply ever
    // reaches the browser.
    g.factoryResetRequested = true;
    result["reset"]         = true;
    result["rebooting"]     = true;

  } else if (strcmp(action, "reboot") == 0) {
    g.rebootRequested   = true;
    result["rebooting"] = true;

  } else if (strcmp(action, "powerOff") == 0) {
    g.powerOffRequested = true;
    result["latched"]   = false;

  } else if (strcmp(action, "calibrateHalls") == 0) {
    // Done here rather than through a flag because the GUI re-reads
    // /api/settings the moment this reply lands and has to see the new
    // polarity. It costs about 2 ms: eight passes of a 250 us scan.
    g.led.stop();  // the indicator must not be holding the address lines
    // Channel 7 is a 100k to GND, so it must read low. If it does not, the
    // address lines or U20 are dead and every hall reading this scan returns is
    // fiction: learning a polarity from it would write nonsense to NVS as if it
    // were a discovered fact, and hide the fault while doing it. Same condition
    // the bring-up tool warns about, except nobody presses this button with a
    // multimeter in the other hand.
    const bool gndRefOk = g.bus.groundReferenceOk();
    if (!gndRefOk) {
      g.bus.park();
      sendErrorMsg(400, "channel 7 ground reference did not read LOW - the mux or the "
                        "address lines are dead, fix that before calibrating");
      return;
    }
    // Sensors that disagree are not a polarity to learn - they are a lid left
    // open, a missing magnet or a dead sensor - so calibrateClosed() writes
    // nothing in that case and we have to say why rather than report the old
    // settings back as a fresh result. A genuinely half-reworked tray is still
    // reachable with {"force": true}.
    const bool    force = body["force"] | false;
    bool          mixed = false;
    const uint8_t raw   = g.halls.calibrateClosed(&g.settings.hw, 8, force, &mixed);
    if (mixed && !force) {
      g.bus.park();
      char msg[72];
      snprintf(msg, sizeof msg,
               "close every lid first (sensors disagree, raw mask 0x%02X)", raw);
      sendErrorMsg(400, msg);
      return;
    }
    // Polarity may have just inverted, so the debounce state describes a world
    // that no longer exists; the loop re-settles it over the next debounce
    // window without blocking here.
    g.halls.resetDebounce(serviceNowMs);
    g.bus.park();
    validate(g.settings);
    g.markSettingsDirty();
    g.logEvent(EventType::ConfigChanged, kNoLid, static_cast<int16_t>(raw));
    result["rawMask"]         = raw;
    result["hallClosedIsLow"] = g.settings.hw.hallClosedIsLow;
    result["hallInvertMask"]  = g.settings.hw.hallInvertMask;
    // The GUI prints the whole result object, so this is the visible proof that
    // the polarity was learnt on a live bus.
    result["gndRefOk"]        = gndRefOk;

  } else if (strcmp(action, "stayAwake") == 0) {
    const long seconds = clampArg(body["seconds"], 300, 1, 86400);
    g.keepAwakeFor(static_cast<uint32_t>(seconds));
    const uint32_t nowLocal = g.nowLocal();
    result["untilS"]  = seconds;
    result["untilAt"] = nowLocal != 0 ? nowLocal + static_cast<uint32_t>(seconds) : 0;

  } else {
    sendErrorMsg(400, "unknown action");
    return;
  }

  sendJson(200, doc);
}

// ---- /api/info, /api/scan, /api/wifi, /api/sim ------------------------------

void handleInfo() {
  touchRequest();
  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"]           = true;
  root["fw"]           = kFirmwareVersion;
  root["buildDate"]    = buildStamp();
  root["chip"]         = ESP.getChipModel();
  root["cores"]        = ESP.getChipCores();
  root["flashMb"]      = ESP.getFlashChipSize() / (1024UL * 1024UL);
  root["heapFree"]     = ESP.getFreeHeap();
  root["heapMin"]      = ESP.getMinFreeHeap();
  root["macAddress"]   = WiFi.macAddress();
  root["resetReason"]  = resetReasonWire();
  root["wakeReason"]   = toString(g.wakeReason);
  root["sdkVersion"]   = ESP.getSdkVersion();
  root["fsUsedBytes"]  = storeFsUsedBytes();
  root["fsTotalBytes"] = storeFsTotalBytes();
  sendJson(200, doc);
}

void handleScan() {
  touchRequest();
  // One of the three deliberately blocking GUI calls (see main.cpp): a station
  // scan cannot be done any other way, and nothing is due while it runs.
  ScanResult   found[kMaxScanResults];
  const size_t n = netScan(found, kMaxScanResults);

  JsonDocument doc;
  JsonObject   root = doc.to<JsonObject>();
  root["ok"]        = true;
  JsonArray nets    = root["networks"].to<JsonArray>();
  for (size_t i = 0; i < n; ++i) {
    JsonObject o = nets.add<JsonObject>();
    o["ssid"]   = found[i].ssid;
    o["rssi"]   = found[i].rssi;
    o["secure"] = found[i].secure;
  }
  sendJson(200, doc);
}

void handleWifi() {
  touchRequest();
  JsonDocument in;
  if (!readBody(in)) return;
  JsonObjectConst body = in.as<JsonObjectConst>();

  const char* ssid = body["ssid"] | "";
  if (ssid[0] == '\0') {
    sendErrorMsg(400, "ssid must not be empty");
    return;
  }
  // Same convention as json::copySecret, which /api/settings uses on this very
  // field: an absent or non-string "pass", like the placeholder, means "keep the
  // stored one" (docs/API.md: anything absent is left alone). An explicit "" is
  // still a clear, which is how an open network is joined. The copy is so that
  // netSetCredentials does not strlcpy the stored password over itself.
  char        keep[sizeof g.settings.wifi.pass];
  const char* pass = body["pass"].as<const char*>();
  if (pass == nullptr || strcmp(pass, json::kSecretPlaceholder) == 0) {
    strlcpy(keep, g.settings.wifi.pass, sizeof keep);
    pass = keep;
  }

  if (!netSetCredentials(ssid, pass)) {
    sendErrorMsg(400, "could not store those credentials");
    return;
  }
  g.markSettingsDirty();

  JsonDocument doc;
  doc["ok"]         = true;
  doc["connecting"] = true;
  sendJson(200, doc);
}

void handleSim() {
  touchRequest();
  // The simulator implements this; on real hardware there is nothing to fake.
  sendErrorMsg(404, "no simulator on the firmware");
}

}  // namespace

void webBegin() {
  // Every asset route comes from the generated table, so adding a file to the
  // GUI means editing only tools/gen_web_assets.py.
  for (size_t i = 0; i < kWebAssetCount; ++i) {
    server.on(kWebAssets[i].path, HTTP_GET, handleAsset);
  }

  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/schedule", HTTP_GET, handleScheduleGet);
  server.on("/api/schedule", HTTP_POST, handleSchedulePost);
  server.on("/api/schedule/bulk", HTTP_POST, handleScheduleBulk);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/sim", handleSim);

  server.onNotFound(handleNotFound);
  server.begin();
}

void webService(uint32_t nowMs) {
  serviceNowMs = nowMs;
  server.handleClient();
}

bool webClientActive(uint32_t nowMs) {
  if (lastRequestMs == 0) return false;
  return (uint32_t)(nowMs - lastRequestMs) < kClientActiveMs;
}

uint32_t webLastRequestMs() { return lastRequestMs; }

}  // namespace ep
