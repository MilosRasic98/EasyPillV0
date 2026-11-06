# Getting a notification onto an iPad

Three transports, in order of how much work they are. Start with ntfy. If it
works, stop reading at the end of section 1.

| Transport | Account needed | Cost | Setup time |
|---|---|---|---|
| **ntfy** | none | free | about four minutes |
| Pushover | yes | one-off per platform | fifteen minutes |
| Webhook | depends | depends | as long as the thing at the other end takes |

---

## 1. ntfy — the short path that works

ntfy is a pub/sub notification service. The box POSTs a message to a URL; the app
on the iPad is subscribed to that URL and shows the message. There is no account,
no API key and no OAuth. The subscription identifier is called a **topic** and it
is a string you make up.

### 1.1 Pick a topic name first

Do this before you touch the iPad, because the name is the one thing that has to
match in two places.

On a public ntfy server, anyone who knows the topic name can read everything
published to it, and anyone can publish to it. There is no password. **The topic
name is the only secret**, so it has to be unguessable. `easypill`,
`grandma-pills` and `easypill-home` are all effectively public.

```bash
echo "easypill-$(openssl rand -hex 8)"
```

That gives you something like `easypill-9f3c71a0bd4e2516`. Write it down
somewhere you will still have it in a year. Changing it later means changing it
on every device that is subscribed.

If you are not comfortable with a shared-secret-in-a-URL model — and it is a
reasonable thing not to be comfortable with — see section 1.6.

### 1.2 Install the app and subscribe

1. On the iPad, install **ntfy** from the App Store.
2. Open it, tap **+** to subscribe to a topic.
3. Type the topic name exactly, including the random part. It is case-sensitive.
4. Leave the server as the default `ntfy.sh`.
5. Allow notifications when iOS asks. If you skipped that prompt, Settings →
   Notifications → ntfy → Allow Notifications.

The subscription will sit there empty. That is expected; nothing has been
published yet.

### 1.3 Prove the topic before involving the ESP32

One less thing to debug later. From any machine:

```bash
curl -d "hello from the bench" https://ntfy.sh/easypill-9f3c71a0bd4e2516
```

The iPad should buzz within a second or two. If it does not, the problem is
between ntfy and the iPad and the box will not fix it. Check the topic spelling
and that notifications are allowed for the app.

### 1.4 Put the topic in the GUI

In the EasyPill GUI → **Settings → Notifications**:

| Field | Value |
|---|---|
| ntfy enabled | on |
| ntfy server | `https://ntfy.sh` |
| ntfy topic | `easypill-9f3c71a0bd4e2516` |
| ntfy token | leave empty on a public server |

Save. Then press **Test** next to the ntfy row. The GUI shows the HTTP status the
box got back, which is the useful part: `200` means ntfy accepted the message, so
anything that goes wrong after that is on the iPad side.

Equivalent over the API, if the GUI is not reachable:

```bash
curl -sX POST http://easypill.local/api/action -H 'content-type: application/json' -d '{"action":"testNotify","target":"ntfy"}'
```

The reply carries the HTTP code the box saw and a short detail string, so a
failure tells you whether the request went out at all (`httpCode: 0` means it did
not).

### 1.5 Subscribe everyone else

The same topic works on any number of devices. Install ntfy on each relative's
phone, subscribe to the same string, done. There is no per-device registration on
the box, which also means the box has no idea who is listening — a feature when
one of the relatives changes phone.

### 1.6 Self-hosting, and the bearer token

If you would rather the messages did not pass through a public server, ntfy is a
single Go binary and runs happily on anything, including the box's own network.

- Set **ntfy server** to your instance, e.g. `https://ntfy.example.org` or
  `http://192.168.1.10` on the LAN.
- Configure access control on the server (`auth-file`, `auth-default-access deny-all`),
  create a token for the box, and put it in **ntfy token**. The firmware sends it
  as `Authorization: Bearer <token>`.
- The token reads back from `GET /api/settings` as `"__set__"`, never as the real
  value, so the GUI can round-trip the form without the secret crossing the
  network again. Posting `"__set__"` back keeps it; posting `""` clears it.

One iOS-specific catch: the ntfy iOS app receives instant push through Apple's
APNs, and that path goes via ntfy.sh. A self-hosted server needs
`upstream-base-url` configured so it forwards to ntfy.sh for the APNs leg, or the
iOS app only sees messages when it is in the foreground. The Android app has no
such constraint. This is an ntfy architecture detail, not something the firmware
can work around.

