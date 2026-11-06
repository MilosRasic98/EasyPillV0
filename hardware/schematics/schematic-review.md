# MR27 – EasyPill V0: schematic review

Reviewed 2026-09-28 from `Schematic_EaSyPill_V1_2026-09-27.pdf` (7 sheets, EasyEDA, drawn 2023-12-24). The PCB is already built to this schematic. The module might be a XIAO ESP32-S3 instead of a C3, and the buck-boost has been reported working.

**Summary:** the board can work, but not reliably on battery as built. There are five real problems, and the protection circuit is the one that would actually stop it. Everything is fixable with bodges and resistor swaps.

## Must fix before it runs on battery

### 1. Battery protection trips at ~30 mA (the deal-breaker)
- U11 is the **AH** variant of the AP9101C. Its thresholds:
  - Discharge-overcurrent: **75 mV** across the FETs, 10 ms delay.
  - Charge-overcurrent: −75 mV.
  - Short-circuit: 0.7 V, 320 µs.
- Q5/Q6 are BSS138BKW: 1.1 Ω typ / 2.2 Ω max each at VGS 4.5 V, and worse at the 3–4 V the AP9101C drives them with. Two in series is ~2.4 Ω, so it trips at **~30 mA** (as low as ~17 mA worst case).
- ESP32 load currents:
  - C3 running its CPU only: ~20–30 mA.
  - Wi-Fi receive: ~80 mA.
  - Wi-Fi TX peaks: 300 mA+.
- About 10 ms into Wi-Fi activity the battery disconnects. 3V3 collapses, the latch drops, and the device switches off. An S3 is worse.
- **Q6 is flipped** (drain on GND, source toward Q5), so both body diodes point from VBAT_NEG to GND. The pair can never block charge current.
- The 500 mA charge current trips the −75 mV charge threshold. Q6 then turns off, and the charge current flows through its body diode. That causes:
  - ~1 V+ in the battery return path.
  - Slow, incomplete charging, because the MCP73831 regulates VBAT relative to GND, not to the cell.
  - FETs rated 320 mA carrying more than that when the cell is flat.

**Fix A (for the video):**
- Use a LiPo with its own protection PCB.
- Jumper VBAT_NEG to GND: U10 pin 2 → TP5, or bridge Q5 source to Q6 drain.
- U11 can stay; it just idles.

**Fix B (keep on-board protection):**
- Replace Q5 with a **Nexperia PMF63UNE**: SOT-323, same G/S/D pinout, ≤65 mΩ at 4.5 V, ≤74 mΩ at 2.5 V.
- Short Q6 drain–source.
- You keep over-discharge (2.5 V), discharge-overcurrent (~1 A) and short protection, and the charge-path drop becomes ~30 mV.
- Don't swap both FETs and keep Q6: at ~130 mΩ, the −75 mV charge threshold sits right at the 500 mA charge current.

**Confirm it:** run from a cell through U10, start Wi-Fi, and watch U11 pin 4 (DO).

**Bring-up:** feed a bench supply into TP2 (VBAT) and TP5 (GND). This bypasses Q5/Q6.

### 2. TPS63021: VINA and PS/SYNC are floating
- Pin 1 (VINA) is the control-stage supply and the UVLO sense. It connects only to pin 13 (PS/SYNC) and C14, never to VIN.
- TI's datasheet requires VINA on the input supply with 100 nF. EN and PS/SYNC must not be left open.
- If it "works", it's either fed parasitically (probably through EN's input protection) or U9 is carrying the rail. PS/SYNC follows VINA, so the mode (power-save or forced PWM) is unknown.
- **Check:** disable U9, then measure C14's non-ground pad:

  | C14 reading | What it means |
  |---|---|
  | ≈ VBAT | There's a PCB connection that isn't in the schematic |
  | A few hundred mV below TP4 | It's parasitic |
  | 0 V | The TPS isn't running |

  Test at 3.0 V and 4.2 V, and under Wi-Fi load.
- **Fix:**
  - Bodge wire from C14's VINA pad to the VIN pad of C15/C16. This forces PWM, which is fine for a demo but costs a few mA at idle.
  - For battery life, also cut PS/SYNC away from that node and tie it to GND: power-save mode, ~25 µA quiescent.

### 3. Dual 3V3: disable U9
Disable the AP7362: lift pin 2 (EN) to pin 1 (GND), or remove it. Leaving it on the rail causes three problems:
- **No reverse-current protection.** Below ~3.4 V battery it's in dropout with the pass FET fully on, so the buck-boost's 3.3 V can flow back into the battery. Whether it does depends on which setpoint is higher.
- **Load sharing.** Above ~3.4 V, whichever regulator has the higher setpoint carries the whole load.
- **Sleep current.** Its ~0.5–1.2 mA ground current is several times the whole sleep budget.

