# EasyPill simulator

`easypill_sim.py` is the whole EasyPill product with the hardware taken out: it
fakes the seven lids, the battery, the chargers and — most importantly — the
clock, and serves **the real GUI out of `../web`** plus the real HTTP contract
from `docs/API.md`. Open the URL it prints and you are looking at what the
device will serve.

It exists so the box can be demonstrated, filmed and debugged with nothing on
the desk, and so the scheduling and adherence maths can be checked without
waiting a week for a week to pass.

```sh
python3 sim/easypill_sim.py          # then open http://127.0.0.1:8080/
```

Python 3.9, standard library only. No pip, no virtualenv, no build step.

## Flags

| flag | default | what it does |
|---|---|---|
| `--port` | `8080` | TCP port |
| `--host` | `127.0.0.1` | bind address; `0.0.0.0` to reach it from a phone or an iPad on the same network |
| `--web` | `../web` | directory holding the GUI's four files |
| `--time-scale` | `1` | simulated seconds per real second, `0.1`–`3600`. `3600` runs a week of schedule in three minutes |
| `--state-file` | `sim/sim_state.json` | persisted between runs |
| `--no-persist` | off | read and write nothing |
| `--demo` | off | drive a scripted week of doses with commentary (see below) |
| `--ntfy TOPIC` | — | enable ntfy on that topic at startup, so pushes land on a real device immediately |
| `--ntfy-server URL` | `https://ntfy.sh` | self-hosted ntfy |
| `--drift-ppm` | `-1840` | how wrong the simulated RC oscillator runs (see *Timekeeping*) |
| `--verbose` | off | log every event, push and HTTP request |

The GUI's files are read from disk on **every request**, so editing
`web/app.js` and reloading the browser is the whole development loop — no
restart. If `web/index.html` is missing, the simulator says so and serves a
small placeholder page that still shows live state and can open and close lids,
so the API is usable before the GUI exists.

## What it fakes

**Seven lids.** Each is open or closed, with the polarity semantics from
`docs/HARDWARE.md` §3: a magnet sits over the AH1912 while the lid is shut, so
closed reads LOW and `rawMask` is built back out of the lid states through
`hallToLid`, `hallClosedIsLow` and `hallInvertMask`. `wakeUpHigh` is the
hardware OR of the seven halls, the button and charger presence (§4), which is
why one lid left open masks everything else.

**A battery.** Discharges slowly (≈2 mV per simulated hour, plus a bite for
every push and a nibble for every HTTP request — radio time is charge),
charges at ≈300 mV/h whenever USB or the coil is on, and reports a percentage
off a single-cell LiPo curve that is deliberately pessimistic at the top.
Crossing `lowBatteryMv` and `criticalBatteryMv` fires the events once each, on
the edge.

**A factory-default prescription.** A fresh device — no state file, or
`factoryReset` — carries 08:00 and 20:00 on all seven lids with one placeholder
`Pill ×1` on each, the same fourteen `applyDefaults()` seeds on the board, so
the first screen is populated rather than looking like a bug.

**A clock with no RTC**, which is the interesting part. See below.

**Enough device info** (`chip: "SIM"`, firmware version, uptime, heap, MAC,
flash use) that the GUI footer renders.

## Timekeeping, because there is no RTC

`docs/HARDWARE.md` §8: this board has no battery-backed clock, the internal RC
oscillator is good to a few per cent, and the firmware's only defences are
syncing SNTP whenever Wi-Fi comes up, measuring its own drift, and never
inventing a wall-clock time. The simulator models all three:

* Two timelines. `trueNow` is the simulated world's reference clock; `now` is
  what the device *believes*, running wrong by `--drift-ppm` (default
  −1840 ppm, i.e. losing about 2.6 minutes a day). `/api/sim` reports
  `clockErrorSec` so you can watch them separate.
