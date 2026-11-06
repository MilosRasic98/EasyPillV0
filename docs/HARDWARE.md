# EasyPill V1 — hardware facts for firmware

Everything in this file was read off
`hardware/schematics/Schematic_EaSyPill_V1_2026-09-27.pdf` (7 sheets) and
cross-checked against `hardware/schematics/schematic-review.md`.
**This file is the single source of truth for the firmware.** If the board is
reworked, change this file first, then `firmware/include/board_pins.h`.

---

## 1. The one thing that shapes the whole firmware

`MUX_A`, `MUX_B`, `MUX_C` are **shared by three XS3A4051 muxes at once**:

| Mux | Role | Common pin (Z) | Direction |
|---|---|---|---|
| U20 (sheet 3) | 7 hall sensors + GND reference | `HALL_IN` → D3 | input, digital |
| U4  (sheet 5) | battery / USB / coil / button / charger-status | `SIG_IN` → D2 | input, analog |
| U5  (sheet 6) | RGB LED FET gates + buzzer FET gate | `SIG_OUT` → D7 | output, digital |

So **one address selects a channel on all three muxes simultaneously**. You cannot
read a hall sensor and hold the LED on at the same time. Every hall read, every
ADC read and every output drive has to time-share one set of address lines.
The firmware funnels all of it through a single owner (`MuxBus`).

Consequences the firmware must honour:

1. **Drive `SIG_OUT` LOW before changing the address.** Otherwise the address
   sweep walks a logic high across Q1…Q4 and flashes the LED / chirps the buzzer.
   While LEDC is driving the pin for a tone, that means dropping the duty **and
   waiting out one PWM period**: `ledcWrite()` only queues a duty and the pad
   keeps driving the tail of the old pulse until the next cycle boundary.
2. **Park on channel 7 when idle.** On the hall mux ch7 is just a 100 k resistor
   to GND (R38); on the output mux ch7 is unconnected. Nothing can be energised.
   **Deep sleep is the one exception** — park on channel 0 instead, see §7.
3. **`WAKE_UP` is the escape hatch.** It is a hardware OR of every event source,
   so even while the mux is busy driving the LED you can still see that
   *something* happened, then scan to find out *what* once the output burst ends.
4. **One owner, and it announces itself.** Everything goes through `MuxBus`, and
   the indicator — the only thing that needs to *hold* an address rather than
   take it and give it back — publishes `holdsBus()`. Nothing else touches the
   address lines while that is true. A hold is bounded (one 2 ms colour slot, or
   a beep on-phase cut into 250 ms slices), so the hall scan can never be starved
   of the bus; the cost is up to 250 ms of added lid-detection latency during a
   long test chirp, which `WAKE_UP` covers anyway.

### Channel map

| Ch | U20 → `HALL_IN` (digital in) | U4 → `SIG_IN` (analog in) | U5 ← `SIG_OUT` (digital out) |
|---|---|---|---|
| 0 | Hall 1 | VBAT × 0.5714 | Q3 → CTL1 pin 4 |
| 1 | Hall 2 | 5V_USB × 0.4286 | Q2 → CTL1 pin 3 |
| 2 | Hall 3 | 5V_COIL × 0.4286 | Q1 → CTL1 pin 2 |
| 3 | Hall 4 | BUTTON_SIG (raw VBAT, clamped) | Q4 → buzzer (U6) |
| 4 | Hall 5 | MCP_CHRG / STAT (clamped) | — |
| 5 | Hall 6 | — | — |
| 6 | Hall 7 | — | — |
| 7 | 100 k to GND (R38) — **park here** | — | — (unconnected) |

XS3A4051 select pins: `S1 = A` (LSB, pin 11), `S2 = B` (pin 10), `S3 = C` (MSB, pin 9).
Address `n` = `A + 2·B + 4·C`. `nE` (pin 6) is tied active on all three.

---

## 2. Pin map

`D0…D10` are the XIAO silkscreen pins. The firmware uses the Arduino `Dn`
macros so the same source builds for both modules.

**The module fitted to the V1 PCB is a XIAO ESP32-C3**, so the C3 column is the
live one and `bringup-c3` / `app-c3` are the envs to flash. The S3 column is
kept current because the footprint takes either, not because anything here has
been run on one.

