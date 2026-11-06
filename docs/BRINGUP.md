# EasyPill V1 bring-up

The order below is the order things bite in. Work down it. Do not jump to the hall
sensors before the mux has proved itself, because every hall reading goes through
the mux and a broken address line looks exactly like seven broken sensors.

Tick the boxes. The tables at the end are the output of this document: they are
what you type into the app firmware's GUI afterwards.

Every command letter in here is the letter the firmware actually dispatches, and
every block quoted as output is the format the firmware actually prints — only
the numbers are from a board that worked. If the two ever drift apart again,
`?` is the authority and this file is the bug.

---

## 0. Rework, before anything is powered

From [HARDWARE.md §10](HARDWARE.md). This order matters — step 3 is pointless
before step 1, and step 2 tells you whether the buck-boost is even alive.

- [ ] **1. Disable U9 (AP7362).** Lift its `EN` pin and tie it to GND. Two 3V3
      regulators are fighting otherwise, and which one wins is not something you
      want to discover later.
- [ ] **2. Bench supply into TP2/TP5. Press the button. Measure TP4, TP1 and C14.**
      The TPS63021's `VINA` and `PS/SYNC` float on this revision, so the control
      stage has no supply and the mode is undefined. Bodge `VINA` → `VIN`.
      Optionally `PS/SYNC` → GND to force PWM rather than power-save mode.
      Do not go further until 3V3 is 3.3 V and stable.
- [ ] **3. Fix the battery protection.** Either use a LiPo with its own protection
      board and jumper `VBAT_NEG` → GND, or swap Q5 for a PMF63UNE and short Q6.
      U11 is the AH variant and Q5/Q6 add about 2.4 Ω, so the trip is near 30 mA.
      Q6 is also fitted the wrong way round and cannot block charge current.
- [ ] **4. 33 k in series at U4 pin 12, 100 k in series at U4 pin 1.** The button's
      raw 4.2 V and the MCP73831's ~4.5 V `STAT` both exceed the XS3A4051's
      `VCC + 0.5 V` rating. These resistors limit the current into the mux's ESD
      diode. They do **not** divide the voltage, so both channels stay
      threshold-only reads. If you want a real button voltage instead, fit 33 k in
      series *and* 100 k from U4 pin 12 to GND, then set `buttonDivNum/Den` to
      `133/100`.
- [ ] **5. R31 → 10 k, or D5 → RB058LAM-40.** D5 leaks up to 100 µA into
      `BUTTON_SIG` while the board is latched, which against R31's 100 k is enough
      for a phantom press. The firmware debounces 80 ms and demands a sustained
      level, which helps but does not fix it.
- [ ] **6. Optional, and all for comfort rather than function:** R29, R8/R7, the
      LED series resistors (470 R → ~220 R red, 68–100 R green/blue), R27 → 0–47 R
      for a louder buzzer, R36 → 3.3 k if the cell is under 500 mAh.

### Why the bench supply, specifically

Until step 3 is done, **every bring-up step must run from a bench supply on
TP2/TP5**, not from a battery. The protection circuit trips at roughly 30 mA.
A Wi-Fi transmit burst is an order of magnitude more than that, so on a battery
the board disconnects itself about 10 ms into the first TX and you get a reboot
loop that looks like a firmware bug. TP2/TP5 feeds the battery node directly and
bypasses Q5/Q6, so the trip never happens.

- [ ] Bench supply set to **3.90 V**, current limit **500 mA**. Write down the
      exact voltage; step 7 compares the ADC against it.
- [ ] Note that with USB plugged in the board cannot be switched off, and the
      XIAO's own regulator drives 3V3 whenever USB is present. Expect the ESP32
      to run the moment you plug USB in, latch or no latch.

---

## 1. Flash and connect

```bash
cd firmware
```

```bash
pio run -e bringup-c3 -t upload
```

```bash
pio device monitor -e bringup-c3
```

`MUX_C` is `U0TXD` and `SIG_OUT` is `U0RXD`, so there is no hardware UART to talk
to. All of this runs over USB-CDC, which is plain `Serial` because both board
definitions set `ARDUINO_USB_CDC_ON_BOOT=1`. If the monitor shows nothing, it is
the USB-CDC port that has not come up, not a baud rate problem.

At every reset the ROM bootloader prints its banner on `U0TXD`, which wiggles
`MUX_C`. That is harmless: during boot `SIG_OUT` is an undriven input and the four
FET gates are held low by their own 100 k pull-downs, so nothing can turn on. You
will see a line of garbage before the prompt. Ignore it.

`pio device monitor` sends CR+LF when you press Enter. The firmware treats that
pair as one keypress, so one Enter is one command and the continuous modes do not
stop on the Enter that started them. `screen`, PuTTY and a bare LF terminal all
work the same way.

- [ ] Monitor open, prompt visible, `> ready` printed.
- [ ] Press `?`. You get the command list.

### Command set

Case matters for `h`/`H`, `m`/`M`, `a`/`A`, `w`/`W`, `l`/`L` and `z`/`Z`. Every
other letter takes either case. A command with arguments wants them on the same
line, separated by spaces — nothing in here prompts you for a value.

