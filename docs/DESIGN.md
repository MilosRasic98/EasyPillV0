# Why the firmware is shaped like this

Notes to a future self. [HARDWARE.md](HARDWARE.md) says what the board is;
[API.md](API.md) says what the HTTP contract is. This file says why the code in
between looks the way it does, and what was deliberately left out.

---

## The shared mux, and who is allowed to touch it

`MUX_A`, `MUX_B` and `MUX_C` address **three** XS3A4051 multiplexers
simultaneously:

| Mux | Role | Common pin | Direction |
|---|---|---|---|
| U20 | 7 hall sensors + a ground reference | `HALL_IN` → D3 | digital in |
| U4 | VBAT / USB / coil / button / charger status | `SIG_IN` → D2 | analog in |
| U5 | three LED FET gates + the buzzer FET gate | `SIG_OUT` ← D7 | digital out |

One address therefore selects a channel on all three at once. There is no
arrangement of these pins in which the box can hold the LED on and read a hall
sensor. Every sensor read, every ADC sample and every output drive time-shares
one three-bit bus.

Everything about the firmware's structure follows from that.

### One owner

`MuxBus` is the only object in the firmware allowed to write `MUX_A/B/C` or
`SIG_OUT`. `HallScanner`, `AnalogSense` and `Indicator` all hold a `MuxBus*` and
go through it. There is no second path, not even for bring-up.

`MuxBus::select()` enforces two rules on every call:

1. **`SIG_OUT` goes low before the address changes.** Otherwise the address walks
   a logic high across Q1–Q4 and the LED flashes or the buzzer chirps on every
   hall scan. This is the single most visible symptom of getting the mux wrong,
   and it is why a sweep that lights the LED is a firmware bug rather than a board
   fault. With LEDC holding the pin for a tone, dropping the duty is not enough:
   `ledcWrite()` only *queues* a duty and the hardware loads it at the next cycle
   boundary, so `select()` waits out one PWM period before it moves the address.
2. **Channel 7 is the parking spot.** On the hall mux it is R38, a 100 k to GND;
   on the output mux it is unconnected. Nothing can be energised while parked, and
   the hall mux's channel 7 doubles as a liveness test: it must read low, so
   `groundReferenceOk()` is cheap proof that U20 is powered and addressable. If it
   fails, a `SensorFault` event is logged and no hall reading means anything. What
   a pass does *not* prove is that the three address lines move independently —
   channel 7 is `0b111`, so a line stuck high still produces address 7 and still
   passes. Only a channel-to-channel comparison can show that, which is the
   bring-up tool's job.

Deep sleep is the one exception to rule 2, and `parkForSleep()` is the only
place it applies: see the sleep section below.

`kSettleUs` is 25 µs, which is generous — the XS3A4051 switches in tens of
nanoseconds and the hall path has no capacitance. A full seven-channel scan plus
the ground reference still costs about 250 µs, so scanning every few milliseconds
out of the main loop is free.

### Cooperative arbitration, not locking

There is no mutex, because there is no second thread (see below). Instead the
indicator says out loud when it needs the address lines, and the loop believes
it:

- `Indicator::holdsBus()` is the whole handshake. While it is true the main loop
  must not scan halls, must not sample the ADC, and must not even call
  `yieldBus()` — all three move the address out from under whatever is being
  driven, and the symptom is a silent buzzer or an LED at a few percent of its
  duty with nothing in the system to report it.
- A hold lasts one colour slot (`kSlotUs`, 2000 µs) or one beep on-phase, and
  never longer than `kMaxHoldUs`, 250 ms. The deadline is on `micros()`, not on
  being serviced, so `holdsBus()` goes false by itself and an indicator that
  stops being serviced cannot lock the bus away.
- `holdsBus()` can flip between two calls three statements apart, so `loop()`
  reads it **once** per pass and keeps the answer.
- The pass on which the hold lapses is the loop's window: it scans, and
  `service()` takes the bus back on the next one. A multiplexed slot ends dark so
  one colour cannot burn on through the scan and skew the mix; a single colour is
  left driving, because there is nothing to skew and whoever takes the bus next
  drops `SIG_OUT` on the way in.
- `service()` restates the address and the drive level on every pass while it
  holds. An address nothing in the firmware moved — a glitch, a reset
  peripheral — is corrected within one loop pass. That only works because
  `MuxBus` has exactly one owner; a second owner moving the address legitimately
  mid-hold would be fought and lost to.
- A single non-zero colour channel is driven statically and costs nothing beyond
  the initial `select()`. Only mixed colours need time-multiplexing, three 2 ms
  slots to a cycle, so roughly 160 Hz — fast enough that a camera at 1/50 s
  integrates three full cycles and shows no rolling-shutter banding, which
  matters because this box gets filmed.
- `busy()` stays true for one pass after a pattern ends, until the bus has been
  handed back. That pass is what turns the LED off, so it is not an
  inefficiency to optimise away.