There's a third source too. The XIAO's own regulator (XC6210 LDO on the C3, SGM6029 buck on the S3) drives 3V3 whenever USB is in. That's harmless with the TPS in power-save; in forced PWM they can fight. It also means the ESP32 runs whenever USB is in.

### 4. The button
- **Overvoltage:** BUTTON_SIG is raw VBAT, up to 4.2 V.
  - U2 pin 9 is fine: the 74LV32A has overvoltage-tolerant inputs with Ioff.
  - **U4 pin 12 is not.** The XS3A4051's switch pins are rated −0.5 V to VCC + 0.5 V, and on channel 3 it passes that voltage to the ESP32 pin on SIG_IN.
  - On every power-on press, the battery charges the whole 3V3 rail through U4's ESD diode.
- **Leakage:** while latched, PWR_EN is ~3 V. D5 (RB050LAM-40, up to 100 µA leakage) leaks into BUTTON_SIG against R31's 100 k. It's fine cold but marginal warm, and it can cause phantom presses that pin WAKE_UP high.
- **Fixes:**
  - Lift U4 pin 12 and dead-bug a 33 k resistor in series.
  - Change R31 to 10 k, and/or D5 to RB058LAM-40 (same SOD-128 footprint, 2.5 µA max leakage).
- **Behavior to design around:**
  - Hold the button until firmware sets D10.
  - Power-off happens when the button is released.
  - With USB in, the board can't turn off.