| Key | Does |
|---|---|
| `?` `h` | print this list |
| `i` | identity: chip, flash, heap, MAC, reset and wake reason, clock, latch, where the mux is parked, pin table |
| `s` | self test: the channel-7 ground reference, seven halls, all five ADC channels, `WAKE_UP` against the OR tree, the latch, then the LED and the buzzer for your eyes and ears |
| `d` | mux channel map reminder: which channel is which hall, which sense node and which FET |
| `H` | live hall table at about 5 Hz, until a key is pressed |
| `H <0-6>` | read one hall sensor once |
| `a` | one pass over analog channels 0–4: raw ADC mV and the scaled value |
| `A` | the same, streaming at about 2 Hz until a key is pressed |
| `w` | read `WAKE_UP` once |
| `W` | `WAKE_UP` at about 4 Hz with a change counter, until a key is pressed |
| `M` | monitor: one timestamped line whenever a lid, the button, the charger, the ground reference or `WAKE_UP` changes |
| `m <0-7>` | select one mux channel and **hold it**; reports `HALL_IN`, `SIG_IN` and `SIG_OUT` there |
| `o <0-3> <0\|1>` | drive one `SIG_OUT` channel and **leave it there** until you set it back |
| `l <r\|g\|b\|w\|off>` | drive the LED one colour, held until `l off`; `w` time-multiplexes all three |
| `L` | identify the LED: ch0, ch1, ch2 for 1.5 s each, and you write down what you saw |
| `z [ms]` | beep exactly as the settings say, default 150 ms. It prints which drive it used |
| `Z <hz>` | set `buzzerToneHz` and sound one beep at the new value. `Z 0` switches to self-drive DC |
| `c` | calibrate hall polarity. **Close every lid first.** |
| `c force` | the same, but accept a tray whose sensors disagree with each other |
| `n <ssid> <pass>` | Wi-Fi + NTP smoke test: connects, syncs the clock, prints the date. The password may contain spaces |
| `p <0\|1>` | `POWER_LATCH` high or low. `p 0 yes` to really drop it |
| `x [s]` | deep sleep for `s` seconds, default 10 — the `WAKE_UP` arming rule |
| `r` | reboot |

Everything the firmware edits — `hallClosedIsLow`, `hallInvertMask`,
`buzzerToneHz`, `buzzerSelfDrive` — it edits in RAM only. The bring-up firmware
never writes NVS, which is why section 12 exists.

### The one off-by-one that will catch you

Mux channel *k* carries **Hall sensor *k+1*** as the schematic labels it. Channel 0
is Hall 1, channel 6 is Hall 7. Everywhere in the firmware and in this document,
"sensor 0" means channel 0, which the schematic calls Hall 1. Lid numbers are a
third, independent numbering, mapped by `hallToLid`, and in weekday mode lid 0 is
Monday.

On the V0 board the two run in opposite directions: `hallToLid` is
`[6,5,4,3,2,1,0]`, so sensor 0 is Sunday and sensor 6 is Monday. That was
measured one sensor at a time (docs/HARDWARE.md §3) and is the compiled default,
so a `H` table on a factory-fresh board already prints the real days. Re-derive
it with `tools/mapdays.py` if the PCB is mounted differently.

---

## 2. Identity — `i`

- [ ] Press `i`.

```text
  firmware        EasyPill V1 bring-up
  built for       Seeed XIAO ESP32-C3 (ARDUINO_XIAO_ESP32C3)
  chip            ESP32-C3 rev 3, 1 core(s) at 160 MHz
  flash           4096 kB
  heap            289728 B free of 327680 B, low water mark 287344 B
  MAC (STA)       54:32:04:xx:xx:xx
  SDK             v4.4.7
  reset reason    power-on (cold boot)
  wake reason     powerOn  (esp cause: not a sleep wake)
  uptime          3.214 s
  clock           NOT SET - no RTC on this board; run  n <ssid> <pass>  to sync it
  POWER_LATCH     HIGH (held)
  WAKE_UP         HIGH
  mux address     7  (100k to GND (R38) | - | - (unconnected))
  SIG_OUT         LOW
```

`built for` is what the binary was compiled against and `chip` is what the
silicon says. They have to agree.

| If instead | It means |
|---|---|
| `chip ESP32-S3` under `built for ... C3` | You flashed the wrong env; the pin map is wrong. Reflash with `-e bringup-s3`. |
| `reset reason software restart` on a cold power-up | Something reset the chip during boot. Suspect 3V3 sag; go back to rework step 2. |
| `POWER_LATCH LOW` | `POWER_LATCH` is not being driven, or D10 is not reaching the latch. On USB you survive anyway because the XIAO regulator is feeding 3V3 — which is exactly why this is easy to miss. |
| `flash 4096 kB` under `built for ... S3` | You have a C3 after all. Check for the shield can. |
| `SIG_OUT HIGH` | A previous `o` or `l` is still holding a FET gate on. `l off` or `m 7` releases it. |

---

## 3. Prove the mux before trusting anything — `s`, then `m`

This is the gate for the rest of the document. `MUX_A/B/C` address three
XS3A4051s at once: the hall mux (U20, common → `HALL_IN`), the analog mux (U4,
common → `SIG_IN`) and the output mux (U5, common ← `SIG_OUT`). One address
selects a channel on all three. If the address lines are not working, every hall
reading and every ADC reading is a meaningless number that still looks plausible.

Channel 7 is the reference. On the hall mux it is nothing but R38, a 100 k to
GND, so **channel 7 must read LOW**. On the output mux channel 7 is unconnected,
which is why it is also the parking spot. The first line of the self test is that
check, and nothing below this section means anything until it passes.

- [ ] Every lid closed, magnets in place.
- [ ] Press `s`. Read only the top of it for now:

```text
  SELF TEST
  [PASS] mux + address    channel 7 of the hall mux reads LOW through R38, so MUX_A/B/C and U20 work
  [PASS] hall 0           LOW (magnet present) -> lid 0 (Monday) CLOSED
  [PASS] hall 1           LOW (magnet present) -> lid 1 (Tuesday) CLOSED
  [PASS] hall 2           LOW (magnet present) -> lid 2 (Wednesday) CLOSED
  [PASS] hall 3           LOW (magnet present) -> lid 3 (Thursday) CLOSED
  [PASS] hall 4           LOW (magnet present) -> lid 4 (Friday) CLOSED
  [PASS] hall 5           LOW (magnet present) -> lid 5 (Saturday) CLOSED
  [PASS] hall 6           LOW (magnet present) -> lid 6 (Sunday) CLOSED
```

Each hall line is read five times before it is graded, so a sensor that will not
hold a level — an unwired part, or an address line being wiggled by something —
fails here rather than looking like a lid that keeps opening.

- [ ] `[PASS] mux + address`.
- [ ] Seven `[PASS] hall n` lines, with no `[FAIL]`.

| If instead | It means |
|---|---|
| `[FAIL] mux + address` | The address is not arriving at U20, or R38 is missing/open, or U20 has no 3V3, or `nE` (pin 6) is not tied active. **Stop here.** Every hall reading below this line is noise, and the firmware says so on each one. |
| `[FAIL] hall n  level flickers across five reads` | That sensor is unwired, or `MUX_C` is being driven as a UART. `MUX_C` is bit 2, so a UART on it corrupts channels 4–7 while 0–3 look fine. Confirm nothing in the build calls `Serial0.begin()` or uses `Serial1`. |
| All seven halls read the same whatever you do with a magnet | The address lines are not moving. Check that D4/D5/D6 are outputs and that R13/R14/R15 (100 k pull-downs) are not shorted to something. |
| The LED flashes or the buzzer chirps while `s` reads sensors | `SIG_OUT` is not being driven low before the address changes, so the scan is walking a logic high across Q1–Q4. That is a firmware bug in `MuxBus::select`, not a board fault. |
| `[PASS] hall n` on all seven, all reading HIGH, with every lid **shut** | The magnets are not over the sensors, or the halls have no supply. With the magnets away from the board all seven should read HIGH; with the lids shut all seven should read LOW. |

Then walk the address by hand, so you have seen the lines move:

- [ ] Press `m 0`, then `m 7`. Meter on D4/D5/D6 confirms `000` and `111`.
      (Address = MUX_A + 2·MUX_B + 4·MUX_C, which is what the `m` output spells
      out as `A=0 B=0 C=0`. MUX_A is S1/pin 11, MUX_B is S2/pin 10, MUX_C is
      S3/pin 9.)

```text
  address         0  ->  A=0 B=0 C=0   (channel = A + 2*B + 4*C)
  HALL_IN         Hall 1                   reads LOW
  SIG_IN          VBAT x 0.5714            2229 mV at the pin
  SIG_OUT         Q3 -> CTL1 pin 4         held LOW
  The address stays here until the next command. `m 7` parks it; `o` drives the
  FET gate on channels 0-3. Nothing else can read a hall sensor meanwhile.
```

`m` holds the address, so it is also how you test one hall sensor with a magnet in
your hand: `m 0`, magnet on the part, and `HALL_IN` must go LOW while it is there.
That is the one measurement in this whole document that cannot be faked by a
wrong setting, and it is worth doing once per sensor on a board you do not trust.

- [ ] `m 7` when you are done, so the bus is parked and nothing is energised.

---

## 4. Which sensor is which lid — `H`

The AH1912 is **omnipolar**: either magnetic pole works. If a sensor never
responds it is distance or a dead part, never the magnet being "the wrong way
round".

Magnet present → output **LOW**. Magnet absent → output **HIGH**.

There is no command that names the lids for you — `n` is the Wi-Fi test, not a
naming command. You read the pairs off the live table and write them into the
table in section 12 by hand; the GUI is where they get typed in.

- [ ] Press `H`. Any key stops it, Enter included.

```text
  Live hall table at about 5 Hz. Wave a magnet over each sensor. Any key stops.
  sNN is the raw sensor output: [1] = no magnet, [.] = magnet. lid0..6 is the
  debounced, polarity-corrected and lid-mapped view: O = open, C = closed.
  The change counters count *raw* transitions, so a sensor that ticks up while
  you keep still is noisy, not triggered.
  hallToLid says: s0=Sunday, s1=Saturday, s2=Friday, s3=Thursday,
                  s4=Wednesday, s5=Tuesday, s6=Monday
  Lift one lid at a time and note which sNN moves - that sensor belongs to the day
  printed on that lid, and the pair goes in the hall-to-lid table in BRINGUP.md.

        time    s0 s1 s2 s3 s4 s5 s6    lid0..6  gnd   WAKE  changes s0..s6
      12.130 s  [.][.][.][.][.][.][.]   CCCCCCC  OK    HIGH   0  0  0  0  0  0  0
      12.330 s  [.][1][.][.][.][.][.]   COCCCCC  OK    HIGH   0  1  0  0  0  0  0
```

`sNN` is in sensor order and `lid0..6` is in lid order. They are the same thing
only while `hallToLid` is the identity, which is why the command prints the
mapping it is using before the first row.

### Bare board

- [ ] Move every magnet well away. Press `H`. All seven `[1]`, i.e. HIGH.
- [ ] Touch one magnet to one sensor at a time. That column goes to `[.]` and its
      change counter ticks by one.

### Assembled tray

Better, because it answers the question you actually have, which is physical:

