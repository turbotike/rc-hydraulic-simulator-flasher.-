# RC Construction Machine Sound & Hydraulic Simulator

Engine sound, hydraulic pump, track rattle, horn, backup beep and more — all from an ESP32 board in your RC construction vehicle.

Based on [TheDIYGuy999's Rc_Engine_Sound_ESP32](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32) sound engine, stripped down and rebuilt for hydraulic construction machines (excavators, dozers, loaders, cranes, graders).

---

## What You Need

### Hardware
| Part | Notes |
|------|-------|
| **ESP32 Dev Board** | ESP32-WROOM-32 or ESP32-D0WD. The [TheDIYGuy999 sound controller PCB v1.2](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32) is recommended but any ESP32 dev board works |
| **Speaker + Amp** | Small 8Ω speaker with a PAM8403 or similar amp. Connect to **GPIO 25** (engine) and **GPIO 26** (horn/aux) |
| **RC Receiver** | FlySky IBUS, Futaba SBUS, Graupner SUMD, or PPM/PWM receiver |
| **USB Cable** | Micro-USB or USB-C depending on your ESP32 board — for flashing and serial monitor |

### Software (Install These First)

**Step 1 — Install VS Code**
1. Go to [https://code.visualstudio.com](https://code.visualstudio.com)
2. Download and install for Windows
3. Open it

**Step 2 — Install PlatformIO**
1. In VS Code, click the **Extensions** icon on the left sidebar (or press `Ctrl+Shift+X`)
2. Search for **PlatformIO IDE**
3. Click **Install**
4. Wait for it to finish — it downloads compilers and tools automatically (takes a few minutes)
5. Restart VS Code when prompted

**Step 3 — Install Python** (needed for the Web Configurator)
1. Go to [https://www.python.org/downloads/](https://www.python.org/downloads/)
2. Download Python 3.10 or newer
3. **IMPORTANT:** Check the box that says **"Add Python to PATH"** during install
4. Click Install

**Step 4 — Install Git** (to download the project)
1. Go to [https://git-scm.com/downloads](https://git-scm.com/downloads)
2. Download and install — use all default options

---

## Getting Started

### Download the Project

Open a terminal (PowerShell or Command Prompt) and run:

```
git clone https://github.com/turbotike/rc-hydraulic-simulator-flasher.-.git HydraulicController
cd HydraulicController
```

Or download the ZIP from GitHub and extract it.

### Open in VS Code

1. Open VS Code
2. Go to **File → Open Folder**
3. Select the `HydraulicController` folder
4. PlatformIO will auto-detect the project and download any missing libraries

---

## Using the Web Configurator

The Web Configurator lets you change all settings, swap sounds, build, and flash — all from your browser.

### Open It

**Option A — Double-click the batch file:**
- Open the `HydraulicController` folder in File Explorer
- Double-click **`Open Configurator.bat`**
- Your browser will open to `http://localhost:8080`

**Option B — From VS Code terminal:**
```
python configure.py
```
This starts the web server and opens your browser automatically.

### What You Can Do

| Tab | What It Does |
|-----|-------------|
| **Machine** | Pick your machine type (Dozer, Excavator, Loader, Crane, etc.) and set the name |
| **Sounds** | Adjust volumes for engine, knock, turbo, horn, hydraulic pump, track rattle, backup beep |
| **RC** | Set which RC channel controls what, reverse channels, enable/disable channels |
| **ESC** | Acceleration, braking, ramp times |
| **Servos** | Min/max/center for each servo output |
| **Sound Lab** | Browse all sounds, preview them, import WAV files, trim/edit, install new sounds |
| **Build** | Pick your COM port, build the firmware, and flash it to the ESP32 — all one click |

### Sound Lab (Live Sound Editor)

1. Go to the **Sound Lab** tab
2. **Browse** sounds on the left — click any sound to load it
3. **Import WAV** — click the Import WAV button to load a .wav file from your computer
4. Use the **loop sliders** to trim the start and end
5. Adjust **speed**, **smoothing**, and **crossfade** (leave crossfade at 0% for startup sounds)
6. Click **Install** to save the .h file to the sounds folder
7. Click the **🗑 trash icon** next to any sound to delete it

---

## Building & Flashing

### From the Web UI (Easiest)
1. Open the Web Configurator
2. Go to the **Build** tab
3. Select your **COM port** from the dropdown (plug in your ESP32 first)
4. Click **Build & Flash**
5. Wait for it to finish — you'll see a green checkmark

### From VS Code
1. Click the **PlatformIO** icon in the left sidebar (alien head icon)
2. Click **Upload** under the esp32 environment
3. Or press `Ctrl+Alt+U`

### From Terminal
```
pio run -t upload --upload-port COM8
```
Replace `COM8` with your actual COM port.

---

## Pin Mapping

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
| 3 | Headlights |
| 22 | Work lights |

---

## Lights

Lights are controlled by the RC lights channel (default CH6). Each press cycles through:

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
| **No sound** | Check speaker is connected to GPIO 25 and/or 26. Check `masterVolume` is not 0 in the web UI |
| **COM port not showing** | Install the CP2102 or CH340 USB driver for your ESP32 board |
| **Upload fails** | Hold the BOOT button on the ESP32 while uploading. Try a different USB cable |
| **Web UI won't start** | Make sure Python is installed and in your PATH. Run `python --version` to check |
| **Settings reset after save** | This is fixed — the web UI now properly preserves TRACK_RATTLE_2 and autoEngineStart |
| **Garbled sound at startup** | The DAC offset fade prevents this — make sure you're on the latest firmware |

---

## License

Based on TheDIYGuy999's Rc_Engine_Sound_ESP32. See original project for license details.
