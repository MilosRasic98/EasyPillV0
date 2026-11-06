// Outbound pushes, queued so that no HTTP request ever happens inside an event
// handler. A TLS handshake is the better part of a second; a lid opening cannot
// wait that long to be logged, and the hall sensors cannot wait that long to be
// scanned.
//
// The send still happens on the loop - there is no second task anywhere in this
// firmware - so notifyService() is the one tenant of loop() besides the web
// handlers that can block for seconds. It is paced for that: at most one
// transport per pass, a gap afterwards so the debounce filter gets a full window
// of samples, a bounded connect and read timeout, and never a send started
// within kLidSettleMs of a lid moving. Worst case for one pass is therefore
// kHttpConnectMs + kHttpReadMs, not three transports' worth of it.
#include "notifier.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <string.h>

#include "ep_schedule.h"
#include "ep_timecalc.h"
#include "net_wifi.h"

namespace ep {
namespace {

constexpr size_t   kQueueDepth   = 6;
// notifyService() runs on the sensing loop, so a POST is a window in which no
// hall sensor is read. Connect plus response is that window, and it used to be
// 6 s + 6 s per transport with all three tried in one pass - 36 s of blindness
// against a server that drops packets. Connect has to swallow the whole TLS
// handshake, which is a second or two of software RSA on a C3, so it keeps a
// generous budget: cutting it to the point where a working ntfy.sh starts
// timing out would cost the family every notification, which is worse than
// anything it would buy. The window is kept small by sending one transport per
// pass instead, and by not starting at all while a lid is moving.
constexpr uint32_t kHttpConnectMs = 3000;
constexpr uint32_t kHttpReadMs    = 2000;
constexpr uint8_t  kMaxAttempts   = 3;
constexpr uint32_t kRetryMs       = 15000;
// One transport per pass, then a gap, so the debounce filter gets a full window
// of samples (hallDebounceMs defaults to 60) before the next one blocks.
constexpr uint32_t kTransportGapMs = 250;
// A dose is taken in a handful of seconds. Do not start a send on top of one.
constexpr uint32_t kLidSettleMs    = 5000;

// The queue lives in RAM and deep sleep wipes it, so an entry that cannot be
// delivered has to be given up on rather than holding the device awake forever.
// Three minutes is a wake, a join and a couple of retries.
constexpr uint32_t kExpiryMs = 180000;

struct QueuedPush {
  char      title[48];
  char      message[192];
  char      tags[32];
  uint8_t   priority;   // 1..5, ntfy's scale
  EventType type;       // Count for a push that is not about an event
  uint8_t   lid;
  uint32_t  localEpoch;
  uint32_t  queuedMs;
  uint8_t   attempts;
  // Which transports this attempt has already been through, and which of them
  // answered 2xx. One transport goes out per loop pass, so the progress has to
  // survive between passes.
  uint8_t   triedMask;
  uint8_t   okMask;
};

QueuedPush queue[kQueueDepth];
size_t     qHead = 0;  // next slot to write
size_t     qTail = 0;  // next entry to send
size_t     qCount = 0;
uint32_t   nextSendMs = 0;

constexpr uint8_t kTargetNtfy     = 0x01;
constexpr uint8_t kTargetPushover = 0x02;
constexpr uint8_t kTargetWebhook  = 0x04;

uint8_t enabledTargets() {
  const NotifySettings& n = g.settings.notify;
  uint8_t m = 0;
  if (n.ntfyEnabled && n.ntfyTopic[0]) m |= kTargetNtfy;
  if (n.pushoverEnabled && n.pushoverToken[0] && n.pushoverUser[0]) m |= kTargetPushover;
  if (n.webhookEnabled && n.webhookUrl[0]) m |= kTargetWebhook;
  return m;
}

bool anyTargetEnabled() { return enabledTargets() != 0; }

// A POST blocks the loop, and HallScanner::scan() is level sampled with a
// debounce rather than edge triggered: a lid lifted and shut again inside the
// block leaves no trace at all, so the dose is reported missed afterwards. The
// cheapest defence is not to open the window while somebody's hands are on the
// box. Deliberately not gated on "a lid is open right now": a lid left standing
// open is a stable level the next scan still sees, and waiting for it to close
// would starve the queue until the expiry dropped every push.
bool lidsSettled(uint32_t nowMs) {
  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    const uint32_t at = g.halls.lidChangedAtMs(lid);
    if (at != 0 && (uint32_t)(nowMs - at) < kLidSettleMs) return false;
  }
  return true;
}

const char* deviceName() {
  return g.settings.ui.deviceName[0] ? g.settings.ui.deviceName : "EasyPill";
}

const char* patientName() {
  return g.settings.ui.patientName[0] ? g.settings.ui.patientName : "The patient";
}

const char* lidNameOf(uint8_t lid) {
  if (lid >= kLidCount) return "the";
  const char* n = g.settings.schedule.lids[lid].name;
  return n[0] ? n : defaultLidName(lid, g.settings.schedule.mode);
}

const char* ntfyPriority(uint8_t p) {
  switch (p) {
    case 1: return "min";
    case 2: return "low";
    case 4: return "high";
    case 5: return "urgent";
    default: return "default";
  }
}

bool inQuietHours(uint32_t localEpoch) {
  const NotifySettings& n = g.settings.notify;
  if (n.quietFromMinute == n.quietToMinute) return false;  // equal means off
  if (localEpoch == 0) return false;                       // no clock, no opinion
  const uint16_t m = minuteOfDay(localEpoch);
  if (n.quietFromMinute < n.quietToMinute) return m >= n.quietFromMinute && m < n.quietToMinute;
  return m >= n.quietFromMinute || m < n.quietToMinute;  // the window wraps midnight
}

// ---- string helpers, all fixed-buffer ---------------------------------------

void jsonEscape(const char* src, char* dst, size_t cap) {
  if (dst == nullptr || cap == 0) return;
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)(src != nullptr ? src : "");
       *p != '\0' && o + 2 < cap; ++p) {
    switch (*p) {
      case '"':
      case '\\':
        dst[o++] = '\\';
        dst[o++] = (char)*p;
        break;
      case '\n': dst[o++] = '\\'; dst[o++] = 'n'; break;
      case '\r': dst[o++] = '\\'; dst[o++] = 'r'; break;
      case '\t': dst[o++] = '\\'; dst[o++] = 't'; break;
      default:
        if (*p < 0x20) {
          if (o + 6 < cap) {
            snprintf(dst + o, cap - o, "\\u%04x", (unsigned)*p);
            o += 6;
          }
        } else {
          dst[o++] = (char)*p;
        }
        break;
    }
  }
  dst[o] = '\0';
}