Plain `http://` to a LAN server is also the fallback when TLS is the problem — see
troubleshooting.

---

## 2. Pushover

Worth it if you want per-device control, priority escalation and
acknowledgement-required alerts. It costs a one-off licence per platform after the
trial.

1. Register at pushover.net and install the iOS app. The **user key** is on the
   dashboard.
2. Create an application ("EasyPill") to get an **API token**.
3. GUI → Settings → Notifications: enable Pushover, paste the token and the user
   key. Both read back as `"__set__"`.
4. Press Test.

```bash
curl -sX POST http://easypill.local/api/action -H 'content-type: application/json' -d '{"action":"testNotify","target":"pushover"}'
```

The user key can be a group key, which is the clean way to notify several
relatives without maintaining a list on the box.

---

## 3. Generic webhook

For anything that accepts a JSON POST: Home Assistant, n8n, Node-RED, a script on
a Pi. Set **webhook URL** and enable it.

The box POSTs its own shape and does not try to match anyone else's. Twelve keys,
always all twelve, always these types:

| Key | Type | What |
|---|---|---|
| `device` | string | `ui.deviceName`, e.g. `EasyPill` |
| `patient` | string | `ui.patientName`, `""` if unset |
| `title` | string | the push title, as the phone shows it |
| `message` | string | the sentence, as the phone shows it |
| `event` | string | the event's wire name, exactly as in [API.md](API.md), and `"unknown"` for a push with no event behind it: a test push, the daily summary |
| `lid` | int | 0–6, and **−1** when the push is not about one compartment |
| `lidName` | string | that lid's name, `""` when `lid` is −1 |
| `localEpoch` | int | local epoch seconds, `0` if the clock was never verified |
| `priority` | int | 1–5, on ntfy's scale |
| `tags` | array of strings | ntfy emoji short names, e.g. `["pill","x"]` |
| `source` | string | `"easypill"` from the box, `"easypill-sim"` from the simulator |
| `fw` | string | firmware version |

```json
{
  "device": "EasyPill",
  "patient": "Grandma",
  "title": "EasyPill",
  "message": "The 08:00 Monday dose was never taken.",
  "event": "doseMissed",
  "lid": 0,
  "lidName": "Monday",
  "localEpoch": 1761992100,
  "priority": 4,
  "tags": ["pill", "x"],
  "source": "easypill",
  "fw": "0.1.0"
}
```

`source` is the only field that distinguishes the simulator from the box, which
is the point: an automation built against the simulator on a desk keeps firing
once the board is flashed.

`priority` and `tags` come from one table, `priorityFor()` and `tagsFor()` in
`firmware/src/app/notifier.cpp`, which `NOTIFY_RULES` in the simulator copies
exactly:

| event | priority | tags |
|---|---|---|
| `batteryCritical` | 5 | `rotating_light`, `battery` |
| `doseMissed` | 4 | `pill`, `x` |
| `doseDue` | 4 | `alarm_clock`, `pill` |
| `unexpectedOpen` | 4 | `warning`, `door` |
| `wrongDayOpen` | 4 | `warning`, `calendar` |
| `batteryLow` | 4 | `battery` |
| `doseTaken` | 3 | `pill`, `white_check_mark` |
| `doseReminder` | 3 | `alarm_clock`, `pill` |
| `lidOpened` | 3 | `door` |
| `boot` | 2 | `arrows_counterclockwise` |

`batteryCritical` ignores both its per-event toggle and quiet hours. By then the
box is minutes from switching itself off, and a silenced warning is the one
nobody can act on.

Still prefer `event` over `priority` when you key an automation: `event` is the
contract, the other two are presentation and may be retuned.

### Home Assistant

The cleanest of the three, because HA can then notify every family device with
one automation.

1. Add an automation with a **Webhook** trigger. HA gives you a URL of the form
   `http://homeassistant.local:8123/api/webhook/<long-random-id>`.
2. Put that URL in the webhook field. The random id is the secret; treat it the
   way you treated the ntfy topic.
3. Action: `notify.mobile_app_<device>` with the message pulled out of
   `trigger.json`.

```bash
curl -sX POST http://easypill.local/api/action -H 'content-type: application/json' -d '{"action":"testNotify","target":"webhook"}'
```

### Discord and Slack

