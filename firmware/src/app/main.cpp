// EasyPill: the application.
//
// The ordering inside loop() is the design, not a style choice. All three muxes
// share one address bus, so the LED, the hall
// sensors and the ADC cannot run at the same time - they take turns, in this
// order, and every handler has to give the bus back quickly. Nothing here
// blocks for more than a few milliseconds except four calls: netScan(),
// notifyTest() and timeSyncBlocking(), which are reachable only from a GUI
// action, and notifyService(), which sends at most one queued push per pass and
// is bounded by that one transport's connect plus read timeout.
#include <Arduino.h>

#include "app_state.h"
#include "board_pins.h"
#include "ep_adherence.h"
#include "ep_schedule.h"
#include "ep_timecalc.h"
#include "net_wifi.h"
#include "notifier.h"
#include "power_latch.h"
#include "store.h"
#include "timekeeper.h"
#include "web_server.h"

namespace ep {

constexpr uint32_t kAnalogIntervalMs    = 2000;
constexpr uint32_t kSchedulerIntervalMs = 10000;
constexpr uint32_t kSensorFaultLogMs    = 60000;

// Hall debounce works on wall time, so polling faster than this buys nothing but
// burns mux time: at the 60 ms default debounce, 4 ms gives fifteen samples per
// decision. While the LED holds the bus the scan runs on every pass instead, in
// the gap between colour slots.
constexpr uint32_t kHallIdleIntervalMs = 4;

// Three consecutive samples at 2 s apart before dropping the latch: a single bad
// ADC read must never be able to switch the box off.
constexpr uint8_t  kCriticalSamples     = 3;
constexpr uint32_t kLowBatteryRepeatSec = 3600;
// Below this the VBAT channel is not reading a battery - no cell fitted, or the
// divider is open - and a shutdown decision on that reading would be wrong.
constexpr uint16_t kVbatSanityFloorMv   = 1000;

constexpr uint32_t kMinSleepSec   = 10;
constexpr uint32_t kBootGraceSec  = 10;
// Give SNTP a chance before the first sleep of a power cycle, or a box with a
// stale NVS seed would sleep through its doses. millis() restarts at zero on
// every wake, so this window has to be keyed on the wake reason: keyed on
// millis() alone it re-arms on every wake, and a box that cannot reach NTP then
// spends a full minute awake out of every seventy for the life of the cell -
// the SNTP retry deadline pins the sleep that follows to kMinSleepSec.
constexpr uint32_t kFirstSyncGraceMs = 60000;
// A timer or pin wake only has to cover an association plus one SNTP round
// trip. The stale-seed problem the minute above protects against was already
// settled at power-on.
constexpr uint32_t kWakeSyncGraceMs  = 15000;
// A raised reminder holds sleep off long enough for the pulse to finish and for
// a push to leave if one is configured. Deliberately not awakeAfterEventSec
// (300 s by default): a dose happens on a schedule, not on somebody touching
// the box, so five minutes of radio per dose is not worth paying.
constexpr uint32_t kReminderAwakeSec = 20;

static uint32_t lastAnalogMs      = 0;
static uint32_t lastSchedulerMs   = 0;
static uint32_t lastHallScanMs    = 0;
static uint32_t lastSensorFaultMs = 0;

static uint8_t criticalSamples    = 0;
static int8_t  lastChargerPresent = -1;  // -1 until the first sample says
// Both of these live in RTC slow memory, next to the "already fired" guards
// below. BUTTON_SIG can sit high for hours - a stuck button, or D5 leaking into
// it on an unreworked board - and that is one
// press, not a fresh press on every wake.
static RTC_DATA_ATTR uint8_t buttonSamples  = 0;
static RTC_DATA_ATTR bool    buttonReported = false;

// Deep sleep wipes RAM, so the "already fired" guards live in RTC slow memory.
// Without that a wake landing after the daily-summary minute would fire the
// summary again on every single wake.
static RTC_DATA_ATTR uint32_t firedDueLocal      = 0;
static RTC_DATA_ATTR uint32_t firedReminderLocal = 0;
static RTC_DATA_ATTR uint32_t firedSummaryDay    = 0;
static RTC_DATA_ATTR uint32_t lastLowBatteryUtc  = 0;

static void logAndNotify(EventType t, uint8_t lid = kNoLid, int16_t detail = 0) {
  g.logEvent(t, lid, detail);
  if (g.events.size() != 0) notifyEvent(g.events.at(0));
}

static void flushPersistent() {
  if (g.settingsDirty && storeSaveSettings(g.settings)) g.settingsDirty = false;
  storeSaveEvents(g.events);
  storeSaveAdherence(g.adherence);
}

// Settle the debounce before loop() starts looking at changes, so a lid that was
// already open when we woke up is the state we start from rather than a fresh
// open to report.
static void seedSensors() {
  g.halls.resetDebounce(millis());
  const uint32_t until = millis() + g.settings.hw.hallDebounceMs + 20;
  while ((int32_t)(millis() - until) < 0) {
    g.halls.scan(millis());
    delay(2);
  }
  lastHallScanMs = millis();
  g.bus.park();
}

static void handleLidChanges() {
  const HallState& hs = g.halls.state();
  if (hs.changedMask == 0) return;

  for (uint8_t lid = 0; lid < kLidCount; ++lid) {
    const uint8_t bit = (uint8_t)(1u << lid);
    if ((hs.changedMask & bit) == 0) continue;

    if ((hs.openMask & bit) == 0) {
      g.logEvent(EventType::LidClosed, lid);
      continue;
    }

    // onLidOpen is "every lid, every time", and the
    // simulator pushes lidOpened before it classifies anything. The toggle is
    // what keeps this quiet, not this call site - logging it and not notifying
    // made the checkbox a no-op on the real box. With no trustworthy clock this
    // is also the only message, and its localEpoch 0 is back-filled after the
    // first sync.
    logAndNotify(EventType::LidOpened, lid);

    if (g.clockUsable()) {
      MatchResult match{};
      g.adherence.recordOpen(g.settings.schedule, lid, g.nowLocal(), &match);

      if (match.matched) {
        // Right time, wrong compartment. This is the one mistake seven hall
        // sensors are genuinely good at spotting, and it *replaces* the
        // dose-taken message rather than arriving next to it: the dose record
        // still holds the open, but nobody may be told the dose was taken.
        if (match.wrongDay) {
          logAndNotify(EventType::WrongDayOpen, lid, match.deltaMin);
        } else {
          logAndNotify(EventType::DoseTaken, lid, match.deltaMin);
        }
        g.led.playPattern(Pattern::DoubleBlinkGreen, 1200);
        if (g.settings.power.buzzerOnReminder) g.led.beep(60, 1);
      } else {
        logAndNotify(EventType::UnexpectedOpen, lid);
      }
    }

    g.keepAwakeFor(g.settings.power.awakeAfterEventSec);
  }
}

static void schedulerTick() {
  if (!g.clockUsable()) return;

  const Schedule& sch = g.settings.schedule;
  const uint32_t  now = g.nowLocal();

  // 1. Anything whose window closed without being opened.
  DoseRecord*  missed[8];
  const size_t missedCount =
      g.adherence.sweepMissed(sch, now, 3, missed, 8, g.bootFloorLocal);
  for (size_t i = 0; i < missedCount; ++i) {
    // detail carries the dose's minute-of-day so the sentence can name it even
    // if the schedule is edited afterwards.
    logAndNotify(EventType::DoseMissed, missed[i]->lid,
                 (int16_t)minuteOfDay(missed[i]->scheduledLocal));
  }
  if (missedCount != 0) {
    if (g.settings.power.ledOnReminder) g.led.playPattern(Pattern::PulseRed, 3000);
    // Nothing else in this path postpones sleep, and maybeSleep() calls
    // g.led.stop() - so without this the box can announce a missed dose and
    // deep-sleep in the same pass, throwing the pulse away unserviced.
    g.keepAwakeFor(kReminderAwakeSec);
  }

  // 2. A dose falling in the current minute. The guard is the last fired
  // instance, because a 10 s tick visits the same minute six times.
  DoseInstance inst[4];
  const uint32_t minuteNow = (now / kSecPerMin) * kSecPerMin;
  size_t n = doseInstancesInRange(sch, minuteNow, minuteNow + kSecPerMin, inst, 4);
  // Guard on the minute rather than the instance. A lid may carry two dose
  // slots at the same time, and every instance inside this minute shares one
  // localEpoch, so a per-instance guard announced the first and then silently
  // swallowed the rest - while sweepMissed, which keys on lid and dose as well,
  // would still go on to report every one of them as missed. One chime and one
  // pulse for the minute, an event for each dose in it.
  if (n != 0 && minuteNow != firedDueLocal) {
    firedDueLocal = minuteNow;
    for (size_t i = 0; i < n; ++i) {
      logAndNotify(EventType::DoseDue, inst[i].lid, (int16_t)minuteOfDay(inst[i].localEpoch));
    }
    if (g.settings.power.buzzerOnReminder) g.led.beep(120, 2);
    if (g.settings.power.ledOnReminder) g.led.playPattern(Pattern::PulseAmber, 5000);
    g.keepAwakeFor(kReminderAwakeSec);  // see the missed branch above
  }

  // 3. The nudge ahead of a dose.
  if (sch.remindBeforeMin != 0) {
    const uint32_t ahead = minuteNow + (uint32_t)sch.remindBeforeMin * kSecPerMin;
    n = doseInstancesInRange(sch, ahead, ahead + kSecPerMin, inst, 4);
    if (n != 0 && ahead != firedReminderLocal) {   // same collision as the due branch
      firedReminderLocal = ahead;
      for (size_t i = 0; i < n; ++i) {
        logAndNotify(EventType::DoseReminder, inst[i].lid, (int16_t)sch.remindBeforeMin);
      }
    }
  }

  // 4. One summary a day, at the configured minute or the first tick after it.
  if (g.settings.notify.onDailySummary) {
    const uint32_t today = dayIndex(now);
    if (today != firedSummaryDay &&
        minuteOfDay(now) >= g.settings.notify.dailySummaryMinute) {
      firedSummaryDay = today;
      notifyDailySummary();
    }
  }
}

static void batteryWatchdog(uint32_t nowMs) {
  const PowerReading& p = g.analog.reading();
  if (!p.valid) return;

  const bool charger = p.usbPresent || p.coilPresent;
  const bool vbatReal = p.vbatMv >= kVbatSanityFloorMv;

  if (lastChargerPresent < 0) {
    lastChargerPresent = charger ? 1 : 0;
  } else if ((lastChargerPresent != 0) != charger) {
    lastChargerPresent = charger ? 1 : 0;
    g.logEvent(charger ? EventType::ChargerIn : EventType::ChargerOut, kNoLid,
               (int16_t)(charger ? (p.usbPresent ? p.usbMv : p.coilMv) : 0));
    g.keepAwakeFor(g.settings.power.awakeAfterEventSec);
  }

  // The button is sampled, not interrupt-driven, and D5 leaks into BUTTON_SIG,
  // so a press has to survive two samples.
  if (p.buttonPressed) {
    if (buttonSamples < 250) ++buttonSamples;
    if (buttonSamples == 2 && !buttonReported) {
      buttonReported = true;
      g.logEvent(EventType::ButtonPress);
      g.keepAwakeFor(g.settings.power.awakeAfterEventSec);
    } else if (buttonSamples == 5) {
      g.logEvent(EventType::ButtonHold);
    }
  } else {
    buttonSamples  = 0;
    buttonReported = false;
  }

  // A charger holds the battery node up, so a critical reading while one is
  // plugged in is the charger talking, not the cell.
  if (vbatReal && !charger && p.vbatMv < g.settings.power.criticalBatteryMv) {
    if (++criticalSamples >= kCriticalSamples) {
      Serial.printf("[power] %u mV is critical, shutting down\n", (unsigned)p.vbatMv);
      logAndNotify(EventType::BatteryCritical, kNoLid, (int16_t)p.vbatMv);

      // Try to get that push out before the lights go off. It is queued, so give
      // notifyService a few passes rather than assuming one will do.
      for (uint8_t i = 0; i < 6 && !notifyQueueEmpty(); ++i) {
        notifyService(millis());
        delay(200);
      }
      g.led.stop();
      flushPersistent();
      storeSaveLastEpoch(g.nowUtc());
      Serial.flush();
      PowerLatch::powerOff();
      return;
    }
  } else {
    criticalSamples = 0;
  }

  if (vbatReal && !charger && p.vbatMv < g.settings.power.lowBatteryMv) {
    const uint32_t nowUtc = g.nowUtc();
    const bool due = nowUtc == 0
                         ? (lastLowBatteryUtc == 0)  // no clock: once per power cycle
                         : (lastLowBatteryUtc == 0 ||
                            (uint32_t)(nowUtc - lastLowBatteryUtc) >= kLowBatteryRepeatSec);
    if (due) {
      lastLowBatteryUtc = nowUtc != 0 ? nowUtc : 1;  // 1 just marks "already said it"
      logAndNotify(EventType::BatteryLow, kNoLid, (int16_t)p.vbatMv);
    }
  }
  (void)nowMs;
}

static void serviceRequests() {
  if (g.factoryResetRequested) {
    g.factoryResetRequested = false;
    Serial.println("[app] factory reset");
    storeFactoryReset();
    applyDefaults(g.settings);
    storeSaveSettings(g.settings);
    delay(100);
    ESP.restart();
    return;
  }

  if (g.rebootRequested) {
    g.rebootRequested = false;
    g.logEvent(EventType::ConfigChanged);
    flushPersistent();
    storeSaveLastEpoch(g.nowUtc());
    Serial.println("[app] rebooting");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }

  if (g.powerOffRequested) {
    g.powerOffRequested = false;
    g.led.stop();
    flushPersistent();
    storeSaveLastEpoch(g.nowUtc());
    Serial.println("[app] dropping the latch");
    Serial.flush();
    PowerLatch::powerOff();
    return;
  }

  if (g.forceTimeSync) {
    g.forceTimeSync = false;
    timeRequestSync();
  }
}

// Seconds until the next thing that actually matters, capped by the configured
// wake interval.
static uint32_t secondsUntilNextDuty() {
  uint32_t secs = g.settings.power.wakeIntervalSec != 0 ? g.settings.power.wakeIntervalSec : 900;

  if (g.clockUsable()) {
    const Schedule& sch = g.settings.schedule;
    const uint32_t  now = g.nowLocal();

    const DoseInstance next = nextDose(sch, now);
    if (next.valid) {
      const uint32_t lead = (uint32_t)sch.remindBeforeMin * kSecPerMin;
      // If the reminder moment has already gone past, aim at the dose itself -
      // otherwise we would sleep a full wake interval through it.
      uint32_t target = next.localEpoch > now + lead ? next.localEpoch - lead
                                                     : next.localEpoch;
      if (target > now && (target - now) < secs) secs = target - now;
    }

    // The deadline for declaring the last dose missed.
    const DoseInstance prev = previousDose(sch, now);
    if (prev.valid) {
      const uint32_t deadline =
          prev.localEpoch +
          (uint32_t)(sch.windowAfterMin + sch.missedGraceMin) * kSecPerMin;
      if (deadline > now && (deadline - now) < secs) secs = deadline - now;
    }
  }

  // Only a scheduled resync of a clock we have actually verified is worth
  // waking early for. While verified is false, nextSyncDueMs is the retry
  // deadline timeRequestSync() set, and honouring that would pin a box with an
  // unreachable NTP server to kMinSleepSec for the life of the battery.
  const uint32_t nowMs = millis();
  if (g.time.verified && (int32_t)(g.time.nextSyncDueMs - nowMs) > 0) {
    const uint32_t untilSync = (g.time.nextSyncDueMs - nowMs) / 1000;
    if (untilSync < secs) secs = untilSync;
  }

  return secs < kMinSleepSec ? kMinSleepSec : secs;
}

static void maybeSleep(uint32_t nowMs) {
  const PowerSettings& ps = g.settings.power;
  if (ps.policy == SleepPolicy::AlwaysOn) return;

  const PowerReading& p = g.analog.reading();
  const bool usbPresent = p.valid && (p.usbPresent || p.coilPresent);
  if (usbPresent && ps.policy != SleepPolicy::Aggressive) return;

  if ((int32_t)(nowMs - g.stayAwakeUntilMs) < 0) return;
  // The indicator still wants bus time: a reminder pulse or a queued beep is
  // mid-flight, and the g.led.stop() below would throw it away unserviced.
  // Every pattern this firmware starts is bounded, so this cannot pin us awake.
  if (g.led.busy()) return;
  if (webClientActive(nowMs)) return;
  if (!notifyQueueEmpty()) return;

  const uint32_t syncGraceMs =
      wokeFromDeepSleep() ? kWakeSyncGraceMs : kFirstSyncGraceMs;
  if (!g.time.verified && nowMs < syncGraceMs) return;

  const uint32_t requested = secondsUntilNextDuty();
  const uint32_t seconds   = timeCorrectSleepSeconds(requested);

  // WAKE_UP is the hardware OR of every lid, the button and the charger, and it
  // is level triggered: arming it while it is already high wakes us instantly
  // and forever. enterDeepSleep() checks the line too, but the reason belongs in
  // the log, so decide it here.
  const bool lidOpen = g.halls.state().openMask != 0;
  const bool lineHigh = wakeUpLineHigh();
  const bool armWakePin = !lidOpen && !usbPresent && !lineHigh;
  if (!armWakePin) {
    Serial.printf("[sleep] timer only (%s%s%s)\n", lidOpen ? "a lid is open " : "",
                  usbPresent ? "charger present " : "", lineHigh ? "WAKE_UP already high" : "");
  }

  Serial.printf("[sleep] %lu s (asked for %lu, drift %ld ppm), wake pin %s\n",
                (unsigned long)seconds, (unsigned long)requested,
                (long)g.settings.time.driftPpm, armWakePin ? "armed" : "off");

  g.logEvent(EventType::SleepEnter, kNoLid,
             (int16_t)(seconds > 32767 ? 32767 : seconds));

  g.led.stop();
  g.bus.park();
  netShutdown();
  flushPersistent();          // includes the SleepEnter event just logged
  storeSaveLastEpoch(g.nowUtc());
  Serial.flush();

  // Last mux access of this power cycle. The pad hold freezes MUX_A/B/C at
  // whatever they are driving, and channel 7 is all three high into R13/R14/R15
  // - about 99 uA for the whole sleep, more than the chip. parkForSleep()
  // leaves the address on channel 0, which costs nothing.
  g.bus.parkForSleep();

  enterDeepSleep(seconds, armWakePin);  // does not return
}

}  // namespace ep