// ntfy carries tags as one comma-separated header, but the webhook payload
// carries them as a real JSON array - that is what the simulator sends and what
// an automation filters on. Writes the array's inside, without the brackets.
void jsonTagArray(const char* src, char* dst, size_t cap) {
  if (dst == nullptr || cap == 0) return;
  dst[0] = '\0';
  size_t o = 0;
  for (const char* p = src != nullptr ? src : ""; *p != '\0';) {
    while (*p == ',') ++p;  // an empty entry is not a tag
    if (*p == '\0') break;
    const char* start = p;
    while (*p != '\0' && *p != ',') ++p;
    size_t len = (size_t)(p - start);

    char one[32];
    if (len > sizeof one - 1) len = sizeof one - 1;
    memcpy(one, start, len);
    one[len] = '\0';
    char esc[72];
    jsonEscape(one, esc, sizeof esc);

    const int w = snprintf(dst + o, cap - o, "%s\"%s\"", o != 0 ? "," : "", esc);
    if (w <= 0 || (size_t)w >= cap - o) {
      dst[o] = '\0';  // no half-quoted tag: drop the rest rather than emit broken JSON
      return;
    }
    o += (size_t)w;
  }
}

size_t appendFormField(char* dst, size_t cap, size_t pos, const char* key, const char* val) {
  static const char kHex[] = "0123456789ABCDEF";
  if (dst == nullptr || pos + 1 >= cap) return pos;
  if (pos != 0 && pos + 1 < cap) dst[pos++] = '&';
  for (const char* k = key; *k != '\0' && pos + 1 < cap; ++k) dst[pos++] = *k;
  if (pos + 1 < cap) dst[pos++] = '=';
  for (const unsigned char* v = (const unsigned char*)(val != nullptr ? val : ""); *v != '\0';
       ++v) {
    const unsigned char c = *v;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (pos + 1 < cap) dst[pos++] = (char)c;
    } else if (c == ' ') {
      if (pos + 1 < cap) dst[pos++] = '+';
    } else if (pos + 3 < cap) {
      dst[pos++] = '%';
      dst[pos++] = kHex[c >> 4];
      dst[pos++] = kHex[c & 0x0F];
    }
  }
  dst[pos] = '\0';
  return pos;
}

