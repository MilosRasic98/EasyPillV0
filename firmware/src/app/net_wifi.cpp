// Wi-Fi as a non-blocking state machine driven from loop().
//
// Nothing in here is allowed to block: a join attempt is started, and loop()
// comes back later to see how it went. The only exception is netScan(), which is
// reachable from a GUI action and nowhere else.
#include "net_wifi.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <string.h>

#include "notifier.h"
#include "timekeeper.h"
#include "web_server.h"

// Credentials baked in at build time, so a freshly flashed board joins the
// house network on its own. The setup AP carries no route to the internet, and
// phones and laptops drop off a network like that within seconds, which makes
// provisioning through it genuinely difficult - this is the way around that.
// The values live in include/wifi_default.h, which is not in version control;
// include/wifi_default.example.h is the template. With no such header the
// firmware behaves exactly as it did before: no credentials, so the setup AP
// is the only way in.
#if __has_include("wifi_default.h")
#include "wifi_default.h"
#endif

#ifndef EASYPILL_DEFAULT_WIFI_SSID
#define EASYPILL_DEFAULT_WIFI_SSID ""
#endif
#ifndef EASYPILL_DEFAULT_WIFI_PASS
#define EASYPILL_DEFAULT_WIFI_PASS ""  // an open network, or no default at all
#endif

namespace ep {
namespace {

// Long enough for a DHCP round trip on a busy home router, short enough that
// four failures still fit inside a sensible setup session.
constexpr uint32_t kJoinTimeoutMs = 12000;

// 2 s, 5 s, 15 s, then once a minute forever.
constexpr uint32_t kBackoffMs[4] = {2000, 5000, 15000, 60000};

// Failures before the setup AP comes up.
constexpr uint8_t kFailsBeforeAp = 3;

// If the AP is up but nobody is configuring the box, the station is worth
// another try: the usual reason for ending up here is a router that was briefly
// down, not credentials that are actually wrong.
constexpr uint32_t kApIdleRetryMs = 600000;

// Nothing in maybeSleep() can hold the box up while the setup AP is the only
// network: no push can be queued in AP mode and the clock can never verify
// without a station, so the first-sync grace expires a minute after boot and
// netShutdown() takes the SSID down with it. On battery that is a 60 s window
// per wake interval for somebody to find the network on a phone and type into
// it. Hold a provisioning window open instead, and refresh it while a phone is
// actually associated.
constexpr uint32_t kApProvisionSec   = 300;
constexpr uint32_t kApClientAwakeSec = 120;

// A truncated SSID or password would join nothing and say nothing about why,
// so the literals are measured against the fields at build time instead.
static_assert(sizeof(EASYPILL_DEFAULT_WIFI_SSID) <= sizeof(WifiSettings::ssid),
              "EASYPILL_DEFAULT_WIFI_SSID is too long for WifiSettings::ssid");
static_assert(sizeof(EASYPILL_DEFAULT_WIFI_PASS) <= sizeof(WifiSettings::pass),
              "EASYPILL_DEFAULT_WIFI_PASS is too long for WifiSettings::pass");

// Provisioned credentials always win; the built-in pair is a fallback, used
// only while nothing is stored. They are read straight out of the literals and
// never copied into g.settings, which is what makes both halves of that behave
// the way you would want: NVS stays empty until somebody actually provisions
// through the GUI, and editing wifi_default.h and reflashing really does change
// which network the box joins rather than being shadowed by a stale NVS blob
// that a plain flash does not erase.
bool provisioned() { return g.settings.wifi.ssid[0] != '\0'; }
const char* activeSsid() { return provisioned() ? g.settings.wifi.ssid : EASYPILL_DEFAULT_WIFI_SSID; }
const char* activePass() { return provisioned() ? g.settings.wifi.pass : EASYPILL_DEFAULT_WIFI_PASS; }

uint32_t attemptStartMs = 0;
uint32_t retryAtMs      = 0;
uint32_t apStartedMs    = 0;
bool     radioSleeping  = false;

uint32_t backoffFor(uint8_t fails) {
  const uint8_t idx = fails == 0 ? 0 : (uint8_t)(fails - 1);
  return kBackoffMs[idx < 4 ? idx : 3];
}

const char* hostnameOrDefault() {
  return g.settings.wifi.hostname[0] ? g.settings.wifi.hostname : "easypill";
}

void startMdns() {
  if (g.net.mdnsUp) return;
  if (!MDNS.begin(hostnameOrDefault())) return;
  MDNS.addService("http", "tcp", 80);
  g.net.mdnsUp = true;
  Serial.printf("[net] mDNS up: http://%s.local\n", hostnameOrDefault());
}

void stopMdns() {
  if (!g.net.mdnsUp) return;
  MDNS.end();
  g.net.mdnsUp = false;
}

void setMode(NetMode m) {
  if (g.net.mode == m) return;
  g.net.mode         = m;
  g.net.lastChangeMs = millis();
}

void startStation(uint32_t nowMs) {
  stopMdns();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);  // credentials live in our own NVS blob, not in the SDK's
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // joining is not the moment to be saving power
  radioSleeping = false;
  WiFi.setHostname(hostnameOrDefault());
  const char* ssid = activeSsid();
  WiFi.begin(ssid, activePass());