Both accept a webhook POST, but both insist on their own field names: Discord
wants `content`, Slack wants `text`. The box sends its own shape, so a raw post
may be rejected with a 400. Two options, both fine:

- Point the webhook at Home Assistant, n8n or a four-line script and reshape there.
- Accept that you will see the message only if the implementation happens to
  include a compatible field.

If you need this to work today and have nothing to relay through, use ntfy. It
takes four minutes and does not care about field names.

---

## 4. What gets sent, and what controls it

Each switch is under `notify` in `GET /api/settings`
([API.md](API.md)) and has a checkbox in the GUI.

| Setting | Fires on | Why you might want it |
|---|---|---|
| `onLidOpen` | `lidOpened` | Every lid, every time. Honest but chatty: seven lids times several doses a day. **On** by default, which means a factory box sends two messages for one normal open — see below. |
| `onDoseTaken` | `doseTaken` — an open matched to a scheduled dose as `taken`, `late` or `early` | The reassuring one. "Monday 08:12, 12 min late." |
| `onDoseMissed` | `doseMissed` — swept after the window closed plus `missedGraceMin` | **The one that matters.** Leave this on. |
| `onDoseDue` | `doseDue`, and `doseReminder` if `remindBeforeMin` is non-zero | A heads-up before the dose. Of limited use to a relative; the person at the box gets the buzzer and the LED instead (`power.buzzerOnReminder`, `power.ledOnReminder`). |
| `onUnexpectedOpen` | `unexpectedOpen` (a lid opened nowhere near any dose) and `wrongDayOpen` (right time, wrong compartment) | The "right time, wrong lid" signal. Worth having on. |
| `onLowBattery` | `batteryLow` at `power.lowBatteryMv` | The warning, while there is still time to do something about it. |
| `onBoot` | `boot` | Useful while you are still reworking the board, noise afterwards. |
| `onDailySummary` | once a day at `dailySummaryMinute` (minute of the local day; `1260` = 21:00) | How the whole day went in one message. The civilised option if the per-event pushes are too much. |

One open can earn more than one push, and the combinations are worth knowing
before you turn things on:

- A normal, matched open fires `lidOpened` *and* `doseTaken`. Both switches are
  on by default, so a factory box sends two messages per dose. Turn `onLidOpen`
  off for one message per open.
- `wrongDayOpen` **replaces** `doseTaken` rather than arriving beside it: the
  dose record still holds the open, but nobody is told the dose was taken.
- An open with no match fires `lidOpened` and `unexpectedOpen`.
- Before the first clock sync, `lidOpened` is the *only* message an open can
  produce. Nothing is matched against a clock the box does not trust.

`batteryCritical`, at `power.criticalBatteryMv`, is the one push with **no
switch and no gate**. It goes out whatever `onLowBattery` says and whatever quiet
hours say, because the firmware drops the latch immediately afterwards to protect
the cell: it is the last thing the box says before it goes quiet, and there is no
later chance to say it.

Events in the log that have **no** push switch at all: `lidClosed`,
`buttonPress`, `buttonHold`, `chargerIn`, `chargerOut`, `timeSynced`, `wifiUp`,
`wifiDown`, `sleepEnter`, `sleepExit`, `configChanged`, `sensorFault`. They show
up in the GUI's activity feed and in `GET /api/events`. `sensorFault` in
particular — the channel-7 ground reference failing, meaning no hall reading can
be trusted — is only visible there, so it is worth glancing at the feed
occasionally or leaving the daily summary on.

### Quiet hours

`quietFromMinute` and `quietToMinute` are minute-of-day values. Equal values
switch quiet hours off, which is the default.

Every push goes through the same gate, with the single exception above, so quiet
hours suppress **everything** else — a missed 23:00 dose included. A suppressed
push is dropped, not deferred. If you want to be woken by a missed night dose,
leave quiet hours off or keep the window narrow.

### A reasonable starting set

```text
onDoseMissed      on
onUnexpectedOpen  on
onLowBattery      on
onDailySummary    on   at 21:00
onDoseTaken       on   for the first fortnight, then decide
onLidOpen         off
onDoseDue         off
onBoot            off
quiet hours       off
```

---