| Net | XIAO pin | ESP32-C3 GPIO | ESP32-S3 GPIO | Mode |
|---|---|---|---|---|
| `WAKE_UP` | D1 | GPIO3 | GPIO2 | input, deep-sleep wake capable on both |
| `SIG_IN` | D2 | GPIO4 (ADC1_CH4) | GPIO3 (ADC1_CH2) | analog in |
| `HALL_IN` | D3 | GPIO5 | GPIO4 | digital in |
| `MUX_A` | D4 | GPIO6 | GPIO5 | digital out |
| `MUX_B` | D5 | GPIO7 | GPIO6 | digital out |
| `MUX_C` | D6 | **GPIO21 = U0TXD** | **GPIO43 = U0TXD** | digital out |
| `SIG_OUT` | D7 | **GPIO20 = U0RXD** | **GPIO44 = U0RXD** | digital out |
| `POWER_LATCH` | D10 | GPIO10 (digital pad) | GPIO9 (RTC pad) | digital out |

`D0`, `D8`, `D9` are unused. `R13/R14/R15` are 100 k pull-downs on `MUX_A/B/C`,
so the address is defined before the firmware initialises.

### `MUX_C` and `SIG_OUT` sit on UART0 — never open `Serial0`

`MUX_C` is `U0TXD` and `SIG_OUT` is `U0RXD`. All logging goes over **USB-CDC**.
Both board JSONs already set `ARDUINO_USB_MODE=1` and
`ARDUINO_USB_CDC_ON_BOOT=1`, so plain `Serial` *is* USB-CDC. Do not call
`Serial0.begin()`, do not use `Serial1` on these pins.

The ROM bootloader still prints its banner on `U0TXD` at every reset, which
wiggles `MUX_C`. That is harmless: during boot `SIG_OUT` is an undriven input and
the four FET gates are held low by their own 100 k pull-downs, so no output can
turn on, and a jittering hall address just means a few meaningless reads.

---

## 3. Hall sensors — polarity and the lid model

Seven AH1912 omnipolar hall switches, push-pull output, one per lid.

- **Magnet present → output LOW.**
- **Magnet absent → output HIGH.**

The intended mechanical arrangement is magnet-on-lid sitting over the sensor
when the lid is **closed**, so:

| Lid | Hall output | `WAKE_UP` contribution |
|---|---|---|
| closed | LOW | 0 |
| open | HIGH | 1 |

**Idle therefore means "every sensor sees a magnet".** If the magnet instead
*approaches* the sensor as the lid opens, every sensor is inverted — so the
firmware keeps `hall_closed_is_low` as a setting plus a per-sensor invert mask,
and the bring-up firmware has a calibration command that works it out for you,
as long as the tray really is shut. A reading that is neither all-low nor
all-high means the sensors disagree, which on an intact tray is a lid left open,
a missing magnet or a dead sensor — not a polarity. Resolving that into an invert
mask would teach the box that a shut lid reads OPEN, and no self-check could ever
catch it, because the mask is derived from the very reading it would check. So
the calibration refuses a mixed reading and writes nothing unless it is forced
(`c force` in bring-up), which is there for a genuinely half-reworked tray: then
the majority sets the global sense and the odd ones out get an invert bit. An
all-high reading needs no force — that is simply a tray wired the other way
round, and `hall_closed_is_low` goes false.

Lid → weekday mapping is a setting too (`hall_to_lid[7]`), because which sensor
ends up under which day depends on how the PCB is oriented in the enclosure.

**Measured on the V0 board, 2026-11-02:** the sensor order runs *opposite* to the
days, so `hall_to_lid = [6, 5, 4, 3, 2, 1, 0]` — sensor 0 is Sunday and sensor 6
is Monday. Each of the seven was proved individually with a magnet, one at a
time, by `tools/mapdays.py`, which watches `rawMask` (sensor-indexed, before any
mapping) and so cannot be fooled by the mapping it is trying to establish. A
clean reversal rather than a scramble is what a board mounted the other way
round looks like, which is the expected shape for this enclosure.