### 5. MCP73831 STAT overdrives the same mux
- STAT is push-pull on the MCP73831 (open-drain only on the '32). At charge-complete it drives to about VDD (~4.5 V) into U4 pin 1.
- With the device off on the coil, it also back-powers 3V3.
- **Fix:** lift U4 pin 1 and add a 100 k resistor in series.
- STAT low means "charging" or "no input" (R37 pulls Hi-Z low). Use the USB/coil dividers to tell them apart.

## Works, but dictates firmware

### 6. Shared MUX_A/B/C lines
| Ch | HALL_IN | SIG_IN | SIG_OUT drives |
|---|---|---|---|
| 0 | Hall 1 | VBAT × 0.571 | Q3 → CTL1 pin 4 |
| 1 | Hall 2 | USB × 0.429 | Q2 → CTL1 pin 3 |
| 2 | Hall 3 | Coil × 0.429 | Q1 → CTL1 pin 2 |
| 3 | Hall 4 | Button | Q4 → buzzer |
| 4 | Hall 5 | MCP STAT | — |
| 5–6 | Hall 6–7 | — | — |
| 7 | GND ref (R38) | — | — |

- One output is on at a time, and every read moves it.
- Pull SIG_OUT low before changing MUX lines.
- Park on channel 7.
- Mix RGB by time-multiplexing channels 0–2 with PWM.
- Watch for rolling-shutter banding on camera.

### 7. WAKE_UP = OR of "magnet absent" + button + charger
- The AH1912 is push-pull and goes low when a magnet is present. Idle must be "every sensor sees a magnet".
- One open lid, a missing magnet, or a charger holds WAKE_UP high and masks everything else. GPIO deep-sleep wake is level-triggered, so fall back to timer wake or wake-on-low.
- If the magnet only approaches a sensor on opening, the logic is inverted.

### 8. Latch, sleep and time
- There's no RTC. Timed reminders need deep sleep with the latch held.
- **C3:** GPIO10 is a digital pad. It needs `gpio_hold_en()` + `gpio_deep_sleep_hold_en()`.
- **S3:** GPIO9 is an RTC pad, so `gpio_hold_en()` is enough.
- Set the hold right after latching at boot, then verify it survives panic/WDT resets.
- **Clock:** the XIAO C3 has a 32.768 kHz crystal on GPIO0/1. Enable it via ESP-IDF menuconfig (Arduino defaults to the internal RC). The S3 doesn't appear to have one, so use NTP.
- **Low battery:** have firmware drop the latch at ~3.3 V.

### 9. UART pins
- MUX_C is on U0TXD and SIG_OUT on U0RXD (D6/D7). The boot log toggles MUX_C.
- Log over USB-CDC only. Never use Serial0.

## Cheap improvements
- **LEDs:** 470 Ω at 3.3 V gives ~3 mA red but only ~0.5–1.5 mA green/blue.
  - Use ~220 Ω for red and 68–100 Ω for green/blue.
  - The LED must be common-anode.
- **Buzzer:** 470 Ω in series leaves ~5–7 mA.
  - Change R27 to 0–47 Ω.
  - Add a diode across a magnetic buzzer.
- **Sleep budget (3V3 on, ESP32 asleep):**

  | State | Current |
  |---|---|
  | As built | ~1 mA to a few mA (weeks) |
  | After items 2–3 | ~140 µA |
  | Also with R29 → 1 M and R8/R7 → 750 k/1 M | ~90 µA (most of a year on 1000 mAh) |

  Breakdown after items 2–3:
  - XIAO C3 ~43 µA (S3 ~14 µA)
  - TPS in power-save ~25 µA
  - 7× AH1912 ~11 µA
  - AP9101C + MCP ~5 µA
  - R29 ~30 µA
  - VBAT divider ~24 µA (drawn even when "off")
- **Charging:** R36 = 2 k sets 500 mA.
  - Expect thermal throttling in SOT-23-5.
  - Use 3.3 k (~300 mA) for cells under 500 mAh.
  - With the ESP32 awake, the charger floats at 4.2 V and never reports "complete".
- **Small stuff:**
  - No reverse-polarity protection. Check the battery lead pinout (U10 pin 1 = +).
  - The XIAO's red charge LED may light with USB in.
  - The inductor is designated "D1". Rename it L1 and check Isat ≥ ~2 A.

## C3 vs S3 mapping
| Net | Pin | XIAO ESP32C3 | XIAO ESP32S3 |
|---|---|---|---|
| WAKE_UP | D1 | GPIO3 (deep-sleep wake OK) | GPIO2 (RTC, ext0/ext1) |
| SIG_IN | D2 | GPIO4 / ADC1_CH4 | GPIO3 / ADC1_CH2 (strap, benign) |
| HALL_IN | D3 | GPIO5 | GPIO4 |
| MUX_A / MUX_B | D4 / D5 | GPIO6 / 7 | GPIO5 / 6 |
| MUX_C | D6 | GPIO21 (U0TXD) | GPIO43 (U0TXD) |
| SIG_OUT | D7 | GPIO20 (U0RXD) | GPIO44 (U0RXD) |
| POWER_LATCH | D10 | GPIO10 (digital) | GPIO9 (RTC) |

- To identify the module, check the upload log ("Chip is ESP32-C3/-S3"). The S3 has a metal shield can.

## Rework order
1. Disable U9.
2. Put 3.7 V into TP2/TP5, press the button, and measure TP4, TP1 and C14. Bodge VINA to VIN if needed, and optionally PS/SYNC to GND.
3. Fix the protection (A or B).
4. On U4: add 33 k at pin 12 and 100 k at pin 1.
5. Change R31 to 10 k, or D5 to RB058LAM-40.
6. Optional: R29, R8/R7, LED resistors, R27, R36.
7. Validate:
   - Sweep 3.0–4.2 V under Wi-Fi load.
   - Latch on/off.
   - Measure sleep current.
   - Charge from USB and from the coil.

Already correct:
- FB is tied to VOUT (fixed 3.3 V part).
- Passives match the reference designs.
- PROG is correct.
- The mux enables are tied.
- WAKE_UP is on a deep-sleep-capable pin.
- Strapping pins are avoided.

## Sources
- TI TPS63020/21 datasheet: https://www.ti.com/lit/ds/symlink/tps63020.pdf
- Diodes AP9101C: https://www.mouser.com/pdfdocs/AP9101C.pdf
- Nexperia BSS138BKW: https://assets.nexperia.com/documents/data-sheet/BSS138BKW.pdf
- Nexperia PMF63UNE: https://assets.nexperia.com/documents/data-sheet/PMF63UNE.pdf
- Nexperia XS3A4051: https://assets.nexperia.com/documents/data-sheet/XS3A4051.pdf
- Nexperia 74LV32A: https://www.nexperia.com/product/74LV32APW
- Diodes AH1912: https://www.diodes.com/part/view/AH1912
- Microchip MCP73831: https://cdn-shop.adafruit.com/product-files/3081/mcp73831.pdf
- Diodes AP7362: https://www.mouser.com/datasheet/2/115/DIODS20454_1-2541896.pdf
- ROHM RB050LAM-40: https://www.rohm.com/products/diodes/schottky-barrier-diodes/low-vf/rb050lam-40-product
- ROHM RB058LAM-40: https://www.rohm.com/products/diodes/schottky-barrier-diodes/ultra-low-ir/rb058lam-40-product
- Seeed XIAO ESP32C3 schematic: https://files.seeedstudio.com/wiki/XIAO_WiFi/Resources/Seeeduino-XIAO-ESP32C3-SCH.pdf
- Seeed XIAO ESP32S3 schematic: https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_SCH_v1.2.pdf
- ESP-IDF GPIO (ESP32-C3): https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/peripherals/gpio.html
