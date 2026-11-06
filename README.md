# EasyPill V1

A seven-lid pillbox with a Seeed XIAO ESP32-C3 (or S3) hidden inside it. Each lid
carries a magnet over an AH1912 hall switch, so the box knows when a lid was
lifted. To the person using it the box has no screen, no buttons and no menus;
everything useful happens on the relatives' side, as push notifications and a web
GUI served by the ESP32 itself.

- [docs/HARDWARE.md](docs/HARDWARE.md) — the authoritative hardware facts, read off the schematic. Source of truth.
- [docs/API.md](docs/API.md) — the HTTP contract shared by the firmware, the simulator and the GUI.
- [docs/BRINGUP.md](docs/BRINGUP.md) — the bench procedure, in order, with the numbers to write down.
- [docs/NOTIFICATIONS.md](docs/NOTIFICATIONS.md) — getting a push onto an iPad, shortest path first.
- [docs/DESIGN.md](docs/DESIGN.md) — why the firmware is shaped the way it is.

---

## Status: running on the first board, partly proven

Running on the first XIAO ESP32-C3 on the V1 PCB since **2026-11-02**. Plenty
of it is still prediction, which is what the two lists below keep apart.

**Measured on the board:**

- Boots, joins Wi-Fi with build-time credentials, brings up mDNS, and syncs over
  NTP — the clock agreed with a laptop to within 0.5 s.
- All seven AH1912 hall sensors respond, each proved individually with a magnet
  rather than inferred from an idle level. Channel 7's ground reference reads
  LOW, so the mux and the address lines are alive.
- Lid opens and closes are debounced, mapped to weekdays and logged, and dose
  records are written with the right late/missed classification.
- The GUI is served gzipped out of flash and renders in a browser.
- **Battery operation, under radio load**: 241 HTTP replies with no failures and
  no reset, finishing with a full active Wi-Fi scan, and the cell sagged 5 mV.
  See [docs/HARDWARE.md](docs/HARDWARE.md) §9 — but note this board has had its
  battery protection *disabled*; a stock board still cannot do this.
- Settings survive a reboot. They did not until 2026-11-02 — see
  [docs/DESIGN.md](docs/DESIGN.md), "Why the settings live in a file, not in
  NVS", for a bug that silently discarded every setting the GUI ever wrote.

**Still not measured, and still a prediction:**

- The LED and the buzzer. No colour has been seen, no tone has been heard.
- Standby and sleep current on a meter, so the sleep policy and the battery-life
  arithmetic remain untested. Bench testing has mostly held the box awake.
- A notification actually arriving on a phone.
- A board with the battery protection *working*, which is what any second unit
  needs rather than the disable-it rework this one got.

`easypill_core` passes 556 host assertions (`tests/run_core_tests.sh`) and the
firmware and simulator agree on 10,246 scheduler answers
(`tests/parity/run_parity.sh`). That covers the scheduler, the adherence model
and the API, and it covers nothing electrical.
[docs/BRINGUP.md](docs/BRINGUP.md) is still the procedure for turning the
remaining predictions into measurements.

---

## What it can and cannot know

Seven lids, one magnet each, one sensor each. That is the whole sensor budget, and
it bounds everything else.

**It knows:** that lid *n* went from closed to open at a particular moment, and
when it closed again. Nothing else.

**It cannot know:** which pill came out, how many came out, whether anything came
out at all, or whether what came out was swallowed. A lid lifted and shut with
nothing taken looks exactly like a dose taken correctly. There is no way around
this with one magnet per lid.

What makes it useful anyway is that three of the four possible outcomes are still
legible:

| Signal | What the box sees | What it means |
|---|---|---|
| Right day, right time | Today's lid opened inside its dose window | Almost certainly fine. Logged `taken`. |
| Right time, wrong lid | A lid opened near a dose time, but not today's lid | Wrong compartment, or the tray is rotated. Worth a notification. |
| Nobody opened it | No open before the window closed plus grace | The one that actually matters. Logged `missed`. |
| Opened at an odd hour | A lid opened nowhere near any dose | Refilling, curiosity, or confusion. Logged `unexpectedOpen`. |