## 5. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Test reports `httpCode: 0` | The request never left. No network, no target enabled, the box is in setup-AP mode with nowhere to send, or quiet hours are active. | Check `wifi.ip` in the GUI footer, check the enable checkbox, check `wifi.mode` is `sta`, check quiet hours. |
| Test fails, but the same URL works from `curl` | The box allows 3 s to connect (TLS handshake included) and 2 s to read the reply, which is tighter than any desktop client. | Confirm against a `curl` run with the same budget. If a healthy server really is slower than that, raise `kHttpConnectMs` in `notifier.cpp`. |
| `401` or `403` | Protected topic with no token, or the wrong token. | Fill in the ntfy token field, or set `auth-default-access` to allow write on that topic. |
| `404` | Server URL has a path on it, or a typo. | `https://ntfy.sh` with no trailing path. The topic goes in its own field. |
| `200` but nothing on the iPad | Not subscribed to that exact topic, notifications not allowed for the app, or Focus/Do-Not-Disturb. | Re-check the topic character by character; it is case-sensitive. Re-run the `curl` from 1.3. |
| TLS handshake failure in the log | Certificate chain or heap. The ESP32 needs ~40 kB free for a TLS handshake. | Try `http://` to a LAN ntfy instance to confirm it is TLS and not the network. Reduce what else is running at the same time. |
| Works on the bench supply, silent on battery | The battery protection trips at about 30 mA and Wi-Fi TX is well past that, so the board disconnects itself ~10 ms into the transmit. | Rework step 3 in [BRINGUP.md](BRINGUP.md). Nothing in firmware fixes this. |
| Notifications arrive in a batch, minutes late | The box was asleep. It wakes on the lid, connects, sends, sleeps. Connection plus DNS plus TLS is a few seconds; a timer-wake backlog can be a quarter of an hour. | Expected on `smart` or `aggressive`. Set `power.policy` to `alwaysOn` while you are testing. |
| The message names no time at all, and the webhook's `localEpoch` is `0` | The clock has not been verified against NTP yet, so the firmware refuses to invent a wall-clock time. | Wait for Wi-Fi and the first SNTP sync, or press Sync time in the GUI. See [DESIGN.md](DESIGN.md#timekeeping-without-an-rtc). |
| Everything works, then stops after a few days | Topic unchanged but the ntfy app was reinstalled, or the Wi-Fi password changed. | Re-subscribe; check `wifi.mode` is `sta` and not `ap`. |

---

## 6. Why this is push and not poll

The GUI is the nice part of this project and it is also the part you cannot rely
on. Here is the sequence on battery:

1. A lid opens. `WAKE_UP` goes high — it is a hardware OR of all seven halls, the
   button and the charger — and the ESP32 comes out of deep sleep.
2. It scans the hall mux to find out *which* lid, matches the open against the
   schedule, and writes a dose record.
3. It brings up Wi-Fi, syncs the clock if a sync is due, sends whatever
   notifications the event earned, and stays awake for `awakeAfterEventSec`
   (300 s by default) in case somebody opens the GUI. The pushes are queued, not
   sent from the handler: one transport per pass of the loop, nothing started
   within five seconds of a lid moving, and three minutes before an undeliverable
   message is given up on. A TLS handshake is time in which no hall sensor is
   being read, so the sending is paced around the sensing rather than the other
   way round.
4. It goes back to sleep until the next `wakeIntervalSec` timer or the next event.

For most of the day the box is in deep sleep with the radio off. It has no IP
address, answers no pings and serves no pages. Anything that wants to *ask* the
box a question has to catch it inside that 300-second window, which is not a
design you can build a family notification system on.

So the box is the one that initiates. It already knows when something happened —
it was woken up by it — and a single HTTPS POST costs a few hundred milliseconds
of radio time. There is no polling interval to tune, nothing needs a port
forward, nothing needs the box to have a stable address, and it works when
everyone is on mobile data in a different country.

The consequences are worth being clear about:

- **The GUI is for setup and for review, not for monitoring.** Reach it while the
  box is awake: on USB, with `power.policy` set to `alwaysOn`, within
  `awakeAfterEventSec` of an event, or after pressing the button.
- `POST /api/action {"action":"stayAwake","seconds":600}` holds it awake while you
  are working on it, and the GUI calls that for you as long as a tab is open and
  polling.
- The event log and the dose records are persisted to LittleFS, so nothing is lost
  while it sleeps. You see the history when you next reach it. You see the events
  as they happen through the notifications.
- If notifications are not configured, the box still works and still records
  everything — you will not find out about a missed dose until you go and
  look. Which is the failure mode this whole project exists to avoid.