  strlcpy(g.net.ssid, ssid, sizeof g.net.ssid);
  g.net.ip[0]     = '\0';
  g.net.rssi      = 0;
  attemptStartMs  = nowMs;
  setMode(NetMode::Connecting);
  Serial.printf("[net] joining \"%s\"%s\n", ssid,
                provisioned() ? "" : " (built in at build time)");
}

void startAp(uint32_t nowMs) {
  stopMdns();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  char ssid[33];
  const char* dev = g.settings.ui.deviceName[0] ? g.settings.ui.deviceName : "EasyPill";
  snprintf(ssid, sizeof ssid, "%.26s-setup", dev);  // a 32-char device name would not fit

  const char* pass = g.settings.wifi.apPassword;
  const bool secured = strlen(pass) >= 8;  // WPA2 will not take less; open is better than refused
  const bool up = secured ? WiFi.softAP(ssid, pass) : WiFi.softAP(ssid);
  if (!up) {
    Serial.println("[net] softAP failed");
    setMode(NetMode::Failed);
    retryAtMs = nowMs + kBackoffMs[3];
    return;
  }

  strlcpy(g.net.ssid, ssid, sizeof g.net.ssid);
  strlcpy(g.net.ip, WiFi.softAPIP().toString().c_str(), sizeof g.net.ip);
  g.net.rssi   = 0;
  apStartedMs  = nowMs;
  setMode(NetMode::AccessPoint);
  startMdns();  // http://easypill.local works on the AP too
  Serial.printf("[net] setup AP \"%s\" (%s) at %s\n", ssid, secured ? "WPA2" : "open",
                g.net.ip);
  g.led.playPattern(Pattern::BlinkBlue, 3000);
  g.keepAwakeFor(kApProvisionSec);  // an SSID we sleep away cannot be joined
}

void onStationUp() {
  strlcpy(g.net.ssid, WiFi.SSID().c_str(), sizeof g.net.ssid);
  strlcpy(g.net.ip, WiFi.localIP().toString().c_str(), sizeof g.net.ip);
  g.net.rssi      = (int8_t)WiFi.RSSI();
  g.net.failCount = 0;
  setMode(NetMode::Station);
  startMdns();
  Serial.printf("[net] up: %s %s (%d dBm)\n", g.net.ssid, g.net.ip, g.net.rssi);
  g.logEvent(EventType::WifiUp, kNoLid, g.net.rssi);

  // There is no RTC on this board, so every single join is an opportunity to
  // find out what time and date it is. This is the only place the clock ever
  // gets its truth from, which is why it happens here rather than on a timer.
  timeRequestSync();
}

void onStationLost(uint32_t nowMs) {
  stopMdns();
  g.net.ip[0] = '\0';
  g.net.rssi  = 0;
  g.logEvent(EventType::WifiDown);
  Serial.println("[net] link lost");
  if (netHaveCredentials()) {
    startStation(nowMs);
  } else {
    setMode(NetMode::Off);
  }
}

}  // namespace

void netBegin() {
  memset(&g.net, 0, sizeof g.net);
  g.net.lastChangeMs = millis();

  if (netHaveCredentials()) {
    startStation(millis());
  } else if (g.settings.wifi.apFallback) {
    startAp(millis());
  } else {
    WiFi.mode(WIFI_OFF);
    setMode(NetMode::Off);
    Serial.println("[net] no credentials and no AP fallback: radio off");
  }
}