- [ ] Close every lid. `H` shows all seven `[.]` and `lid0..6` all `C`.
- [ ] Lift **one** lid. Note which `sNN` goes `[1]`. Write it in the hall-to-lid
      table in section 12 against the day printed on that lid. Close it.
- [ ] Repeat for all seven. In Monday-to-Sunday order you will not lose your place.

`M` is the same information said the other way round: it prints
`LID 3 (Thursday) OPENED` whenever a lid moves, using the mapping currently in
RAM. Useful once the mapping is right, and misleading while it is not.

| If instead | It means |
|---|---|
| Two columns move for one lid | Magnets are too close together or too strong for the sensor spacing. Fit a weaker magnet, or increase the gap. The firmware cannot disambiguate this. |
| One lid moves no column at all | Magnet too far from the sensor at rest, or that sensor is dead. Check it with `m` on that channel and a magnet held directly against the part. |
| Columns flicker with nothing moving | The change counters tick up while you keep still. Raise `hallDebounceMs` from 60. If it flickers at a channel boundary rather than on a lid, go back to section 3. |
| `gnd FAIL` anywhere in the stream | Stop. The address lines died mid-run; the whole table is noise. |

---

## 5. Hall polarity — `c`

The intended mechanics are magnet-on-lid sitting over the sensor while the lid is
**closed**, so closed → LOW and idle means every sensor sees a magnet. If your
mechanism instead brings the magnet *towards* the sensor as the lid opens, every
sensor is inverted and the box will think all seven lids are permanently open.

- [ ] **Close every lid.** All of them. Calibration reads the resting state and
      believes it.
- [ ] Press `c`. It prints a prompt and waits.
- [ ] Press Enter. Anything else on the line cancels.

```text
  CALIBRATE HALL POLARITY
  Close every lid, so all seven magnets sit over their sensors, then press Enter.
  Anything else on the line cancels.

  raw mask        .......   (0x00, bit 0 leftmost, 1 = sensor output HIGH)
  result          hallClosedIsLow = true
  result          hallInvertMask  = 0x00   (.......)
  check           all seven lids now read CLOSED - polarity is right
```

That is the normal case. The two abnormal ones say so themselves:

**All seven HIGH** — a legitimate result for a tray inverted end to end, and also
exactly what a tray with no magnets, an unwired loom or a dead sensor supply looks
like. The firmware refuses to let that pass quietly:

```text
  raw mask        1111111   (0x7F, bit 0 leftmost, 1 = sensor output HIGH)
  result          hallClosedIsLow = false
  result          hallInvertMask  = 0x00   (.......)
  warning         all seven read HIGH with the lids supposedly shut
  That is either a tray inverted end to end, or no magnets, or no sensors - and
  the check below cannot tell them apart, because hallClosedIsLow = false is
  exactly what makes an all-HIGH mask mean CLOSED.
  Prove it with a meter before you carry false into the GUI: `m 0`, then hold a
  magnet on hall 1. HALL_IN must go LOW while it is there. If it does not, this
  calibration is noise.
  check           every lid reads CLOSED - all-HIGH always does, so this proves nothing
```

- [ ] If you get this: `m 0`, magnet against hall 1, watch `HALL_IN`. LOW with the
      magnet on the part and HIGH without it means the sensors work and your tray
      really is inverted — carry `hallClosedIsLow = false` over. No change at all
      means the sensor or its supply is the fault. Fix that first.

**Some HIGH and some LOW** — the sensors disagree, so there is no polarity here
to learn and **nothing is written**:

```text
  raw mask        .1..1..   (0x12, bit 0 leftmost, 1 = sensor output HIGH)
  no magnet       s1 (Tuesday), s4 (Friday)
  magnet seen     s0 (Monday), s2 (Wednesday), s3 (Thursday), s5 (Saturday), s6 (Sunday)
  refused         nothing was written - the sensors disagree, so there is no polarity here to learn
  unchanged       hallClosedIsLow = true, hallInvertMask = 0x00
```

On an intact tray that is a lid left ajar, a missing magnet or a dead AH1912, and
the lids to go and look at are the ones listed as `no magnet`. Resolving it into
an invert mask instead would teach the box that a shut lid reads OPEN, and no
self-check could ever catch it: the mask is derived from the very reading it would
be checked against. The family would then get an unexpected-open alert for that
day, for good.

- [ ] Fix the lid, then press `c` again.
- [ ] Only if the tray really is assembled half one way and half the other:
      `c force` accepts the mixed mask and builds `hallInvertMask` from it. The
      mask works. A mixed tray is a thing you will have forgotten by next year,
      so write it in section 12 either way.

| Output | What to do |
|---|---|
| `check  all seven lids now read CLOSED - polarity is right` | Nothing. Carry both values over. |
| `check  still reads OPEN: lid 3 (Thursday)` | That lid is not shut, or its sensor is dead or unwired. A sensor that cannot see its magnet is a part to fix, not a mask to work around. |
| `warning  the channel-7 ground reference did not read LOW` | Stop; this calibration is built on nonsense. Back to section 3. |

- [ ] Press `s` afterwards and confirm all seven halls read `LOW (magnet present)
      -> CLOSED` with everything shut, and that lifting one lid shows exactly one
      `OPEN`.

---

## 6. Snapshot sanity — `s`

Now read the whole self test. Run it with all lids closed, USB connected and the
bench supply on.

- [ ] Press `s`.