using ep::g;

void setup() {
  // First statement, before Serial, before anything: the user is still holding
  // the button and lets go the moment the box looks alive.
  ep::PowerLatch::assertEarly();

  Serial.begin(115200);
  const uint32_t serialStart = millis();
  while (!Serial && (uint32_t)(millis() - serialStart) < 1500) delay(10);

  g.wakeReason = ep::lastWakeReason();
  Serial.printf("\n[boot] EasyPill %s, wake: %s\n", ep::kFirmwareVersion,
                ep::toString(g.wakeReason));
  g.bootFloorBegin();

  g.bus.begin();

  ep::storeBegin();
  ep::storeLoadSettings(g.settings);
  if (ep::validate(g.settings)) {
    Serial.println("[boot] settings needed clamping, will re-save");
    g.markSettingsDirty();
  }

  g.halls.begin(&g.bus, &g.settings.hw);
  g.analog.begin(&g.bus, &g.settings.hw);
  g.led.begin(&g.bus, &g.settings.hw);

  ep::storeLoadEvents(g.events);
  ep::storeLoadAdherence(g.adherence);

  ep::timeBegin();
  g.logEvent(ep::EventType::Boot, ep::kNoLid, (int16_t)(uint8_t)g.wakeReason);

  ep::netBegin();
  ep::webBegin();
  ep::notifyBegin();
  if (const ep::Event* boot = g.events.findLatest(ep::EventType::Boot)) ep::notifyEvent(*boot);

  // A level that was already asserted while we slept is not a new press, so the
  // RTC latch is re-armed only when this wake could be the press itself:
  // BUTTON_SIG is one of the nine inputs to WAKE_UP. After a timer wake the
  // latch stays set and batteryWatchdog() keeps quiet about a button that has
  // never been seen low. buttonSamples has to go with it, because the report is
  // an exact `== 2`.
  if (g.wakeReason != ep::WakeReason::Timer) {
    ep::buttonSamples  = 0;
    ep::buttonReported = false;
  }

  // Seed both sensor paths before loop() starts reacting to them.
  g.analog.sampleAll(millis());
  ep::batteryWatchdog(millis());
  ep::seedSensors();

  g.keepAwakeFor(ep::kBootGraceSec);
  Serial.printf("[boot] ready, %u events, %u dose records, %u bytes heap\n",
                (unsigned)g.events.size(), (unsigned)g.adherence.size(),
                (unsigned)ESP.getFreeHeap());
}