The price is stated rather than hidden: while a hold is in force nothing is
watching the lids. The worst case is 250 ms plus the 60 ms hall debounce instead
of ~4 ms plus 60 ms, and it is only reachable from a long test chirp —
`POST /api/action {"action":"beep"}` allows up to 5000 ms, which is sliced into
250 ms holds with a scan window between them. Every beep the firmware itself
raises (120 ms twice when a dose falls due, 60 ms once when an open is matched to
one) is shorter than a single hold and is never cut. Nothing is lost by the delay
either: `WAKE_UP` bypasses the mux entirely, the lid is still open when the scan
resumes, and 310 ms against a ±15-minute dose window is nothing.

It cuts the other way too. A push leaving during an announcement can hold the
loop for seconds; the indicator's hold lapses meanwhile and the colour goes dark
until the next `service()`. That is the bounded hold working as designed, and
nothing in the loop can prevent it.

The LED is off in normal operation anyway. The box is supposed to look like a
dumb plastic pillbox to the person using it, so the indicator only takes the bus
for short announcements.

### `WAKE_UP` as the escape hatch

The one thing that does not go through the mux is `WAKE_UP` (D1), a hardware OR
of all seven halls, the button and charger presence. It means the firmware can
know that *something* happened even while the mux is busy driving a colour, then
scan to find out *what* once the burst ends. Without it, an LED announcement would
be a window of blindness.

### Single-threaded on purpose

Everything runs out of `loop()`: the hall scan, the ADC pass, the indicator slot,
the Wi-Fi state machine, the notifier queue, the store's dirty-flush, and the web
handlers. `ep::g` is a plain global that any of them can touch, because only one
of them is ever running. Requests from the web layer are flags on `AppState`
(`rebootRequested`, `powerOffRequested`, `factoryResetRequested`,
`forceTimeSync`) that the loop picks up on its next pass.

Four calls are allowed to take their time, and they are the only four:
`netScan()`, `notifyTest()` and `timeSyncBlocking()`, each reachable only from a
GUI action, and `notifyService()`, which sends at most one queued push per pass
and is bounded by that one transport's connect plus read timeout — 5 s worst
case. Everything else is expected back in milliseconds.

The cost is real and worth stating: **while a web handler runs, nothing is
scanning the hall sensors.** Handlers have to stay short, `GET /api/state` in
particular, since it is on a 1 Hz poll. The benefit is that there are no locks,
no queues, no task priorities and no "which core did this run on" debugging —
which on an ESP32 with a watchdog and a shared mux is a large benefit.

---

## Why the GUI polls

`GET /api/state` on a 1000 ms interval while the tab is visible, 5000 ms when it
is hidden. No WebSockets, no server-sent events. Four reasons, in order of weight:

1. **The box is asleep most of the time.** On battery it deep-sleeps between
   events. A long-lived socket would be torn down on every sleep and would have to
   be re-established on every wake, so the client needs reconnect-and-poll logic
   regardless. Having built that, the socket adds nothing.
2. **Dependencies.** SSE and WebSockets on Arduino-ESP32 2.0.17 mean
   `ESPAsyncWebServer` and `AsyncTCP`, which bring their own watchdog and
   lifetime problems. ArduinoJson is the only external dependency in this entire
   project and that is worth protecting.
3. **Async would break the single-thread model.** `AsyncTCP` runs handlers on its
   own task. Every handler that touches `ep::g` — which is all of them — would
   then need locking against the mux owner. That is a lot of new failure modes to
   buy a one-second latency improvement.
4. **One second is imperceptible for this.** The event being watched is a lid
   being lifted by an eighty-year-old. A second of latency on a web page nobody is
   staring at is not the problem; the box being asleep is the problem, and that is
   solved by [push notifications](NOTIFICATIONS.md), not by a faster transport.

The GUI polling also doubles as the stay-awake signal. `webClientActive()` is true
when a browser has polled recently, which the sleep logic uses to keep the box
reachable exactly while somebody is looking at it, with no separate heartbeat.

The flip side: `/api/state` has to stay cheap, because it is the only thing
between a lid opening and the screen updating, and it is the main loop's largest
recurring cost.

---

## Why there is a simulator and host unit tests

The hardware did not exist when this firmware was written. The schematic existed,
the review existed, the PCB was not yet in hand. Writing a scheduler and an
adherence model with no way to run them is how you end up with a box that
compiles and does the wrong thing.

So the code is split by what it needs to run:

| Library | Needs | Tested |
|---|---|---|
| `easypill_core` | nothing. Pure C++17, no `Arduino.h`, no heap, no clock of its own | host unit tests in `tests/` |
| `easypill_json` | ArduinoJson only | host, alongside the core |
| `easypill_hw` | a board | bring-up firmware, by hand |
| `src/app` | a board and a network | the simulator, for the HTTP layer |