```text
  [PASS] adc 0 VBAT       3899 mV (72 %) - inside 3000..4400 mV
  [PASS] adc 1 5V_USB     5049 mV, and a USB host is attached - agrees
  [INFO] adc 2 5V_COIL    9 mV, coil charger absent
  [PASS] adc 3 BUTTON     6 mV at the pin -> released (threshold 1500 mV)
  [INFO] adc 4 STAT       3 mV -> LOW; LOW means charging *or* no input, so read it with channels 1 and 2
  [PASS] WAKE_UP          HIGH, and that is what halls ....... + button up + charger in add up to
  [PASS] POWER_LATCH      HIGH - asserted at boot and still held

  Watch the LED and listen to the buzzer:
  flashing        red, which settings say is channel 2
  flashing        green, which settings say is channel 1
  flashing        blue, which settings say is channel 0
  [INFO] LED              three colours were driven on channels 2/1/0 - did you see red, then green, then blue? If the order was wrong, run `L`
  [INFO] buzzer           one 150 ms tone on channel 3 - did you hear it? Either buzzer type sounds on a tone, so `o 3 1` is the test for which one is fitted

  result          13 PASS, 0 FAIL, 4 for your eyes and ears
```

`WAKE_UP HIGH` is **correct here** and it is not a fault: USB counts as a
charger. The line is only a `[PASS]` because the firmware worked out what the OR
tree *should* say from the halls, the button and the two rail channels, and the
pin agreed. See section 9.

`[INFO]` is for the things with no right answer on a bench: the coil rail, the
clamped `STAT` channel, and the two that only a human can grade — the LED and the
buzzer. Four of them is what you should see on USB. They never count as failures,
and `0 FAIL` is the line that matters.

- [ ] `0 FAIL`.
- [ ] You saw red, then green, then blue, in that order. If not, section 8.

---

## 7. Analog, and the 1.75× divider — `a` / `A`

Dividers are 75 k/100 k on VBAT and 100 k/75 k on the two rails. The firmware
stores them as integer numerator/denominator pairs and multiplies the ADC reading:

| Channel | Signal | Divider | ADC mV → real mV | Settings keys |
|---|---|---|---|---|
| 0 | VBAT | 75 k / 100 k = 0.5714 | × 175 / 100 | `vbatDivNum` / `vbatDivDen` |
| 1 | 5V_USB | 100 k / 75 k = 0.4286 | × 175 / 75 | `railDivNum` / `railDivDen` |
| 2 | 5V_COIL | 100 k / 75 k = 0.4286 | × 175 / 75 | same as above |
| 3 | BUTTON_SIG | clamped, not divided | threshold only | `buttonThresholdMv` |
| 4 | MCP_CHRG `STAT` | clamped, not divided | threshold only | `statThresholdMv` |

- [ ] Put the meter on the bench supply output, i.e. on TP2/TP5. Write the reading
      down to the millivolt.
- [ ] Press `a`.

Check the arithmetic in both directions. At a bench supply of 3.900 V the ADC
should read 3900 × 100 / 175 = **2229 mV**, and the firmware should report
2229 × 175 / 100 = **3900 mV**.

```text
  ch  signal             raw @ pin      scaled   meaning
   0  VBAT                2229 mV     3900 mV   battery 72 %   (x 175/100)
   1  5V_USB              2164 mV     5049 mV   USB present   (x 175/75)
   2  5V_COIL                4 mV        9 mV   coil charger absent   (x 175/75)
   3  BUTTON_SIG             6 mV          -   CLAMPED / threshold only: released (>= 1500 mV)
   4  MCP_CHRG STAT          3 mV          -   CLAMPED / threshold only: LOW (>= 1500 mV)
      derived: CHARGING. STAT LOW means charging *or* no input, so charger presence
      comes from channels 1 and 2, which are real measurements.
```

The `raw @ pin` column is the voltage at the ADC and the `scaled` column is what
the firmware believes the source is. Channels 3 and 4 print no scaled value at
all, because there is nothing to scale — they clamp through the mux's ESD diode
and only the threshold means anything.

| If instead | It means |
|---|---|
| `scaled` equals `raw @ pin` on channel 0 | The divider correction is not being applied. Check `vbatDivNum/Den` are 175/100 and not 1/1. |
| `scaled` is off by a few percent, consistently | The resistors are not exactly 75 k/100 k. Set `vbatDivNum`/`vbatDivDen` to whatever makes the scaled value match your meter. That is what they are for. |
| `scaled` stops climbing around 4.1–4.2 V as you raise the supply | ADC ceiling, not a divider fault. At 4.2 V the node is 2.40 V and the 11 dB attenuation range tops out near 2.5 V. The reading compresses at the top. Live with it; the percentage curve is deliberately pessimistic up there anyway. |
| The reading jumps around by tens of mV | The divider nodes carry 100 nF permanently (C3/C6/C7/C8), so they are always settled — noise here is the ADC, not RC. A median of five is already being taken. |
| `ch1 5V_USB` reads near 0 with USB plugged in | The USB rail is not reaching U4 ch1, or the divider is open. Charger presence comes from channels 1 and 2, so this matters. |
| `ch3 BUTTON_SIG` sits at 1000–1500 mV with nothing pressed | D5's leakage against R31. Rework step 5. Raise `buttonThresholdMv` only as a last resort. |
| `! AnalogSense reports the reading as invalid` | The mux or the ADC is unhappy; the numbers above it mean nothing. Back to section 3. |

