# EasyPill HTTP API

One contract, three implementations: the ESP32 firmware (`firmware/src/app/web_server.cpp`),
the host simulator (`sim/easypill_sim.py`) and the web GUI that talks to both
(`web/`). If you change anything here, change all three.

- All bodies are JSON, `Content-Type: application/json`.
- All timestamps are **local epoch seconds**: Unix seconds already shifted by
  the device's UTC offset, so the GUI can render them without knowing the
  timezone. `tzOffsetSec` is reported so you can recover UTC if you need it. The
  one exception carries it in its name: `lastSyncUtc`.
- A timestamp of `0` never means 1970. It means "no trustworthy time for this",
  which is a real state on a box with no RTC and an unverified clock.
- `POST` endpoints accept a partial object and merge it. Anything absent is
  left alone.
- Every `POST` replies `{"ok": true, ...}` or
  `{"ok": false, "error": "human readable"}` with a 4xx status.
- Errors never 500 on bad input. Most fields clamp and report the clamp; the few
  where guessing wrong would destroy data reject with a 400 instead, and say
  what to send.

## `GET /`

The single-page GUI, served gzipped from flash. `/app.css` and `/app.js` are
served the same way. `/favicon.svg` too.

## `GET /api/state`

The polling endpoint. The GUI hits this every 1000 ms while visible, 5000 ms
when the tab is hidden. Keep it cheap — it is the only thing between a lid
opening and the screen updating.

```json
{
  "ok": true,
  "now": 1762000000,
  "tzOffsetSec": 3600,
  "timeVerified": true,
  "watchingSince": 1761996000,
  "uptimeS": 8143,
  "fw": "0.1.0",
  "chip": "ESP32-C3",
  "simulated": false,

  "lids": [
    {
      "lid": 0,
      "name": "Monday",
      "open": false,
      "lastOpenedAt": 1761990120,
      "activeToday": false,
      "doseCount": 2,
      "todayState": "taken",
      "nextAt": 1762086000
    }
  ],

  "nextDose": {
    "valid": true,
    "at": 1762003600,
    "inMin": 60,
    "lid": 1,
    "dose": 0,
    "lidName": "Tuesday",
    "label": "Morning",
    "pills": [{"name": "Metformin", "dose": "500 mg", "count": 1}]
  },

  "power": {
    "vbatMv": 3920,
    "percent": 72,
    "usbMv": 5020,
    "coilMv": 0,
    "usbPresent": true,
    "coilPresent": false,
    "charging": true,
    "buttonPressed": false,
    "lowBattery": false
  },

  "wifi": {
    "mode": "sta",
    "ssid": "Home",
    "rssi": -54,
    "ip": "192.168.1.42",
    "hostname": "easypill"
  },

  "adherence": {
    "pct7d": 86,
    "pct30d": 91,
    "dueToday": 2,
    "takenToday": 1,
    "missedToday": 0,
    "pendingToday": 1
  },

  "sensors": {
    "rawMask": 0,
    "openMask": 0,
    "gndRefOk": true,
    "wakeUpHigh": false
  },

  "events": [
    {
      "at": 1761990120,
      "uptimeMs": 8000,
      "type": "lidOpened",
      "lid": 0,
      "detail": 0,
      "timeVerified": true,
      "text": "Monday lid opened"
    }
  ]
}
```

`todayState` is one of `pending`, `taken`, `late`, `early`, `missed`,
`skipped`, `none` (nothing scheduled for that lid today, or nothing this boot is
entitled to judge — see the boot floor below). A lid can hold several doses in
one day, so it reports the worst state present, ranked
`missed` > `pending` > `late` > `early` > `skipped` > `taken`. `doseCount`
counts that lid's *enabled* doses.

`wifi.mode` is `off` | `connecting` | `sta` | `ap` | `failed`. `ap` is the setup
network, raised after three failed joins when `wifi.apFallback` is on, or
immediately when no SSID is stored at all.

