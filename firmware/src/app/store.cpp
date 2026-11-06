// Persistence: three LittleFS files, plus one small NVS key for the clock.
//
// Every file is raw structs behind a header that records the magic, the format
// version and sizeof(record). A struct layout change therefore shows up as a
// header mismatch and the file is discarded instead of being reinterpreted as
// garbage - which matters because these structs are still moving.
//
// The settings used to live in an NVS blob and did not survive a reboot on the
// C3 at all. sizeof(Settings) is a little over 6 kB, min_spiffs.csv gives nvs
// 0x5000 (20 kB), and NVS has to hold the new copy beside the old one and keep a
// page spare for garbage collection, so every putBytes() failed outright: the
// box ran permanently on applyDefaults() and silently forgot its Wi-Fi
// credentials, its schedule and its hall mapping on every power cycle. A 6 kB
// record is simply the wrong shape for a key/value store. LittleFS has no such
// limit and was already proven here by the history files.
#include "store.h"

#include <LittleFS.h>
#include <Preferences.h>

namespace ep {
namespace {

constexpr const char* kNamespace    = "easypill";
constexpr const char* kKeySettings  = "settings";
constexpr const char* kKeyLastEpoch = "lastEpoch";

constexpr const char* kEventsPath    = "/events.bin";
constexpr const char* kAdherencePath = "/adherence.bin";
constexpr const char* kSettingsPath  = "/settings.bin";
// Written beside the live file and renamed over it, so a power cut cannot leave
// a half-written settings file as the only copy.
constexpr const char* kSettingsTmpPath = "/settings.new";

constexpr uint32_t kEventsMagic    = 0x45505645u;  // 'EPVE'
constexpr uint32_t kAdherenceMagic = 0x45504144u;  // 'EPAD'
constexpr uint32_t kSettingsMagic  = 0x45505354u;  // 'EPST'
constexpr uint16_t kFileVersion    = 1;

struct FileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t recordSize;
  uint16_t count;
  uint16_t reserved;
};

// How often a burst of GUI edits is allowed to reach flash, and how often the
// history files are rewritten. Both exist to keep flash wear down.
constexpr uint32_t kSettingsCoalesceMs = 3000;
constexpr uint32_t kHistoryIntervalMs  = 30000;
// A write that fails now means a full or sick filesystem, which three-second
// retries would only wear harder without fixing.
constexpr uint32_t kSettingsRetryMs = 60000;

Preferences prefs;
bool prefsOk = false;
bool fsOk    = false;

uint32_t lastSettingsWriteMs = 0;
uint32_t settingsRetryMs     = kSettingsCoalesceMs;
uint32_t lastHistoryCheckMs  = 0;
uint32_t lastEpochWritten    = 0;

// The store interface has no "events changed" flag and the event log has no way
// to report one, so dirtiness is detected by hashing what would be written. 64
// events of integer maths is far cheaper than the flash write it avoids.
uint32_t savedEventsHash    = 0;
uint32_t savedAdherenceHash = 0;

inline uint32_t fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

uint32_t hashEvents(const EventLog& log) {
  uint32_t h = fnv(2166136261u, (uint32_t)log.size());
  for (size_t i = 0; i < log.size(); ++i) {
    const Event& e = log.at(i);
    h = fnv(h, e.localEpoch);
    h = fnv(h, e.uptimeMs);
    h = fnv(h, (uint32_t)(uint8_t)e.type);
    h = fnv(h, (uint32_t)e.lid);
    h = fnv(h, (uint32_t)(uint16_t)e.detail);
  }
  return h;
}

uint32_t hashAdherence(const AdherenceLog& log) {
  uint32_t h = fnv(2166136261u, (uint32_t)log.size());
  for (size_t i = 0; i < log.size(); ++i) {
    const DoseRecord& r = log.at(i);
    h = fnv(h, r.scheduledLocal);
    h = fnv(h, r.actualLocal);
    h = fnv(h, (uint32_t)r.lid | ((uint32_t)r.dose << 8) |
                   ((uint32_t)(uint8_t)r.state << 16));
    h = fnv(h, (uint32_t)(uint16_t)r.deltaMin);
    // sweepMissed clears this flag in its second pass, and on a record whose
    // state and delta are already settled that clear is the only thing that
    // moves. Without it in the hash the file never gets rewritten, and a reboot
    // in that window re-pushes a miss the family has already been told about.
    h = fnv(h, (uint32_t)r.unreportedMiss);
  }
  return h;
}

bool readHeader(File& f, uint32_t magic, uint16_t maxCount, FileHeader* out) {
  if (f.read((uint8_t*)out, sizeof(FileHeader)) != sizeof(FileHeader)) return false;
  return out->magic == magic && out->version == kFileVersion && out->count <= maxCount;
}

// One settings blob out of `path`, straight into `s`. False for anything
// unexpected, including a version the running build does not understand.
bool readSettingsFile(const char* path, Settings& s) {
  if (!fsOk) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  FileHeader h{};
  bool ok = readHeader(f, kSettingsMagic, 1, &h) && h.count == 1 &&
            h.recordSize == (uint16_t)sizeof(Settings);
  if (ok) ok = f.read((uint8_t*)&s, sizeof(s)) == (int)sizeof(s);
  f.close();
  return ok && s.version == kSettingsVersion;
}

}  // namespace