This is the *compiled default* — see `applyDefaults()` in
`firmware/lib/easypill_core/ep_settings.cpp`. Note that it does **not** reach a
board that already has settings stored: `storeLoadSettings()` only falls back to
the defaults when the NVS blob is missing or its `version` changed, so flashing
new firmware will not fix a wrongly-mapped box. Re-run `tools/mapdays.py` (no
reflash, takes effect at once) or factory-reset it. Channel 0..6 → sensor 0..6 in
`board_pins.h` is unaffected; that is a mux fact, this is a mechanical one.

---

## 4. `WAKE_UP` logic (sheet 4)

Two 74LV32A quad-OR packages (U2, U3) OR nine signals into one line:

```
WAKE_UP = Hall1 | Hall2 | Hall3 | Hall4 | Hall5 | Hall6 | Hall7 | BUTTON_SIG | SIG_CHARGE
```

- `BUTTON_SIG` is raw VBAT while the button is held (R31 = 100 k pull-down).
- `SIG_CHARGE` is high while a charger (USB or coil) is present.
- `R1…R6` = 100 k pull-downs on the spare gate inputs.

It is **level**-triggered, which matters for deep sleep:

- Any single open lid, missing magnet, or plugged-in charger holds `WAKE_UP`
  high and masks every other event.
- A level-triggered GPIO wake with the line already high wakes instantly and
  forever. **Before sleeping, read `WAKE_UP`; if it is already high, arm only
  the timer wake, not the GPIO wake.**

---

## 5. Analog channels (sheet 5)

Dividers from R7…R12 (75 k / 100 k):

| Channel | Signal | Top | Bottom | Ratio | Scale ADC mV → real mV |
|---|---|---|---|---|---|
| 0 | VBAT | 75 k | 100 k | 0.5714 | `× 175 / 100` |
| 1 | 5V_USB | 100 k | 75 k | 0.4286 | `× 175 / 75` |
| 2 | 5V_COIL | 100 k | 75 k | 0.4286 | `× 175 / 75` |
| 3 | BUTTON_SIG | — | — | 1.0 (see below) | threshold only |
| 4 | MCP_CHRG (STAT) | — | — | 1.0 (see below) | threshold only |

100 nF sits on each divider node (C3, C6, C7, C8), permanently connected, so the
node is always settled; switching the mux to it needs only microseconds.

**Confirmed on the board, 2026-11-01.** With the board on USB and no cell
fitted, `a` reported channel 1 at `2204 mV` raw → `5145 mV` scaled, which is
VBUS to within a percent. That single reading validates the whole chain at
once: mux addressing, the ADC path, the 11 dB attenuation and the `× 175 / 75`
arithmetic. Channels 3 and 4 behaved exactly as the clamped-channel analysis
below predicts. Channel 0 was confirmed once a cell went on the JST: `4082 mV
(88 %)`, which is a real single-cell reading through the `× 175 / 100` divider,
so all three real measurement channels are now proven.

A 4.2 V battery reads 2.40 V at the ADC. With the default 11 dB attenuation the
usable range is roughly 0–2.5 V, so VBAT sits near the top of the range — use
`analogReadMilliVolts()` (eFuse-calibrated) and take a median of several samples.

### Channels 3 and 4 are clamped, not measured

`BUTTON_SIG` (up to 4.2 V) and `MCP_CHRG` (up to ~4.5 V) both exceed the
XS3A4051's `VCC + 0.5 V` rating. The review's rework adds a series resistor
(33 k at U4 pin 12, 100 k at U4 pin 1) which limits the current into the mux's
ESD diode but does **not** divide the voltage — with no load on the mux side the
pin still sits at the source voltage and clamps to ~3.3 V through that diode.

So treat both as **digital**: read the ADC and compare against a threshold
(default 1500 mV). Charger presence should be taken from channels 1 and 2, which
*are* real measurements.

**Measured, 2026-11-01.** Pressing the button reads `2829 mV` at the pin and
releasing reads `146 mV`. That is the clamp doing exactly what is described
above: the source is VBAT at ~4.1 V, and what reaches the ADC is the clamped
~3 V, not the battery voltage. The two states are nowhere near each other, so
the 1500 mV threshold has enormous margin and the button is entirely reliable
as a digital input — it simply cannot be used to measure anything.

> **Better rework, if you want a real button voltage:** after lifting U4 pin 12,
> fit 33 k in series *and* 100 k from that pin to GND. That makes a proper
> 33 k/100 k divider — 4.2 V → 3.16 V, inside the rating, nothing clamps.
> Then set `button_divider = 133/100` in settings and you get a usable reading.
> `firmware` supports both; the default assumes the review's version.