// ---- the one place an HTTP request happens ----------------------------------

int postWith(WiFiClient& client, const char* url, const char* contentType, const char* body,
             const char* const* headerNames, const char* const* headerValues,
             size_t headerCount) {
  HTTPClient http;
  http.setConnectTimeout(kHttpConnectMs);
  http.setTimeout(kHttpReadMs);
  http.setReuse(false);
  if (!http.begin(client, url)) return 0;

  for (size_t i = 0; i < headerCount; ++i) {
    if (headerValues[i] != nullptr && headerValues[i][0] != '\0') {
      http.addHeader(headerNames[i], headerValues[i]);
    }
  }
  if (contentType != nullptr && contentType[0] != '\0') http.addHeader("Content-Type", contentType);

  const int code = http.POST((uint8_t*)body, strlen(body));
  http.end();
  return code;
}

int postRequest(const char* url, const char* contentType, const char* body,
                const char* const* headerNames, const char* const* headerValues,
                size_t headerCount) {
  if (url == nullptr || url[0] == '\0') return 0;

  // The two client types are kept in separate scopes rather than behind one
  // pointer because constructing a WiFiClientSecure allocates an mbedTLS
  // context, and a plain http webhook should not pay for one.
  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure tls;
    // There is no certificate store on a 4 MB C3 - min_spiffs already hands the
    // app 1.9 MB of the 4 - and before the first SNTP answer there is no clock to
    // check notBefore/notAfter against either. The payload is "a pill lid
    // opened", so an unauthenticated channel is the right trade; it is stated
    // here rather than hidden in a flag.
    tls.setInsecure();
    tls.setTimeout(kHttpReadMs / 1000);
    return postWith(tls, url, contentType, body, headerNames, headerValues, headerCount);
  }
  WiFiClient plain;
  plain.setTimeout(kHttpReadMs / 1000);
  return postWith(plain, url, contentType, body, headerNames, headerValues, headerCount);
}

bool httpOk(int code) { return code >= 200 && code < 300; }

int sendNtfy(const QueuedPush& q, char* detail, size_t detailN) {
  const NotifySettings& n = g.settings.notify;
  const char* server = n.ntfyServer[0] ? n.ntfyServer : "https://ntfy.sh";

  size_t len = strlen(server);
  while (len > 0 && server[len - 1] == '/') --len;  // tolerate a trailing slash

  char url[128];
  snprintf(url, sizeof url, "%.*s/%s", (int)len, server, n.ntfyTopic);

  char auth[96] = "";
  if (n.ntfyToken[0]) snprintf(auth, sizeof auth, "Bearer %s", n.ntfyToken);

  const char* names[]  = {"Title", "Priority", "Tags", "Authorization"};
  const char* values[] = {q.title, ntfyPriority(q.priority), q.tags, auth};

  const int code = postRequest(url, "text/plain", q.message, names, values, 4);
  snprintf(detail, detailN, "ntfy %s: %d", n.ntfyTopic, code);
  return code;
}