bool storeBegin() {
  prefsOk = prefs.begin(kNamespace, false);
  // NVS now holds only lastEpoch; the settings moved to the filesystem.
  if (!prefsOk) Serial.println("[store] NVS open failed, the clock will not carry over");

  fsOk = LittleFS.begin(true);  // format on first use
  if (!fsOk) {
    Serial.println("[store] LittleFS mount failed, retrying with a format");
    fsOk = LittleFS.begin(true);
  }
  if (!fsOk) Serial.println("[store] no filesystem, NOTHING will survive a reboot");

  lastEpochWritten = prefsOk ? prefs.getUInt(kKeyLastEpoch, 0) : 0;
  return fsOk && prefsOk;
}

bool storeLoadSettings(Settings& s) {
  // Read straight into the caller's object rather than via a temporary: Settings
  // is about 6 kB and the Arduino loop task only has 8 kB of stack. A candidate
  // that fails half way through may leave `s` part-written, which is harmless -
  // the next candidate overwrites all of it, and applyDefaults() does if none
  // of them works.
  if (readSettingsFile(kSettingsPath, s)) return true;

  if (readSettingsFile(kSettingsTmpPath, s)) {
    // Power went away between the remove and the rename. The new copy is good,
    // so finish the swap this boot instead of carrying the temp file forever.
    Serial.println("[store] settings recovered from an interrupted write");
    storeSaveSettings(s);
    return true;
  }

  // Boards whose nvs partition was large enough to hold the blob - the S3 build
  // among them - kept their settings there until this version. Carry them over
  // once; after that the file is the only copy that counts.
  if (prefsOk && prefs.getBytesLength(kKeySettings) == sizeof(Settings) &&
      prefs.getBytes(kKeySettings, &s, sizeof(s)) == sizeof(s) &&
      s.version == kSettingsVersion) {
    Serial.println("[store] migrating settings from NVS into the filesystem");
    if (storeSaveSettings(s)) prefs.remove(kKeySettings);
    return true;
  }

  Serial.println("[store] no usable settings blob, writing defaults");
  applyDefaults(s);
  storeSaveSettings(s);
  return false;
}

bool storeSaveSettings(const Settings& s) {
  if (!fsOk) return false;
  // Write the new copy under its own name, then rename it over the live one. A
  // hard power cut mid-write therefore costs nothing: the previous file is still
  // intact, and a cut inside the swap itself leaves the new copy under
  // kSettingsTmpPath, which storeLoadSettings() tries second. This box gets
  // power-cycled by hand constantly, and losing the Wi-Fi credentials and the
  // schedule to a half-written file is far worse than losing the event log.
  File f = LittleFS.open(kSettingsTmpPath, "w");
  if (!f) return false;

  const FileHeader h{kSettingsMagic, kFileVersion, (uint16_t)sizeof(Settings), 1, 0};
  const bool wrote = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h) &&
                     f.write((const uint8_t*)&s, sizeof(s)) == sizeof(s);
  f.close();
  lastSettingsWriteMs = millis();

  // Size it only after the close. Writes go through a buffered FILE*, so a short
  // write can surface at flush time rather than from write() itself, and asking
  // before the close would read a size that is not on flash yet.
  bool ok = wrote;
  if (ok) {
    File check = LittleFS.open(kSettingsTmpPath, "r");
    ok = check && check.size() == sizeof(h) + sizeof(s);
    if (check) check.close();
  }

  if (!ok) {
    LittleFS.remove(kSettingsTmpPath);
    return false;
  }
  LittleFS.remove(kSettingsPath);
  return LittleFS.rename(kSettingsTmpPath, kSettingsPath);
}

bool storeLoadEvents(EventLog& log) {
  if (!fsOk) return false;
  File f = LittleFS.open(kEventsPath, "r");
  if (!f) return false;

  FileHeader h{};
  bool ok = readHeader(f, kEventsMagic, EventLog::kCapacity, &h) &&
            h.recordSize == sizeof(Event);
  if (ok) {
    log.clear();
    for (uint16_t i = 0; i < h.count; ++i) {
      Event e{};
      if (f.read((uint8_t*)&e, sizeof(e)) != sizeof(e)) {
        ok = false;
        break;
      }
      // Anything in this file was logged before the current power cycle, so its
      // uptimeMs belongs to a millis() that no longer exists. Say so, or
      // backfillTimes() measures against this boot's clock and stamps the event
      // somewhere it never happened.
      e.fromEarlierBoot = true;
      log.push(e);  // written oldest first, so push order restores the ring
    }
  }
  f.close();

  if (!ok) {
    Serial.println("[store] events.bin unreadable or from another layout, discarding");
    LittleFS.remove(kEventsPath);
    log.clear();
  }
  savedEventsHash = hashEvents(log);
  return ok;
}

