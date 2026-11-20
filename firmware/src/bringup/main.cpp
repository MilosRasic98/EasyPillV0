// EasyPill V0 bring-up firmware.
//
// This is the first thing flashed onto a board that has never been powered, so
// it is deliberately the simplest program in the project: no scheduler, no web
// server, no persistence, one USB-CDC command line, one hardware fact per
// answer. If something in here says PASS, that part of the board works.
//
// Three facts shape every line below:
//   * MUX_A/B/C address three XS3A4051s at once, so every access goes through
//     MuxBus, which drops SIG_OUT low before the address moves and parks on
//     channel 7 when idle.
//   * MUX_C is U0TXD and SIG_OUT is U0RXD. UART0 does not exist for us; all
//     logging is USB-CDC, i.e. plain Serial.
//   * POWER_LATCH goes high before anything else, or the board dies in the
//     user's hand the moment they let go of the button.
//
// Nothing here may block indefinitely: the continuous modes poll Serial every
// iteration and end on any byte, and every wait is bounded.

#include <Arduino.h>
#include <WiFi.h>

#include <ctype.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "analog_sense.h"
#include "board_pins.h"
#include "ep_settings.h"
#include "ep_types.h"
#include "hall_scanner.h"
#include "indicator.h"
#include "mux_bus.h"
#include "power_latch.h"

#if !defined(ARDUINO_USB_CDC_ON_BOOT)
#error "Bring-up logs over USB-CDC because MUX_C is U0TXD and SIG_OUT is U0RXD. \
Use a board definition that sets ARDUINO_USB_CDC_ON_BOOT=1."
#endif

namespace {

// ---------------------------------------------------------------------------
// Hardware, and the settings that tell it how it is wired
// ---------------------------------------------------------------------------

// The scanner and the indicator both need a HardwareSettings to know hall
// polarity and which SIG_OUT channel is which colour. Taking it from
// applyDefaults() means the bring-up tool and the application agree about the
// board; `c` and `Z` then edit this RAM copy so you can find the right values
// before anyone commits them to NVS.
ep::Settings    gSettings;
ep::MuxBus      gBus;
ep::HallScanner gHall;
ep::AnalogSense gSense;
ep::Indicator   gLed;

constexpr const char* kFirmwareName = "EasyPill V0 bring-up";

#if defined(ARDUINO_XIAO_ESP32C3)
constexpr const char* kBuiltFor = "Seeed XIAO ESP32-C3 (ARDUINO_XIAO_ESP32C3)";
#else
constexpr const char* kBuiltFor = "Seeed XIAO ESP32-S3 (ARDUINO_XIAO_ESP32S3)";
#endif

// Any epoch past this means the clock has been set from the network at least
// once; below it, time() is just seconds since boot. There is no RTC on this
// board, so that distinction has to be made explicitly
// everywhere a time is printed.
constexpr uint32_t kPlausibleEpoch = 1735689600u;  // 2025-01-01 00:00:00 UTC

// ---------------------------------------------------------------------------
// Output helpers. Fixed buffers only: no String, no heap churn.
// ---------------------------------------------------------------------------

void outf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void outf(const char* fmt, ...) {
  char buf[512];  // whole paragraphs come through here, not just single lines
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// Every "label: value" line in this firmware goes through here, so the columns
// line up without anyone counting spaces in a format string.
void row(const char* label, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void row(const char* label, const char* fmt, ...) {
  char value[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(value, sizeof value, fmt, ap);
  va_end(ap);
  outf("  %-15s %s\n", label, value);
}

enum class Verdict : uint8_t { Pass, Fail, Info };

uint8_t gPassCount = 0;
uint8_t gFailCount = 0;
uint8_t gInfoCount = 0;

// Info is for the things only a human can judge - whether the LED lit, whether
// the buzzer made a noise - so it must not count as a failure.
void verdict(Verdict v, const char* item, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
void verdict(Verdict v, const char* item, const char* fmt, ...) {
  char detail[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof detail, fmt, ap);
  va_end(ap);

  const char* tag = "INFO";
  if (v == Verdict::Pass) {
    tag = "PASS";
    ++gPassCount;
  } else if (v == Verdict::Fail) {
    tag = "FAIL";
    ++gFailCount;
  } else {
    ++gInfoCount;
  }
  outf("  [%s] %-16s %s\n", tag, item, detail);
}

void stamp(uint32_t ms) { outf("  %6u.%03u s  ", ms / 1000u, ms % 1000u); }

// Bit 0 leftmost, so the string reads in the same order as the live hall table.
void maskToBits(uint8_t mask, uint8_t bits, char* out) {
  for (uint8_t i = 0; i < bits; ++i) out[i] = ((mask >> i) & 1u) ? '1' : '.';
  out[bits] = '\0';
}

uint16_t absDiff(uint16_t a, uint16_t b) {
  return (a > b) ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

// ---------------------------------------------------------------------------
// Static reference tables: the pin map and the mux channel map
// ---------------------------------------------------------------------------

struct PinRow {
  const char* net;
  const char* silk;
  uint8_t     gpio;
  const char* role;
};

const PinRow kPinTable[] = {
    {"WAKE_UP", "D1", ep::pins::kWakeUp, "input, hardware OR of everything, deep-sleep wake"},
    {"SIG_IN", "D2", ep::pins::kSigIn, "analog in, ADC1, U4 common pin"},
    {"HALL_IN", "D3", ep::pins::kHallIn, "digital in, U20 common pin"},
    {"MUX_A", "D4", ep::pins::kMuxA, "digital out, address bit 0 (S1)"},
    {"MUX_B", "D5", ep::pins::kMuxB, "digital out, address bit 1 (S2)"},
    {"MUX_C", "D6", ep::pins::kMuxC, "digital out, address bit 2 (S3)  == U0TXD"},
    {"SIG_OUT", "D7", ep::pins::kSigOut, "digital out, U5 common pin, Q1..Q4 gates  == U0RXD"},
    {"POWER_LATCH", "D10", ep::pins::kPowerLatch, "digital out, hold HIGH or the board dies"},
};

// One address selects a channel on all three muxes at once; this is that table.
struct MuxRow {
  const char* hall;   // U20 -> HALL_IN
  const char* sense;  // U4  -> SIG_IN
  const char* out;    // U5  <- SIG_OUT
};

const MuxRow kMuxMap[ep::mux::kChannelCount] = {
    {"Hall 1", "VBAT x 0.5714", "Q3 -> CTL1 pin 4"},
    {"Hall 2", "5V_USB x 0.4286", "Q2 -> CTL1 pin 3"},
    {"Hall 3", "5V_COIL x 0.4286", "Q1 -> CTL1 pin 2"},
    {"Hall 4", "BUTTON_SIG, clamped", "Q4 -> buzzer (U6)"},
    {"Hall 5", "MCP_CHRG STAT, clamped", "-"},
    {"Hall 6", "-", "-"},
    {"Hall 7", "-", "-"},
    {"100k to GND (R38)", "-", "- (unconnected)"},
};

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on (cold boot)";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic or exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    // Expected on this module rather than a fault: a reset driven over the
    // native USB-serial-JTAG - which is what esptool does the moment it
    // finishes flashing - is not one the chip can attribute afterwards.
    case ESP_RST_UNKNOWN:   return "not determinable (normal right after a USB flash)";
    default:                return "unknown";
  }
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "esp cause: not a sleep wake";
    case ESP_SLEEP_WAKEUP_TIMER:     return "esp cause: timer";
    case ESP_SLEEP_WAKEUP_EXT0:      return "esp cause: ext0 pin";
    case ESP_SLEEP_WAKEUP_EXT1:      return "esp cause: ext1 pin mask";
    case ESP_SLEEP_WAKEUP_GPIO:      return "esp cause: gpio";
    case ESP_SLEEP_WAKEUP_UART:      return "esp cause: uart";
    case ESP_SLEEP_WAKEUP_ULP:       return "esp cause: ulp";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "esp cause: touchpad";
    default:                         return "esp cause: other";
  }
}

const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_SCAN_COMPLETED:  return "scan completed";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "connect failed (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "no radio / unknown";
  }
}

// ---------------------------------------------------------------------------
// Settings-derived helpers
// ---------------------------------------------------------------------------

uint8_t lidForSensor(uint8_t sensor) {
  if (sensor >= ep::kLidCount) return 0;
  const uint8_t lid = gSettings.hw.hallToLid[sensor];
  return (lid < ep::kLidCount) ? lid : sensor;  // validate() should have fixed this
}

const char* lidName(uint8_t lid) {
  if (lid >= ep::kLidCount) return "?";
  const char* name = gSettings.schedule.lids[lid].name;
  return name[0] ? name : ep::defaultLidName(lid, gSettings.schedule.mode);
}

// A mask of lid bits spelled out as the days printed on the box. "0x12" is not
// something anyone wants to decode with a meter in one hand.
void maskToLidNames(uint8_t mask, char* out, size_t n) {
  if (n == 0) return;
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
    if (((mask >> lid) & 1u) == 0) continue;
    const int wrote = snprintf(out + used, n - used, "%slid %u (%s)", used ? ", " : "",
                               (unsigned)lid, lidName(lid));
    if (wrote < 0) break;
    used += (size_t)wrote;
    if (used + 1 >= n) break;  // truncated; what fits is still readable
  }
  if (out[0] == '\0') snprintf(out, n, "none");
}

// The same for a raw *sensor* mask, which is a different numbering: labelled
// the way the live hall table labels its columns, with the day each one sits
// under so the fault is a lid you can put your hand on.
void sensorMaskToNames(uint8_t mask, char* out, size_t n) {
  if (n == 0) return;
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t s = 0; s < ep::kLidCount; ++s) {
    if (((mask >> s) & 1u) == 0) continue;
    const int wrote = snprintf(out + used, n - used, "%ss%u (%s)", used ? ", " : "",
                               (unsigned)s, lidName(lidForSensor(s)));
    if (wrote < 0) break;
    used += (size_t)wrote;
    if (used + 1 >= n) break;
  }
  if (out[0] == '\0') snprintf(out, n, "none");
}