int sendPushover(const QueuedPush& q, char* detail, size_t detailN) {
  const NotifySettings& n = g.settings.notify;

  // Pushover's scale is -2..2 and its 2 ("emergency") is only legal together
  // with retry/expire parameters, so urgent lands on 1 instead of being refused.
  int prioValue = (int)q.priority - 3;
  if (prioValue > 1) prioValue = 1;
  if (prioValue < -2) prioValue = -2;
  char prio[4];
  snprintf(prio, sizeof prio, "%d", prioValue);

  char body[640];
  size_t pos = 0;
  pos = appendFormField(body, sizeof body, pos, "token", n.pushoverToken);
  pos = appendFormField(body, sizeof body, pos, "user", n.pushoverUser);
  pos = appendFormField(body, sizeof body, pos, "title", q.title);
  pos = appendFormField(body, sizeof body, pos, "message", q.message);
  appendFormField(body, sizeof body, pos, "priority", prio);

  const int code = postRequest("https://api.pushover.net/1/messages.json",
                               "application/x-www-form-urlencoded", body, nullptr, nullptr, 0);
  snprintf(detail, detailN, "pushover: %d", code);
  return code;
}

int sendWebhook(const QueuedPush& q, char* detail, size_t detailN) {
  const NotifySettings& n = g.settings.notify;

  char dev[64], pat[64], title[96], msg[384], lidName[48], tags[96];
  jsonEscape(deviceName(), dev, sizeof dev);
  jsonEscape(patientName(), pat, sizeof pat);
  jsonEscape(q.title, title, sizeof title);
  jsonEscape(q.message, msg, sizeof msg);
  jsonEscape(q.lid < kLidCount ? lidNameOf(q.lid) : "", lidName, sizeof lidName);
  jsonTagArray(q.tags, tags, sizeof tags);

  // The simulator posts this same object with source "easypill-sim", so an
  // automation built on the desk against priority, tags or event keeps firing
  // once the board is flashed. "event" is the wire name of the event behind the
  // push and "unknown" for the ones that have none: a test, the daily summary.
  char body[1024];
  snprintf(body, sizeof body,
           "{\"device\":\"%s\",\"patient\":\"%s\",\"title\":\"%s\",\"message\":\"%s\","
           "\"event\":\"%s\",\"lid\":%d,\"lidName\":\"%s\",\"localEpoch\":%lu,"
           "\"priority\":%u,\"tags\":[%s],\"source\":\"easypill\",\"fw\":\"%s\"}",
           dev, pat, title, msg, toString(q.type), q.lid < kLidCount ? (int)q.lid : -1,
           lidName, (unsigned long)q.localEpoch, (unsigned)q.priority, tags,
           kFirmwareVersion);

  const int code = postRequest(n.webhookUrl, "application/json", body, nullptr, nullptr, 0);
  snprintf(detail, detailN, "webhook: %d", code);
  return code;
}

// snprintf returns the length it *would* have written, so the running offset has
// to be clamped or the next call gets a negative remaining size.
void appendDetail(char* dst, size_t cap, size_t* pos, const char* text) {
  if (cap == 0 || *pos + 1 >= cap) return;
  const int w = snprintf(dst + *pos, cap - *pos, "%s%s", *pos != 0 ? "; " : "", text);
  if (w <= 0) return;
  *pos += (size_t)w;
  if (*pos > cap - 1) *pos = cap - 1;
}

void popQueue() {
  if (qCount == 0) return;
  qTail = (qTail + 1) % kQueueDepth;
  --qCount;
}

bool enqueue(const char* title, const char* message, uint8_t priority, const char* tags,
             EventType type, uint8_t lid, uint32_t localEpoch) {
  if (!anyTargetEnabled()) return false;
  if (type != EventType::BatteryCritical && inQuietHours(localEpoch)) return false;

  // The AP is a setup network with no route out, and with no credentials at all
  // there never will be one, so there is nothing to wait for. Credentials baked
  // in at build time count here: netHaveCredentials() rather than the stored
  // SSID, which such a board leaves empty.
  if (g.net.mode == NetMode::AccessPoint) return false;
  if (!netHaveCredentials()) return false;

  if (qCount == kQueueDepth) {
    // Six deep is already more than one wake's worth of events; drop the oldest
    // so the newest - which is what a carer actually wants - gets through.
    popQueue();
  }

  QueuedPush& q = queue[qHead];
  memset(&q, 0, sizeof q);
  strlcpy(q.title, title != nullptr ? title : deviceName(), sizeof q.title);
  strlcpy(q.message, message != nullptr ? message : "", sizeof q.message);
  strlcpy(q.tags, tags != nullptr ? tags : "", sizeof q.tags);
  q.priority   = priority < 1 ? 3 : (priority > 5 ? 5 : priority);
  q.type       = type;
  q.lid        = lid;
  q.localEpoch = localEpoch;
  q.queuedMs   = millis();
  q.attempts   = 0;

  qHead = (qHead + 1) % kQueueDepth;
  ++qCount;
  nextSendMs = millis();
  g.keepAwakeFor(30);  // do not sleep before this has had a chance to go out
  return true;
}