---

## 6. Outputs (sheet 6)

Four BSS138BKW low-side switches, each with a 330 R gate series resistor and a
100 k gate pull-down, fed from `SIG_OUT` through U5:

| Ch | FET | Load | Series R |
|---|---|---|---|
| 0 | Q3 | CTL1 pin 4 | 470 R |
| 1 | Q2 | CTL1 pin 3 | 470 R |
| 2 | Q1 | CTL1 pin 2 | 470 R |
| 3 | Q4 | buzzer (U6, JST-PH2) | 470 R (R27) |

- `CTL1` is a JST-PH-4; pin 1 is the **common anode** to 3V3, pins 2–4 are the
  three cathodes. **Which colour is on which pin is not knowable from the
  schematic** — it depends on the LED you wire up. `led_channel_red/green/blue`
  are settings; the bring-up firmware lets you identify them in about ten seconds.
- 470 R at 3.3 V gives roughly 3 mA of red but only 0.5–1.5 mA of green/blue, so
  expect a red-biased white. Swapping to ~220 R red / 68–100 R green-blue is on
  the review's improvement list.
- The buzzer type is unknown. A self-oscillating piezo just needs DC; a magnetic
  buzzer needs a square wave near its resonance. The firmware supports both
  (`buzzer_self_drive`, `buzzer_tone_hz`, default 2700 Hz) and generates the tone
  with LEDC on `SIG_OUT` while it holds the address on channel 3. Leaving that
  channel obeys the PWM-period rule in §1, or the tail of the last pulse lands
  on whichever gate the new address selects.
- 470 R in series leaves only ~5–7 mA for the buzzer; R27 → 0–47 R makes it loud.

### Mixing LED colours

Because the address lines are shared, a mixed colour means time-multiplexing
channels 0–2. The firmware keeps the LED off during normal operation (it is
meant to be a dumb-looking box) and only takes the mux for short indication
bursts, interleaving hall scans between colour slots. A slot is 2 ms, so three
channels make a cycle of roughly 160 Hz — fast enough that a camera at 1/50 s
integrates three full cycles and shows no rolling-shutter banding. A single
non-zero channel needs no multiplexing at all: it is selected once and left
driving, which is also why a solid colour survives a hall scan unflickered and a
mixed one dims very slightly during it.

---

## 7. Power, latch and sleep

- `POWER_LATCH` (D10) holds the soft-latch on. **Assert it HIGH as the very
  first thing in `setup()`** — the user has to hold the button until firmware
  takes over, and power-off happens when the button is *released*.
- With USB plugged in, the board cannot turn off, and the XIAO's own regulator
  drives 3V3 whenever USB is in — so the ESP32 runs whenever USB is in.
- Holding the latch through deep sleep:
  - **C3:** GPIO10 is a plain digital pad → needs `gpio_hold_en(GPIO_NUM_10)`
    **and** `gpio_deep_sleep_hold_en()`.
  - **S3:** GPIO9 is an RTC pad → `gpio_hold_en(GPIO_NUM_9)` is enough.
  - The firmware calls both on both targets; the extra call is harmless.
  - Set the hold immediately after latching at boot and confirm it survives a
    panic/WDT reset.
  - `gpio_deep_sleep_hold_en()` is a **single global bit**: it freezes every
    digital pad at the level it is driving, not just `POWER_LATCH`. Parked on
    channel 7, `MUX_A/B/C` are three highs into the 100 k pull-downs R13/R14/R15
    for the whole sleep — 3 × 33 µA, about **99 µA**, which is more than the chip
    draws asleep. So the last mux access before `esp_deep_sleep_start()` parks
    the address on **channel 0**, where all three lines are low and cost nothing,
    and which is also what the pull-downs give at reset. `SIG_OUT` is driven low
    and frozen low either way, so no FET gate can be energised during the sleep.
- **Drop the latch at ~3.3 V** to protect the cell. The on-board AP9101C
  over-discharge trip is at 2.5 V, which is far too low for a LiPo that still
  has to run Wi-Fi.