// HallScanner::sensorSaysOpen is private, so the same rule is spelled out here
// for the raw single-shot reads: hallClosedIsLow, flipped per sensor by
// hallInvertMask.
bool hallSaysOpen(uint8_t sensor, bool rawHigh) {
  const bool inverted  = ((gSettings.hw.hallInvertMask >> sensor) & 1u) != 0;
  const bool closedLow = gSettings.hw.hallClosedIsLow != inverted;
  return closedLow ? rawHigh : !rawHigh;
}

// ---------------------------------------------------------------------------
// Line reader
// ---------------------------------------------------------------------------

constexpr size_t kLineMax = 160;  // "n " + 32-char SSID + 64-char password, with room to spare

char   gLine[kLineMax];
size_t gLineLen         = 0;
bool   gAwaitCloseLids  = false;  // the `c` prompt is the one place a bare Enter means something
bool   gCalForce        = false;  // `c force`, carried across the prompt

// One Enter is CR+LF with `pio device monitor`, and the
// two bytes arrive together. Both halves must count as a single keypress, in
// the line reader and in the stop-key check alike, so this remembers whether
// the byte just consumed was the CR - wherever it was consumed.
bool   gLastWasCR       = false;

// Throws away everything pending, so the keystroke that ended a continuous
// mode cannot be mistaken for the next command.
void drainSerial() {
  const uint32_t start = millis();
  uint32_t lastByte = start;
  while (millis() - start < 200u) {
    if (Serial.available() > 0) {
      const int d = Serial.read();
      gLastWasCR  = (d == '\r');  // so an LF that trails us by a packet is still eaten
      lastByte    = millis();
      continue;
    }
    if (millis() - lastByte > 20u) break;
    delay(1);
  }
  gLineLen = 0;
}

// Every continuous mode calls this once per iteration; true means "stop".
//
// It consumes the byte rather than just counting it: dispatch() runs from
// inside the line reader with the LF of the Enter that started the command
// still in the RX FIFO, so a check that stopped on "something is available"
// would abort every streaming mode in the same millisecond it began. The LF
// half of a CR+LF is not a keystroke. Everything else is, Enter's own CR
// included - "any key stops" has to mean the key a human actually presses.
bool keyPressed() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    const bool pairedLf = (c == '\n' && gLastWasCR);
    gLastWasCR = (c == '\r');
    if (pairedLf) continue;
    drainSerial();  // the rest of the burst belongs to the keystroke that stopped us
    return true;
  }
  return false;
}

// A bounded wait that a keystroke can cut short. Returns false if interrupted.
bool waitMs(uint32_t ms) {
  const uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0) {
    if (keyPressed()) return false;
    delay(2);
  }
  return true;
}

// The same wait, but keeping the indicator alive - it needs a service() call
// every couple of milliseconds to hold a colour or finish a beep.
bool waitMsWithLed(uint32_t ms) {
  const uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0) {
    gLed.service(millis());
    if (keyPressed()) return false;
    delay(1);
  }
  return true;
}

void serviceUntilIdle(uint32_t capMs) {
  const uint32_t start = millis();
  while (gLed.busy() && (millis() - start) < capMs) {
    gLed.service(millis());
    if (keyPressed()) break;
    delay(1);
  }
  gLed.stop();
}

// Debounce is on wall time, so give the scanner a stretch of real milliseconds
// before trusting openMask.
// Long enough for the debounce to commit: two scans either side of the window,
// plus slack, taken from the live setting rather than a hard-coded guess.
uint32_t hallSettleMs() {
  const uint32_t d = gSettings.hw.hallDebounceMs;
  return (d < 5u ? 5u : d) * 2u + 20u;
}

void settleHalls(uint32_t ms) {
  const uint32_t start = millis();
  while ((millis() - start) < ms) {
    gHall.scan(millis());
    delay(4);
  }
}

char* nextToken(char** cursor) {
  char* p = *cursor;
  while (*p == ' ' || *p == '\t') ++p;
  if (!*p) {
    *cursor = p;
    return nullptr;
  }
  char* start = p;
  while (*p && *p != ' ' && *p != '\t') ++p;
  if (*p) {
    *p = '\0';
    ++p;
  }
  *cursor = p;
  return start;
}

bool parseUint(const char* s, uint32_t* out) {
  if (!s || !*s) return false;
  uint32_t v = 0;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10u + (uint32_t)(*p - '0');
    if (v > 1000000u) return false;
  }
  *out = v;
  return true;
}