- [ ] Hold the button. `ch3` should jump to roughly 3.2–3.3 V (it clamps through
      the mux's ESD diode) and read `PRESSED`.
- [ ] Press `A` and leave it streaming while you wobble the supply. Values should
      track. Any key stops it.

```text
      34.512 s  VBAT 3899 mV  72 %   USB 5049 mV   COIL    9 mV   BTN    6 mV released  STAT    3 mV low   charging
```

---

## 8. Outputs: which colour is on which pin — `o`, `l`, `L`, `z`, `Z`

Four BSS138BKW low-side switches hang off `SIG_OUT` through U5:

| Mux ch | FET | Load |
|---|---|---|
| 0 | Q3 | CTL1 pin 4 |
| 1 | Q2 | CTL1 pin 3 |
| 2 | Q1 | CTL1 pin 2 |
| 3 | Q4 | buzzer, U6 on the JST-PH2 |

`CTL1` is a JST-PH-4. Pin 1 is the common anode to 3V3; pins 2–4 are the three
cathodes. **Which colour sits on which pin is not knowable from the schematic** —
it depends entirely on the LED you wired up. That is why
`ledChannelRed/Green/Blue` are settings.

- [ ] Press `o 0 1`. Something should light, or you have nothing on CTL1. Then
      `o 0 0` to release it.

`o` is raw FET control and it **holds**: the gate stays energised and the address
stays on that channel until you set it back or run a command that takes the bus.
That is deliberate — it is the only way to put a meter on a gate at your own pace —
but it is also the one command that can leave something on. `m 7` parks the bus
and ends it.

- [ ] Press `L`. Each channel is driven on its own for 1.5 s, with no
      multiplexing, so there is no doubt about which colour belongs to which.

```text
  Identifying the LED. Each channel is driven for 1.5 s - write down the colour
  you see. Any key stops early.
  live now        channel 0  (Q3 -> CTL1 pin 4)  - what colour is this?
  live now        channel 1  (Q2 -> CTL1 pin 3)  - what colour is this?
  live now        channel 2  (Q1 -> CTL1 pin 2)  - what colour is this?

  Put what you saw into Settings.hw (GUI: advanced > hardware):
    ledChannelRed   = the channel that looked red
    ledChannelGreen = the channel that looked green
    ledChannelBlue  = the channel that looked blue
  current guess   red=ch2 green=ch1 blue=ch0
```

Write the three colours in the table in section 12. Expect the brightness to be
lopsided: 470 R at 3.3 V gives roughly 3 mA through a red die but only 0.5–1.5 mA
through green or blue, so a dim green or blue is the resistors, not a dead
channel, and mixed white comes out red-biased. Swapping to ~220 R red /
68–100 R green-blue is on the improvement list.

- [ ] Press `l w`. That time-multiplexes all three channels in 2 ms slots. If it
      looks like one colour stepping rather than a blend, one channel is dead —
      `l r`, `l g`, `l b` each hold a single colour, which is the way to find out
      which. `l off` releases the bus.

| If instead | It means |
|---|---|
| Nothing lights on any channel | CTL1 pin 1 is not on 3V3, the LED is in backwards (pins 2–4 are cathodes), or `SIG_OUT` is not reaching the gates. Meter D7 while `o 0 1` holds a channel. |
| One channel lights, the other two do not | Two cathodes not connected, or two dead dice. `o 0 1` / `o 1 1` / `o 2 1` each on their own to confirm. |
| A channel lights faintly and will not go off | Its 100 k gate pull-down is missing, or `SIG_OUT` is floating rather than being driven low. |
| All three light at once | The three cathodes are tied together somewhere. |

### Buzzer

The buzzer type is not knowable either. A self-oscillating piezo makes noise on
plain DC; a magnetic buzzer needs a square wave near its resonance and is silent
or clicks on DC.

**`z` is not the type test.** It plays whatever the settings say, and the defaults
are `buzzerSelfDrive = false` with `buzzerToneHz = 2700`, so on a fresh board `z`
drives a 2700 Hz square wave — and *both* buzzer types sound on that. `z` prints
which drive it used; read that line before you believe anything you heard.

The DC test is `o 3 1`: the buzzer FET held on, no LEDC involved.

- [ ] Press `o 3 1`, listen, then `o 3 0` to release the FET.

| `o 3 1` gives | Fitted buzzer | Set |
|---|---|---|
| A tone that holds for as long as the FET is on | self-oscillating piezo | `buzzerSelfDrive = true`. Done — and `Z 0` puts the firmware in that mode so `z` and `s` match the hardware. |
| Silence, or one faint click at each edge | magnetic | leave `buzzerSelfDrive = false` and find the resonance below |

- [ ] Press `Z 2700`. It sets `buzzerToneHz` and sounds one 150 ms beep at the new
      value with the address on channel 3. Sweep it — `Z 2400`, `Z 2500`,
      `Z 2600`, … — until it is loudest, and write that number down.

```text
  buzzer          2500 Hz square wave on SIG_OUT via LEDC
  This edits the in-RAM copy only. Once you find the loudest tone, put it in
  Settings.hw.buzzerToneHz / buzzerSelfDrive.
```

- [ ] Press `z` to hear it as the app firmware will play it, and check the mode it
      prints is the one you decided on.

R27's 470 R leaves only about 5–7 mA for the buzzer, so even the right tone will
be quiet. R27 → 0–47 R makes it loud. Decide that before you glue the enclosure.

---

## 9. `WAKE_UP` — `w`, `W`, and why it reads high on your bench

Two 74LV32A quad-OR packages OR nine signals into one line:

```text
WAKE_UP = Hall1|Hall2|Hall3|Hall4|Hall5|Hall6|Hall7|BUTTON_SIG|SIG_CHARGE
```

`SIG_CHARGE` is high whenever a charger is present, and USB counts as a charger.
**So while you are connected over USB-CDC, `WAKE_UP` is high and stays high.**
That is correct behaviour and it is also the whole problem with the line: any one
open lid, any missing magnet, or any charger holds it high and masks every other
event.

- [ ] Press `w` with USB in. Expect `WAKE_UP HIGH`. This proves the `SIG_CHARGE`
      branch of the OR and that the line is not stuck low.

```text
  WAKE_UP         HIGH   (D1 / GPIO 3)
  meaning         at least one lid is open, or the button is held, or a charger is in
```

- [ ] Press `W`, then lift a lid. The line is already high, so the state does not
      change and the `changes` counter stays put. That is the expected non-result,
      and it is why `s` grades the line against what the halls, the button and the
      rails say rather than against a constant.

To prove the **hall branch**, you need USB out, which means no serial. Do it with
a meter:

- [ ] USB unplugged, bench supply on TP2/TP5, button pressed to latch, every lid
      closed. Meter on D1 (`WAKE_UP`): should be **LOW**.
- [ ] Lift any lid. D1 goes **HIGH**. Close it; back to low.
- [ ] Press the button with all lids closed. D1 goes high (that is `BUTTON_SIG`,
      raw VBAT through R31's pull-down).

| If instead | It means |
|---|---|
| D1 high with USB out, all lids closed, nothing pressed | One hall is reading high — go back to section 5 — or one of R1–R6 (the 100 k pull-downs on the spare gate inputs) is missing and a spare input is floating. |
| D1 never goes high on a lid | The hall output is not reaching U2/U3, or one OR package has no supply. |
| D1 stuck high and `s` shows all halls LOW | `SIG_CHARGE` is asserted with no charger. Check the coil channel; `ch2` should read near 0. |
| `s` says `[FAIL] WAKE_UP  reads LOW but ... say it should be HIGH` | U2/U3 or their supply. This is the only test that can prove those two packages are alive, so do not skip past it. |

---

## 10. Deep sleep and waking — `x`

- [ ] Press `x`. Ten seconds is the default; `x 20` sleeps twenty.

With USB connected `WAKE_UP` is high, so the correct output is the timer-only
fallback:

```text
  sleep           10 s requested
  WAKE_UP         HIGH -> timer only; a level wake would fire instantly, forever
  That fallback is the point of this test, and on USB it is the normal path:
  SIG_CHARGE holds WAKE_UP high, so enterDeepSleep() refuses the pin wake
  (docs/HARDWARE.md 4). Expect wake reason `timer`.
  latch           pad hold re-armed for the sleep (plain pad: + gpio_deep_sleep_hold_en)
  The board must come back on its own, banner and all, in about 10 s and with
  POWER_LATCH still HIGH - that is the pad hold working. `i` then reports the wake
  reason. USB-CDC drops while it sleeps, so the monitor goes quiet and reattaches;
  if it does not, reopen it.
```

A level-triggered GPIO wake armed while the line is already high wakes the chip
instantly, and then again, forever — on battery that is a flat cell by morning.
The firmware reads the line first and drops to a timer-only sleep when it is high.
That is the one failure mode HARDWARE.md §4 is written about, and this is where it
gets exercised before the app firmware inherits it.

- [ ] The banner comes back by itself after about 10 s, without you touching the
      button. If the board dies instead, the pad hold did not take: on the C3, D10
      is GPIO10, a plain digital pad, and needs both `gpio_hold_en(GPIO_NUM_10)`
      **and** `gpio_deep_sleep_hold_en()`. On the S3, D10 is GPIO9, an RTC pad,
      where the first call alone would do.
- [ ] The banner's `reset reason` says `wake from deep sleep` and its `wake reason`
      says `timer`. `i` says the same.
- [ ] `POWER_LATCH HIGH` in that banner.

To see the **pin** wake instead, the line has to be low first, which means no USB —
so no serial. Do the equivalent with the bench supply and a meter:

- [ ] USB out, bench supply on TP2/TP5, latched, every lid closed. D1 is LOW.
      There is no console here; what you are proving is that the app firmware's
      sleep will arm the pin wake in this state, because `x` prints
      `WAKE_UP  LOW -> arming the GPIO 3 high-level wake as well as the timer`
      whenever it sees the line low.
- [ ] Note the lid-wakes-the-box check as an open item against the app firmware
      rather than pretending it was tested here.

---

## 11. The latch — `r` and `p`

- [ ] Press `r`. The board reboots and comes back **without you holding the
      button**. If it dies instead, the pad hold is not surviving the reset and a
      panic or a WDT reset will kill the box in the field. `assertEarly()` must be
      the first statement in `setup()`, before `Serial`, and the hold must be armed
      immediately after.

```text
  rebooting - POWER_LATCH is pad-held, so it survives the reset
```

- [ ] Press `p 0` with USB connected. It refuses and tells you what it would do:

```text
  !! STOP. `p 0` drops POWER_LATCH. On battery that ends this session: the board
  !! powers off and you have to hold the button again to bring it back. With USB
  !! plugged in the XIAO's own regulator keeps 3V3 alive, so nothing visible
  !! happens (docs/HARDWARE.md 7).
  !! Type   p 0 yes   if that is really what you want.
```

- [ ] Press `p 0 yes`, still on USB. The board does **not** die:

```text
  POWER_LATCH     going LOW now
  still running   USB is holding 3V3 up, so the latch dropped but the chip did not
```

That is not a bug — with USB in, the board cannot be turned off at all, by design
and by physics.

- [ ] Press `p 1` to put the latch back before you unplug anything.

```text
  POWER_LATCH     HIGH, pad hold armed - the board stays up with the button released
```

- [ ] The battery-side check, which needs no console: with USB out, release the
      button before the firmware has latched and the board should drop dead
      immediately. Same mechanism, verified from the other end.

| Behaviour | Means |
|---|---|
| Board drops dead on `p 0 yes` **with USB connected** | 3V3 is not coming from the XIAO regulator. Check rework step 1 — U9 may still be fighting. |
| Board survives `p 0 yes` with USB out | `POWER_LATCH` is not reaching the latch FET, or the latch is held by something else. |
| Board dies a second or two after boot, every time, on the bench supply | The latch is being asserted too late. It has to be the first statement in `setup()`. |
| `i` says `POWER_LATCH LOW` and the board is still running | Only possible on USB. On battery this is the box dying in your grandmother's hand. |

---

## 12. Carry these over

Everything above exists to fill in these four tables. They go into the app
firmware's GUI under **Settings → Hardware** (the JSON keys are the ones in
[API.md](API.md) under `GET /api/settings` → `hw`). The bring-up firmware never
writes NVS, so nothing you found above survives a reset until it is typed in here.

### Measured values

| What | Where measured | Meter says | Firmware says | Setting to adjust |
|---|---|---|---|---|
| Bench supply / VBAT | TP2/TP5 | | | `vbatDivNum` / `vbatDivDen` |
| 3V3 rail | TP4 / C14 | | — | — |
| 5V_USB | USB side of the charger | | | `railDivNum` / `railDivDen` |
| 5V_COIL | coil input | | | same |
| Button pressed | U4 ch3, at the ADC | | | `buttonThresholdMv` |
| Button released | U4 ch3, at the ADC | | | `buttonThresholdMv` |
| `STAT` charging | U4 ch4, at the ADC | | | `statThresholdMv` |
| `STAT` complete | U4 ch4, at the ADC | | | `statThresholdMv` |
| `WAKE_UP` idle, USB out | D1 | | | — |
| Sleep current | supply readout | | — | — |
| Awake, Wi-Fi idle | supply readout | | — | — |

### Hall sensor → lid

Sensor index is the mux channel. Schematic name is `Hall <index+1>`. The column
labels in the `H` table are `s0`…`s6`, in this order.

| Sensor | Schematic | Lid printed on the box | Lid index |
|---|---|---|---|
| 0 | Hall 1 | | |
| 1 | Hall 2 | | |
| 2 | Hall 3 | | |
| 3 | Hall 4 | | |
| 4 | Hall 5 | | |
| 5 | Hall 6 | | |
| 6 | Hall 7 | | |

`hallToLid` = `[ , , , , , , ]` — index by sensor, value is the lid. In weekday
mode lid 0 is Monday and lid 6 is Sunday.

### LED channel → colour

| Mux ch | FET | CTL1 pin | Colour you saw | Setting |
|---|---|---|---|---|
| 0 | Q3 | 4 | | |
| 1 | Q2 | 3 | | |
| 2 | Q1 | 2 | | |

`ledChannelRed` = ___  `ledChannelGreen` = ___  `ledChannelBlue` = ___
(defaults are 2 / 1 / 0, which is a guess and probably wrong for your LED)

### Settings to type into the GUI

| Key | Default | Yours | Came from |
|---|---|---|---|
| `hallClosedIsLow` | `true` | | `c` |
| `hallInvertMask` | `0` | | `c`, and only ever non-zero after `c force` |
| `hallToLid` | `[6,5,4,3,2,1,0]` | | `H`, one lid at a time, or `tools/mapdays.py` |
| `hallDebounceMs` | `60` | | `H` flicker, if any |
| `ledChannelRed` | `2` | | `L` |
| `ledChannelGreen` | `1` | | `L` |
| `ledChannelBlue` | `0` | | `L` |
| `ledBrightness` | `120` | | taste |
| `buzzerChannel` | `3` | `3` | fixed by the schematic |
| `buzzerSelfDrive` | `false` | | `o 3 1` — **not** `z` |
| `buzzerToneHz` | `2700` | | `Z <hz>`, swept |
| `vbatDivNum` / `vbatDivDen` | `175` / `100` | | `a` vs meter |
| `railDivNum` / `railDivDen` | `175` / `75` | | `a` vs meter |
| `buttonDivNum` / `buttonDivDen` | `1` / `1` | | `133`/`100` only if you did the extra 100 k-to-GND rework |
| `buttonThresholdMv` | `1500` | | `a`, button held vs released |
| `statThresholdMv` | `1500` | | `a`, charging vs complete |
| `buttonDebounceMs` | `80` | | raise if D5 leakage gives phantom presses |

One more note for the app firmware: with the ESP32 awake the MCP73831 floats at
4.2 V and never reports "complete", and `STAT` is push-pull on the '31 rather than
open-drain, so low means *charging or no input* and R37 pulls the Hi-Z complete
state low too. Charger presence has to come from channels 1 and 2, which are real
measurements. Do not try to infer it from `STAT` alone.

And one for the clock: there is no RTC. `n <ssid> <pass>` is the only way this
board ever learns the time or the date, the app firmware re-syncs on every Wi-Fi
connection, and nothing in either firmware is allowed to invent a time it was not
given.

Next: [NOTIFICATIONS.md](NOTIFICATIONS.md) to get a push onto the iPad, and
[DESIGN.md](DESIGN.md) if you want to know why any of this is the way it is.