// Which switch governs this event type, or null when the type is never pushed.
const bool* toggleFor(EventType t) {
  const NotifySettings& n = g.settings.notify;
  switch (t) {
    case EventType::LidOpened:       return &n.onLidOpen;
    case EventType::DoseTaken:       return &n.onDoseTaken;
    case EventType::DoseMissed:      return &n.onDoseMissed;
    case EventType::DoseDue:
    case EventType::DoseReminder:    return &n.onDoseDue;
    case EventType::UnexpectedOpen:
    case EventType::WrongDayOpen:    return &n.onUnexpectedOpen;
    case EventType::BatteryLow:      return &n.onLowBattery;
    case EventType::Boot:            return &n.onBoot;
    default:                         return nullptr;
  }
}

uint8_t priorityFor(EventType t) {
  switch (t) {
    case EventType::BatteryCritical: return 5;
    case EventType::DoseMissed:
    case EventType::WrongDayOpen:    return 4;
    case EventType::DoseDue:
    case EventType::UnexpectedOpen:
    case EventType::BatteryLow:      return 4;
    case EventType::Boot:            return 2;
    default:                         return 3;
  }
}

const char* tagsFor(EventType t) {
  switch (t) {
    case EventType::LidOpened:       return "door";
    case EventType::DoseTaken:       return "pill,white_check_mark";
    case EventType::DoseMissed:      return "pill,x";
    case EventType::DoseDue:
    case EventType::DoseReminder:    return "alarm_clock,pill";
    case EventType::UnexpectedOpen:  return "warning,door";
    case EventType::WrongDayOpen:    return "warning,calendar";
    case EventType::BatteryLow:      return "battery";
    case EventType::BatteryCritical: return "rotating_light,battery";
    case EventType::Boot:            return "arrows_counterclockwise";
    default:                         return "pill";
  }
}

// A sentence somebody actually wants to read on their phone.
void buildMessage(const Event& e, char* out, size_t n) {
  const char* lid = lidNameOf(e.lid);
  char when[8] = "";

  switch (e.type) {
    case EventType::LidOpened:
      snprintf(out, n, "%s opened the %s lid.", patientName(), lid);
      return;

    case EventType::DoseTaken: {
      if (e.localEpoch != 0) {
        const uint32_t scheduled = (uint32_t)((int64_t)e.localEpoch - (int64_t)e.detail * 60);
        formatHhMm(minuteOfDay(scheduled), when, sizeof when);
      }
      if (when[0] == '\0') {
        snprintf(out, n, "%s opened the %s lid and the dose is marked taken.", patientName(),
                 lid);
      } else if (e.detail == 0) {
        snprintf(out, n, "%s opened the %s lid right on time for the %s dose.", patientName(),
                 lid, when);
      } else if (e.detail < 0) {
        snprintf(out, n, "%s opened the %s lid %d min before the %s dose.", patientName(), lid,
                 -e.detail, when);
      } else {
        snprintf(out, n, "%s opened the %s lid %d min after the %s dose.", patientName(), lid,
                 e.detail, when);
      }
      return;
    }

    case EventType::WrongDayOpen:
      snprintf(out, n, "%s opened the %s lid, which is not today's compartment.",
               patientName(), lid);
      return;

    case EventType::UnexpectedOpen:
      snprintf(out, n, "%s opened the %s lid with no dose due anywhere near now.",
               patientName(), lid);
      return;

    case EventType::DoseMissed:
      if (e.detail >= 0 && e.detail < (int16_t)kMinPerDay) {
        formatHhMm((uint16_t)e.detail, when, sizeof when);
        snprintf(out, n, "The %s %s dose was never taken.", when, lid);
      } else {
        snprintf(out, n, "A %s dose was never taken.", lid);
      }
      return;

    case EventType::DoseDue:
      if (e.detail >= 0 && e.detail < (int16_t)kMinPerDay) {
        formatHhMm((uint16_t)e.detail, when, sizeof when);
        snprintf(out, n, "Time for the %s dose: the %s lid.", when, lid);
      } else {
        snprintf(out, n, "Time for the %s dose.", lid);
      }
      return;

    case EventType::DoseReminder:
      snprintf(out, n, "The %s dose is due in %d min.", lid, (int)e.detail);
      return;

    case EventType::BatteryLow:
      snprintf(out, n, "%s battery is down to %d mV. Time to charge it.", deviceName(),
               (int)e.detail);
      return;

    case EventType::BatteryCritical:
      snprintf(out, n, "%s battery is critical at %d mV and is shutting down. No reminders "
                       "until it is charged.",
               deviceName(), (int)e.detail);
      return;

    case EventType::Boot:
      snprintf(out, n, "%s restarted (%s).", deviceName(),
               toString((WakeReason)(uint8_t)e.detail));
      return;

    default:
      g.describeEvent(e, out, n);
      return;
  }
}

}  // namespace