`easypill_core` takes time in as a `uint32_t` **local epoch** and nothing else.
No timezone, no `time_t`, no `struct tm`, no `millis()`. That one decision is what
makes `matchOpen`, `nextDose`, `sweepMissed` and `adherencePercent` testable on a
laptop: feed in a schedule and a number, assert on the answer. Daylight saving,
NTP and the RC oscillator are all somebody else's problem, specifically
`timekeeper.cpp`'s.

The simulator (`sim/easypill_sim.py`) implements the whole of
[API.md](API.md) plus `POST /api/sim`, which fakes lid opens, button presses,
battery voltage, charger presence and the passage of time at up to 3600× real
speed. It serves the real GUI out of `web/`. That means the GUI, the schedule
editor, the adherence grid and the notification plumbing can all be exercised
end to end with no board on the desk, and a fortnight of dosing can be replayed
in a few seconds.

The cost of this arrangement is that **one contract now has three
implementations**: the firmware, the simulator and the GUI. [API.md](API.md) says
so at the top. Change one, change all three, or the simulator quietly stops
telling the truth — which is worse than not having it.

What is *not* covered: `easypill_hw` has no tests, because every line of it is
about a pin. That gap is what the bring-up firmware and
[BRINGUP.md](BRINGUP.md) are for.

---

## Timekeeping without an RTC

There is no RTC chip, no coin cell and no battery-backed clock on this board.
Everything about the date and time is software, and it matters more than it
sounds like it should.

### What breaks when the clock is wrong

Ranked, because the ranking is not intuitive:

1. **The date decides which lid is "today".** In weekday mode `lidForDay()` maps
   the day index to a lid, so a date that is one day out makes every correct
   lid-open look like `wrongDayOpen` and the actually-active lid look like it was
   never opened. A wrong *date* corrupts the adherence record; a wrong *time of
   day* only shifts a window.
2. Dose window matching (`windowBeforeMin` / `windowAfterMin`, 60 and 120 minutes
   by default) — generous, so minutes of error are harmless and an hour is not.
3. The missed sweep, where `missedGraceMin` of 15 is a hard edge.
4. Timestamps in notifications and in the event log, which are cosmetic until
   somebody tries to reconstruct what happened.

This is why the sync fetches a full epoch rather than a time of day. SNTP returns
seconds since 1970; the date, the weekday and the day index all fall out of that
one number through `dayIndex()` and `weekdayMon0()`. There is nothing to set by
hand and no separate date source to get out of step.

### What survives what

| Event | System time survives? |
|---|---|
| Deep sleep | **Yes.** The RTC timer keeps running, so the clock only has to be set once per power cycle. |
| `esp_restart()` / reboot from the GUI | No |
| Panic, WDT reset | No |
| Latch drop, battery removed, `powerOff` | No |

### The sequence

1. **Boot.** `timeBegin()` applies the POSIX TZ string from settings, then decides
   whether to step the clock. A deep-sleep wake comes back with the RTC timer
   still running, so what the chip already holds is strictly better than
   `storeLoadLastEpoch()` — which is the epoch at sleep *entry*, and stepping back
   to it would rewind the clock by the whole sleep, on every wake, forever. So the
   seed is only applied when the chip holds no plausible time at all (a real
   power-on, or a reset that lost the RTC domain) or when the seed is somehow
   ahead of it. The test is on the clock rather than on the wake reason, because
   `lastWakeReason()` reports a panic or a WDT reset as a power-on and those keep
   the RTC domain. Either way `g.time.verified` is **false**: the clock is
   approximately right and the firmware knows it is not to be trusted.
2. **Wi-Fi comes up.** `netService()` reaching the `Station` state fires
   `timeRequestSync()`. **Every** successful association triggers a sync attempt,
   not only the first one, because an association is the cheapest possible signal
   that the network is there and a sync costs one UDP round trip. On the first
   sync of a power cycle `timeSyncBlocking()` is used at boot so the box does not
   start matching doses against a guessed clock.
3. **Sync lands.** `g.time.verified` becomes true, `lastSyncUtc` and `syncCount`
   are updated, and `EventLog::backfillTimes()` gives every event logged before
   this moment a real timestamp by working backwards from its `uptimeMs`. This is
   also the first instant the box can say what the local time is, so it is where
   the boot floor is named (below).
4. **Periodically.** `timeService()` fires a resync when `nextSyncDueMs` comes
   round — `resyncIntervalSec`, 21600 s, i.e. every 6 hours — and persists the
   epoch to NVS every few minutes so the next cold boot starts close.
5. **On demand.** `POST /api/action {"action":"syncTime"}` from the GUI.

Two servers are configured (`ntp1` = `pool.ntp.org`, `ntp2` = `time.google.com`)
because a box that cannot resolve one of them should not lose its clock.

### Local epoch, and where the timezone lives