void netService(uint32_t nowMs) {
  switch (g.net.mode) {
    case NetMode::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        onStationUp();
      } else if ((uint32_t)(nowMs - attemptStartMs) >= kJoinTimeoutMs) {
        if (g.net.failCount < 255) g.net.failCount++;
        WiFi.disconnect(true);
        if (g.net.failCount >= kFailsBeforeAp && g.settings.wifi.apFallback) {
          Serial.printf("[net] %u failed joins, falling back to the setup AP\n",
                        (unsigned)g.net.failCount);
          startAp(nowMs);
        } else {
          retryAtMs = nowMs + backoffFor(g.net.failCount);
          setMode(NetMode::Failed);
          Serial.printf("[net] join failed (%u), retrying in %lu ms\n",
                        (unsigned)g.net.failCount,
                        (unsigned long)backoffFor(g.net.failCount));
        }
      }
      break;

    case NetMode::Station: {
      if (WiFi.status() != WL_CONNECTED) {
        onStationLost(nowMs);
        break;
      }
      g.net.rssi = (int8_t)WiFi.RSSI();
      // Modem sleep costs a few tens of milliseconds of latency per packet, which
      // nobody notices on a push but everybody notices in the GUI.
      const bool wantSleep = !webClientActive(nowMs);
      if (wantSleep != radioSleeping) {
        WiFi.setSleep(wantSleep);
        radioSleeping = wantSleep;
      }
      break;
    }

    case NetMode::Failed:
      if ((int32_t)(nowMs - retryAtMs) >= 0) {
        if (netHaveCredentials()) {
          startStation(nowMs);
        } else if (g.settings.wifi.apFallback) {
          startAp(nowMs);
        } else {
          setMode(NetMode::Off);
        }
      }
      break;

    case NetMode::AccessPoint:
      // A phone sitting on the setup network is somebody part-way through
      // provisioning, and that has to count before the GUI is loaded: there is
      // no DNS server here, so the captive-portal probe never reaches us and
      // webClientActive() is still false while they are typing 192.168.4.1.
      if (WiFi.softAPgetStationNum() != 0) g.keepAwakeFor(kApClientAwakeSec);

      // Nobody has touched the GUI for ten minutes: the AP is not being used for
      // setup, so give the real network another chance.
      if (netHaveCredentials() &&
          (uint32_t)(nowMs - apStartedMs) >= kApIdleRetryMs &&
          (uint32_t)(nowMs - webLastRequestMs()) >= kApIdleRetryMs) {
        g.net.failCount = 0;
        Serial.println("[net] setup AP idle, retrying the station");
        startStation(nowMs);
      }
      break;

    case NetMode::Off:
    default:
      if (netHaveCredentials()) startStation(nowMs);
      break;
  }
}

bool netHaveCredentials() { return activeSsid()[0] != '\0'; }

bool netConnected() {
  return g.net.mode == NetMode::Station && WiFi.status() == WL_CONNECTED;
}

bool netSetCredentials(const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') return false;
  strlcpy(g.settings.wifi.ssid, ssid, sizeof g.settings.wifi.ssid);
  if (pass != nullptr) strlcpy(g.settings.wifi.pass, pass, sizeof g.settings.wifi.pass);
  g.markSettingsDirty();
  g.logEvent(EventType::ConfigChanged);

  g.net.failCount = 0;
  startStation(millis());
  return true;
}

size_t netScan(ScanResult* out, size_t maxOut) {
  if (out == nullptr || maxOut == 0) return 0;

  const bool apUp = (g.net.mode == NetMode::AccessPoint);
  if (apUp) WiFi.mode(WIFI_AP_STA);  // a scan needs the station interface to exist

  const int found = WiFi.scanNetworks(false, false);
  size_t n = 0;
  for (int i = 0; i < found; ++i) {
    // WiFi.SSID() hands back a String; copy it out immediately and let it go.
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof ssid);
    if (ssid[0] == '\0') continue;

    const int8_t rssi  = (int8_t)WiFi.RSSI(i);
    const bool secure  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    size_t j = 0;
    for (; j < n; ++j) {
      if (strcmp(out[j].ssid, ssid) == 0) break;
    }
    if (j < n) {  // same SSID on another band or another AP: keep the strongest
      if (rssi > out[j].rssi) {
        out[j].rssi   = rssi;
        out[j].secure = secure;
      }
      continue;
    }
    if (n >= maxOut) continue;
    strlcpy(out[n].ssid, ssid, sizeof out[n].ssid);
    out[n].rssi   = rssi;
    out[n].secure = secure;
    ++n;
  }
  WiFi.scanDelete();
  if (apUp) WiFi.mode(WIFI_AP);

  for (size_t i = 1; i < n; ++i) {  // insertion sort, strongest first
    const ScanResult key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].rssi < key.rssi) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }
  return n;
}

void netShutdown() {
  stopMdns();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  radioSleeping = false;
  g.net.ip[0]   = '\0';
  g.net.rssi    = 0;
  setMode(NetMode::Off);
}

}  // namespace ep