bool storeSaveEvents(const EventLog& log) {
  if (!fsOk) return false;
  File f = LittleFS.open(kEventsPath, "w");
  if (!f) return false;

  const FileHeader h{kEventsMagic, kFileVersion, (uint16_t)sizeof(Event),
                     (uint16_t)log.size(), 0};
  bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
  for (size_t i = log.size(); ok && i > 0; --i) {
    const Event& e = log.at(i - 1);  // at(0) is newest, so count down to write oldest first
    ok = f.write((const uint8_t*)&e, sizeof(e)) == sizeof(e);
  }
  f.close();

  if (ok) savedEventsHash = hashEvents(log);
  return ok;
}

bool storeLoadAdherence(AdherenceLog& log) {
  if (!fsOk) return false;
  File f = LittleFS.open(kAdherencePath, "r");
  if (!f) return false;

  FileHeader h{};
  bool ok = readHeader(f, kAdherenceMagic, AdherenceLog::kCapacity, &h) &&
            h.recordSize == sizeof(DoseRecord);
  if (ok) {
    log.clear();
    for (uint16_t i = 0; i < h.count; ++i) {
      DoseRecord r{};
      if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) {
        ok = false;
        break;
      }
      DoseRecord* slot = log.findOrCreate(r.scheduledLocal, r.lid, r.dose);
      if (slot != nullptr) *slot = r;
    }
  }
  f.close();

  if (!ok) {
    Serial.println("[store] adherence.bin unreadable or from another layout, discarding");
    LittleFS.remove(kAdherencePath);
    log.clear();
  }
  savedAdherenceHash = hashAdherence(log);
  return ok;
}

bool storeSaveAdherence(const AdherenceLog& log) {
  if (!fsOk) return false;
  File f = LittleFS.open(kAdherencePath, "w");
  if (!f) return false;

  const FileHeader h{kAdherenceMagic, kFileVersion, (uint16_t)sizeof(DoseRecord),
                     (uint16_t)log.size(), 0};
  bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
  for (size_t i = 0; ok && i < log.size(); ++i) {
    const DoseRecord& r = log.at(i);  // at(0) is oldest here, so plain ascending order
    ok = f.write((const uint8_t*)&r, sizeof(r)) == sizeof(r);
  }
  f.close();

  if (ok) savedAdherenceHash = hashAdherence(log);
  return ok;
}

uint32_t storeLoadLastEpoch() {
  if (!prefsOk) return 0;
  return prefs.getUInt(kKeyLastEpoch, 0);
}

void storeSaveLastEpoch(uint32_t utc) {
  if (!prefsOk || utc == 0) return;
  // Callers pass g.nowUtc(), which is already 0 when the clock is nonsense. Only
  // move the stored value when it has drifted a minute, so the five-minute
  // heartbeat in timeService() does not chew through NVS.
  const uint32_t diff = utc > lastEpochWritten ? utc - lastEpochWritten
                                               : lastEpochWritten - utc;
  if (lastEpochWritten != 0 && diff < 60) return;
  prefs.putUInt(kKeyLastEpoch, utc);
  lastEpochWritten = utc;
}

void storeFactoryReset() {
  if (prefsOk) prefs.clear();
  lastEpochWritten = 0;
  if (fsOk) {
    LittleFS.remove(kEventsPath);
    LittleFS.remove(kAdherencePath);
    LittleFS.remove(kSettingsPath);
    LittleFS.remove(kSettingsTmpPath);
  }
  savedEventsHash    = 0;
  savedAdherenceHash = 0;
}

size_t storeFsUsedBytes() { return fsOk ? LittleFS.usedBytes() : 0; }
size_t storeFsTotalBytes() { return fsOk ? LittleFS.totalBytes() : 0; }

void storeService(uint32_t nowMs) {
  if (g.settingsDirty && (int32_t)(nowMs - lastSettingsWriteMs) >= (int32_t)settingsRetryMs) {
    if (storeSaveSettings(g.settings)) {
      g.settingsDirty = false;
      settingsRetryMs = kSettingsCoalesceMs;
    } else {
      Serial.println("[store] settings write failed, backing off");
      settingsRetryMs = kSettingsRetryMs;
    }
    lastSettingsWriteMs = nowMs;
  }

  if ((int32_t)(nowMs - lastHistoryCheckMs) < (int32_t)kHistoryIntervalMs) return;
  lastHistoryCheckMs = nowMs;

  if (hashEvents(g.events) != savedEventsHash) storeSaveEvents(g.events);
  if (hashAdherence(g.adherence) != savedAdherenceHash) storeSaveAdherence(g.adherence);
}

}  // namespace ep