The core works entirely in **local epoch**: Unix seconds already shifted by the
current UTC offset. `timekeeper.cpp` owns the conversion, because it is the only
part with tzdata and a POSIX TZ string; `timeTzOffsetSec()` exposes the offset so
the API can report `tzOffsetSec` and the GUI can recover UTC if it wants to.

The API talks local epoch for the same reason: the GUI can render a timestamp
without knowing anything about where the box is.

The leak is daylight saving. A dose can land an hour out for a single day across
a transition, because the offset used to build the local epoch changes underneath
the schedule. For pill reminders inside a ±60/120-minute window that is
acceptable, and it self-corrects the next day. Fixing it properly would mean
carrying wall-clock fields rather than an epoch through the core, which would cost
the host-testability that the whole arrangement exists to buy.

### The drift-measurement trick

`RTC_SLOW_CLK` defaults to the internal RC oscillator. Its raw, untrimmed spread
is ±3–5 %. What saves this is that the IDF calibrates it against the 40 MHz
crystal at every boot, so what is left is a calibration residue plus the
oscillator's temperature coefficient — minutes per day rather than tens of
minutes. Still too much for a schedule with a 15-minute grace window if the box
goes days without a sync.

So the firmware measures it:

- On the **second and later** sync, compare the RTC seconds elapsed since the last
  sync against the NTP seconds elapsed over the same interval. The reference sits
  in RTC memory, so it carries across a deep sleep: the first sync after a wake
  measures a sleep-long baseline, which is the case worth measuring.
- `ppm = (rtcElapsed − ntpElapsed) × 1e6 / ntpElapsed`, signed. Intervals under
  600 s are ignored rather than averaged in, and a result past ±100 000 ppm is
  rejected as a bad measurement rather than applied to every sleep.
- Store it in `settings.time.driftPpm` (so it survives a reboot) and report it as
  `g.time.measuredDriftPpm`, averaged with the stored value when there is one so
  that one sync's whole-second rounding cannot throw the estimate around.
- Apply it in two places: `timeCorrectSleepSeconds()` scales a requested sleep so
  the chip actually sleeps the wall-clock duration asked for, and the clock is
  corrected between syncs so the error does not accumulate linearly.

The default in the API example is `-1840` ppm, which is 0.184 %, which is 159
seconds — about two and a half minutes — of drift per day. Correcting a single
900-second timer wake moves it by 1.7 s, which sounds pointless; it is not,
because there are 96 of those a day and that is exactly where the two and a half
minutes comes from.

With two syncs a day and the correction applied, drift stays inside a few
seconds.

### Limits of the trick, which are not small

- **The estimate is only as good as its baseline.** Over a ten-minute interval, a
  ±1 s uncertainty in the SNTP result is ±1700 ppm of measurement noise, which is
  larger than the thing being measured. Only baselines of hours give a usable
  number, so short intervals have to be ignored rather than averaged in.
- **It is a point estimate at one temperature.** The RC oscillator's rate depends
  on die temperature. A ppm learned on USB on a warm bench does not hold in a cold
  hallway, and it does not hold while the Wi-Fi front end is heating the module.
- **It does not survive a reboot cleanly.** The IDF recalibrates `RTC_SLOW_CLK` at
  every boot, so a stored `driftPpm` is approximately but not exactly right after
  a restart. It is still better than zero.
- **It corrects rate, not offset.** Between syncs the clock still walks away; the
  correction slows the walk. Nothing here replaces a sync.
- **It cannot help the first interval of a power cycle**, and it cannot help at
  all on a box that never sees Wi-Fi.

### Never inventing a time

Until SNTP has answered at least once this power cycle, events are recorded with
`localEpoch = 0` plus their `uptimeMs`, and both the event and the global state
carry a `timeVerified` flag through [API.md](API.md). The GUI renders unverified
events relative to uptime. A notification sent before the first sync names no
time at all — "A Monday dose was never taken" rather than a plausible-looking
wall-clock time that is wrong — and its `localEpoch` goes out as 0.

`backfillTimes()` reads the interval as **signed**. A `millis()` rollover at 49
days still comes out as a small positive number, but an event stamped by a
previous power cycle reads negative, because `millis()` restarts at every
power-on and the two uptimes are not on the same clock. Such an event keeps
`localEpoch` 0 for good; the alternative is an event confidently dated 49.7 days
in the past.

This is a deliberate refusal. A pillbox that confidently reports the wrong time
for a dose is worse than one that says it does not know.

The one concession is `storeLoadLastEpoch()`: a cold boot without Wi-Fi starts
from the last persisted epoch, which keeps the event log in order and usually
picks the right lid, while still being flagged unverified until a sync confirms
it.

### The crystal that is right there and cannot be used

The XIAO ESP32-C3 has a 32.768 kHz crystal on GPIO0/1, which would cut drift to a
few ppm and make all of the above unnecessary. Selecting it needs
`CONFIG_RTC_CLK_SRC_EXT_CRYS`, which means a custom `sdkconfig`, which means an
ESP-IDF build rather than an Arduino one. The S3 does not appear to have one at
all. Noted as the single highest-value change if drift ever turns out to matter
in practice.