void loop() {
  const uint32_t nowMs = millis();

  // The LED owns the address lines while it is announcing something, so it goes
  // first. While it says it still holds them, nothing else may touch the bus -
  // not a hall scan, not an ADC sample, not even the yieldBus() that hands them
  // back - because any of those moves the address out from under the colour slot
  // or the beep on-phase being driven, and the symptom is a silent buzzer or an
  // LED at a few percent of its duty with nothing in the system to report it.
  const bool ledBusy = g.led.busy();
  if (ledBusy) g.led.service(nowMs);

  // Read once and keep it for the pass: the hold ends on a micros() deadline, so
  // it can go false between two calls that are three statements apart.
  const bool ledHoldsBus = g.led.holdsBus();
  if (!ledHoldsBus) {
    if (ledBusy) g.led.yieldBus();

    if (ledBusy || (uint32_t)(nowMs - ep::lastHallScanMs) >= ep::kHallIdleIntervalMs) {
      g.halls.scan(nowMs);
      ep::lastHallScanMs = nowMs;
      ep::handleLidChanges();

      // Channel 7 is a 100k to GND: if it does not read low, the address lines
      // are not working and every hall reading this pass was fiction.
      if (!g.halls.state().gndRefOk &&
          (uint32_t)(nowMs - ep::lastSensorFaultMs) >= ep::kSensorFaultLogMs) {
        ep::lastSensorFaultMs = nowMs;
        g.logEvent(ep::EventType::SensorFault);
      }
    }

    if ((uint32_t)(nowMs - ep::lastAnalogMs) >= ep::kAnalogIntervalMs) {
      ep::lastAnalogMs = nowMs;
      g.analog.sampleAll(nowMs);
      ep::batteryWatchdog(nowMs);
    }
  }

  ep::netService(nowMs);
  ep::webService(nowMs);
  ep::timeService(nowMs);
  ep::notifyService(nowMs);
  ep::storeService(nowMs);

  if ((uint32_t)(nowMs - ep::lastSchedulerMs) >= ep::kSchedulerIntervalMs) {
    ep::lastSchedulerMs = nowMs;
    ep::schedulerTick();
  }

  ep::serviceRequests();
  ep::maybeSleep(nowMs);

  // Feed the idle task so the task watchdog stays quiet; skipped while the LED
  // wants the next slot.
  if (!g.led.busy()) delay(1);
}