bool sameWord(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// ---------------------------------------------------------------------------
// Time, without an RTC
// ---------------------------------------------------------------------------

void formatEpoch(uint32_t epoch, bool utc, char* out, size_t n) {
  const time_t t = (time_t)epoch;
  struct tm parts;
  if (utc) {
    gmtime_r(&t, &parts);
  } else {
    localtime_r(&t, &parts);
  }
  if (strftime(out, n, "%a %Y-%m-%d %H:%M:%S %Z", &parts) == 0 && n > 0) out[0] = '\0';
}

void printClockRow() {
  const uint32_t now = (uint32_t)time(nullptr);
  if (now < kPlausibleEpoch) {
    row("clock", "NOT SET - no RTC on this board; run  n <ssid> <pass>  to sync it");
    return;
  }
  char when[48];
  formatEpoch(now, false, when, sizeof when);
  row("clock", "%s  (local, from the last NTP sync)", when);
}

// With no RTC chip, everything the clock does between NTP syncs rides on
// RTC_SLOW_CLK, and which source the chip settled on is the one number that
// decides whether drift is parts-per-million or parts-per-thousand. The
// schematic cannot answer it: the 32.768 kHz crystal, if this module carries
// one, sits on GPIO0/GPIO1 inside the XIAO, and neither pad is broken out. So
// ask the silicon.
void printRtcClockRow() {
  const rtc_slow_freq_t src = rtc_clk_slow_freq_get();
  const char* name;
  const char* drift;
  switch (src) {
    case RTC_SLOW_FREQ_32K_XTAL:
      name  = "external 32.768 kHz crystal";
      drift = "tens of ppm - a day of sleep lands within seconds";
      break;
    case RTC_SLOW_FREQ_8MD256:
      name  = "internal 17.5 MHz RC / 256";
      drift = "percent-level and temperature dependent - resync often";
      break;
    case RTC_SLOW_FREQ_RTC:
    default:
      name  = "internal 136 kHz RC";
      drift = "percent-level and temperature dependent - resync often";
      break;
  }
  row("RTC slow clock", "%s, %u Hz", name, (unsigned)rtc_clk_slow_freq_get_hz());
  row("expect drift", "%s", drift);
}

// ---------------------------------------------------------------------------
// Banner and help
// ---------------------------------------------------------------------------

void printPinTable() {
  outf("  net           silk   GPIO  role\n");
  for (const PinRow& p : kPinTable) {
    outf("  %-12s  %-4s   %3u   %s\n", p.net, p.silk, (unsigned)p.gpio, p.role);
  }
  outf("  deep-sleep wake GPIO is %u; POWER_LATCH is GPIO %u (%s)\n",
       (unsigned)ep::pins::kWakeUpGpio, (unsigned)ep::pins::kPowerLatchGpio,
       ep::pins::kLatchIsRtcPad ? "RTC pad" : "plain digital pad, needs the deep-sleep hold");
}

void printHelp() {
  Serial.print(
      "\n"
      "Commands. Case matters for the six pairs below; everything else takes either case.\n"
      "  ?  h             this help\n"
      "  i                chip info, clock and the pin table\n"
      "  s                one-shot self test - \"is the board alive\"\n"
      "  d                mux channel map reminder\n"
      "\n"
      "  H                live hall table at about 5 Hz, any key stops\n"
      "  H <0-6>          read one hall sensor once\n"
      "  a   /  A         all 5 ADC channels once  /  live at about 2 Hz\n"
      "  w   /  W         read WAKE_UP once  /  live\n"
      "  M                monitor: one timestamped line whenever anything changes\n"
      "  j                one machine-readable JSON line; tools/hwtest.py drives it\n"
      "\n"
      "  m <0-7>          park the mux address on one channel and leave it there\n"
      "  o <0-3> <0|1>    drive one SIG_OUT channel, raw FET control\n"
      "  l <r|g|b|w|off>  drive the LED; w time-multiplexes all three\n"
      "  L                identify which CTL1 pin carries which colour\n"
      "  z [ms]           beep as the settings say, default 150 ms; it prints which drive\n"
      "  Z <hz>           set the buzzer tone and sound it once; 0 = self-drive DC\n"
      "  c                calibrate hall polarity with every lid shut\n"
      "  c force          same, but accept a tray whose lids disagree\n"
      "\n"
      "  n <ssid> <pass>  Wi-Fi + NTP smoke test - reads the clock and the date\n"
      "  p <0|1>          POWER_LATCH high/low; \"p 0 yes\" to really drop it\n"
      "  x [s]            deep sleep s seconds, default 10 - the WAKE_UP arming rule\n"
      "  r                reboot\n"
      "\n");
}

void printBanner() {
  Serial.print("\n\n");
  outf("=== %s ===\n", kFirmwareName);
  row("built for", "%s", kBuiltFor);
  row("chip reports", "%s rev %u, %u core(s)", ESP.getChipModel(),
      (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores());
  row("reset reason", "%s", resetReasonName(esp_reset_reason()));
  row("wake reason", "%s  (%s)", ep::toString(ep::lastWakeReason()),
      wakeCauseName(esp_sleep_get_wakeup_cause()));
  row("POWER_LATCH", "%s", ep::PowerLatch::isAsserted()
                               ? "HIGH - asserted before Serial, you can let go of the button"
                               : "LOW - something is wrong, the board should not be running");
  printClockRow();
  Serial.print("\n");
  printPinTable();
  Serial.print(
      "\n"
      "  !! Power this from a bench supply on TP2 (VBAT) / TP5 (GND) -- the on-board\n"
      "  !! protection trips at about 30 mA and will drop the battery as soon as Wi-Fi\n"
      "  !! transmits.\n"
      "\n"
      "  MUX_C is U0TXD and SIG_OUT is U0RXD: this console is USB-CDC and UART0 must\n"
      "  stay shut. One address drives all three muxes, so `d` is worth reading first.\n");
  printHelp();
}

// ---------------------------------------------------------------------------
// i - chip info
// ---------------------------------------------------------------------------

void cmdInfo() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  const uint32_t up = millis();

  Serial.print("\n");
  row("firmware", "%s", kFirmwareName);
  row("built for", "%s", kBuiltFor);
  row("chip", "%s rev %u, %u core(s) at %u MHz", ESP.getChipModel(),
      (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
      (unsigned)getCpuFrequencyMhz());
  row("flash", "%u kB", (unsigned)(ESP.getFlashChipSize() / 1024u));
  row("heap", "%u B free of %u B, low water mark %u B", (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getHeapSize(), (unsigned)ESP.getMinFreeHeap());
  row("MAC (STA)", "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
      mac[5]);
  row("SDK", "%s", ESP.getSdkVersion());
  row("reset reason", "%s", resetReasonName(esp_reset_reason()));
  row("wake reason", "%s  (%s)", ep::toString(ep::lastWakeReason()),
      wakeCauseName(esp_sleep_get_wakeup_cause()));
  row("uptime", "%u.%03u s", up / 1000u, up % 1000u);
  printClockRow();
  printRtcClockRow();
  row("POWER_LATCH", "%s", ep::PowerLatch::isAsserted() ? "HIGH (held)" : "LOW");
  row("WAKE_UP", "%s", ep::wakeUpLineHigh() ? "HIGH" : "LOW");
  if (gBus.channel() < ep::mux::kChannelCount) {
    row("mux address", "%u  (%s | %s | %s)", (unsigned)gBus.channel(),
        kMuxMap[gBus.channel()].hall, kMuxMap[gBus.channel()].sense,
        kMuxMap[gBus.channel()].out);
  } else {
    row("mux address", "not selected yet");
  }
  row("SIG_OUT", "%s", gBus.sigOut() ? "HIGH - a FET gate is energised" : "LOW");
  Serial.print("\n");
  printPinTable();
}

// ---------------------------------------------------------------------------
// d - the channel map
// ---------------------------------------------------------------------------

void cmdMuxMap() {
  Serial.print(
      "\n"
      "  MUX_A/MUX_B/MUX_C address three XS3A4051s at the same time. One address, three\n"
      "  channels. Address = A + 2*B + 4*C.\n"
      "\n"
      "  ch  U20 -> HALL_IN         U4 -> SIG_IN              U5 <- SIG_OUT\n");
  for (uint8_t ch = 0; ch < ep::mux::kChannelCount; ++ch) {
    outf("  %2u  %-22s %-25s %s%s\n", (unsigned)ch, kMuxMap[ch].hall, kMuxMap[ch].sense,
         kMuxMap[ch].out, (ch == ep::mux::kPark) ? "   <- park here" : "");
  }
  Serial.print(
      "\n"
      "  SIG_OUT is driven LOW before every address change, otherwise the sweep walks a\n"
      "  logic high across Q1..Q4 and flashes the LED or chirps the buzzer. Channel 7 is\n"
      "  the safe parking spot: a 100k to GND on the hall mux, nothing on the output mux.\n"
      "  WAKE_UP bypasses all of it, so an event is still visible while the bus is busy.\n");
}

// ---------------------------------------------------------------------------
// ADC table, shared by a / A / s
// ---------------------------------------------------------------------------

void printAdcTable() {
  const ep::HardwareSettings& hw = gSettings.hw;

  gSense.sampleAll(millis());
  const ep::PowerReading& r = gSense.reading();

  uint16_t raw[5];
  for (uint8_t ch = 0; ch < 5; ++ch) raw[ch] = gSense.rawChannelMv(ch, 5);
  gBus.park();

  outf("  ch  signal             raw @ pin      scaled   meaning\n");
  outf("   0  VBAT               %5u mV    %5u mV   battery %u %%   (x %u/%u)\n", raw[0],
       r.vbatMv, (unsigned)r.batteryPercent, (unsigned)hw.vbatDivNum, (unsigned)hw.vbatDivDen);
  outf("   1  5V_USB             %5u mV    %5u mV   USB %s   (x %u/%u)\n", raw[1], r.usbMv,
       r.usbPresent ? "present" : "absent", (unsigned)hw.railDivNum, (unsigned)hw.railDivDen);
  outf("   2  5V_COIL            %5u mV    %5u mV   coil charger %s   (x %u/%u)\n", raw[2],
       r.coilMv, r.coilPresent ? "present" : "absent", (unsigned)hw.railDivNum,
       (unsigned)hw.railDivDen);

  // Channels 3 and 4 exceed the mux's VCC+0.5 V rating and clamp through its ESD
  // diode, so the number is not a measurement - only the threshold means
  // anything.
  if (hw.buttonDivNum == hw.buttonDivDen) {
    outf("   3  BUTTON_SIG         %5u mV          -   CLAMPED / threshold only: %s "
         "(>= %u mV)\n",
         r.buttonRawMv, r.buttonPressed ? "PRESSED" : "released",
         (unsigned)hw.buttonThresholdMv);
  } else {
    outf("   3  BUTTON_SIG         %5u mV    %5u mV   divider rework fitted (x %u/%u): %s "
         "(>= %u mV)\n",
         r.buttonRawMv,
         (uint16_t)((uint32_t)r.buttonRawMv * hw.buttonDivNum / hw.buttonDivDen),
         (unsigned)hw.buttonDivNum, (unsigned)hw.buttonDivDen,
         r.buttonPressed ? "PRESSED" : "released", (unsigned)hw.buttonThresholdMv);
  }
  outf("   4  MCP_CHRG STAT      %5u mV          -   CLAMPED / threshold only: %s "
       "(>= %u mV)\n",
       r.statRawMv, r.statHigh ? "HIGH" : "LOW", (unsigned)hw.statThresholdMv);
  outf("      derived: %s. STAT LOW means charging *or* no input, so charger presence\n",
       r.charging ? "CHARGING" : "not charging");
  outf("      comes from channels 1 and 2, which are real measurements.\n");
  if (!r.valid) {
    outf("  ! AnalogSense reports the reading as invalid - the mux or the ADC is unhappy\n");
  }
}

void cmdAdcOnce() {
  gLed.stop();
  Serial.print("\n");
  printAdcTable();
}

void cmdAdcLive() {
  gLed.stop();
  Serial.print(
      "\n  Live ADC at about 2 Hz. Channels 3 and 4 are CLAMPED - threshold only.\n"
      "  Any key stops.\n");
  while (!keyPressed()) {
    const uint32_t now = millis();
    gSense.sampleAll(now);
    const ep::PowerReading& r = gSense.reading();
    stamp(now);
    outf("VBAT %4u mV %3u %%   USB %4u mV   COIL %4u mV   BTN %4u mV %-8s  "
         "STAT %4u mV %-4s  %s\n",
         r.vbatMv, (unsigned)r.batteryPercent, r.usbMv, r.coilMv, r.buttonRawMv,
         r.buttonPressed ? "PRESSED" : "released", r.statRawMv, r.statHigh ? "high" : "low",
         r.charging ? "charging" : "");
    if (!waitMs(450)) break;
  }
  gBus.park();
}

// ---------------------------------------------------------------------------
// H - hall sensors
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// j - one machine-readable line, for tools/hwtest.py
// ---------------------------------------------------------------------------

// Everything a host-side test needs, in one snapshot taken at one instant. The
// alternative is scraping the human tables, and those are laid out for someone
// at a bench at 3am - they will be reformatted the moment that stops being
// true, and a test that breaks when a column moves is worse than no test.
//
// uptimeMs is in here for one specific reason: it is how a script tells "the
// board stayed up" from "the board rebooted", which is the entire battery
// handover test. Pull USB, plug it back, and see whether uptime kept counting.
void cmdJson() {
  gLed.stop();
  // Settle rather than scan once. The debounce commits a change only on a scan
  // that happens at least hallDebounceMs after the one that first saw it, so a
  // single scan can never move openMask off whatever it was last left at - and
  // a lone `j` would report every lid closed no matter what the sensors say.
  // rawMask is instantaneous and always right; openMask needs the window.
  settleHalls(hallSettleMs());
  const ep::HallState& h = gHall.state();
  gSense.sampleAll(millis());
  const ep::PowerReading& r = gSense.reading();
  gBus.park();

  outf("{\"uptimeMs\":%lu,\"raw\":%u,\"open\":%u,\"gnd\":%u,\"wake\":%u,"
       "\"vbat\":%u,\"pct\":%u,\"usb\":%u,\"coil\":%u,"
       "\"btnMv\":%u,\"btn\":%u,\"statMv\":%u,\"stat\":%u,"
       "\"usbIn\":%u,\"coilIn\":%u,\"chg\":%u,\"valid\":%u}\n",
       (unsigned long)millis(), (unsigned)h.rawMask, (unsigned)h.openMask,
       h.gndRefOk ? 1u : 0u, ep::wakeUpLineHigh() ? 1u : 0u, (unsigned)r.vbatMv,
       (unsigned)r.batteryPercent, (unsigned)r.usbMv, (unsigned)r.coilMv,
       (unsigned)r.buttonRawMv, r.buttonPressed ? 1u : 0u, (unsigned)r.statRawMv,
       r.statHigh ? 1u : 0u, r.usbPresent ? 1u : 0u, r.coilPresent ? 1u : 0u,
       r.charging ? 1u : 0u, r.valid ? 1u : 0u);
}

void cmdHallOnce(uint8_t sensor) {
  gLed.stop();
  const bool gnd  = gBus.groundReferenceOk();
  const bool high = gHall.rawHall(sensor);
  gBus.park();

  const uint8_t lid  = lidForSensor(sensor);
  const bool    open = hallSaysOpen(sensor, high);
  Serial.print("\n");
  row("sensor", "%u  (mux channel %u)", (unsigned)sensor, (unsigned)sensor);
  row("raw level", "%s  (%s)", high ? "HIGH" : "LOW",
      high ? "no magnet over the sensor" : "magnet over the sensor");
  row("derived", "lid %u (%s) is %s", (unsigned)lid, lidName(lid), open ? "OPEN" : "CLOSED");
  row("ground ref", "%s", gnd ? "OK - channel 7 reads LOW, the address lines work"
                              : "FAILED - this reading means nothing");
}

void cmdHallLive() {
  gLed.stop();
  Serial.print(
      "\n  Live hall table at about 5 Hz. Wave a magnet over each sensor. Any key stops.\n"
      "  sNN is the raw sensor output: [1] = no magnet, [.] = magnet. lid0..6 is the\n"
      "  debounced, polarity-corrected and lid-mapped view: O = open, C = closed.\n"
      "  The change counters count *raw* transitions, so a sensor that ticks up while\n"
      "  you keep still is noisy, not triggered.\n");

  // sNN is in sensor order and lid0..6 is in lid order, which are the same
  // thing only while hallToLid is the identity. Print the mapping it is using,
  // because working out which sensor sits under which day is what this command
  // is for: lift one lid, see which sNN moves, write that pair down.
  outf("  hallToLid says:");
  for (uint8_t s = 0; s < ep::kLidCount; ++s) {
    if (s == 4) outf("\n                 ");  // keeps the line inside 80 columns
    outf(" s%u=%s%s", (unsigned)s, lidName(lidForSensor(s)),
         (s + 1 < ep::kLidCount) ? "," : "\n");
  }
  Serial.print(
      "  Lift one lid at a time and note which sNN moves - that sensor belongs to the day\n"
      "  printed on that lid. That pairing is the hall-to-lid map (Settings > Hardware in the GUI).\n\n");

  uint16_t changes[ep::kLidCount] = {0};
  bool     last[ep::kLidCount];

  gHall.resetDebounce(millis());
  settleHalls(120);
  for (uint8_t s = 0; s < ep::kLidCount; ++s) {
    last[s] = ((gHall.state().rawMask >> s) & 1u) != 0;
  }

  uint32_t nextPrint = millis();
  uint8_t  rowsSinceHeader = 0;

  while (!keyPressed()) {
    const uint32_t now = millis();
    gHall.scan(now);

    const uint8_t rawMask = gHall.state().rawMask;
    for (uint8_t s = 0; s < ep::kLidCount; ++s) {
      const bool bit = ((rawMask >> s) & 1u) != 0;
      if (bit != last[s]) {
        last[s] = bit;
        if (changes[s] < 99) ++changes[s];
      }
    }

    if ((int32_t)(now - nextPrint) >= 0) {
      nextPrint = now + 200;

      if (rowsSinceHeader == 0) {
        Serial.print("        time    s0 s1 s2 s3 s4 s5 s6    lid0..6  gnd   WAKE  "
                     "changes s0..s6\n");
      }
      rowsSinceHeader = (uint8_t)((rowsSinceHeader + 1) % 20);

      char halls[3 * ep::kLidCount + 1];
      char* p = halls;
      for (uint8_t s = 0; s < ep::kLidCount; ++s) {
        *p++ = '[';
        *p++ = ((rawMask >> s) & 1u) ? '1' : '.';
        *p++ = ']';
      }
      *p = '\0';

      const uint8_t openMask = gHall.state().openMask;
      char lids[ep::kLidCount + 1];
      for (uint8_t l = 0; l < ep::kLidCount; ++l) {
        lids[l] = ((openMask >> l) & 1u) ? 'O' : 'C';
      }
      lids[ep::kLidCount] = '\0';

      // Hand-formatted rather than snprintf'd: the counters are capped at 99, so
      // two digits and a space are always exactly right.
      char chg[3 * ep::kLidCount + 1];
      for (uint8_t s = 0; s < ep::kLidCount; ++s) {
        const uint16_t n = changes[s];
        chg[3 * s + 0] = (n >= 10) ? (char)('0' + (n / 10)) : ' ';
        chg[3 * s + 1] = (char)('0' + (n % 10));
        chg[3 * s + 2] = ' ';
      }
      chg[3 * ep::kLidCount] = '\0';

      outf("  %6u.%03u s  %s   %-7s  %-4s  %-4s  %s\n", now / 1000u, now % 1000u, halls, lids,
           gHall.state().gndRefOk ? "OK" : "FAIL", ep::wakeUpLineHigh() ? "HIGH" : "LOW", chg);
    }
    delay(4);
  }
  gBus.park();
}

void cmdHall(char* args) {
  char*    tok = nextToken(&args);
  uint32_t sensor = 0;
  if (!tok) {
    cmdHallLive();
    return;
  }
  if (!parseUint(tok, &sensor) || sensor > ep::mux::kHallLast) {
    outf("! usage: H  (live)  or  H <0-6>  (one sensor)\n");
    return;
  }
  cmdHallOnce((uint8_t)sensor);
}

// ---------------------------------------------------------------------------
// w / W - WAKE_UP
// ---------------------------------------------------------------------------

void cmdWakeOnce() {
  const bool hi = ep::wakeUpLineHigh();
  Serial.print("\n");
  row("WAKE_UP", "%s   (D1 / GPIO %u)", hi ? "HIGH" : "LOW", (unsigned)ep::pins::kWakeUpGpio);
  row("meaning", "%s", hi ? "at least one lid is open, or the button is held, or a charger is in"
                          : "every sensor sees its magnet, button up, no charger");
  Serial.print(
      "  WAKE_UP = hall1|..|hall7|BUTTON_SIG|SIG_CHARGE through U2/U3, level triggered.\n"
      "  Level triggered is why the firmware never arms a GPIO wake while it is already\n"
      "  high: that wakes the chip instantly and forever.\n");
}

void cmdWakeLive() {
  Serial.print("\n  Live WAKE_UP at about 4 Hz. Any key stops.\n");
  bool     last    = ep::wakeUpLineHigh();
  uint16_t changes = 0;
  while (!keyPressed()) {
    const uint32_t now = millis();
    const bool     hi  = ep::wakeUpLineHigh();
    if (hi != last) {
      last = hi;
      if (changes < 999) ++changes;
    }
    stamp(now);
    outf("WAKE_UP %-4s   changes %u\n", hi ? "HIGH" : "LOW", (unsigned)changes);
    // The indicator is left running on purpose: WAKE_UP is a direct pin, so you
    // can watch it while the LED holds the mux.
    if (!waitMsWithLed(250)) break;
  }
}

// ---------------------------------------------------------------------------
// m / o - raw mux and raw FET control
// ---------------------------------------------------------------------------

void cmdMuxAddress(char* args) {
  char*    tok = nextToken(&args);
  uint32_t ch  = 0;
  if (!tok || !parseUint(tok, &ch) || ch >= ep::mux::kChannelCount) {
    outf("! usage: m <0-7>\n");
    return;
  }
  gLed.stop();
  gBus.select((uint8_t)ch);  // drives SIG_OUT low first, then settles

  Serial.print("\n");
  row("address", "%u  ->  A=%u B=%u C=%u   (channel = A + 2*B + 4*C)", (unsigned)ch,
      (unsigned)(ch & 1u), (unsigned)((ch >> 1) & 1u), (unsigned)((ch >> 2) & 1u));
  row("HALL_IN", "%-24s reads %s", kMuxMap[ch].hall, gBus.readHall() ? "HIGH" : "LOW");
  row("SIG_IN", "%-24s %u mV at the pin", kMuxMap[ch].sense, gBus.readSenseMvMedian(5));
  row("SIG_OUT", "%-24s held %s", kMuxMap[ch].out, gBus.sigOut() ? "HIGH" : "LOW");
  outf("  The address stays here until the next command. `m %u` parks it; `o` drives the\n"
       "  FET gate on channels 0-3. Nothing else can read a hall sensor meanwhile.\n",
       (unsigned)ep::mux::kPark);
}

void cmdSigOut(char* args) {
  char*    chTok  = nextToken(&args);
  char*    lvlTok = nextToken(&args);
  uint32_t ch = 0, lvl = 0;
  if (!chTok || !lvlTok || !parseUint(chTok, &ch) || !parseUint(lvlTok, &lvl) || ch > 3 ||
      lvl > 1) {
    outf("! usage: o <0-3> <0|1>     0=Q3/CTL1-4  1=Q2/CTL1-3  2=Q1/CTL1-2  3=buzzer\n");
    return;
  }
  gLed.stop();
  gBus.select((uint8_t)ch);       // SIG_OUT goes low before the address moves
  gBus.setSigOut(lvl != 0);

  Serial.print("\n");
  row("channel", "%u  ->  %s", (unsigned)ch, kMuxMap[ch].out);
  row("SIG_OUT", "%s  ->  that FET is %s", lvl ? "HIGH" : "LOW", lvl ? "ON" : "OFF");
  if (ch == ep::mux::kOutBuzzer && lvl) {
    // This, not `z`, is the test that tells the two buzzer types apart: `z`
    // plays whatever the settings say, and both types sound on a tone.
    outf("  DC on the buzzer FET, ON until you type `o 3 0`. A tone that holds for as long\n"
         "  as the FET is on means a self-oscillating piezo -> buzzerSelfDrive = true. A\n"
         "  click at each edge, or silence, means a magnetic buzzer -> leave it false and\n"
         "  find its resonance with `Z <hz>`.\n");
  }
  outf("  The address is parked on %u until the next command, so no hall sensor can be\n"
       "  read right now. `m %u` puts it back.\n",
       (unsigned)ch, (unsigned)ep::mux::kPark);
}

// ---------------------------------------------------------------------------
// l / L - the LED
// ---------------------------------------------------------------------------

void cmdLed(char* args) {
  char* tok = nextToken(&args);
  if (!tok) {
    outf("! usage: l <r|g|b|w|off>\n");
    return;
  }

  uint8_t r = 0, g = 0, b = 0;
  const char* what = nullptr;
  if (sameWord(tok, "off")) {
    gLed.stop();
    gBus.park();
    row("LED", "off, bus released, address parked on %u", (unsigned)ep::mux::kPark);
    return;
  } else if (sameWord(tok, "r")) {
    r = 255;
    what = "red";
  } else if (sameWord(tok, "g")) {
    g = 255;
    what = "green";
  } else if (sameWord(tok, "b")) {
    b = 255;
    what = "blue";
  } else if (sameWord(tok, "w")) {
    r = g = b = 255;
    what = "white (all three, time-multiplexed)";
  } else {
    outf("! usage: l <r|g|b|w|off>\n");
    return;
  }

  gLed.setColor(r, g, b);
  gLed.playPattern(ep::Pattern::Solid, 0);  // 0 = until `l off` or another bus command

  Serial.print("\n");
  row("LED", "%s, held until `l off`", what);
  row("channels", "red=ch%u green=ch%u blue=ch%u, brightness %u/255",
      (unsigned)gSettings.hw.ledChannelRed, (unsigned)gSettings.hw.ledChannelGreen,
      (unsigned)gSettings.hw.ledChannelBlue, (unsigned)gSettings.hw.ledBrightness);
  outf("  Those channel numbers are a guess until you run `L`. A mixed colour costs bus\n"
       "  time, so the hall sensors are only scanned between colour slots.\n");
}

void cmdIdentifyLed() {
  gLed.stop();

  struct Step {
    uint8_t     channel;
    const char* where;
  };
  const Step steps[3] = {
      {ep::mux::kOutQ3Ctl1Pin4, "Q3 -> CTL1 pin 4"},
      {ep::mux::kOutQ2Ctl1Pin3, "Q2 -> CTL1 pin 3"},
      {ep::mux::kOutQ1Ctl1Pin2, "Q1 -> CTL1 pin 2"},
  };

  // Straight DC on one gate at a time, not Indicator::setColor: the point is to
  // light exactly one CTL1 pin with nothing multiplexed, so there is no doubt
  // about which colour belongs to which channel. It still goes through MuxBus,
  // so SIG_OUT is low across every address change.
  Serial.print(
      "\n  Identifying the LED. Each channel is driven for 1.5 s - write down the colour\n"
      "  you see. Any key stops early.\n");
  for (const Step& s : steps) {
    row("live now", "channel %u  (%s)  - what colour is this?", (unsigned)s.channel, s.where);
    gBus.select(s.channel);
    gBus.setSigOut(true);
    const bool finished = waitMs(1500);
    gBus.setSigOut(false);
    if (!finished) break;
  }
  gBus.park();

  Serial.print(
      "\n  Put what you saw into Settings.hw (GUI: advanced > hardware):\n"
      "    ledChannelRed   = the channel that looked red\n"
      "    ledChannelGreen = the channel that looked green\n"
      "    ledChannelBlue  = the channel that looked blue\n");
  row("current guess", "red=ch%u green=ch%u blue=ch%u",
      (unsigned)gSettings.hw.ledChannelRed, (unsigned)gSettings.hw.ledChannelGreen,
      (unsigned)gSettings.hw.ledChannelBlue);
  outf("  470 R in series gives roughly 3 mA of red but under 1.5 mA of green and blue,\n"
       "  so a dim green or blue is the resistors, not a dead channel.\n");
}

// ---------------------------------------------------------------------------
// z / Z - the buzzer
// ---------------------------------------------------------------------------

void cmdBeep(char* args) {
  char*    tok = nextToken(&args);
  uint32_t ms  = 150;
  if (tok && !parseUint(tok, &ms)) {
    outf("! usage: z [ms]\n");
    return;
  }
  if (ms < 10) ms = 10;
  if (ms > 5000) ms = 5000;

  gLed.stop();
  Serial.print("\n");
  if (gSettings.hw.buzzerSelfDrive) {
    row("beep", "%u ms, DC on channel %u (self-oscillating piezo)", (unsigned)ms,
        (unsigned)gSettings.hw.buzzerChannel);
  } else {
    row("beep", "%u ms, %u Hz square wave on channel %u", (unsigned)ms,
        (unsigned)gSettings.hw.buzzerToneHz, (unsigned)gSettings.hw.buzzerChannel);
  }
  gLed.beep((uint16_t)ms, 1);
  serviceUntilIdle(ms + 1000u);
  gBus.park();
  // `z` plays whatever the settings say, and the default is the tone. Both
  // buzzer types sound on a square wave, so this command cannot identify one -
  // say so here, because the wrong buzzerSelfDrive is a silent box.
  if (gSettings.hw.buzzerSelfDrive) {
    outf("  Only a self-oscillating piezo sounds on DC. Silence here means a magnetic\n"
         "  buzzer is fitted: `Z 2700` puts the tone back and starts the sweep.\n");
  } else {
    outf("  That was a tone, and BOTH buzzer types sound on a tone - this says nothing\n"
         "  about which one is fitted. `o 3 1` is the DC test for that; `o 3 0` releases.\n"
         "  Nothing audible? Sweep with `Z <hz>` near the resonance. R27 at 470 R leaves\n"
         "  only 5-7 mA, so quiet is expected either way.\n");
  }
}

void cmdTone(char* args) {
  char*    tok = nextToken(&args);
  uint32_t hz  = 0;
  if (!tok || !parseUint(tok, &hz) || (hz != 0 && (hz < 100 || hz > 10000))) {
    outf("! usage: Z <hz>     0 = self-drive DC, otherwise 100..10000\n");
    return;
  }
  gSettings.hw.buzzerSelfDrive = (hz == 0);
  gSettings.hw.buzzerToneHz    = (uint16_t)hz;

  gLed.stop();
  Serial.print("\n");
  if (hz == 0) {
    row("buzzer", "self-drive: plain DC on the gate, for a self-oscillating piezo");
  } else {
    row("buzzer", "%u Hz square wave on SIG_OUT via LEDC", (unsigned)hz);
  }
  // Sound what was just set, so hunting the resonance is one keystroke a step
  // rather than `Z 2500` followed by `z`.
  gLed.beep(150, 1);
  serviceUntilIdle(1150u);
  gBus.park();
  outf("  This edits the in-RAM copy only. Once you find the loudest tone, put it in\n"
       "  Settings.hw.buzzerToneHz / buzzerSelfDrive.\n");
}

// ---------------------------------------------------------------------------
// c - hall polarity calibration
// ---------------------------------------------------------------------------

void cmdCalibrate(char* args) {
  char* tok = nextToken(&args);
  if (tok && !sameWord(tok, "force")) {
    outf("! usage: c   (or  c force  to accept a tray whose lids disagree)\n");
    return;
  }
  gCalForce = (tok != nullptr);

  gLed.stop();
  gBus.park();
  Serial.print(
      "\n  CALIBRATE HALL POLARITY\n"
      "  Close every lid, so all seven magnets sit over their sensors, then press Enter.\n"
      "  Anything else on the line cancels.\n");
  if (gCalForce) {
    Serial.print(
        "  force: a mask where some sensors read HIGH and others LOW will be accepted and\n"
        "  turned into an invert mask instead of being refused.\n");
  }
  gAwaitCloseLids = true;
}

void finishCalibration(const char* answer) {
  gAwaitCloseLids = false;
  const bool force = gCalForce;
  gCalForce = false;
  if (answer[0] != '\0') {
    outf("  cancelled - `c` again once every lid is shut\n");
    return;
  }

  // calibrateClosed() refuses a mixed mask unless forced, and writes nothing
  // when it does, because an invert bit for the odd sensor out would teach the
  // box that a shut lid reads OPEN and no self-check could catch it. So the
  // refusal has to be reported here, with the lids to go and look at.
  bool          mixed   = false;
  const uint8_t rawMask = gHall.calibrateClosed(&gSettings.hw, 8, force, &mixed);
  if (mixed && !force) {
    char highNames[180], lowNames[180];
    const uint8_t all = (uint8_t)((1u << ep::kLidCount) - 1u);
    sensorMaskToNames(rawMask, highNames, sizeof highNames);
    sensorMaskToNames((uint8_t)(all & ~rawMask), lowNames, sizeof lowNames);
    char rawMixedBits[ep::kLidCount + 1];
    maskToBits(rawMask, ep::kLidCount, rawMixedBits);

    Serial.print("\n");
    row("raw mask", "%s   (0x%02X, bit 0 leftmost, 1 = sensor output HIGH)", rawMixedBits,
        (unsigned)rawMask);
    row("no magnet", "%s", highNames);
    row("magnet seen", "%s", lowNames);
    row("refused", "nothing was written - the sensors disagree, so there is no polarity "
                   "here to learn");
    row("unchanged", "hallClosedIsLow = %s, hallInvertMask = 0x%02X",
        gSettings.hw.hallClosedIsLow ? "true" : "false",
        (unsigned)gSettings.hw.hallInvertMask);
    outf("  Expected with every lid shut: all seven the same. The lids listed as \"no\n"
         "  magnet\" are the odd ones out - that lid is ajar, its magnet is missing or too\n"
         "  far away, or its AH1912 is dead. Check one with `m <channel>` and a magnet in\n"
         "  your hand: HALL_IN must go LOW while the magnet is on the part.\n"
         "  If the tray really is half-inverted by design, `c force` accepts it.\n");
    gBus.park();
    return;
  }

  char rawBits[ep::kLidCount + 1];
  char invBits[ep::kLidCount + 1];
  maskToBits(rawMask, ep::kLidCount, rawBits);
  maskToBits(gSettings.hw.hallInvertMask, ep::kLidCount, invBits);

  Serial.print("\n");
  row("raw mask", "%s   (0x%02X, bit 0 leftmost, 1 = sensor output HIGH)", rawBits,
      (unsigned)rawMask);
  row("result", "hallClosedIsLow = %s", gSettings.hw.hallClosedIsLow ? "true" : "false");
  row("result", "hallInvertMask  = 0x%02X   (%s)", (unsigned)gSettings.hw.hallInvertMask,
      invBits);

  // An all-HIGH mask is a legitimate result for a tray that really is inverted
  // end to end, and it is also exactly what a tray with no magnets in it yet,
  // an unwired loom or a dead sensor supply looks like. Nothing in software can
  // tell those apart, so say so instead of printing a result and moving on.
  const bool allHigh = rawMask == (uint8_t)((1u << ep::kLidCount) - 1u);
  if (allHigh) {
    row("warning", "all seven read HIGH with the lids supposedly shut");
    outf("  That is either a tray inverted end to end, or no magnets, or no sensors - and\n"
         "  the check below cannot tell them apart, because hallClosedIsLow = false is\n"
         "  exactly what makes an all-HIGH mask mean CLOSED.\n"
         "  Prove it with a meter before you carry false into the GUI: `m 0`, then hold a\n"
         "  magnet on hall 1. HALL_IN must go LOW while it is there. If it does not, this\n"
         "  calibration is noise.\n");
  }

  // Prove it: with the same lids still shut, every lid must now read CLOSED.
  gHall.resetDebounce(millis());
  settleHalls(200);
  const uint8_t openMask = gHall.state().openMask;
  if (openMask == 0) {
    if (allHigh) {
      row("check", "every lid reads CLOSED - all-HIGH always does, so this proves nothing");
    } else {
      row("check", "all seven lids now read CLOSED - polarity is right");
    }
  } else {
    char openNames[160];
    maskToLidNames(openMask, openNames, sizeof openNames);
    row("check", "still reads OPEN: %s", openNames);
    outf("  Either those lids are not shut, or their sensors are dead or unwired. A sensor\n"
         "  that cannot see its magnet is a part to fix, not a mask to work around.\n");
  }
  if (!gHall.state().gndRefOk) {
    row("warning", "the channel-7 ground reference did not read LOW, so this calibration "
                   "is built on nonsense - fix the mux first");
  }
  gBus.park();
  Serial.print(
      "  Write those two values into Settings.hw. They are the whole difference between a\n"
      "  magnet that sits over the sensor when the lid is shut and one that approaches as\n"
      "  it opens.\n");
}

// ---------------------------------------------------------------------------
// s - the self test
// ---------------------------------------------------------------------------

void cmdSelfTest() {
  gLed.stop();
  gPassCount = gFailCount = gInfoCount = 0;
  const ep::HardwareSettings& hw = gSettings.hw;

  Serial.print("\n  SELF TEST\n");

  // 1. The address lines, proved by the only channel whose value is knowable.
  const bool gnd = gBus.groundReferenceOk();
  if (gnd) {
    verdict(Verdict::Pass, "mux + address",
            "channel 7 of the hall mux reads LOW through R38, so MUX_A/B/C and U20 work");
  } else {
    verdict(Verdict::Fail, "mux + address",
            "channel 7 must read LOW through the 100k R38 but reads HIGH - check MUX_A/B/C "
            "(GPIO %u/%u/%u), U20 and its supply",
            (unsigned)ep::pins::kMuxA, (unsigned)ep::pins::kMuxB, (unsigned)ep::pins::kMuxC);
  }

  // 2. Seven hall sensors. We cannot know where a magnet is, but a sensor that
  // will not hold a steady level over five reads is not wired, and a glitching
  // address line shows up here first.
  gHall.resetDebounce(millis());
  settleHalls(150);
  for (uint8_t s = 0; s < ep::kLidCount; ++s) {
    const bool first  = gHall.rawHall(s);
    bool       stable = true;
    for (uint8_t k = 0; k < 4; ++k) {
      if (gHall.rawHall(s) != first) stable = false;
    }
    char item[16];
    snprintf(item, sizeof item, "hall %u", (unsigned)s);
    const uint8_t lid = lidForSensor(s);

    if (!gnd) {
      verdict(Verdict::Fail, item, "reads %s, but the ground reference failed so it means "
                                   "nothing",
              first ? "HIGH" : "LOW");
    } else if (!stable) {
      verdict(Verdict::Fail, item,
              "level flickers across five reads - unwired sensor, or MUX_C (GPIO %u) is "
              "being wiggled by something",
              (unsigned)ep::pins::kMuxC);
    } else if (!first) {
      // Driving LOW is the only self-evident proof: an AH1912 that is powered,
      // soldered and seeing a magnet pulls the line down. Nothing else here can.
      verdict(Verdict::Pass, item, "LOW (magnet present) -> lid %u (%s) CLOSED",
              (unsigned)lid, lidName(lid));
    } else {
      // HIGH is the sensor's idle state - and also what an unpowered, floating
      // or unsoldered one reads, because nothing is driving the mux input
      // either way. Calling that a PASS is how a ripped ground pad gets signed
      // off as working, which is exactly what happened on this board. Only a
      // magnet can tell the two apart, so say so instead of claiming a result.
      verdict(Verdict::Info, item,
              "HIGH -> lid %u (%s) OPEN. Idle, OR unpowered/floating - a dead sensor "
              "reads the same. Prove it with `H` and a magnet.",
              (unsigned)lid, lidName(lid));
    }
  }

  // 3. The five analog channels, numbers first and then a verdict each.
  Serial.print("\n");
  printAdcTable();
  Serial.print("\n");
  const ep::PowerReading& r = gSense.reading();

  if (r.vbatMv >= 3000 && r.vbatMv <= 4400) {
    verdict(Verdict::Pass, "adc 0 VBAT", "%u mV (%u %%) - inside 3000..4400 mV", r.vbatMv,
            (unsigned)r.batteryPercent);
  } else {
    verdict(Verdict::Fail, "adc 0 VBAT",
            "%u mV is outside 3000..4400 mV - bench supply on TP2/TP5? divider x %u/%u?",
            r.vbatMv, (unsigned)hw.vbatDivNum, (unsigned)hw.vbatDivDen);
  }

  // We are talking over USB-CDC, so VBUS is physically present and the 5V_USB
  // channel has a right answer.
  if (Serial) {
    if (r.usbMv >= 4000) {
      verdict(Verdict::Pass, "adc 1 5V_USB", "%u mV, and a USB host is attached - agrees",
              r.usbMv);
    } else {
      verdict(Verdict::Fail, "adc 1 5V_USB",
              "a USB host is attached, so VBUS is present, yet this reads %u mV - check the "
              "100k/75k divider (R9/R10) and U4 pin 14",
              r.usbMv);
    }
  } else {
    verdict(Verdict::Info, "adc 1 5V_USB", "%u mV, USB %s (headless, nothing to compare to)",
            r.usbMv, r.usbPresent ? "present" : "absent");
  }

  verdict(Verdict::Info, "adc 2 5V_COIL", "%u mV, coil charger %s", r.coilMv,
          r.coilPresent ? "present" : "absent");

  if (!r.buttonPressed) {
    verdict(Verdict::Pass, "adc 3 BUTTON", "%u mV at the pin -> released (threshold %u mV)",
            r.buttonRawMv, (unsigned)hw.buttonThresholdMv);
  } else {
    verdict(Verdict::Fail, "adc 3 BUTTON",
            "%u mV -> reads PRESSED; if nobody is holding the button this is D5 leaking "
            "into R31",
            r.buttonRawMv);
  }

  verdict(Verdict::Info, "adc 4 STAT",
          "%u mV -> %s; LOW means charging *or* no input, so read it with channels 1 and 2",
          r.statRawMv, r.statHigh ? "HIGH" : "LOW");

  // 4. The OR tree. This is the one test that can prove U2/U3 are alive.
  const uint8_t rawMask  = gHall.state().rawMask;
  const bool    chargerIn = r.usbPresent || r.coilPresent;
  const bool    expect   = (rawMask != 0) || r.buttonPressed || chargerIn;
  const bool    actual   = ep::wakeUpLineHigh();
  char rawBits[ep::kLidCount + 1];
  maskToBits(rawMask, ep::kLidCount, rawBits);
  if (actual == expect) {
    verdict(Verdict::Pass, "WAKE_UP", "%s, and that is what halls %s + button %s + charger "
                                      "%s add up to",
            actual ? "HIGH" : "LOW", rawBits, r.buttonPressed ? "down" : "up",
            chargerIn ? "in" : "out");
  } else {
    verdict(Verdict::Fail, "WAKE_UP",
            "reads %s but halls %s + button %s + charger %s say it should be %s - check U2/U3 "
            "(74LV32A) and their supply",
            actual ? "HIGH" : "LOW", rawBits, r.buttonPressed ? "down" : "up",
            chargerIn ? "in" : "out", expect ? "HIGH" : "LOW");
  }

  if (ep::PowerLatch::isAsserted()) {
    verdict(Verdict::Pass, "POWER_LATCH", "HIGH - asserted at boot and still held");
  } else {
    // On USB the board runs regardless, which is exactly how this failure hides
    // until the first battery run.
    verdict(Verdict::Fail, "POWER_LATCH",
            "LOW, yet the board is running - USB is feeding 3V3 and the latch would have "
            "dropped on battery; check D10 (GPIO %u) and the pad hold",
            (unsigned)ep::pins::kPowerLatchGpio);
  }

  // 5. The parts only a human can grade.
  Serial.print("\n  Watch the LED and listen to the buzzer:\n");
  const struct {
    uint8_t     red, green, blue;
    const char* name;
    uint8_t     channel;
  } flashes[3] = {
      {255, 0, 0, "red", hw.ledChannelRed},
      {0, 255, 0, "green", hw.ledChannelGreen},
      {0, 0, 255, "blue", hw.ledChannelBlue},
  };
  for (const auto& f : flashes) {
    row("flashing", "%s, which settings say is channel %u", f.name, (unsigned)f.channel);
    gLed.setColor(f.red, f.green, f.blue);
    gLed.playPattern(ep::Pattern::Solid, 350);
    waitMsWithLed(400);
    gLed.stop();
  }
  gLed.beep(150, 1);
  serviceUntilIdle(1200);
  gBus.park();

  verdict(Verdict::Info, "LED", "three colours were driven on channels %u/%u/%u - did you see "
                                "red, then green, then blue? If the order was wrong, run `L`",
          (unsigned)hw.ledChannelRed, (unsigned)hw.ledChannelGreen,
          (unsigned)hw.ledChannelBlue);
  verdict(Verdict::Info, "buzzer",
          "one %u ms %s on channel %u - did you hear it? Either buzzer type sounds on a "
          "tone, so `o 3 1` is the test for which one is fitted",
          150u, hw.buzzerSelfDrive ? "DC pulse" : "tone", (unsigned)hw.buzzerChannel);

  Serial.print("\n");
  row("result", "%u PASS, %u FAIL, %u unproven (need you, a magnet, or your ears)",
      (unsigned)gPassCount, (unsigned)gFailCount, (unsigned)gInfoCount);
  if (gFailCount == 0) {
    outf("  Nothing electrical failed - but an idle hall is UNPROVEN, not passed: a sensor\n"
         "  with no ground reads HIGH exactly like a good one with no magnet. Next: `H`\n"
         "  with a magnet over all seven, `c` to fix polarity, `L` to name the LED\n"
         "  colours, `n <ssid> <pass>` to set the clock, then `x` for the sleep rule.\n");
  } else {
    outf("  Fix the failures before trusting anything else. `d` for the channel map,\n"
         "  the README for the known board errors and how to fix them.\n");
  }
}

// ---------------------------------------------------------------------------
// M - monitor mode, the demo
// ---------------------------------------------------------------------------

// Levels that only matter once they have held still for a while: the button
// because D5 leaks into BUTTON_SIG, the charger because a contact bounces, and
// WAKE_UP because it follows all of them.
struct Level {
  bool     state   = false;
  bool     pending = false;
  uint32_t since   = 0;

  void prime(bool initial, uint32_t nowMs) {
    state   = initial;
    pending = initial;
    since   = nowMs;
  }

  // True exactly once, on the edge where the new level becomes trustworthy.
  bool update(bool raw, uint32_t nowMs, uint16_t stableMs) {
    if (raw != pending) {
      pending = raw;
      since   = nowMs;
    }
    if (raw != state && (nowMs - since) >= (uint32_t)stableMs) {
      state = raw;
      return true;
    }
    return false;
  }
};

void cmdMonitor() {
  gLed.stop();
  const ep::HardwareSettings& hw = gSettings.hw;

  Serial.print("\n  MONITOR - one line whenever anything changes. Any key stops.\n");
  outf("  Lids are debounced %u ms, the button %u ms because D5 leaks into BUTTON_SIG and\n"
       "  can fake a press. The battery is reported on every\n"
       "  50 mV of movement.\n\n",
       (unsigned)hw.hallDebounceMs, (unsigned)hw.buttonDebounceMs);

  gHall.resetDebounce(millis());
  settleHalls(150);
  uint32_t now = millis();
  gSense.sampleAll(now);
  const ep::PowerReading& r = gSense.reading();

  uint8_t  lastOpen = gHall.state().openMask;
  uint16_t lastVbat = r.vbatMv;
  Level    button, charger, wake, gnd;
  button.prime(r.buttonPressed, now);
  charger.prime(r.usbPresent || r.coilPresent, now);
  wake.prime(ep::wakeUpLineHigh(), now);
  gnd.prime(gHall.state().gndRefOk, now);

  char lids[ep::kLidCount + 1];
  for (uint8_t l = 0; l < ep::kLidCount; ++l) lids[l] = ((lastOpen >> l) & 1u) ? 'O' : 'C';
  lids[ep::kLidCount] = '\0';
  stamp(now);
  outf("BASELINE  lids %s, button %s, charger %s, WAKE_UP %s, battery %u mV (%u %%)\n", lids,
       button.state ? "down" : "up", charger.state ? "in" : "out",
       wake.state ? "HIGH" : "LOW", lastVbat, (unsigned)r.batteryPercent);

  uint32_t lastSense = now;
  while (!keyPressed()) {
    now = millis();
    gHall.scan(now);

    const uint8_t openMask = gHall.state().openMask;
    if (openMask != lastOpen) {
      for (uint8_t lid = 0; lid < ep::kLidCount; ++lid) {
        const uint8_t bit = (uint8_t)(1u << lid);
        if ((openMask & bit) != (lastOpen & bit)) {
          stamp(now);
          outf("LID %u (%s) %s\n", (unsigned)lid, lidName(lid),
               (openMask & bit) ? "OPENED" : "CLOSED");
        }
      }
      lastOpen = openMask;
    }

    if (gnd.update(gHall.state().gndRefOk, now, 50)) {
      stamp(now);
      outf("GROUND REFERENCE %s\n",
           gnd.state ? "OK again" : "FAILED - every hall reading above is meaningless");
    }

    if (wake.update(ep::wakeUpLineHigh(), now, 20)) {
      stamp(now);
      outf("WAKE_UP went %s\n", wake.state ? "HIGH" : "LOW");
    }

    // Sampling five ADC channels costs about 2 ms of bus time, so it is not
    // worth doing on every pass; a lid is still caught within one scan.
    if ((now - lastSense) >= 200u) {
      lastSense = now;
      gSense.sampleAll(now);

      if (button.update(r.buttonPressed, now, hw.buttonDebounceMs)) {
        stamp(now);
        outf("BUTTON %s  (%u mV at the pin)\n", button.state ? "PRESSED" : "RELEASED",
             r.buttonRawMv);
      }
      if (charger.update(r.usbPresent || r.coilPresent, now, 200)) {
        stamp(now);
        outf("CHARGER %s  (USB %u mV, coil %u mV, STAT %s)\n", charger.state ? "IN" : "OUT",
             r.usbMv, r.coilMv, r.statHigh ? "high" : "low");
      }
      if (absDiff(r.vbatMv, lastVbat) >= 50) {
        const bool rising = r.vbatMv > lastVbat;
        lastVbat = r.vbatMv;
        stamp(now);
        outf("BATTERY %u mV (%u %%), %s\n", r.vbatMv, (unsigned)r.batteryPercent,
             rising ? "rising" : "falling");
      }
    }
    delay(3);
  }
  gBus.park();
  outf("  monitor stopped\n");
}

// ---------------------------------------------------------------------------
// n - Wi-Fi and NTP, i.e. where the clock comes from
// ---------------------------------------------------------------------------

void cmdNetTest(char* args) {
  const char* ssid = nextToken(&args);
  if (!ssid) {
    outf("! usage: n <ssid> <password>    the password may contain spaces, the SSID may "
         "not; leave the password off for an open network\n");
    return;
  }
  while (*args == ' ') ++args;
  const char* pass = (*args != '\0') ? args : nullptr;

  Serial.print(
      "\n"
      "  !! This is the command most likely to kill the board on battery: the AH-variant\n"
      "  !! protection trips at about 30 mA and Wi-Fi TX is far above that, so the cell\n"
      "  !! disconnects about 10 ms into the first burst. Run it\n"
      "  !! from a bench supply on TP2 (VBAT) / TP5 (GND).\n"
      "  !! Any key in the next 1.5 s aborts.\n");
  if (!waitMs(1500)) {
    outf("  aborted\n");
    return;
  }

  gLed.stop();
  gBus.park();  // Wi-Fi does not touch the mux, but leave the bus somewhere safe

  WiFi.persistent(false);  // a test tool has no business writing credentials to NVS
  WiFi.mode(WIFI_STA);
  if (pass) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin(ssid);
  }
  Serial.print("\n");
  row("connecting", "\"%s\" (%s)", ssid, pass ? "with a password" : "open");

  const uint32_t connectStart = millis();
  bool           up      = false;
  bool           aborted = false;
  bool           synced  = false;
  while ((millis() - connectStart) < 20000u) {
    if (WiFi.status() == WL_CONNECTED) {
      up = true;
      break;
    }
    if (keyPressed()) {
      aborted = true;
      break;
    }
    delay(50);
  }

  if (up) {
    const IPAddress ip = WiFi.localIP();
    const IPAddress gw = WiFi.gatewayIP();
    row("connected in", "%u ms", (unsigned)(millis() - connectStart));
    row("IP", "%u.%u.%u.%u   gateway %u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1],
        (unsigned)ip[2], (unsigned)ip[3], (unsigned)gw[0], (unsigned)gw[1], (unsigned)gw[2],
        (unsigned)gw[3]);
    row("signal", "%d dBm on channel %u", (int)WiFi.RSSI(), (unsigned)WiFi.channel());

    // There is no RTC, so this is the only way the box ever learns the time or
    // the date.
    const uint32_t before = (uint32_t)time(nullptr);
    configTzTime(gSettings.time.tz, gSettings.time.ntp1, gSettings.time.ntp2);
    row("NTP servers", "%s%s%s", gSettings.time.ntp1,
        gSettings.time.ntp2[0] ? ", " : "", gSettings.time.ntp2);
    row("TZ", "%s", gSettings.time.tz);

    const uint32_t syncStart = millis();
    while ((millis() - syncStart) < 15000u) {
      if ((uint32_t)time(nullptr) >= kPlausibleEpoch) {
        synced = true;
        break;
      }
      if (keyPressed()) {
        aborted = true;
        break;
      }
      delay(100);
    }

    if (synced) {
      const uint32_t after = (uint32_t)time(nullptr);
      char localStr[48], utcStr[48];
      formatEpoch(after, false, localStr, sizeof localStr);
      formatEpoch(after, true, utcStr, sizeof utcStr);
      row("synced in", "%u ms", (unsigned)(millis() - syncStart));
      row("local time", "%s", localStr);
      row("UTC time", "%s", utcStr);
      row("epoch", "%u s", after);
      if (before < kPlausibleEpoch) {
        row("clock step", "the clock had never been set - it was showing 1970-01-01 plus "
                          "%u s of uptime",
            before);
      } else {
        const int32_t delta = (int32_t)(after - before);
        row("clock step", "%s%d s against the pre-sync clock", (delta >= 0) ? "+" : "",
            (int)delta);
      }
      Serial.print(
          "  That clock now lives in RAM and the RTC timer: it survives deep sleep but not\n"
          "  a power cycle, and the internal RC oscillator is good to only 3-5 %, i.e. up to\n"
          "  a couple of minutes a day.\n");
      outf("  So the application re-syncs on every Wi-Fi connection and at least every %u h,\n",
           (unsigned)(gSettings.time.resyncIntervalSec / 3600u));
      Serial.print(
          "  learns a ppm correction from the gap between two syncs, and persists the last\n"
          "  epoch to NVS so a cold boot starts roughly right. Until the first sync it\n"
          "  timestamps lid events monotonically and marks them time_unverified.\n");
    } else if (aborted) {
      row("NTP", "aborted");
    } else {
      row("NTP", "no answer in 15 s - DNS or UDP 123 blocked? captive portal?");
      outf("  Without a sync there is no date and no time of day on this board at all.\n");
    }
  } else if (aborted) {
    row("Wi-Fi", "aborted after %u ms", (unsigned)(millis() - connectStart));
  } else {
    row("Wi-Fi", "no connection in 20 s: %s", wifiStatusName(WiFi.status()));
    Serial.print(
        "  The ESP32 is 2.4 GHz only, will not join WPA3-only networks, and needs the\n"
        "  SSID spelled exactly. A password with spaces is fine - everything after the\n"
        "  SSID is taken verbatim.\n");
  }

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  // The clock may already have been set by an earlier `n`, so report what the
  // board actually knows rather than what this attempt achieved.
  const bool haveClock = (uint32_t)time(nullptr) >= kPlausibleEpoch;
  row("radio", "disconnected and off%s",
      haveClock ? ", and the clock keeps running without it" : ", and the clock is unset");
}

// ---------------------------------------------------------------------------
// p / r - the latch and the reset
// ---------------------------------------------------------------------------

void cmdLatch(char* args) {
  char* levelTok = nextToken(&args);
  char* confirm  = nextToken(&args);
  if (!levelTok || levelTok[1] != '\0' || (levelTok[0] != '0' && levelTok[0] != '1')) {
    outf("! usage: p <0|1>     use  p 0 yes  to really drop the latch\n");
    return;
  }

  if (levelTok[0] == '1') {
    ep::PowerLatch::assertEarly();
    Serial.print("\n");
    row("POWER_LATCH", "HIGH, pad hold armed - the board stays up with the button released");
    return;
  }

  if (!confirm || !sameWord(confirm, "yes")) {
    Serial.print(
        "\n"
        "  !! STOP. `p 0` drops POWER_LATCH. On battery that ends this session: the board\n"
        "  !! powers off and you have to hold the button again to bring it back. With USB\n"
        "  !! plugged in the XIAO's own regulator keeps 3V3 alive, so nothing visible\n"
        "  !! happens.\n"
        "  !! Type   p 0 yes   if that is really what you want.\n");
    return;
  }

  row("POWER_LATCH", "going LOW now");
  Serial.flush();
  ep::PowerLatch::releaseHold();
  ep::PowerLatch::powerOff();
  delay(250);
  // Still alive means USB is feeding 3V3 past the latch.
  row("still running", "USB is holding 3V3 up, so the latch dropped but the chip did not");
}

// ---------------------------------------------------------------------------
// x - deep sleep, and the level-triggered WAKE_UP arming rule
// ---------------------------------------------------------------------------

void cmdSleepTest(char* args) {
  char*    tok  = nextToken(&args);
  uint32_t secs = 10;
  if (tok && (!parseUint(tok, &secs) || secs < 1 || secs > 300)) {
    outf("! usage: x [seconds]     default 10, 1..300\n");
    return;
  }

  gLed.stop();
  gBus.park();

  const bool wakeHigh = ep::wakeUpLineHigh();
  Serial.print("\n");
  row("sleep", "%u s requested", (unsigned)secs);
  if (wakeHigh) {
    row("WAKE_UP", "HIGH -> timer only; a level wake would fire instantly, forever");
    outf("  That fallback is the point of this test, and on USB it is the normal path:\n"
         "  SIG_CHARGE holds WAKE_UP high, so enterDeepSleep() refuses the pin wake.\n"
         "  Expect wake reason `timer`.\n");
  } else {
    row("WAKE_UP", "LOW -> arming the GPIO %u high-level wake as well as the timer",
        (unsigned)ep::pins::kWakeUpGpio);
    outf("  Lift a lid while it sleeps and it should come back early, reason `wakeUpPin`.\n");
  }
  row("latch", "pad hold re-armed for the sleep (%s)",
      ep::pins::kLatchIsRtcPad ? "RTC pad: gpio_hold_en is enough"
                               : "plain pad: + gpio_deep_sleep_hold_en");
  outf("  The board must come back on its own, banner and all, in about %u s and with\n"
       "  POWER_LATCH still HIGH - that is the pad hold working. `i` then reports the wake\n"
       "  reason. USB-CDC drops while it sleeps, so the monitor goes quiet and reattaches;\n"
       "  if it does not, reopen it.\n",
       (unsigned)secs);
  Serial.flush();
  delay(100);
  ep::enterDeepSleep(secs, true);  // checks the line itself; does not return
}

void cmdReboot() {
  outf("\n  rebooting - POWER_LATCH is pad-held, so it survives the reset\n");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void dispatch(char* line) {
  char* cursor = line;
  char* tok    = nextToken(&cursor);
  if (!tok) return;
  if (tok[1] != '\0') {
    outf("! unknown command \"%s\" - ? for help\n", tok);
    return;
  }

  switch (tok[0]) {
    // Case is meaningful for these six pairs and nowhere else.
    case 'h': case '?': printHelp(); break;
    case 'H': cmdHall(cursor); break;
    case 'm': cmdMuxAddress(cursor); break;
    case 'M': cmdMonitor(); break;
    case 'a': cmdAdcOnce(); break;
    case 'A': cmdAdcLive(); break;
    case 'l': cmdLed(cursor); break;
    case 'L': cmdIdentifyLed(); break;
    case 'z': cmdBeep(cursor); break;
    case 'Z': cmdTone(cursor); break;
    case 'w': cmdWakeOnce(); break;
    case 'W': cmdWakeLive(); break;

    case 'i': case 'I': cmdInfo(); break;
    case 's': case 'S': cmdSelfTest(); break;
    case 'd': case 'D': cmdMuxMap(); break;
    case 'j': case 'J': cmdJson(); break;
    case 'o': case 'O': cmdSigOut(cursor); break;
    case 'c': case 'C': cmdCalibrate(cursor); break;
    case 'p': case 'P': cmdLatch(cursor); break;
    case 'n': case 'N': cmdNetTest(cursor); break;
    case 'x': case 'X': cmdSleepTest(cursor); break;
    case 'r': case 'R': cmdReboot(); break;

    default: outf("! unknown command '%c' - ? for help\n", tok[0]); break;
  }
}

void pumpSerial() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;

    // One Enter, one line. The monitor sends CR+LF and the `c` prompt treats a
    // bare Enter as its answer, so ending a line on both halves would let the
    // LF of the keypress that ran `c` answer the prompt `c` has just printed -
    // and calibration would sample the lids before the operator touched them.
    const bool pairedLf = (c == '\n' && gLastWasCR);
    gLastWasCR = (c == '\r');
    if (pairedLf) continue;

    if (c == '\n' || c == '\r') {
      gLine[gLineLen] = '\0';
      const size_t len = gLineLen;
      gLineLen = 0;
      if (gAwaitCloseLids) {
        finishCalibration(gLine);  // a bare Enter is the answer here, not an empty line
      } else if (len > 0) {
        dispatch(gLine);
      }
      continue;
    }
    if (c == 0x08 || c == 0x7F) {  // backspace, for humans in a terminal
      if (gLineLen > 0) --gLineLen;
      continue;
    }
    if (c < 0x20) continue;  // stray control bytes, e.g. an escape sequence
    if (gLineLen + 1u >= kLineMax) {
      gLineLen = 0;
      outf("! line too long, discarded\n");
      continue;
    }
    gLine[gLineLen++] = (char)c;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

void setup() {
  // First statement, before Serial, before anything: the user is holding the
  // button and the board dies the moment they let go without this.
  ep::PowerLatch::assertEarly();

  Serial.begin(115200);
  // Wait for a USB host, but briefly: this firmware has to work headless too,
  // and a board that hangs here looks identical to a dead board.
  const uint32_t serialStart = millis();
  while (!Serial && (millis() - serialStart) < 1500u) delay(10);

  ep::applyDefaults(gSettings);
  const bool repaired = ep::validate(gSettings);

  // Apply the timezone without touching the network, so any time printed before
  // the first sync is at least in the right zone. `n` starts SNTP.
  setenv("TZ", gSettings.time.tz, 1);
  tzset();

  gBus.begin();
  gHall.begin(&gBus, &gSettings.hw);
  gSense.begin(&gBus, &gSettings.hw);
  gLed.begin(&gBus, &gSettings.hw);
  gBus.park();

  // WAKE_UP is driven by the 74LV32A push-pull output and belongs to no driver
  // class, so it is the one pin this file sets up itself.
  pinMode(ep::pins::kWakeUp, INPUT);

  printBanner();
  if (repaired) {
    outf("! ep::validate() had to repair the default settings - that is a bug in "
         "easypill_core\n");
  }
  Serial.print("> ready\n");
}

void loop() {
  pumpSerial();
  // The indicator holds the mux in short slots and needs servicing every couple
  // of milliseconds; it does nothing and holds no bus while idle.
  gLed.service(millis());
}