- Deep-sleep wake sources:
  - C3: `esp_deep_sleep_enable_gpio_wakeup(BIT(3), ESP_GPIO_WAKEUP_GPIO_HIGH)`
  - S3: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1)`
  - plus `esp_sleep_enable_timer_wakeup()` on both.

---

## 8. There is no RTC

Timekeeping is entirely software:

- System time **does** survive deep sleep (the RTC timer keeps running), so the
  clock only has to be set once per power cycle.
- `RTC_SLOW_CLK` defaults to the internal RC oscillator, roughly ±3–5 %, i.e.
  **up to a couple of minutes of drift per day**. For dose reminders that is too
  much on its own.
- Mitigation, in order:
  1. Re-sync SNTP on every wake where Wi-Fi comes up, and at least every 6 h.
  2. **Measure the drift and correct for it.** The firmware compares elapsed RTC
     seconds against elapsed NTP seconds between syncs, derives a ppm
     correction, stores it in NVS and applies it to sleep durations and to the
     clock between syncs. Two syncs a day are enough to get drift under a few
     seconds.
  3. Persist the last known epoch to NVS so a cold boot without Wi-Fi starts
     from an approximately-right time, flagged as unverified.
- **Measured on the board, 2026-11-01:** `RTC slow clock: internal 136 kHz RC,
  136000 Hz`. There is no 32.768 kHz crystal in play, so drift is percent-level
  and temperature dependent, and the measured-ppm correction above is carrying
  the entire feature. Two syncs a day is a floor, not a target. The bring-up
  `i` command reprints this, so re-check it if the module is ever swapped.
  An external crystal would bring drift to tens of ppm, but on the C3 it sits
  on GPIO0/GPIO1 — which the XIAO does not break out — and selecting it needs
  `CONFIG_RTC_CLK_SRC_EXT_CRYS`, i.e. a custom `sdkconfig` and therefore an
  ESP-IDF build rather than Arduino. Worth the port only if drift turns out to
  matter in practice.
- Until the clock has been verified against NTP at least once, the firmware
  **records lid events with a monotonic timestamp and marks them
  `time_unverified`**, then back-fills real timestamps after the first sync. It
  never silently invents a wall-clock time.
- Because the clock does not survive a power cycle, neither does the box's claim
  to have been watching. The firmware keeps a **boot floor**: the local epoch of
  the most recent cold boot, named at the first verified sync, and doses
  scheduled before it are not reported missed. It lives in RTC slow memory
  (`RTC_DATA_ATTR`) precisely because that is carried through a deep-sleep wake
  and reloaded from the image on anything else — a dose that falls while the box
  sleeps is still a real miss, because the box slept intending to wake for it,
  while a dose from before the cell was plugged in was never the box's business.
  See [DESIGN.md](DESIGN.md#the-boot-floor).

---

## 9. Charging

- MCP73831T-2ACI/OT, `PROG` = 2 k (R36) → **500 mA**. Use 3.3 k (~300 mA) for
  cells under 500 mAh. Expect thermal throttling in SOT-23-5.
- `STAT` is push-pull on the '31 (not open-drain). Low = charging *or* no input;
  R37 pulls the Hi-Z "complete" state low. Use the USB/coil channels to tell the
  two apart.
- With the ESP32 awake the charger floats at 4.2 V and never reports complete.
- No reverse-polarity protection on the battery lead; U10 pin 1 is `+`.
- **A bare board is indistinguishable from a nearly full one.** Measured
  2026-11-02, both on the same board and firmware minutes apart, USB connected:

  | | `vbatMv` | `percent` | `charging` | `lowBattery` |
  |---|---|---|---|---|
  | no cell fitted at all | 4107–4117 | 90–91 | true | false |
  | charged cell fitted | 4126–4137 | 92–93 | true | false |

  Twenty to thirty millivolts apart, and the **empty** board reads the *lower*
  of the two, so no threshold can separate them: with no load the charger drives
  its `BAT` pin to the float voltage and the divider faithfully reports it.
  Note the limit of this claim — a *discharged* cell on charge reads far lower
  (around 3.6 V) and is perfectly distinguishable. What cannot be told apart is
  an absent cell and a **full or nearly full** one, which is the common case on
  the bench and the dangerous one, because `lowBattery` stays `false` and
  nothing in the API hints that the pack may not be there.

  So a check of the form "the gauge says 90 %, the cell must be fine" proves
  nothing while USB is connected, and neither does `charging: true`. A badly
  seated JST looks exactly like a healthy pack. **The only proof that a cell is
  connected is to pull USB and see the board stay up** — the handover test in
  this section, and `tools/battery_wifi.py` under load. Everything else is the
  charger talking to itself.

**Measured, 2026-11-01 — the handover works, at idle.** With a cell at
`4082 mV (88 %)`, pulling the USB cable and plugging it back in left uptime
running from 58.4 s to 62.5 s. It never restarted, so the board genuinely runs
from the cell: the latch holds, the buck/boost carries the load and the charger
reports charging again when USB returns.

Read that for exactly what it is. The board was **idle** the whole time, and
the protection problem in section 10.1 is a *current* limit, not a voltage one.
Nothing here says the cell survives a Wi-Fi TX burst, which is where the ~30 mA
trip is predicted to bite. The test that matters next is Wi-Fi up while running
on battery — until that passes, battery operation is proven only for a box that
is doing nothing.

---

## 10. Known hardware problems the firmware has to live with

From the review, in the order they bite. The firmware cannot fix these, but it
is written not to make them worse and the bring-up firmware helps confirm them.

1. **Battery protection trips at ~30 mA** (U11 is the AH variant; Q5/Q6 are
   ~2.4 Ω in series). Wi-Fi will disconnect the battery about 10 ms into a TX
   burst. **Bring-up must run from a bench supply on TP2/TP5**, which bypasses
   Q5/Q6 entirely. Q6 is also flipped and cannot block charge current.

   **Measured 2026-11-02, with the protection disabled on the first assembled
   board: Wi-Fi on battery is fine.** 241 HTTP requests over 131 s, every reply
   a TX burst, 0 failures and no reset; uptime ran 529 s → 660 s unbroken, and
   a full active Wi-Fi scan — a probe on every channel, the worst the radio can
   be asked to do — completed in 7.6 s. The cell moved 3956 mV → 3951 mV, a 5 mV
   sag, range 3939–3969 mV. `tools/battery_wifi.py` is that test; it runs
   entirely over the network, because a board on battery has no USB to watch.

   What this does **not** show is that a stock board survives: the protection
   was out of circuit for the measurement, which is the rework below working as
   intended rather than the prediction being wrong. Until someone runs the same
   test on an un-reworked board, treat the ~30 mA figure as unrefuted and keep
   the bench supply rule for bring-up.
2. **TPS63021 `VINA`/`PS/SYNC` float** — the control-stage supply is not
   connected to VIN. Mode is undefined until the bodge is in.
3. **Two 3V3 regulators fight.** Disable U9 (AP7362) by lifting its EN to GND.
4. **The button's 4.2 V reaches U4 pin 12** (see §5) and the MCP's `STAT` reaches
   U4 pin 1 at ~4.5 V.
5. `D5` (RB050LAM-40) leaks up to 100 µA into `BUTTON_SIG` against R31's 100 k
   while latched — can cause phantom presses. R31 → 10 k and/or D5 →
   RB058LAM-40 fixes it. The app firmware samples `BUTTON_SIG` with the rest of
   the analog channels, every 2 s, and requires the level to survive two
   consecutive samples before it reports a press — so a press is confirmed over
   seconds rather than milliseconds, and a leak that is marginal at the
   threshold still gets through. It helps; it is not a substitute for the
   rework. (`buttonDebounceMs`, 80 ms, is a setting the *bring-up* firmware
   honours, where sampling is fast enough for it to mean something.)
6. The inductor is designated `D1` on the schematic; it is L1, 1.5 µH.

### Rework order before you trust a battery run

1. Disable U9.
2. Bench supply into TP2/TP5, press button, measure TP4/TP1/C14. Bodge
   `VINA`→`VIN`, optionally `PS/SYNC`→GND.
3. Fix the protection (own-protection LiPo + jumper `VBAT_NEG`→GND, or swap Q5
   for a PMF63UNE and short Q6).
4. 33 k at U4 pin 12, 100 k at U4 pin 1.
5. R31 → 10 k, or D5 → RB058LAM-40.
6. Optional: R29, R8/R7, LED resistors, R27, R36.