* At boot the clock comes from the state file, is marked **unverified**, and
  events logged in the meantime get `at: 0` and `timeVerified: false` — exactly
  what the firmware does, because a timestamp it cannot stand behind is worse
  than none. The first sync back-fills them from their uptime.
* A sync happens on Wi-Fi coming up, when `POST /api/wifi` stores new
  credentials, when a charger appears (mains means a radio), on
  `POST /api/action {"action":"syncTime"}`, and every
  `time.resyncIntervalSec` of simulated time (6 h by default).
* From the second sync on, the error over the interval is turned into a ppm
  figure and written to `time.driftPpm`, which is how the firmware learns to
  correct sleep durations. Run at `--time-scale 3600` and you can watch it
  recover the −1840 ppm within a couple of syncs.
* Drift accrues with **lived** simulated time. An explicit `setClock` or
  `advanceMin` moves the whole world — the device and the reference clock
  together — and keeps whatever error the device already had.

Everything else derives from the simulated clock and never from `time.time()`:
dose due and missed detection, the daily summary, the battery, the uptime.

All API timestamps are **local epoch seconds** (`tzOffsetSec` is reported), and
the offset comes from the host's own timezone.

## Notifications really are sent

This is the point of the box, so it is not stubbed. ntfy, Pushover and the
generic webhook all go out over `urllib.request`, in a worker thread so a slow
network cannot block an HTTP response. `testNotify` is the one synchronous
path, because the GUI wants the real status code.

Fastest way to get a push onto an iPad tonight: install the ntfy app, subscribe
to a topic nobody else will guess, then

```sh
python3 sim/easypill_sim.py --ntfy easypill-grandma-7f3a91
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"action":"testNotify","target":"all"}' http://127.0.0.1:8080/api/action
```

The reply carries the real HTTP status, in `result.httpCode`, beside
`result.sent` — the top-level `ok` is the verdict on the *request*, so a push
that was attempted and refused still answers `{"ok": true}` with the failure in
the payload, exactly as `notifyTest()` does on the box. Only a bad `target` is
a 400. Everything else is configurable from the GUI through
`POST /api/settings` (and a token is better set there than on a command line,
where it lands in your shell history).

Wire formats, which the firmware must match:

* **ntfy** — `POST <server>/<topic>`, the message as the plain-text body, with
  `Title`, `Priority` (1–5) and `Tags` headers, plus
  `Authorization: Bearer <token>` when a token is stored.
* **Pushover** — `POST https://api.pushover.net/1/messages.json`, form-encoded
  `token`, `user`, `title`, `message`, `priority`. ntfy's 1–5 maps onto
  Pushover's −2–2 as `{1:-2, 2:-1, 3:0, 4:1, 5:2}`.
* **webhook** — `POST <url>` with
  `{"device", "patient", "title", "message", "event", "lid", "lidName",
  "localEpoch", "priority", "tags": [...], "source", "fw"}`. `lid` is `-1` when
  the push is not about one compartment and `event` is `"unknown"` for a push
  that is not an event (the test push, the daily summary), which is what
  `toString(EventType::Count)` gives the firmware. `source` is the only field
  that differs between the two: `easypill-sim` here, `easypill` on the box.

The five secrets — `wifi.pass`, `wifi.apPassword`, `notify.ntfyToken`,
`notify.pushoverToken` and `notify.pushoverUser` — are never served in
cleartext: `GET /api/settings` reports `"__set__"` for a stored one and `""` for
an empty one, posting `"__set__"` back leaves it alone and posting `""` clears
it. So `curl GET | curl POST` round-trips the whole tree without touching a
credential, and the literal `__set__` can never end up stored.

Per-event toggles (`onLidOpen`, `onDoseTaken`, `onDoseMissed`, `onDoseDue`,
`onUnexpectedOpen`, `onLowBattery`, `onBoot`, `onDailySummary`) and quiet hours
(`quietFromMinute`/`quietToMinute`, equal values = off, wrapping midnight is
fine) are honoured. With the defaults, a day of normal use is two lid-open and
two dose-taken pushes; turn `onLidOpen` off if that is too chatty.