---

## Sleep, wake, and the level-triggered trap

### Policy

`power.policy` has three values:

| Policy | Behaviour | For |
|---|---|---|
| `alwaysOn` | never deep-sleeps | the bench, filming, debugging |
| `smart` | stays awake while USB is present, sleeps on battery between events | the default, and the right answer |
| `aggressive` | sleeps as soon as there is nothing to do, USB or not | squeezing standby current |

Sleep is postponed while any of these hold, in the order `maybeSleep()` tests
them:

- the policy is `alwaysOn`;
- a charger is present and the policy is not `aggressive`;
- `stayAwakeUntilMs` is in the future — `keepAwakeFor()`, which the `stayAwake`
  action and every logged event both call, `awakeAfterEventSec` being 300 s. A
  raised reminder uses 20 s instead: a dose happens on a schedule, not because
  somebody touched the box, and five minutes of radio per dose is not worth
  paying. Raising the setup AP takes the same route, with a 300 s provisioning
  window refreshed while a phone is actually associated — an SSID the box sleeps
  away cannot be joined;
- the indicator still wants bus time (`g.led.busy()`), because the `g.led.stop()`
  below would throw a mid-flight pulse away unserviced;
- a browser has polled recently (`webClientActive()`);
- the notify queue is not empty;
- the clock is still unverified and the grace window has not run out:
  `kFirstSyncGraceMs`, 60 s, after a power-on, and `kWakeSyncGraceMs`, 15 s,
  after a deep-sleep wake. Keyed on the wake reason, because `millis()` restarts
  on every wake — keyed on `millis()` alone, a box that cannot reach NTP would
  spend a full minute awake out of every seventy for the life of the cell.

There is deliberately no `netBusy()` helper rolling those conditions into one
call. It existed once, had no callers, and duplicated three of the gates above;
two copies of a decision this subtle drift apart, and the version with the
reasons attached is the one worth keeping. Unwritten settings are not a gate
either — the way down logs `sleepEnter`, stops the indicator,
parks the bus, shuts the radio, flushes everything dirty to LittleFS and
persists the epoch, so a dirty tree is *written* rather than waited for.

Otherwise `enterDeepSleep(seconds, armWakePin)`, with `seconds` =
`timeCorrectSleepSeconds(secondsUntilNextDuty())`. The duty is the earliest of
the next dose (less `remindBeforeMin`), the deadline at which the last dose has
to be declared missed, and a resync of an already-verified clock — capped by
`wakeIntervalSec`, 900 s by default. Aiming at the deadline rather than at a
fixed interval is what makes a sweep-based missed-dose model work on a box that
sleeps.

`parkForSleep()` is the last mux access of the power cycle, and the single place
channel 7 is the wrong answer. The pad hold that keeps `POWER_LATCH` up through
the sleep is one global bit: it freezes *every* digital pad at the level it is
driving. Parked on channel 7, `MUX_A/B/C` are three highs into the 100 k
pull-downs R13/R14/R15 for the whole sleep — 3 × 33 µA, about 99 µA, more than
the chip itself draws asleep. Channel 0 holds all three low and costs nothing,
and it is also what the pull-downs give at reset, so the wake path finds the
address where it expects it.

### The trap

`WAKE_UP` is `Hall1|…|Hall7|BUTTON_SIG|SIG_CHARGE`, ORed in hardware by two
74LV32A packages. It is **level** triggered, and that has two consequences that
are easy to get wrong and expensive to get wrong:

- **Any single held-high source masks every other event.** One lid left open, one
  missing magnet, or a plugged-in charger pins the line high and no further lid
  can be distinguished from it.
- **Arming a level-triggered GPIO wake while the line is already high wakes the
  chip instantly, and then again instantly, forever.** On battery that is a flat
  cell by morning.

So `enterDeepSleep()` reads `wakeUpLineHigh()` first and silently falls back to a
timer-only sleep when the line is high. `armWakePin = true` is a request, not a
command.

What that costs, stated plainly: with a lid left open the box is on a 15-minute
timer instead of waking instantly, so event latency goes from ~0 to up to
`wakeIntervalSec`. It still works; it is slower. And because USB counts as a
charger, a USB-powered box is *always* in the timer-only case — which is fine,
since on USB it has no reason to sleep at all.

Wake sources differ by chip, which is why they are behind
`enterDeepSleep()`:

- C3: `esp_deep_sleep_enable_gpio_wakeup(BIT(3), ESP_GPIO_WAKEUP_GPIO_HIGH)`
- S3: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1)`
- both: `esp_sleep_enable_timer_wakeup()`

### The latch has to outlive everything

`PowerLatch::assertEarly()` is the **first statement in `setup()`**, before
`Serial`, before `MuxBus::begin()`, before anything. The user holds the button
only until firmware takes over, and power-off happens on release, so every
microsecond before that assertion is a chance for the box to drop dead mid-boot.

`holdThroughSleep()` then arms the pad holds so a deep sleep, a panic or a
watchdog reset cannot drop the latch mid-reboot. On the C3, D10 is GPIO10, a
plain digital pad, and needs both `gpio_hold_en()` **and**
`gpio_deep_sleep_hold_en()`; on the S3, D10 is GPIO9, an RTC pad, where the first
would do. Both are called on both targets because the extra call is harmless and
a conditional there is a bug waiting to happen.

The latch is dropped deliberately at `criticalBatteryMv`, default **3300 mV** —
not at the AP9101C's 2.5 V over-discharge trip, which is far too low for a cell
that still has to run a Wi-Fi transmit.

### After a wake

`millis()` restarts at zero out of deep sleep, which invalidates every
millisecond-based timestamp the firmware was holding. `HallScanner::resetDebounce()`
exists for exactly this: without it, the debounce candidates carry timestamps
from a previous epoch of `millis()` and every lid looks like it changed a moment
ago.

The button needs the opposite treatment, and it lives in RTC memory for it.
`BUTTON_SIG` can sit high for hours — a stuck button, or D5 leaking into it on an
unreworked board ([HARDWARE.md §10.5](HARDWARE.md)) — and that is one press, not
a fresh press on every wake. So the "already reported" latch survives a timer
wake and is only re-armed when this wake could *be* the press, i.e. anything that
is not the timer. The residue: on a leaky board a lid wake re-arms it too, so one
phantom `buttonPress` can follow a lid open. Bounded to lid and charger wakes,
which is better than swallowing real presses, and the honest fix is the §10.5
rework.

Order on wake also matters. `netBegin()` only starts the radio's state machine —
association happens later, out of `netService()` — and both sensor paths are
sampled and debounce-seeded before `loop()` makes its first decision. The reason
the box woke is sitting in the mux right now, so it is established before any
push can go out: find out what happened first, then go and tell somebody.

---

## The adherence model

The device sees one thing: lid *n* went from closed to open at moment *T*. All of
adherence is deciding which scheduled dose that open belongs to, and noticing the
doses nobody ever opened.

### Matching

`matchOpen(schedule, lid, openLocal)` considers that lid's dose instances on the
**previous, current and next** day, so an open at 00:10 still finds a 23:30 dose.
It picks the nearest candidate inside
`[D − windowBeforeMin, D + windowAfterMin + missedGraceMin]` (60, 120 and 15
minutes by default) and breaks ties towards the earlier instance.

The grace period is part of the match window, not a gap in front of it. Its upper
edge is the exact instant `sweepMissed()` takes over, so an open can never fall
between the two — which is what would otherwise happen, and it produced *two*
notifications for one dose that was taken: an `unexpectedOpen` when the lid was
lifted, then a `doseMissed` for the same instance minutes later. Inside the grace
the open is attributed and reported `Late`, and the sweep then finds a resolved
record and leaves the instance alone.

It sets `wrongDay` when the opened lid is not the active lid for **the day the
lid was opened on** — not the day of the instance it matched. That is the
interesting signal: right time, wrong compartment. One consequence falls out and
is correct by that rule: a 23:50 Tuesday dose opened at 00:10 on Wednesday is
`wrongDay` too, because Tuesday's lid is no longer the active one. It is
self-explanatory once the matched dose time is shown beside it.

An open that matches nothing is **not** recorded in the adherence log at all. The
caller logs it as an `unexpectedOpen` event instead, which keeps the dose records
clean — they are a record of scheduled doses, not of lid activity. Lid activity
lives in the event log.

### What each `DoseState` means

| State | Meaning | Where it comes from |
|---|---|---|
| `Pending` | In the future, or inside its window and not yet opened | the default for every instance |
| `Taken` | Opened inside `[D − 15 min, D + windowAfterMin/2]` | `matchOpen` |
| `Late` | Opened inside the window but after that | `matchOpen` |
| `Early` | Opened inside the window but before that | `matchOpen` |
| `Missed` | `windowAfterMin + missedGraceMin` elapsed and nothing ever opened it | `sweepMissed` |
| `Skipped` | A human marked it skipped from the GUI | `markState` |

The three boundaries inside the window are fixed rather than configurable: 15
minutes early still counts as on time, and half the after-window splits "a bit
late" from "very late" without another setting. So with the defaults, a dose at
08:00 reads `Early` before 07:45, `Taken` to 09:00, `Late` to 10:15, and
`Missed` from 10:16 if nobody ever opened it.

`isTakenState()` is true for `Taken`, `Late` and `Early` — all three mean the lid
was actually lifted for that dose, which is as much as the hardware can establish.
`deltaMin` on the record carries the signed offset so the difference is still
visible.

The API adds a seventh value, `none`, for `lids[].todayState`. It is not a
`DoseState`; it means nothing is scheduled for that lid today, or nothing that
this boot is entitled to judge.

### The sweep

`sweepMissed()` creates records for, and marks `Missed`, every instance whose
window closed more than `missedGraceMin` ago and that was never opened. It looks
back `lookbackDays` whole days from the day it is called on, writes the
newly-missed records to an output array so the caller can fire exactly one
notification each, and is idempotent — a record that is already `Missed` is not
re-reported.

`D + windowAfterMin + missedGraceMin` is the **one** deadline at which a dose
counts as failed, and all three consumers use the same one. Until it passes the
sweep leaves the instance alone, `adherencePercent()` leaves it out of the
denominator, and `today()` still calls it pending. That identity is worth
protecting: two numbers on the same screen disagreeing by one dose for fifteen
minutes a day is the kind of bug that gets explained away for weeks.

Exactly-once reporting is a flag on the record, `unreportedMiss`: 1 while the
sweep has marked an instance but no caller has yet been handed it to notify.
Because it lives inside the persisted record rather than in RAM, a mark survives
a reboot and is still pushed — and the clear has to reach flash like any other
field change, or a reboot in the gap re-pushes a miss already sent. The sense of
the flag is deliberate: a record read back from an adherence file written before
the field existed sees the old padding byte, which is 0, i.e. "already
reported", so an old file cannot replay pushes the family has already had.

It has to be a sweep rather than a timer because the box sleeps through the moment
a dose goes missing. Nothing fires at `D + windowAfter + grace`; the next wake
notices that it passed, which is also why `secondsUntilNextDuty()` aims a timer
wake at that deadline.

### The boot floor

A box powered up at 10:00 must not announce that the 08:00 dose was missed.
Nobody was watching it: the cell was out, or the enclosure was open, or it had
just been flashed. Reporting a miss nobody could have prevented, every single
time the battery is changed, is how a notification channel gets muted.

So `sweepMissed()`, `today()` and `adherencePercent()` all take a trailing
`floorLocal`, and an instance whose `localEpoch` is below it is skipped
entirely — not due, not missed, not pending. `AppState::bootFloorLocal` carries
it and every call site in the app passes it, `lidTodayState()` included, or the
GUI's day card would sit on "pending" forever for a dose the sweep will never
resolve.

The floor is **the local epoch of the most recent cold boot**, and the
distinction from a deep-sleep wake is the whole point:

| Came back from | Floor | Why |
|---|---|---|
| Power-on, battery change, latch drop | the moment the clock is first verified | nothing was watching before it |
| Panic, WDT reset, reboot from the GUI | the same | RTC slow memory with an initialiser is reloaded from the image on anything that is not a deep-sleep wake |
| Deep sleep | unchanged, carried through | the box slept *intending* to wake for that dose, so a dose that fell while it slept is a real miss |

That is exactly the line `RTC_DATA_ATTR` draws, which is why the floor lives
there rather than in NVS.

It cannot be named at boot, because at boot there is no local time to name it
with. `0` means "no floor" and is the right answer until the first sync lands;
`bootFloorFromClock()` then back-fills it and only ever moves it earlier, so a
clock stepped backwards cannot leave the floor in the future. The cost of waiting
is a few seconds of dose instances on a cold boot, and the `DoseDue` beep is
unaffected either way — `doseInstancesInRange()` has no floor, so the patient is
still reminded. Only the arithmetic looks away.

One consequence to be clear about: a cold boot at noon also hides a dose that
*was* taken that morning and has a persisted record, so `today()` reports one due
rather than "one of two taken". `/api/history` still shows the record, and no
false miss is ever pushed. It is the one place where honesty about what the box
observed costs a count the family might have liked.

### Storage and arithmetic

`AdherenceLog` is a fixed ring of 128 `DoseRecord`s, newest at the end, which is
comfortably more than a fortnight at four doses a day. `findOrCreate()` never
returns null — it evicts the oldest record when full. No allocation, no
fragmentation, bounded worst case.

`adherencePercent(days)` counts only instances whose window has **closed**, grace
included, so a dose due this evening does not drag today's number down, and it
returns 100 when nothing was due at all. There is no honest way to score a day
with no doses, and reporting 0 % would be a lie that generates a notification. A
`Skipped` dose leaves the denominator: somebody said it was not supposed to be
taken, so counting it as a failure would be an argument with the user.

`today()` returns `due` / `taken` / `missed` / `pending` for the local day, which
is what the GUI header shows. `Skipped` counts in `due` but in none of the three
buckets, so the three do not always add up — the GUI shows skipped separately.

---

## Why the settings live in a file, not in NVS

`Settings` is one struct of about 6.3 kB, and the obvious home for it is the
`Preferences` library — a key/value store on the `nvs` partition. That is where
it started, and on the C3 it never worked once.

`min_spiffs.csv` gives `nvs` `0x5000`, which is 20 kB. NVS is a log-structured
store: rewriting a key writes the new value before retiring the old one, and it
keeps a page in hand for garbage collection. A 6.3 kB value needs two copies plus
slack inside 20 kB, and `putBytes()` simply failed every time. The box therefore
ran permanently on `applyDefaults()`, and because the event log and the dose
records live in LittleFS and worked perfectly, it *looked* like a device that
remembered things. It forgot its Wi-Fi credentials, its schedule and its hall
mapping on every single power cycle, and the only outward sign was one line on a
serial port nobody was watching:

```
[store] settings write failed, backing off
[store] no usable settings blob, writing defaults    <- on every boot
```

Growing the partition would have worked, but it moves every offset in the flash
map, needs a full erase to apply, and buys nothing that a file does not. A 6 kB
record is the wrong shape for a key/value store in the first place. So the
settings became a third LittleFS file next to the two that already worked, with
the same magic/version/`sizeof` header, so a struct layout change is detected
rather than reinterpreted.

The settings file is written through a temp file and a rename, which the history
files do not bother with. The asymmetry is deliberate: losing the event log to a
power cut during a write is an annoyance, whereas losing the Wi-Fi credentials
and the schedule means a box that cannot be reached to be fixed. `nvs` still
holds `lastEpoch`, which is four bytes and exactly what it is good at.

---

## Deliberately not implemented

In rough order of how much it should bother you.

- **No authentication on the HTTP API.** Anyone on the same network can read the
  schedule, the event log and the patient's name, and can rewrite all of it. There
  is no login, no token, no TLS on the server side. The only protection is the
  network the box is on. This is the biggest known gap and it is a conscious
  trade: a single-unit project for one family, where a forgotten password on a
  sealed box is a worse failure than a hostile LAN.
- **The Wi-Fi password sits in NVS unencrypted.** NVS encryption needs flash
  encryption and key management, which on a board that will be reflashed over USB
  repeatedly is a reliable way to brick it.
- **No OTA.** Updates mean opening the box and a USB cable.
- **No WebSockets or SSE.** Covered above.
- **No cloud, no account, no second box.** Notifications go out to a service you
  choose; nothing comes back in.
- **No per-pill tracking, ever, with this hardware.** One magnet per lid is one
  bit per lid. The box cannot tell a dose taken from a lid opened and shut.
- **No escalation.** A missed dose fires one notification. There is no "nobody
  acknowledged, ring somebody else" ladder.
- **No SMS and no email.** Both need a relay; that is what the webhook is for.
- **No timezone detection.** A POSIX TZ string is typed into settings.
- **Battery percentage is a voltage curve**, deliberately pessimistic at the top so
  a freshly charged pack does not read 100 % for an hour. No coulomb counting, and
  the reading compresses above about 4.1 V because the divider node is near the
  ADC's ceiling.
- **`sensorFault` has no notification switch.** The channel-7 ground reference
  failing means no hall reading can be trusted, which is arguably the most
  important thing the box could tell you, and it is currently only visible in the
  event feed.
- **No tests for `easypill_hw`.** Every line of it is about a pin.
- **DST transitions slip by an hour for one day.** Accepted, see above.

## Obvious next steps

1. **Finish the hardware rework on any further boards.** A *stock* board cannot
   run on a battery: the protection trips at around 30 mA and the radio pulls
   well past that. The first unit was reworked by disabling the protection, and
   with that done battery operation has been measured and holds up — 241 HTTP
   replies with no failures and no reset, a full active Wi-Fi scan, and 5 mV of
   sag ([HARDWARE.md §9 and §10](HARDWARE.md)) — so the sleep policy, the drift
   correction and the notification path are no longer untested. What is still
   untested is a board with the protection *working*, which is what a second
   unit or a V1 respin needs to address properly rather than by disabling it.
   [BRINGUP.md §0](BRINGUP.md).
2. **A shared secret on the API.** Even a static bearer token checked on every
   `/api/*` route, with the GUI holding it in `localStorage`, would close the
   worst of the gap for an afternoon's work.
3. **OTA**, so a firmware fix does not mean opening a glued enclosure.
4. **An ESP-IDF build to select the 32.768 kHz crystal.** Single-digit ppm would
   retire the whole drift-measurement mechanism, and retiring code is the best
   kind of improvement.
5. **A second sensor per lid.** Two magnets, or a hall plus a reed, would give
   open/close direction and a dwell time, which is enough to distinguish "lifted
   and immediately shut" from "lifted, something removed, shut". That is the only
   route to a real answer on whether a pill came out.
6. **LED and buzzer resistors** — 470 R gives a red-biased white and a buzzer you
   cannot hear across a room. ~220 R red, 68–100 R green/blue, R27 to 0–47 R.
7. **A hardware test fixture**, even a jig with seven solenoids, so `easypill_hw`
   gets regression tests instead of a document and a careful evening.