`adherence` and `todayState` both honour the **boot floor**: a dose scheduled
before this device last *cold* booted is not counted as due, taken, missed or
pending, because nothing was watching the box then. A dose that fell during a
deep sleep still counts — the box intended to wake for it. See
[DESIGN.md](DESIGN.md#the-boot-floor).

`lastOpenedAt` is the newest `lidOpened` event's timestamp, and `0` when that
open has no verified time yet — the same convention an event's `at` follows.

`events` carries the 12 newest. Use `/api/events` for more. An event's `lid` is
`255` when it is not about one particular lid, and its `at` is `0` when the
clock had never been verified at the time — use `uptimeMs` against the
`watchingSince` is the local epoch at which the box last **cold** booted — a
power-on, not a deep-sleep wake, because the box sleeps intending to wake for a
dose. Nothing scheduled before it was ever this device's business to watch, so
`adherence` and `lids[].todayState` leave those instances out entirely: not
due, not missed, not pending. A client that renders the day from the schedule
must do the same, or it shows a dose as untaken that the counter beside it does
not count. `0` means there is no floor yet, which is the case until the clock
is first verified.

top-level `uptimeS` for those. The first sync back-fills the zeroes it can, but
an event reloaded from flash after a reboot keeps `at` 0 for good: its
`uptimeMs` was measured against a `millis()` that no longer exists, so its
`uptimeMs` is *larger* than `uptimeS × 1000` and there is nothing to work back
from. Render those as "before this boot" rather than as a wall-clock time.

`detail` is a signed 16-bit number whose unit depends on `type`:

| `type` | `detail` |
|---|---|
| `doseTaken` | signed minutes between the open and the dose time |
| `doseMissed`, `doseDue` | minute of the local day the dose was scheduled for |
| `doseReminder` | minutes until the dose |
| `batteryLow`, `batteryCritical`, `chargerIn` | millivolts |
| `wifiUp` | RSSI in dBm |
| `sleepEnter` | seconds of sleep asked for |
| `timeSynced` | measured drift in ppm |
| `boot` | the wake reason as a number |
| `configChanged` | lids touched by a bulk edit, or the raw sensor mask after a calibration |
| everything else | 0 |

## `GET /api/settings` · `POST /api/settings`

The whole settings tree, minus secrets. There are exactly five secret fields —
`wifi.pass`, `wifi.apPassword`, `notify.ntfyToken`, `notify.pushoverToken` and
`notify.pushoverUser` — and all five follow one protocol in both directions:

- **Out:** `"__set__"` when a value is stored, `""` when it is not. A stored
  secret is never echoed in cleartext, not even to a client that just set it.
- **In:** `"__set__"` means "leave the stored value alone", `""` means "clear
  it", anything else replaces it, and an absent key is left alone like every
  other field. The literal string `"__set__"` is therefore never stored.

That is what lets the GUI round-trip the whole form without ever receiving a
password.

```json
{
  "ok": true,
  "ui":     {"deviceName": "EasyPill", "patientName": "Grandma", "showAdvanced": false},
  "wifi":   {"ssid": "Home", "pass": "__set__", "hostname": "easypill",
             "apFallback": true, "apPassword": "__set__"},
  "time":   {"ntp1": "pool.ntp.org", "ntp2": "time.google.com",
             "tz": "CET-1CEST,M3.5.0,M10.5.0/3", "driftPpm": -1840,
             "resyncIntervalSec": 21600},
  "notify": {"ntfyEnabled": false, "ntfyServer": "https://ntfy.sh",
             "ntfyTopic": "", "ntfyToken": "",
             "pushoverEnabled": false, "pushoverToken": "", "pushoverUser": "",
             "webhookEnabled": false, "webhookUrl": "",
             "onLidOpen": true, "onDoseTaken": true, "onDoseMissed": true,
             "onDoseDue": false, "onUnexpectedOpen": true,
             "onLowBattery": true, "onBoot": false, "onDailySummary": false,
             "dailySummaryMinute": 1260,
             "quietFromMinute": 0, "quietToMinute": 0},
  "power":  {"policy": "smart", "awakeAfterEventSec": 300,
             "wakeIntervalSec": 900, "lowBatteryMv": 3500,
             "criticalBatteryMv": 3300, "buzzerOnReminder": true,
             "ledOnReminder": true},
  "hw":     {"hallClosedIsLow": true, "hallInvertMask": 0,
             "hallToLid": [6,5,4,3,2,1,0],
             "ledChannelRed": 2, "ledChannelGreen": 1, "ledChannelBlue": 0,
             "ledBrightness": 120,
             "buzzerChannel": 3, "buzzerToneHz": 2700, "buzzerSelfDrive": false,
             "vbatDivNum": 175, "vbatDivDen": 100,
             "railDivNum": 175, "railDivDen": 75,
             "buttonDivNum": 1, "buttonDivDen": 1,
             "buttonThresholdMv": 1500, "statThresholdMv": 1500,
             "hallDebounceMs": 60, "buttonDebounceMs": 80}
}
```

`power.policy` is `alwaysOn` | `smart` | `aggressive`. The ranges the device
will actually keep: `awakeAfterEventSec` 10–3600, `wakeIntervalSec` 30–21600,
`lowBatteryMv` 3000–4200, `criticalBatteryMv` 2800–4200 — and then forced to
`lowBatteryMv - 100` whenever it is not already below it, because critical has
to trip first or the warning never fires. `resyncIntervalSec` is 600–86400.

The six section names are fixed. Both implementations emit `ui`, `wifi`,
`time`, `notify`, `power` and `hw` in every reply, and the GUI refuses a reply
that is missing one.

`POST` merges and replies with the validated result plus
`{"changed": true}` when validation had to clamp something. It also accepts a
whole `schedule` object alongside the six sections, which is how a backup is
restored in one request.

## `GET /api/schedule` · `POST /api/schedule`

```json
{
  "ok": true,
  "mode": "weekday",
  "sequentialAnchorDay": 0,
  "windowBeforeMin": 60,
  "windowAfterMin": 120,
  "missedGraceMin": 15,
  "remindBeforeMin": 0,
  "lids": [
    {
      "lid": 0,
      "name": "Monday",
      "doses": [
        {
          "enabled": true,
          "time": "08:00",
          "minute": 480,
          "label": "Morning",
          "pills": [{"name": "Metformin", "dose": "500 mg", "count": 1}]
        }
      ]
    }
  ]
}
```

`POST` replaces every lid that appears in `lids`, identified by its own `lid`
index: that lid's `doses` array becomes its whole dose list. Lids absent from
the array are left alone, and so is a listed lid's dose list if the entry
carries no `doses` key, which is how a rename-only post works. The top-level
fields merge like everywhere else. Accept either `time` (`"HH:MM"`) or `minute`
(0–1439); `time` wins if both are present.

`windowBeforeMin`, `missedGraceMin` and `remindBeforeMin` are clamped to
0–1439 and `windowAfterMin` to 1–1439: `matchOpen` only looks one day either
side of an open, so neither half of the window may reach a day.

Doses stay in slot order. Nothing sorts them by time, so a dose written into
slot 3 stays after slot 2 even if it is earlier in the day — the device reports
them in the order they are stored and the GUI sorts its own display copy.

## `POST /api/schedule/bulk`

The "set this pill on every day" button.

```json
{
  "lidMask": 127,
  "doseIndex": 0,
  "op": "set",
  "dose": {"enabled": true, "time": "08:00", "label": "Morning",
           "pills": [{"name": "Metformin", "dose": "500 mg", "count": 1}]}
}
```

`lidMask` bit *n* selects lid *n*; `127` is all seven. It is required, and
0–127: an absent, non-numeric or out-of-range mask is `lidMask must be 0..127`
with a 400, never a silently narrowed mask. `op` is required too, and is
`set` | `remove` | `clear` (`clear` wipes every dose on the selected lids and
ignores `dose`). `doseIndex` defaults to `255`, which means "append into the
first free slot" and is only meaningful for `op: "set"`.

`op: "remove"` with `doseIndex: 255` is a **400** carrying exactly
`op 'remove' needs doseIndex 0..3; use op 'clear' to wipe a lid`. The core's
`removeDoseFromLids()` reads 255 as "empty this lid", so letting it through
would wipe a week of prescriptions from the control the GUI leaves on its
default. Both implementations answer with that string, and the GUI never sends
the combination. Any other out-of-range index is
`doseIndex must be 0..3 or 255`.

Reply: `{"ok": true, "changed": 7}` — how many lids the call actually touched.

## `GET /api/events?limit=64&since=<localEpoch>`

```json
{"ok": true, "events": [ /* newest first, same shape as in /api/state */ ]}
```

A `since` of anything but 0 skips the events whose `at` is 0, since an event
with no verified time cannot be placed on either side of a cutoff.

## `GET /api/history?days=14`

Dose records, for the adherence grid.

```json
{
  "ok": true,
  "records": [
    {"scheduledAt": 1761984000, "actualAt": 1761985800, "lid": 0, "dose": 0,
     "state": "taken", "deltaMin": 30}
  ]
}
```

Oldest first, because the adherence grid reads left to right. `state` is one of
`pending`, `taken`, `late`, `early`, `missed`, `skipped`. Nothing opened a
`missed` or `skipped` record, so both carry `actualAt` 0 and `deltaMin` 0.
Records are kept whatever the boot floor says: the floor decides what counts
towards adherence, not what is remembered.

## `POST /api/action`

One-shot commands. `{"action": "...", ...}`.

| action | extra fields | does |
|---|---|---|
| `beep` | `onMs` (10–5000), `repeats` (1–20) | chirps the buzzer |
| `led` | `r`, `g`, `b`, `ms` (0–60000) | sets the LED; `ms: 0` turns it off |
| `identifyLed` | — | drives colour slot 0, then 1, then 2 for about 4.5 s in total and returns immediately, reporting `msEach` and the `configured` slot-to-mux-channel mapping it is testing, so you can map colours to channels |
| `testNotify` | `target` (`ntfy`\|`pushover`\|`webhook`\|`all`) | sends a test push; reports `sent`, `httpCode` and `detail`. A bad `target` is the only 400 — a push that failed is still a successful request |
| `syncTime` | — | forces an SNTP sync and blocks until it answers or times out; reports `synced`, `verified`, `before`, `after`, `correctionSec`, `lastSyncUtc`, `syncCount`, `driftPpm` |
| `markTaken` | `scheduledAt`, `lid`, `dose` | manual override. `scheduledAt` and `lid` are required, `dose` defaults to 0 |
| `markSkipped` | `scheduledAt`, `lid`, `dose` | manual override |
| `clearEvents` | — | empties the event log |
| `clearHistory` | — | empties the dose records |
| `factoryReset` | `confirm: true` | defaults, then reboot |
| `reboot` | — | restarts |
| `powerOff` | — | drops the latch |
| `calibrateHalls` | — | **close every lid first.** Learns polarity and writes `hallClosedIsLow` / `hallInvertMask`, reporting those plus `rawMask` and `gndRefOk`. **400** when the reading cannot be trusted, with nothing written and nothing logged |
| `stayAwake` | `seconds` (1–86400) | postpones deep sleep, so the GUI stays reachable |

Reply: `{"ok": true, "result": {...}}`.

`syncTime`'s `before` and `after` are local epochs like every other timestamp
here, and `lastSyncUtc` is the one field in the whole API that is UTC — it says
so in its name. `correctionSec` stays a UTC-to-UTC delta, so a DST change
landing inside the sync window cannot be mistaken for an hour of drift.

`calibrateHalls` refuses for two different reasons and the `error` string says
which, so a client should print it rather than interpret it: the sensors
disagreeing (a lid left open, a missing magnet, a dead sensor — not a polarity,
see [HARDWARE.md §3](HARDWARE.md)), or channel 7's ground reference not reading
LOW, which means the address lines or the hall mux are dead and every reading is
fiction. On success `gndRefOk` is the visible proof the polarity was learnt on a
live bus.

## `GET /api/info`

Build and chip detail for the footer: `fw`, `buildDate`, `chip`, `cores`,
`flashMb`, `heapFree`, `heapMin`, `macAddress`, `resetReason`, `wakeReason`,
`sdkVersion`, `fsUsedBytes`, `fsTotalBytes`.

## `GET /api/scan` · `POST /api/wifi`

`GET /api/scan` → `{"ok": true, "networks": [{"ssid": "Home", "rssi": -54, "secure": true}]}`

`POST /api/wifi` → `{"ssid": "...", "pass": "..."}`, replies with
`{"ok": true, "connecting": true}` and the GUI then polls `/api/state`. `pass`
follows the secret protocol above: `"__set__"`, or absent, or not a string,
means "keep the stored password"; `""` clears it, which is how an open network
is joined. An empty `ssid` is a 400.

## `POST /api/sim` — simulator only

The firmware returns 404. The simulator uses it to fake hardware so the GUI
and the whole workflow can be exercised with no board on the desk.

| field | effect |
|---|---|
| `openLid` / `closeLid` | lid index, drives a hall transition |
| `toggleLid` | lid index |
| `pressButton` | `ms` |
| `setVbatMv` | battery voltage |
| `setUsb` / `setCoil` | charger presence, boolean |
| `timeScale` | 1 = real time, 60 = a minute a second, up to 3600 |
| `setClock` | local epoch to jump to |
| `advanceMin` | jump forward n minutes |
| `reset` | back to defaults |

Reply: `{"ok": true, "sim": { ...current sim state... }}`.
