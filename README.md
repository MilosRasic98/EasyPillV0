# EasyPill V0

![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-yellow.svg)
![Status](https://img.shields.io/badge/Status-Working_Prototype-orange)
![Platform](https://img.shields.io/badge/Platform-ESP32--C3-blue)
![Language-C++](https://img.shields.io/badge/Language-C++-blue)
![Language-Python](https://img.shields.io/badge/Language-Python-blue)
![Version](https://img.shields.io/badge/Version-V0-purple)

**VIDEO OUT WITH DETAILED PROJECT EXPLANATIONS - CHECK BELOW!**

Has grandma taken her medication? This is the question I started with over 3 years ago when I first started thinking about this type of project. Like most people her age, my grandma needs to take a lot of medication throughout the day for cholesterol, hypertension, etc. When someone is home to double check whether she has taken the right medication at the right time, all is good, but what happens when everyone is at work? She is generally okay with this, but there are times she forgets to take her medication, or takes the wrong medication, or doubles some medication because she forgot she already took it etc. My idea with this project is to try and track this, without being a hindrance to her!

This repository covers EasyPill V0, a pillbox that looks like a plain plastic box from the outside, but has an ESP32, 7 hall effect sensors and a battery hidden inside of it, so it can tell when each of the lids has been opened. On this page you will find the CAD files for the 3D printed parts, the schematics, Gerber files and the PCB project, the firmware for the ESP32, and the web dashboard that runs on it. If you have any questions or comments, feel free to contact me!

This project was originally created for the [Element14 Presents](https://www.youtube.com/@Element14presents) YouTube channel. You can find the video and the blog that covers the whole build in more detail on the element14 community.

**MAIN VIDEO AND ELEMENT14 COMMUNITY BLOG - [CLICK HERE](https://community.element14.com/challenges-projects/element14-presents/b/projects/posts/has-grandma-taken-her-pills-this-esp32-pillbox-can-help-you-find-out?ICID=I-HP-has-grandma-taken-her-pills-this-esp32-EP741)**

![Finished EasyPill V0 next to the original sketches](pictures/readme/IMG_4024.jpg)

## Idea and Plan

So now that you have an idea for the general concept behind this project, let's see what would be our system requirements here!

![EasyPill Concept](pictures/readme/Drawing1_Picture.png)

The main goal is to keep the design looking like an ordinary off-the-shelf pillbox. This means it should have the minimal amount of visible electronics and look completely plastic. But why? Wouldn't it be cooler to have a screen, LEDs, buttons etc? No, my idea here is to make the box look as stupid as possible. This thing shouldn't push the user to take any medication, as far as the user goes, they shouldn't even care whether there is anything inside it electronics wise. From their perspective, it is just a plastic box. The reason behind this is that a lot of older people don't like interacting with electronic gadgets, and would rather stay using something they are familiar with, and this is the reason I chose this approach. My goals for this project were:

1. Keep it looking like a plastic box, but make it "smart"
2. Zero (or minimal) interactions from the user
3. Never force or tell the user what to take
4. Keep the family members in the loop
5. 7 columns, 1 for each day, and 3 rows for 3 time slots per day (it would be cool to be able to switch between 1, 2, 3 and 4 time slots)

This meant that any trace of electronics should be hidden as best as possible but still be able to track what is going on with the actual box. There are multiple ways of tackling this problem, and for this V0 version, I decided to go with tracking the individual lid openings, where each lid corresponds to a day. While this doesn't give the perfect result of tracking whether a pill has been taken out of the actual pillbox etc, it can still give you info whether the user interacted with the pillbox at the right time and took the pill out of the right day container (the right day issue is something that has happened before). In the end, this is how I envisioned the design behind the box.

![EasyPill V0 Design](pictures/readme/Drawing2_Picture.png)

As you can see from the drawing above, everything is based around magnets! To detect whether a lid is closed or not, behind a thin plastic wall, there is a hall effect sensor, which is a type of sensor that can work as a switch if you get a magnet close to it. Each lid has a small magnet on it, and since the magnet doesn't have to touch the sensor, all of the electronics can stay hidden behind the plastic.

I drew the plans for this build around 3 years ago, and below, you can find all of the sketches I made back then.

<p align="center">
  <img src="pictures/readme/IMG_4032.jpg" width="32%" alt="EasyPill brainstorming sketch">
  <img src="pictures/readme/IMG_4033.jpg" width="32%" alt="Electronics logic sketch">
  <img src="pictures/readme/IMG_4034.jpg" width="32%" alt="ESP32 preliminary pin connections sketch">
</p>
<p align="center">
  <img src="pictures/readme/IMG_4035.jpg" width="32%" alt="Design sketch">
  <img src="pictures/readme/IMG_4036.jpg" width="32%" alt="Design sketch with the two ways of approaching magnets">
  <img src="pictures/readme/IMG_4037.jpg" width="32%" alt="PCB layout sketch and checklist">
</p>

## Electronics

I began the design of the pillbox with the electronics. If you take a look at all of the sketches that I have made so far as well as the second blue drawing, you can get the sense of how I wanted to pack in the electronics and what I needed. Everything runs from an ESP32 C3 XIAO board and there are 7 hall effect sensors. Everything else here is just purely there to assist with these two main electronic subsystems. This includes a lot of multiplexers and OR gates. The XIAO board I went for doesn't have a lot of pins, so as is done on keyboards, I used multiplexers to be able to read all of the hall effect values. I also used OR gates so that I can easily wake the ESP32 from its sleep mode when any of the lids is opened or the button is pressed. The device also essentially keeps itself awake, when you press the button you drive the EN pin of the buck-boost converter high, then in FW, you need to drive a dedicated GPIO high to keep that EN high once the button is released. This is done through an OR circuit that was made using two diodes. You can check out the full schematics below and also watch the video for more details! These are the main parts of the electronics:

1. XIAO ESP32-C3 - The brains of the whole thing, with WiFi and BLE on board.
2. 7x AH1912 hall effect switches - One for each lid. They are omnipolar, so it doesn't matter which way the magnets are turned.
3. 3x 8:1 analog multiplexers - One reads the hall effect sensors, one reads the analog signals (battery, USB and wireless coil voltage, button and charger status), and one drives the outputs for the RGB LED and the buzzer. All three share the same 3 address lines to save pins.
4. Wake up circuit - Two 74LV32A OR gate ICs that combine all of the hall effect sensors, the button and the charger signal into a single wake up signal for the ESP32.
5. 3V3 buck-boost regulator - TPS63021, so the device stays operational as the battery voltage drops below 3.3 V. There is also a footprint for a backup LDO on the PCB, since this was my first time soldering a buck-boost in such a small package, but in the end I didn't need it.
6. Battery charging and protection - MCP73831 charger that can be powered from either the USB or a 5 V wireless charging coil, and an AP9101C battery protection circuit.

The PCB is a long and thin 4-layer board, with the XIAO at one end and the 7 hall effect sensors on the bottom side, spaced so that each one lines up with its lid. The pictures below show the top and the bottom layer of the PCB, and the assembled board from both sides.

![PCB top layer](pictures/Screenshots/PCB_Layout_Top.png)

![PCB bottom layer](pictures/Screenshots/PCB_Layout_Bottom.png)

<p align="center">
  <img src="pictures/readme/IMG_4063.jpg" width="49%" alt="Assembled PCB, top side">
  <img src="pictures/readme/IMG_4064.jpg" width="49%" alt="Assembled PCB, bottom side with the hall effect sensors">
</p>

The PCB was designed in EasyEDA Pro, and everything you need for making your own is in the `hardware` directory. There you can find the full schematics as a PDF, the Gerber files, the pick and place file, and the project file so you can do your own edits to it. One thing to note, the PCB and some of the files say V1 on them (REV01-DEC2023 on the silkscreen), this is the same board that this V0 of the project is built around.

### Schematics

The schematics are split into 7 sheets, and you can click on any of them to see it in full size:

1. Battery Charge & Protection
2. Power Electronics
3. Output Signals
4. Input Signals
5. Hall Effect Sensing
6. ESP32 C3
7. Wake Up Circuit

<p align="center">
  <img src="hardware/schematics/1_BatteryChargeProtection.png" width="49%" alt="Schematic 1 - Battery Charge and Protection">
  <img src="hardware/schematics/2_PowerElectronics.png" width="49%" alt="Schematic 2 - Power Electronics">
</p>
<p align="center">
  <img src="hardware/schematics/3_OutputSignals.png" width="49%" alt="Schematic 3 - Output Signals">
  <img src="hardware/schematics/4_InputSignals.png" width="49%" alt="Schematic 4 - Input Signals">
</p>
<p align="center">
  <img src="hardware/schematics/5_HallEffectSensing.png" width="49%" alt="Schematic 5 - Hall Effect Sensing">
  <img src="hardware/schematics/6_ESP32C3.png" width="49%" alt="Schematic 6 - ESP32 C3">
</p>
<p align="center">
  <img src="hardware/schematics/7_WakeUpCircuit.png" width="49%" alt="Schematic 7 - Wake Up Circuit">
</p>

### ERRORS

If you wish to replicate this project, be aware that there are some errors in the project that you need to watch out for:

1. Error in the battery protection circuit - Connect VBAT_NEG to GND using a piece of wire.
2. Change R36 to 3k3 to lower the charging current.
3. There is a GND break in the PCB if you don't solder the LDO, so make sure to connect the GND pin of the LDO to the GND pad of the LDO using a piece of wire.
4. For the external button, make sure to connect a 33k resistor in series with the button or you risk burning out the ESP32.
5. Wrong footprints for the LED MOSFETs.
6. The JST connectors turned out to be too big for the enclosure, so I soldered the wires directly to the pads on the PCB.

### Bill of Materials

| # | Product Name | Manufacturer | Quantity | Farnell Order Code | Newark SKU |
|---|---|---|---|---|---|
| 1 | Resistor 100k 1% 0603, CRCW0603100KFKEA | VISHAY | 21 | 1469649 | 27AM5987 |
| 2 | Resistor 330R 1% 0603, CRCW0603330RFKEA | VISHAY | 6 | 1469803 | 52K8432 |
| 3 | Resistor 75k 1% 0603, CRCW060375K0FKEAC | VISHAY | 4 | 4177692 | 72AK6382 |
| 4 | Resistor 470R 1% 0603, CRCW0603470RFKEAC | VISHAY | 4 | 4177643 | 26AK5047 |
| 5 | Resistor 1M 1% 0603, CRCW06031M00FKEAC | VISHAY | 1 | 4177585 | 67AK7940 |
| 6 | Resistor 2k7 1% 0603, CRCW06032K70FKEA | VISHAY | 1 | 1469768 | 61M5353 |
| 7 | Resistor 2k 1% 0603, CRCW06032K00FKEA | VISHAY | 1 | 1469764 | 61M5349 |
| 8 | Capacitor 100nF 50V X7R 0603, MC0603B104K500CT | MULTICOMP PRO | 23 | 1759122 | 06R4927 |
| 9 | Capacitor 4.7uF 25V X7R 0805, C0805C475K3RACTU | KEMET | 3 | 4246323 | 29AH8976 |
| 10 | Capacitor 10uF 16V X7R 0805, 0805YC106KAT2A | KYOCERA AVX | 3 | 4229688 | 08AK4862 |
| 11 | Capacitor 22uF 6.3V X5R 0805, C0805C226M9PACTU | KEMET | 5 | 1108326 | 70K9142 |
| 12 | XIAO ESP32-C3 microcontroller board, 113991054 | SEEED STUDIO | 1 | 4060379 | 61AK2694 |
| 13 | LDO regulator 3.3V 1.5A, AP7362-33SP-13 | DIODES INC | 1 | 3482985 | 76AH4793 |
| 14 | Li-ion charge controller, MCP73831T-2ACI/OT | MICROCHIP | 1 | 1332158 | 56K7045 |
| 15 | 8:1 analogue multiplexer, NX3L4051PW,118 | NXP | 3 | 2775927 | 31AC6745 |
| 16 | Omnipolar hall effect switch, AH1912-W-7 | DIODES INC | 7 | 3518282 | 81AH8722 |
| 17 | Quad 2-input OR gate TSSOP-14, 74LV32APWJ | NEXPERIA | 2 | 3010255 | 97AC8288 |
| 18 | N-channel MOSFET 60V SOT-323, 2N7002BKW,115 | NEXPERIA | 6 | 1894713 | 55T7739 |
| 19 | Schottky rectifier 40V 3A, RB050LAM-40TR | ROHM | 4 | 2918848 | 72AC6588 |
| 20 | TVS diode 5V 400W SMA, SMAJ5.0A-E3/61 | VISHAY | 1 | 9550941 | 05R5746 |
| 21 | Power inductor 1.5uH 2.1A 3x3mm, SRN3015-1R5Y | BOURNS | 1 | 4203910 | 64T2365 |

Parts used outside of Newark/Farnell:

| # | Name | Description |
|---|---|---|
| 1 | Buck-boost converter TPS63021DSJR | 3.3 V output, VSON-14, fits U7 |
| 2 | Li-ion battery protector AP9101CK-AHTRG1 | SOT-23-5, fits U11 |
| 3 | 3D printing filament | White PLA |
| 4 | Polycarbonate sheet | 4 mm thick, cut to size |
| 5 | Li-ion cell | Small single cell, reused. Soldered to the U10 pads, pin 1 = + |
| 6 | M2.5 screws | |
| 7 | Hook-up wire | Leads soldered to the U6, U8, U10, U21 and CTL1 pads |

The schematics show the XS3A4051PWJ multiplexers and BSS138BKW MOSFETs. The NX3L4051PW and 2N7002BKW from the table above have the same pinout and are the ones that were in stock. Also, the LDO from the table is the backup for the buck-boost, so don't solder both of them (this is written on the PCB as well).

## Mechanical Design

The final thing before assembling the whole thing is of course the mechanical design. You can see where all of the sketches were going, and I actually managed to make the final thing look pretty close to the drawings that I made all those years ago. There are a couple of things to point out on the mechanical design. The concept is meant to be printed on an FDM printer, hence a lot of the slop and tolerances in the system. Besides 3D printing, since totally clear lids are needed, I had to cut them out of a 4mm polycarbonate sheet by scoring them with an exacto knife over and over again before breaking them. This gave pretty nice results in the end. The only additional hardware that was needed were some small M2.5 screws to hold everything together, but I would like to not have those either so I can get a completely seamless look!

![CAD model of the EasyPill V0](pictures/Screenshots/CAD_Assembly.png)

The PCB sits in a slot along the edge of the main shell, right under the free ends of the lids, and all 7 lids share a single metal pin as the hinge. Each polycarbonate lid has two small printed pieces glued to it, one for the hinge and one on the other end for the magnet. In the pictures below you can see the main shell with all of the lids open, the PCB in its slot, the hinge, and a closed lid sitting right above the PCB.

<p align="center">
  <img src="pictures/readme/IMG_4058.jpg" width="49%" alt="Main shell with all of the lids open">
  <img src="pictures/readme/IMG_4059.jpg" width="49%" alt="PCB inside of the main shell">
</p>
<p align="center">
  <img src="pictures/readme/IMG_4060.jpg" width="49%" alt="Hinge with the metal pin">
  <img src="pictures/readme/IMG_4061.jpg" width="49%" alt="Closed lid sitting above the PCB">
</p>

The part with the compartments is a separate insert that just drops into the main shell, so the same box can be used with a different number of time slots per day by printing a different insert. For now, there is only the 3x7 one. On the side of the box there is the end cap, which houses the battery and the push button.

<p align="center">
  <img src="pictures/readme/IMG_4057.jpg" width="49%" alt="3x7 insert">
  <img src="pictures/readme/IMG_4055.jpg" width="49%" alt="End cap with the battery and the push button">
</p>

All of the 3MF files for the 3D printed parts, and a STEP file of the whole assembly, are in the `CAD` directory:

1. Main Shell V2 - The main body of the box that holds the PCB and the lids
2. 3x7 Insert V2 - The insert with 21 compartments
3. End Cap V2 and V3 - Two versions of the end cap for the battery and the push button
4. Push Button Mount V2 - Holds the push button inside of the end cap
5. Lid Hinge and Lid Lock V1 - The two printed pieces for each lid, you need 7 of each

<p align="center">
  <img src="pictures/readme/IMG_4026.jpg" width="60%" alt="Finished EasyPill V0 above the sketch it started from">
</p>

## Software

To bring everything together, I needed some software of course. While the original plan was for this to be able to connect to WiFi and send notifications via a custom app for Android/iOS, for now, I made it so that there is a dashboard on the local network that runs on the ESP32, so you can look at the live view of all of the hall effect sensors and tune anything that you need to. For programming the firmware and the dashboard, I used Claude.

![EasyPill dashboard with the Monday lid open](pictures/readme/GUI_Now_LidOpen_crop.png)

On the dashboard you can:

1. See the live state of all 7 lids, when the next dose is, and the latest activity
2. Set up the schedule, with up to 4 doses per day and a time window around each dose in which opening the lid counts for that dose
3. Look at the history of the doses that were on time, late or missed
4. Change the settings like the WiFi, time zone and sleep, and set which hall effect sensor is under which day

Keep in mind that the box only knows that a lid has been opened, it can't know whether the pill was actually taken!

Below you can see the history tab, the schedule tab and the hardware settings.

![History tab of the dashboard](pictures/readme/GUI_History_crop.png)

<p align="center">
  <img src="pictures/readme/GUI_Schedule_crop.png" width="49%" alt="Schedule tab of the dashboard">
  <img src="pictures/readme/GUI_Settings_Hardware_crop.png" width="49%" alt="Hardware settings of the dashboard">
</p>

The firmware is a PlatformIO project (Arduino framework) in the `firmware` directory, and the dashboard from the `web` directory gets packed into the firmware when you build it, so there is only one thing to flash. Since there is no RTC on the PCB, the time is synced over NTP whenever the WiFi is connected. The firmware also has support for sending notifications over ntfy, Pushover or a webhook, which you can set up from the dashboard, but the local dashboard itself is what I focused on for this version.

### Flashing the Firmware

1. Install [PlatformIO](https://platformio.org/install).
2. Copy `firmware/include/wifi_default.example.h` to `firmware/include/wifi_default.h` and put in the name and the password of your WiFi.
3. Connect the XIAO over USB and upload the firmware:

```bash
cd firmware
pio run -e app-c3 -t upload
```

After that, the dashboard is available at `http://easypill.local`. If you skip step 2, or the box can't connect to your WiFi, it starts its own access point called `EasyPill-setup`, where you can set up the WiFi at `http://192.168.4.1`.

There is also a second firmware for testing a freshly soldered board over the serial port, one thing at a time (hall effect sensors, multiplexer channels, voltage measurements, LED, buzzer and the power latch). You can upload it with `pio run -e bringup-c3 -t upload`. Both firmwares can also be built for the XIAO ESP32-S3 (`app-s3` and `bringup-s3`), but I have only tested them on the C3.

### Trying out the Dashboard Without the Hardware

If you just want to click through the dashboard without building anything, there is a simulator written in Python that serves the same dashboard and fakes the lids, the battery and the time. It only needs Python 3, no extra libraries:

```bash
python3 sim/easypill_sim.py
```

Then open `http://127.0.0.1:8080` in your browser. If you run it with `--demo`, it goes through a whole week of doses on its own.

## Repository Structure

```
EasyPillV0/
├── CAD/            # 3MF files for the 3D printed parts and a STEP file of the assembly
├── hardware/       # Schematics, Gerber files, pick and place file and the EasyEDA Pro project
├── firmware/       # PlatformIO project for the XIAO ESP32-C3
├── web/            # The dashboard that gets packed into the firmware
├── sim/            # Simulator for trying out the dashboard without the hardware
├── tools/          # Scripts for flashing and testing the board
├── tests/          # Tests for the scheduling logic that run on a PC
├── pictures/       # Photos, drawings and screenshots
├── LICENSE         # GPL-3.0
└── README.md
```

## What's Next

This is just the V0 of EasyPill, and there is a lot I would like to change and add for the V1, including (but not limited to):

1. Detecting whether the pills were actually taken out of the compartments, and not just whether the lid was opened
2. A new PCB that fixes all of the errors from the list above, so there are no more bodge wires
3. Using an ESP32 module instead of the XIAO board, so I have more GPIOs and a 32 kHz crystal for timekeeping
4. Smaller passive components to free up some space on the PCB
5. Adding an IMU so the box can wake up when somebody picks it up
6. Wireless charging, so the USB port and the button are not needed at all
7. Getting rid of the visible screws for a completely seamless look
8. Notifications on the phones of the family members

If you have suggestions for what else you'd like to see added, feel free to reach out!

## Disclaimer

This project is intended for **educational, experimental, and hobby use only**. EasyPill is **not a medical device**, is **not certified**, and must not be relied on to make sure somebody has taken their medication. It can only tell that a lid has been opened, not that a pill has been taken.

The dashboard has no login and no encryption, it is a proof of concept meant to be used on your local network, so don't expose it to the internet.

All hardware, firmware, and software are provided **"as-is" without warranty**, and the author assumes **no responsibility** for any damages, injuries, or losses resulting from the use of this project.