## `POST /api/sim` — the fake hardware

The firmware 404s this endpoint; here it is the whole board.

| field | effect |
|---|---|
| `openLid` / `closeLid` / `toggleLid` | lid index 0–6, drives a hall transition |
| `pressButton` | milliseconds (≥1200 counts as a hold) |
| `setVbatMv` | battery voltage, 2500–4300 |
| `setUsb` / `setCoil` | charger presence, boolean |
| `timeScale` | 0.1–3600 |
| `setClock` | local epoch to jump to |
| `advanceMin` | jump forward (or back) n minutes |
| `reset` | default hardware, empty logs, real time again — **settings are kept**; `/api/action factoryReset` is the one that resets settings |

The reply is `{"ok": true, "applied": [...], "sim": {...}}`, and the `sim`
block also reports `clockErrorSec`, `syncCount`, `driftPpm`, `notifyTargets`,
`pushesSent`, `floorLocal` and whether the demo is running.

```sh
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"openLid":2}' http://127.0.0.1:8080/api/sim
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"timeScale":600}' http://127.0.0.1:8080/api/sim
```

## `--demo`: a week in a couple of minutes

Drives a believable scenario against the simulated clock and prints a running
commentary. Run it with the GUI open — it visibly drives it, and it is what
makes it possible to shoot a video or sanity-check the adherence maths without
waiting a week.

Monday to Sunday, 08:00 and 20:00, Metformin and Ramipril set on all seven lids
through the same `/api/schedule/bulk` call the GUI's "set this pill on every
day" button uses. Most doses are taken on time; Tuesday evening is taken 90
minutes late (inside the 120-minute window, so it counts, but it is recorded
`late`); Wednesday evening is never taken at all and goes `missed` once its
window plus grace closes; on Thursday *Friday's* lid is opened on the right day
(`doseTaken` and then `wrongDayOpen` — right time, wrong compartment, which is
the only thing the box can actually tell you, and it is reported beside the
taken event rather than in place of it); on Friday the battery falls below the
warning threshold and then a charger appears. It ends by printing the 7- and
30-day adherence and the tally of dose records, and leaves the server running.

The demo raises the time scale to at least 120x so each step takes a second or
two of real time, and it clears the event log and dose history first so the
week's numbers are clean. Add `--ntfy <topic>` and every notification in that
week lands on your phone.

## Behaviour reimplemented from the headers

The scheduling and adherence logic is a deliberate line-for-line
reimplementation of `firmware/lib/easypill_core/ep_schedule.h`,
`ep_adherence.h` and `ep_timecalc.h`, so that a disagreement between the
simulator and the firmware is a bug in one of them:

* local-epoch integer arithmetic; 1970-01-01 was a Thursday, which is what
  makes the weekday one-liners work; lid 0 = Monday in weekday mode, and a
  rolling tray from `sequentialAnchorDay` in sequential mode;
* `matchOpen` considers the lid's instances on the **previous, current and next
  day**, so an open just after midnight still finds a 23:50 dose; the nearest
  candidate inside `[D − windowBefore, D + windowAfter + missedGraceMin]` wins
  and ties go to the earlier instance. The window runs through the grace period
  so that a lid opened while the dose is merely overdue belongs to that dose
  instead of reading as an `unexpectedOpen` followed by a `doseMissed` for the
  very same dose;
* inside that window the verdict is `taken` in
  `[D − 15 min, D + windowAfter/2]`, `early` before it and `late` after it —
  `kOnTimeSlackSec` and the half-window from `ep_adherence.cpp`, compared in
  seconds, not in whole minutes. All three count as adherent, per
  `isTakenState`;