void notifyBegin() {
  qHead = qTail = qCount = 0;
  nextSendMs = 0;
  memset(queue, 0, sizeof queue);
}

bool notifyPush(const char* title, const char* message, uint8_t priority, const char* tags) {
  return enqueue(title, message, priority, tags, EventType::Count, kNoLid, g.nowLocal());
}

void notifyEvent(const Event& e) {
  if (e.type != EventType::BatteryCritical) {
    const bool* toggle = toggleFor(e.type);
    if (toggle == nullptr || !*toggle) return;
  }

  char message[192];
  buildMessage(e, message, sizeof message);
  enqueue(deviceName(), message, priorityFor(e.type), tagsFor(e.type), e.type, e.lid,
          e.localEpoch != 0 ? e.localEpoch : g.nowLocal());
}

void notifyDailySummary() {
  if (!g.settings.notify.onDailySummary) return;

  const uint32_t now = g.nowLocal();
  // Same boot floor as /api/state, or the first summary after a battery change
  // would tell the family about doses that were due before the box was on.
  const AdherenceLog::DayStats d =
      g.adherence.today(g.settings.schedule, now, g.bootFloorLocal);
  const uint8_t pct7 =
      g.adherence.adherencePercent(g.settings.schedule, now, 7, g.bootFloorLocal);

  char message[192];
  snprintf(message, sizeof message,
           "%s took %u of %u doses today, %u missed, %u still to come. Adherence over the "
           "last week: %u%%.",
           patientName(), (unsigned)d.taken, (unsigned)d.due, (unsigned)d.missed,
           (unsigned)d.pending, (unsigned)pct7);

  char title[48];
  snprintf(title, sizeof title, "%s daily summary", deviceName());
  enqueue(title, message, 2, "bar_chart", EventType::Count, kNoLid, now);
}