The box never claims a pill was taken. It reports that a lid was opened and lets
the adherence model attach that open to a scheduled dose. See
[docs/DESIGN.md](docs/DESIGN.md#the-adherence-model) for what each state means.

---

## Build targets

Four environments, defined in [firmware/platformio.ini](firmware/platformio.ini).
Build from inside `firmware/`.

| Env | What it is |
|---|---|
| `bringup-c3` | Interactive serial test harness for the C3. Flash this first. |
| `bringup-s3` | Same, for the S3. |
| `app-c3` | The real firmware for the C3: scheduler, web GUI, notifications. |
| `app-s3` | Same, for the S3. |

```bash
cd firmware
pio run -e bringup-c3 -t upload      # the test harness goes on first
pio device monitor -e bringup-c3     # drive it over USB-CDC
pio run -e app-c3 -t upload          # then the real firmware
```

Swap `-c3` for `-s3` throughout on an S3.

There is no `uploadfs` step and no filesystem image to flash. The GUI is baked
into the firmware by `tools/gen_web_assets.py` as gzipped PROGMEM blobs, so a
plain `upload` carries any change under `web/` with it. LittleFS on the board
holds only the event log and the dose records, and since there is no
`firmware/data/`, running `uploadfs` would overwrite that history with nothing.

### Which module do you have?

- The **S3** module has a metal shield can over it. The **C3** does not.
- The upload log prints `Chip is ESP32-C3 (revN)` or `Chip is ESP32-S3 (revN)`.
- The C3 has 4 MB of flash and uses the `min_spiffs` partition table; the S3 has
  8 MB and uses the stock table.

Flashing a C3 build onto an S3 fails at the `board_pins.h` guard rather than
silently mis-driving pins, so a mix-up costs a compile error, not hardware.

### Bench supply, not battery

Run every bring-up step from a **bench supply on TP2/TP5**, current limit around
500 mA. The battery protection on this revision (U11, the AH variant, with
Q5/Q6 at roughly 2.4 Ω in series) trips at about **30 mA**, which is less than a
Wi-Fi transmit burst draws. On a battery the board will disconnect itself roughly
10 ms into the first TX. TP2/TP5 bypasses Q5/Q6 entirely.

With that protection **disabled**, Wi-Fi on battery has been measured and is
solid — 241 requests, no failures, no reset, 5 mV of sag, full scan included
(`tools/battery_wifi.py`, 2026-11-02; see [HARDWARE.md §10](docs/HARDWARE.md)).
That is the rework succeeding, not the trip being imaginary, so bring-up still
belongs on the bench supply.

Do not try to characterise Wi-Fi, notifications or sleep current on a battery
until the protection rework in
[docs/HARDWARE.md §10](docs/HARDWARE.md) is in. Everything else the bring-up
procedure asks for can be done on the bench supply.

---

## Quick start

1. **Rework the board** in the order given in [docs/HARDWARE.md §10](docs/HARDWARE.md).
   At minimum: disable U9, bodge `VINA`→`VIN`. Do not skip to step 2 before this.
2. **Bring-up firmware.** `pio run -e bringup-c3 -t upload`, then open the
   monitor and press `?`. Work through
   [docs/BRINGUP.md](docs/BRINGUP.md) and fill in its table. You end up with the
   hall-to-lid map, the hall polarity, the LED channel colours, the buzzer drive
   mode and the divider numbers.
3. **App firmware.** `pio run -e app-c3 -t upload`. The GUI goes with it.
4. **Wi-Fi.** Easiest is to put the network in the build before you flash:

   ```bash
   cp firmware/include/wifi_default.example.h firmware/include/wifi_default.h
   ```

   Fill in the SSID and password and the board joins on first boot, no AP
   involved. That file holds a password in clear text and is in `.gitignore`;
   the `.example.h` beside it is the tracked template. Leave it out and nothing
   changes from how this used to work.

   Otherwise, with no credentials stored — or after three failed joins — the box
   raises an access point called `EasyPill-setup` and holds it up for five
   minutes at a time, refreshed while a phone is connected to it. Join it, open
   `http://192.168.4.1`, pick a network. Be warned that phones and laptops tend
   to leave a network with no internet on it within seconds, which is what makes
   the baked-in route worth the bother.

   Either way the box then lives at `http://easypill.local` (or whatever you set
   as the hostname) on your own network.

   A network provisioned through the GUI always beats the built-in one, and the
   built-in pair is never written to NVS — so editing `wifi_default.h` and
   reflashing really does change which network the box joins, instead of being
   shadowed by a stale blob that a plain flash does not erase.
5. **GUI.** Enter the values from the bring-up table under Settings → Hardware,
   set the schedule, name the lids.
6. **Notifications.** Follow [docs/NOTIFICATIONS.md](docs/NOTIFICATIONS.md).
   ntfy with an unguessable topic is about four minutes of work. Press the test
   button before you trust it.

Step 6 is the one that makes the box worth building. The GUI is only reachable
while the box is awake; the notifications work regardless.

---

## Repository map

```
docs/            HARDWARE.md and API.md are the contracts; the rest explains them
hardware/
  schematics/    the V1 schematic and its review: the origin of every constraint
firmware/
  platformio.ini four envs: bringup-{c3,s3}, app-{c3,s3}
  include/
    board_pins.h  pin map, mux channel map, divider ratios
  lib/
    easypill_core/  pure C++17: settings, schedule, adherence, time maths, events.
                    No Arduino.h anywhere in here, which is what makes it testable
    easypill_hw/    board drivers: MuxBus, HallScanner, AnalogSense, Indicator,
                    PowerLatch
    easypill_json/  ArduinoJson <-> core conversions, kept out of core on purpose
  src/
    bringup/        the serial test harness
    app/            app_state, store, net_wifi, timekeeper, notifier, web_server
  tools/
    gen_web_assets.py  gzips web/ into a header before the app builds
sim/             easypill_sim.py: the same HTTP API, no hardware
tests/           run_core_tests.sh: host unit tests for easypill_core
tools/
  check.sh       everything that can be verified without a board, in one command
web/             the single-page GUI, served gzipped from flash
```

`easypill_core` is the part worth protecting. It has no Arduino dependency, no
heap allocation and no clock of its own — everything goes in as a local epoch
integer. That is why the schedule and adherence logic can be tested on a laptop.

---

## Running it without hardware

The host simulator implements the whole of
[docs/API.md](docs/API.md) including `POST /api/sim`, which fakes lid opens,
button presses, battery voltage, charger presence and the passage of time. It
serves the real GUI from `web/`, so the GUI runs against the same contract the
firmware implements.

```bash
python3 sim/easypill_sim.py --port 8080
```

Then open `http://127.0.0.1:8080`. To open Monday's lid:

```bash
curl -sX POST localhost:8080/api/sim -H 'content-type: application/json' -d '{"openLid":0}'
```

To run a simulated day in a minute:

```bash
curl -sX POST localhost:8080/api/sim -H 'content-type: application/json' -d '{"timeScale":1440}'
```

The core unit tests build with a host compiler and need neither PlatformIO nor a
board:

```bash
tests/run_core_tests.sh
```

It prints `556 checks, 0 failures`. `tools/check.sh core` runs the same thing,
and `tools/check.sh` on its own also builds all four firmware targets and
regenerates the embedded web assets.

The contract those two honour: the simulator answers every endpoint in
`docs/API.md` on a local port with `/api/sim` enabled (the firmware 404s that
route), and `run_core_tests.sh` compiles `firmware/lib/easypill_core` against a
C++17 host toolchain and exits non-zero on the first failure. If you change
`docs/API.md`, three implementations have to change with it: the firmware, the
simulator and the GUI.

The third check is there for exactly that:

```bash
tools/check.sh parity
```

It asks `easypill_core` and `easypill_sim.py` the same 10,246 scheduler
questions — which lid owns a day, which dose an open belongs to, when a dose
counts as missed, what the boot floor hides — and diffs the two reports. It
prints `10246 answers, firmware and simulator agree`, or the lines where they
do not.

The simulator is how the box gets demonstrated and tuned with nothing on the
desk, so a divergence does not just produce a wrong number: it teaches you
something false about your own device.
Of the 87 defects the first review turned up, 28 were exactly that, and nothing
else in the tree notices when the two drift apart. Adding a question means
adding it to both `tests/parity/parity_driver.cpp` and
`tests/parity/parity_py.py`; the diff is the assertion, so there is no expected
output to keep up to date.

---

## Licence and provenance

No licence yet, so it is all rights reserved by default: readable, not
reusable. Needs settling before this goes public.

Single-unit personal project; the hardware notes in `docs/HARDWARE.md` come from
`hardware/schematics/Schematic_EaSyPill_V1_2026-09-27.pdf` and the schematic
review alongside it. Nothing here has been through EMC or any kind of medical
assessment, and it should not be the only thing standing between somebody and
their medication.