* `wrongDay` when a lid is opened on a day that is not its day, decided by **the
  day the lid was opened**, not by the day of the dose it matched — so a 23:50
  dose opened at 00:10 is a wrong-day open too, and a foreign lid opened
  nowhere near a dose time still reports one. It is set whether or not anything
  matched, and a wrong-day open is logged *as well as* the `doseTaken`, never
  instead of it. The record is kept so history shows what happened, but a
  foreign lid is not a timeline instance, so it cannot flatter the percentages;
* `sweepMissed` reports each newly-missed instance exactly once, because the
  mark it leaves behind is what stops it coming back;
* a dose has failed only once `windowAfterMin + missedGraceMin` has elapsed, and
  that one deadline governs `sweepMissed`, `today()` and both adherence
  percentages — a dose still inside its grace has not failed yet and must not
  drag the number down. Adherence returns 100 % when nothing was due, leaves
  human-skipped doses out of the denominator, and rounds half **up** with
  integer arithmetic, which is what the firmware does and what Python's
  `round()` does not.

### Decisions the headers left open

Three places where the headers do not say, so the simulator chose. **The
firmware should match these, or this file should be updated to match the
firmware.**

1. **A lid's `todayState`** is the worst news among that lid's instances today:
   `missed` outranks `pending`, which outranks `late`, `early`, `taken`,
   `skipped`. A lid that is not today's lid reports `none`.
2. **Doses from before the device started** are nobody's business: they are
   not counted as due and never declared missed — unless a record exists for
   them, because a lid that actually opened says more than the start time does.
   Without this, every run would open with a burst of missed-dose pushes for a
   week that never happened. `floorLocal` in `/api/sim` is that cutoff. This is
   the one rule the firmware was changed to match rather than the other way
   round: a real box pushed a false missed-dose after every power-on or battery
   change, so `easypill_core` took a `floorLocal` argument and the app passes
   the epoch of the last **cold** boot. A deep-sleep wake is not one — the box
   intends to wake for a dose, so a dose that falls during sleep is still
   swept.
3. **`doseDue` events** fire only for simulated time actually lived through. A
   deliberate jump of hours or days does not replay every dose it skipped;
   `sweepMissed` is what notices what was skipped over.

Smaller ones: `lid` is `255` on events that are not about a lid (`ep::kNoLid`,
which is what the firmware's JSON writer will emit); doses are stored in the
order the GUI wrote them and are never re-sorted, because every dose record is
keyed on the storage index and the core sorts only while generating instances —
so a bulk set into slot 3 of a lid holding one dose leaves a **disabled**
00:00 placeholder in slot 2, not a midnight dose; `POST /api/schedule/bulk`
takes `doseIndex: 255` to mean "no particular slot", which is only meaningful
for `op: "set"` and is refused with a 400 for `op: "remove"` — `op: "clear"` is
how a client asks for a whole lid; `/api/history` is oldest
first, matching the order a firmware loop over `AdherenceLog` would produce;
`markTaken` is taken at its word and stores `taken` however late it is, only a
real lid open gets graded; `powerOff` and a critical battery log the event and
set `poweredOff` but keep serving, because a simulator that goes dark is not
much use.

## What this does **not** prove

Everything the mux does. The three XS3A4051s share one address bus, `SIG_OUT`
must be driven low before any address change, channel 7 is the parking spot,
`MUX_C` and `SIG_OUT` sit on UART0 — none of that exists here, and none of it
is exercised by this file. Nor is deep sleep, the power latch, hall debounce,
ADC calibration, LED multiplexing, buzzer drive, or Wi-Fi in any real sense.
This simulator tells you the product behaves; only the board tells you the
firmware does.

## Persistence

Settings, schedule, event log, dose records, lid states, battery and the last
known epoch go to `sim/sim_state.json` (atomically, at most every couple of
seconds, and on exit). Delete the file, or pass `--no-persist`, for a
factory-fresh device. `POST /api/action {"action":"factoryReset","confirm":true}`
does the same thing from the GUI.