void notifyService(uint32_t nowMs) {
  if (qCount == 0) return;

  QueuedPush& q = queue[qTail];

  if ((uint32_t)(nowMs - q.queuedMs) >= kExpiryMs) {
    Serial.printf("[notify] giving up on \"%s\" after %lu ms undelivered\n", q.message,
                  (unsigned long)(nowMs - q.queuedMs));
    popQueue();
    return;
  }

  if (!netConnected()) return;
  if ((int32_t)(nowMs - nextSendMs) < 0) return;

  const uint8_t targets = enabledTargets();
  if (targets == 0) {
    // Every target was switched off after this was queued, so there is nowhere
    // left to send it.
    popQueue();
    nextSendMs = nowMs;
    return;
  }

  // Try again in a moment: a lid has just moved and the next few seconds belong
  // to the hall scan, not to TLS.
  if (!lidsSettled(nowMs)) {
    nextSendMs = nowMs + kTransportGapMs;
    return;
  }

  // One transport per pass. Three of them back to back is three blocking posts
  // with no scan between, which is the whole problem.
  const uint8_t pending = (uint8_t)(targets & ~q.triedMask);
  char          detail[96] = "";
  uint8_t       which      = 0;
  int           code       = 0;
  if (pending & kTargetNtfy) {
    which = kTargetNtfy;
    code  = sendNtfy(q, detail, sizeof detail);
  } else if (pending & kTargetPushover) {
    which = kTargetPushover;
    code  = sendPushover(q, detail, sizeof detail);
  } else if (pending & kTargetWebhook) {
    which = kTargetWebhook;
    code  = sendWebhook(q, detail, sizeof detail);
  }

  if (which != 0) {
    q.triedMask = (uint8_t)(q.triedMask | which);
    if (httpOk(code)) q.okMask = (uint8_t)(q.okMask | which);
    Serial.printf("[notify] %s\n", detail);
    if ((uint8_t)(targets & ~q.triedMask) != 0) {
      nextSendMs = nowMs + kTransportGapMs;  // the rest of them, one pass each
      return;
    }
  }

  // Every enabled transport has now had this attempt. One delivery is enough:
  // the family only needs to be told once.
  if (q.okMask != 0) {
    popQueue();
    nextSendMs = nowMs + kTransportGapMs;
    return;
  }

  if (++q.attempts >= kMaxAttempts) {
    Serial.println("[notify] dropping push after three failed attempts");
    popQueue();
    nextSendMs = nowMs + kTransportGapMs;
  } else {
    q.triedMask = 0;  // the retry is a fresh round through every transport
    nextSendMs  = nowMs + kRetryMs;
  }
}

NotifyOutcome notifyTest(const char* target) {
  NotifyOutcome outcome{};
  outcome.ok       = false;
  outcome.httpCode = 0;

  if (!netConnected()) {
    snprintf(outcome.detail, sizeof outcome.detail, "no network");
    return outcome;
  }

  const char* which = (target != nullptr && target[0] != '\0') ? target : "all";
  const bool all = strcmp(which, "all") == 0;

  QueuedPush q{};
  strlcpy(q.title, deviceName(), sizeof q.title);
  snprintf(q.message, sizeof q.message, "Test notification from %s. If you can read this, %s "
                                        "reminders will reach you.",
           deviceName(), patientName());
  strlcpy(q.tags, "test_tube", sizeof q.tags);
  q.priority   = 3;
  q.type       = EventType::Count;
  q.lid        = kNoLid;
  q.localEpoch = g.nowLocal();

  const NotifySettings& n = g.settings.notify;
  size_t pos = 0;
  bool attempted = false;

  if ((all || strcmp(which, "ntfy") == 0) && n.ntfyTopic[0]) {
    char detail[96];
    outcome.httpCode = sendNtfy(q, detail, sizeof detail);
    outcome.ok |= httpOk(outcome.httpCode);
    attempted = true;
    appendDetail(outcome.detail, sizeof outcome.detail, &pos, detail);
  }
  if ((all || strcmp(which, "pushover") == 0) && n.pushoverToken[0] && n.pushoverUser[0]) {
    char detail[96];
    outcome.httpCode = sendPushover(q, detail, sizeof detail);
    outcome.ok |= httpOk(outcome.httpCode);
    attempted = true;
    appendDetail(outcome.detail, sizeof outcome.detail, &pos, detail);
  }
  if ((all || strcmp(which, "webhook") == 0) && n.webhookUrl[0]) {
    char detail[96];
    outcome.httpCode = sendWebhook(q, detail, sizeof detail);
    outcome.ok |= httpOk(outcome.httpCode);
    attempted = true;
    appendDetail(outcome.detail, sizeof outcome.detail, &pos, detail);
  }

  if (!attempted) {
    snprintf(outcome.detail, sizeof outcome.detail, "%s is not configured", which);
  }
  return outcome;
}

bool notifyQueueEmpty() { return qCount == 0; }

}  // namespace ep
