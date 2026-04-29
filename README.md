# RC Construction Machine Sound & Hydraulic Simulator

Realistic engine sound, hydraulic pump whine, track rattle, horn, backup beep, headlights, work lights and servo outputs — all from a single ESP32 board in your RC construction vehicle.

Built for **complete beginners**. If you can plug in a USB cable and click a button, you can flash this firmware. The web configurator does everything for you — no Arduino IDE, no command lines, no editing C++ code.

Based on [TheDIYGuy999's RC_Engine_Sound_ESP32](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32) sound engine, stripped down and rebuilt specifically for hydraulic construction machines (excavators, dozers, loaders, cranes, graders, skid steers).

![logo](logo.png)

---

## Table of Contents

1. [What You Need](#what-you-need)
2. [Step 1 — Install the Software](#step-1--install-the-software)
3. [Step 2 — Download This Project](#step-2--download-this-project)
4. [Step 3 — Open the Web Configurator](#step-3--open-the-web-configurator)
5. [Step 4 — Pick Your Machine](#step-4--pick-your-machine)
6. [Step 5 — Plug In Your ESP32](#step-5--plug-in-your-esp32)
7. [Step 6 — Build & Flash](#step-6--build--flash)
8. [Step 7 — Wire It Up](#step-7--wire-it-up)
9. [The Sound Lab](#the-sound-lab)
10. [Pin Reference](#pin-reference)
11. [Troubleshooting](#troubleshooting)
12. [FAQ](#faq)

---

## What You Need

### Hardware

| Part | Notes |
|------|-------|
| **ESP32 Dev Board** | Must be a **classic ESP32** (ESP32-WROOM-32 or ESP32-D0WD). **ESP32-S3, S2, C3, C6 do NOT work** — they have no hardware DAC. The [TheDIYGuy999 sound controller PCB v1.2](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32) is the recommended board. |
| **Speaker + Amp** | A small 4–8 Ω speaker driven by a PAM8403 (or any small Class-D amp). Engine sound comes out of **GPIO 25**, horn/aux out of **GPIO 26**. |
| **RC Receiver** | FlySky IBUS, Futaba SBUS, Graupner SUMD, PPM, or up to 6 PWM channels. |
| **5 V Power Supply** | A BEC/UBEC from your ESC, or a separate 5 V battery. ESP32 takes 5 V on the VIN pin. |
| **USB Cable** | Micro-USB or USB-C (whichever your board has). Used for flashing only — not needed once installed. |

### Software (One-Time Setup)

You only install these once. After that, just open the configurator and click buttons.

- **VS Code** — the editor that runs PlatformIO
- **PlatformIO** — builds the firmware
- **Python 3.10+** — runs the web configurator
- **Git** *(optional)* — for downloading and updating the project

---

## Step 1 — Install the Software

### 1a. Install VS Code
1. Go to **[https://code.visualstudio.com](https://code.visualstudio.com)**
2. Click the big blue **Download** button
3. Run the installer — accept all defaults
4. Open VS Code when it finishes

### 1b. Install PlatformIO inside VS Code
1. In VS Code, click the **Extensions** icon on the left sidebar (or press `Ctrl+Shift+X`)
2. Type **PlatformIO IDE** in the search box
3. Click the blue **Install** button on the first result
4. Wait — it downloads compilers automatically (3–5 minutes the first time)
5. When it says "Please restart VS Code," click **Restart**

### 1c. Install Python
1. Go to **[https://www.python.org/downloads/](https://www.python.org/downloads/)**
2. Click **Download Python 3.x** (any 3.10 or newer)
3. Run the installer
4. **CRITICAL:** Check the box **"Add python.exe to PATH"** at the bottom of the first screen before clicking Install
5. Click **Install Now**

To verify it worked, open PowerShell and type:
```
python --version
```
You should see something like `Python 3.12.3`. If you see "command not found," reinstall and make sure the PATH box was checked.

### 1d. Install Git *(optional but recommended)*
1. Go to **[https://git-scm.com/downloads](https://git-scm.com/downloads)**
2. Download for Windows, run the installer, accept all defaults

---

## Step 2 — Download This Project

### Option A — Using Git (recommended, easy to update later)
Open PowerShell anywhere (Windows key → type "powershell" → Enter) and run:
```powershell
cd $HOME\Documents
git clone https://github.com/turbotike/rc-hydraulic-simulator-flasher.-.git HydraulicController
cd HydraulicController
```

### Option B — Download ZIP
1. Go to the GitHub page
2. Click the green **Code** button → **Download ZIP**
3. Extract the ZIP into `Documents\HydraulicController`

---

## Step 3 — Open the Web Configurator

This is where the magic happens. Everything is done in a browser.

### Easiest way — double-click
1. Open the `HydraulicController` folder in File Explorer
2. Double-click **`Open Configurator.bat`**
3. A black PowerShell window pops up, then your default browser opens to **http://localhost:8080**

### Alternative — from VS Code
1. **File → Open Folder** → pick the `HydraulicController` folder
2. Open a terminal: **Terminal → New Terminal** (or `` Ctrl+` ``)
3. Type:
   ```
   python configure.py
   ```
4. Your browser opens automatically

> **Leave the PowerShell/terminal window open while you use the configurator.** Closing it stops the web server.

---

## Step 4 — Pick Your Machine

In the web UI, the **left sidebar** has tabs. Click them in order:

### 1. **Machine** tab
- Pick your machine type: **Excavator**, **Dozer**, **Loader**, **Crane**, **Skid Steer**, or **Grader**
- Optionally type a name for your build (saved in the firmware as a comment)

### 2. **Vehicle Profiles**
- Click **Load Profile** to pre-fill the entire config from a known-good vehicle (e.g. *CAT 730*, *Volvo EC550E*, *Caterpillar D6 Dozer*)
- This is the **fastest path to a working build** — pick a profile that matches your machine, click load, then jump straight to Build

### 3. **Sounds** tab
- Each row is a sound slot: idle, rev, knock, turbo, horn, brake, hydraulic pump, track rattle, etc.
- Use the **dropdown** to pick which sound file plays for that slot
- Use the **slider** to set its volume (0–200%)
- The **Sound Lab** button opens the live editor (see below)

### 4. **RC** tab
- Pick your protocol: **IBUS** (FlySky), **SBUS** (Futaba), **SUMD** (Graupner), **PPM**, or **PWM**
- Assign which channel controls what (throttle, steering, lights, horn, etc.)
- Reverse any channels if your transmitter sticks are inverted

### 5. **ESC**, **Servos**, **Lights** tabs
- Tweak acceleration, braking, ramp time, servo end-points, light cycle behavior
- Defaults from a vehicle profile are already sensible — leave alone if unsure

---

## Step 5 — Plug In Your ESP32

1. Plug the ESP32 board into your PC with a USB cable
2. Windows will install drivers automatically (give it 30 seconds the first time)
3. If Windows can't find a driver, install the right one for your board:
   - **CP2102** chip → [Silicon Labs CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   - **CH340** chip → [WCH CH340 driver](http://www.wch-ic.com/downloads/CH341SER_EXE.html)
4. Open Device Manager (Windows key → "Device Manager") → expand **Ports (COM & LPT)**
5. You should see something like **Silicon Labs CP210x USB to UART Bridge (COM8)** — note the COM number

---

## Step 6 — Build & Flash

In the web UI:

1. Click the **Build** tab
2. Click the **COM Port** dropdown — pick your ESP32's port (e.g. COM8)
3. Click the big **Build & Flash** button
4. Watch the log window. It will:
   - Compile the firmware (30 sec – 2 min depending on your PC)
   - Upload it over USB (10–20 sec)
   - Show a green ✅ when done
5. **That's it.** Unplug USB. Your ESP32 now runs your custom config.

> **First build takes longer** because PlatformIO downloads the ESP32 toolchain. Subsequent builds are much faster.

### If the upload step fails
- Hold the **BOOT** button on the ESP32 while clicking Build & Flash
- Release BOOT once you see "Connecting…" in the log
- If it still fails, try a different USB cable (some are charge-only)

---

## Step 7 — Wire It Up

Power off the ESP32 first. Use the table below.

| ESP32 Pin | Connect To | Notes |
|-----------|-----------|-------|
| **5V (VIN)** | BEC / 5V battery + | Power input |
| **GND** | BEC / battery − | Common ground (also tie receiver GND here) |
| **GPIO 36** | Receiver signal pin | SBUS/IBUS/SUMD/PPM input. For PWM, use the per-channel pins below. |
| **GPIO 25** | Amp left input | Engine sound DAC1 |
| **GPIO 26** | Amp right input | Horn / aux DAC2 (optional, can tie to same speaker) |
| **GPIO 33** | ESC signal wire | 3.3 V — most ESCs work; if not, add a 3.3→5V level shifter |
| **GPIO 13** | Servo CH1 signal | Steering / left track |
| **GPIO 12** | Servo CH2 signal | Shifting / right track |
| **GPIO 14** | Servo CH3 signal | Boom / winch |
| **GPIO 27** | Servo CH4 signal | Bucket / blade |
| **GPIO 3**  | Headlight LED + | Through a current-limit resistor (e.g. 220 Ω for a 3 mm white LED) |
| **GPIO 22** | Work-light LED + | Same — resistor required |

> **Don't bridge VIN to 3V3.** VIN takes 5 V; 3V3 is the regulator output.

---

## The Sound Lab

The Sound Lab is a built-in audio editor that runs in your browser. No Audacity needed.

1. Open the **Sound Lab** tab
2. Browse all `.h` sound files in the left list — click any one to load it
3. **Preview** button — plays it through your PC speakers
4. **Import WAV** — drag any `.wav` file from your computer into the panel
5. Adjust:
   - **Loop start / loop end** — for engine idle/rev loops, trim to a single cycle
   - **Speed** — pitch-shift up or down
   - **Smoothing** — low-pass filter to take harsh edges off
   - **Crossfade** — smooths the loop join (set to 0 for one-shot sounds like horn or startup)
6. **Install** — saves it back as a `.h` file with a name you choose
7. **Trash icon** — deletes a sound file from the project
8. The sound is now selectable in the **Sounds** tab

> Tip: 22 050 Hz mono 8-bit signed is the sweet spot. The Sound Lab auto-converts.

---

## Pin Reference

| GPIO | Function |
|------|----------|
| 36 | RC input (SBUS/IBUS/SUMD/PPM) |
| 25 | DAC1 — Engine sound output |
| 26 | DAC2 — Horn / aux sound output |
| 13 | Servo CH1 (steering / left track) |
| 12 | Servo CH2 (shifting / right track) |
| 14 | Servo CH3 (boom / winch) |
| 27 | Servo CH4 (bucket / blade) |
| 33 | ESC output |
| 3  | Headlights |
| 22 | Work lights |

---

## Lights

Lights are controlled by the RC lights channel (default CH6). Each switch press cycles through:

1. **Press 1** — Headlights ON (GPIO 3)
2. **Press 2** — Headlights + Work lights ON (GPIO 3 + 22)
3. **Press 3** — All OFF

---

## Machine Modes

| Mode | Controls |
|------|----------|
| **Dozer** | Left track, right track, blade, ripper, tilt |
| **Excavator** | Boom, stick, bucket, swing, left track, right track |
| **Loader** | Boom, bucket, steering |
| **Crane** | Boom, extend, swing |
| **Skid Steer** | Boom, bucket |
| **Grader** | Blade, circle, tilt, articulation |

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| **`python` not recognized** | Reinstall Python and make sure **"Add python.exe to PATH"** is checked. Restart PowerShell. |
| **Web UI won't open** | Make sure no other program is using port 8080. Look at the PowerShell window — it shows the URL it's serving on. |
| **No sound at all** | Check speaker wiring on GPIO 25/26. Check `masterVolume` slider in the web UI is not at 0. Check amp has 5 V power. |
| **Sound is distorted/clipping** | Drop the per-sound volume sliders to ~80–100 %. Master volume to 70 %. |
| **COM port not showing in dropdown** | Install the CP2102 or CH340 USB driver. Click the **Refresh** button next to the dropdown. Try a different USB cable. |
| **Upload fails ("Failed to connect")** | Hold the **BOOT** button on the ESP32 while clicking Build & Flash. Release once you see "Connecting…". |
| **ESC won't arm** | The ESP32 outputs 3.3 V on GPIO 33. Most ESCs accept this, but some need 5 V — add a level shifter. |
| **Build fails after editing config** | The web UI auto-fixes config corruption on save. Close the browser, re-open the configurator, click **Save Settings** once, then **Build & Flash** again. |
| **"Settings reset after save"** | Fixed — the configurator now correctly preserves all defines and slot assignments across saves. |
| **Sounds dropdown is empty** | Make sure the `src/sounds/` folder has `.h` files in it. If empty, re-download the project. |
| **Garbled sound at startup** | The DAC offset fade-in handles this. If you still get a pop, lower master volume slightly or add a 100 µF cap across the speaker leads. |

---

## FAQ

**Can I use an ESP32-S3 / C3 / C6?**
No. Those chips have **no hardware DAC**. This firmware uses the classic ESP32's DAC1/DAC2 on GPIO 25/26 to generate analog audio directly. You'd need to rewrite the audio path to use I²S + an external DAC like a MAX98357A.

**Can I run multiple machines from one ESP32?**
One firmware build = one machine config. But swapping is fast — pick a different vehicle profile in the web UI, click Build & Flash, done.

**Can I add my own engine sound?**
Yes — drop a `.wav` file into the **Sound Lab** tab, trim the loop, click Install. Or convert a WAV to `.h` externally and drop it in `src/sounds/`.

**Does this work without an RC receiver?**
For testing yes — set the protocol to PWM and leave channels unconnected, the firmware will idle. For real use you need a receiver.

**How do I update the project later?**
If you used Git:
```powershell
cd $HOME\Documents\HydraulicController
git pull
```
If you used ZIP, just download the new ZIP and overwrite (back up your `src/config.h` first).

**Where's my config saved?**
In `src/config.h`. Every time you click **Save Settings** in the web UI, that file is rewritten. You can hand-edit it too, but the web UI is safer — it auto-handles variable-name conflicts and missing includes.

---

## License

Based on [TheDIYGuy999's RC_Engine_Sound_ESP32](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32). See the original project for license details.

---

## Credits

- **TheDIYGuy999** — original sound engine, PCB design, sound samples
- **turbotike** — hydraulic-machine fork, web configurator, Sound Lab, Vehicle Profiles
