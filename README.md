# RC Construction Machine — Sound, Hydraulics & Lights

Turn one little ESP32 board into the brain of your RC construction machine: **real diesel engine sound, a hydrostatic drive whine that rises with track speed, hydraulic pump whine, track rattle, horn, backup beep, work lights, controller rumble + light bar, and 6 servo/ESC outputs** — for dozers, excavators, loaders, cranes, graders, skid steers and backhoes.

**Made for complete beginners.** No coding, no Arduino IDE, no command line. You download one app, click through some menus, plug in a USB cable, and hit **Flash**. That's it. The app quietly downloads everything it needs the first time.

Drive it with your normal **RC transmitter** *or* a **PS4 / PS5 / Xbox controller** over Bluetooth.

Based on [TheDIYGuy999's RC_Engine_Sound_ESP32](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32) sound engine, rebuilt around hydraulic construction machines.

> 🤖 **Vibe-coded.** This whole thing was built conversationally with an AI coding assistant — a hobbyist and a chatbot going back and forth until it felt right. It works and it's a blast, but treat it like the hobby build it is: **test everything on the bench first**, double-check your wiring, and don't trust it with anything safety-critical.

![logo](logo.png)

---

## 🛠️ It's a living hydraulic system — not just sound

Most RC sound kits just play a clip when you nudge a stick. This one **simulates the machine's hydraulics and driveline**, so it *reacts* to what you're doing — the sound **and** the outputs change with the load, like the real thing. There are no sensors and no extra hardware: it reads what you're doing with the sticks and models the machine from there.

- **Hydrostatic drive.** Track speed is real pump flow — **engine RPM × swashplate**. Throttle up and it goes faster; at idle it just creeps. The swashplate ramps like a real pump-and-motor, so it *builds* and eases off, and pulling back into a turn counter-rotates (spins on the spot).
- **It bogs under load.** Push hard, or work the hydraulics, and the engine **lugs** — RPM sags, the diesel digs in, and the tracks actually **slow down** as it strains, then recover as it catches its breath.
- **The drive sings.** A real recorded **hydrostatic whine** rises and falls with track speed, and the **track rattle clatters faster** the quicker you go. The engine even **ducks back** a touch as the tracks get busy, so the drive sound cuts through instead of piling up.
- **Proportional valves.** Blade, tilt and ripper move smoothly with a deadzone and ramp, and the pump whine rises with how much you're asking of it.
- **You feel it, too.** On a game controller the rumble follows the machine — a diesel idle lope, a thump that pulses with track speed, a swell under load, and a hard jolt when it bogs. The **light bar** glows green → amber → red with the load.
- **It has real consequences.** Flog it at full load for too long and the coolant climbs into the red (lightbar + rumble warning) and the engine **overheats and dies** — you wait for it to cool, then restart. Lug it too hard under load until the RPM collapses and it **stalls**. The turbo whistle **spools up with lag** instead of snapping on. And a built-in **hour meter** logs your run-time like a real service meter. *(All of it is tunable, and can be switched off, in the firmware.)*

The result is a machine that feels like it has weight and power in reserve — and grunts when you put it to work. Fire it up, throttle on, drop the blade into a pile and *push* — and listen to it dig in.

---

## Contents

1. [What you need](#what-you-need)
2. [Quick start (5 steps)](#quick-start-5-steps)
3. [The configurator tabs](#the-configurator-tabs)
4. [Wiring it up](#wiring-it-up)
5. [Driving with a game controller](#driving-with-a-game-controller)
6. [Work lights](#work-lights)
7. [Real hydraulics (Hydraulic mode)](#real-hydraulics-hydraulic-mode)
8. [Sounds — use the Sound Technician](#sounds--use-the-sound-technician)
9. [Reversing an output that runs backwards](#reversing-an-output-that-runs-backwards)
10. [Troubleshooting](#troubleshooting)
11. [FAQ](#faq)

---

## What you need

| Part | Notes |
|------|------|
| **ESP32 board** | Must be a **classic ESP32** (ESP32-WROOM-32 / D0WD). **S3, S2, C3, C6 do NOT work** — they have no built-in sound (DAC) hardware. |
| **The DIYGuy999 Sound & Light Controller PCB** | v1.2 SMD 30-pin board — the recommended carrier. It breaks out all the headers you'll wire to (CH1–CH6, ESC, 32, lights, speaker). |
| **Small speaker + amp** | 4–8 Ω speaker on a PAM8403 (or any tiny Class-D amp). The board has one on it. |
| **Motor drivers** | An **ESC** for each track/drive motor, and an **H-bridge / actuator driver** for each 12 V linear actuator, or a servo for each blade function. The board sends *signals*, not power. |
| **RC receiver *or* a game controller** | RC: FlySky iBUS, Futaba SBUS, or PWM. Or a PS4 / PS5 / Xbox pad (Bluetooth — no receiver needed). |
| **5 V power** | A BEC/UBEC from your ESC, or a 5 V battery, into the board's 5 V input. |
| **USB cable** | To flash it the first time. Not needed after that. |

---

## Quick start (5 steps)

### 1. Get the configurator
Go to the **[Releases page](../../releases)** and download the app for your computer:
- **Windows** → `RC-Hydraulic-Configurator-Windows.zip`
- **Mac** → `RC-Hydraulic-Configurator-macOS.zip`

Unzip it and run **RC Hydraulic Configurator**. Your web browser opens the configurator automatically.

> First run only: when you Flash, it downloads the ESP32 build tools (a few minutes, one time). After that it's instant.

### 2. Pick your machine
On the **Machine** tab, choose your machine (**Dozer, Excavator, Loader, Crane, Grader, Skid Steer, or Backhoe**). On the **Controls** tab, choose how you'll drive it — **RC transmitter** (and the bus: IBUS / SBUS / PWM) or a **Game controller**.

### 3. Set your sounds & levels *(optional)*
The **Sound Technician** tab lets you pick engine/pump/horn sounds. The **Levels** tab sets how loud each one is. You can skip this and tune it later.

### 4. Plug in the ESP32 and Flash
Connect the board with USB, then click **⚡ Flash** at the top. It finds the board, builds, and uploads. Watch the log — when it says done, it's on the board.

> ⚠️ **Never plug in USB while the battery / BEC is powering the board.** Feeding it from two power sources at once can back-feed the regulator and damage the board or your USB port. **Disconnect the battery before you plug in USB to flash**, and unplug USB before you power it from the battery.

### 5. Wire it up and go
Plug your ESCs/servos into the output headers (see [Wiring](#wiring-it-up)), power it up, and press the engine-start button (Triangle on a controller). 🚜

---

## The configurator tabs

| Tab | What it does |
|-----|--------------|
| **🚜 Machine** | Pick the machine type and drive mode, the feel tuning (drive-stick expo, reversing beeper, light-bar colour, rattle ducking), and the **Hydraulic mode** toggle for real-pump builds (see [Real hydraulics](#real-hydraulics-hydraulic-mode)). |
| **🎚️ Levels** | Volume mixer — master volume, engine, pump, drive whine, rattle, horn, etc. Nothing can clip: both audio channels have a built-in soft limiter, so crank away. |
| **🔊 Sound Technician** | Choose the actual sound for each slot, preview it, or upload your own WAV. |
| **🎮 Controls** | Pick RC transmitter (and bus: IBUS/SBUS/PWM) or game controller, map sticks/buttons to each function, and reverse any output. |
| **⚡ Flash** | Build and upload to the board. |

**One Save button.** The **Save** button at the top saves *everything* across all tabs. **Flash** saves first, then uploads — so you never flash a stale setup.

---

## Wiring it up

Every machine drives the same **6 output headers** on the board. What each one does follows the machine you picked — the labels in the app tell you exactly where to plug in. For a **dozer**:

| Board header | Drives | Plug in |
|--------------|--------|---------|
| **CH1** | Right track | Track ESC |
| **CH2** | Left track | Track ESC |
| **ESC** | Blade lift | Actuator driver / servo |
| **CH4** | Blade tilt | Actuator driver / servo |
| **CH3** | Blade angle | Actuator driver / servo |
| **32** | Ripper | Actuator driver / servo |

> Switch the machine (e.g. Excavator) and the same headers become boom / stick / bucket / swing — the Controls tab always shows the current labels.

**Sound:** the board's speaker output comes off the two DAC pins (GPIO 25 = engine + pump, GPIO 26 = knock / horn / track rattle / drive whine) into the onboard amp → your speaker.

**Power:** 5 V into the board's 5 V input (from a BEC or a 5 V battery). Never feed more than 3.3 V into a signal pin. **Never have USB and the battery/BEC connected at the same time** — flash over USB with the battery *off*, run off the battery with USB *unplugged*.

> **CH5 and CH6 are inputs only** (they're input-only pins on the ESP32) — you can read two extra RC channels on them, but you can't drive anything from them. Your 6 outputs live on CH1 / CH2 / CH3 / CH4 / ESC / 32.

---

## Driving with a game controller

Pick **Game controller** on the **Controls** tab and Flash. Then pair your pad:

**PS4 (DualShock 4):** with the controller off, hold **SHARE + PS** for ~3–5 s until the light bar **double-flashes**. The board grabs it — light bar goes **solid** = connected.

*(PS5 DualSense = Create + PS · Xbox = the small pair button on top.)*

Once paired, the board **remembers** your controller — on later power-ups it reconnects on its own the moment you tap **PS** (no re-pairing). Bluetooth comes up first thing at boot, so it's ready to grab the pad almost immediately after the battery goes in. To pair a *different* controller, hold the ESP32's **BOOT** button for ~3 s while it's running.

**Handy in-game shortcuts** (gamepad): hold **both bumpers (L1 + R1)** and use the **right stick** — up/down = **master volume**, a left/right flick = **vibration on/off**. Hold **both bumpers + tap D-pad ◂** to **blink the engine hours** out on the light bar (**long flash = tens, short = ones** — so `12 h` = 1 long + 2 short). The controller **light bar** shows engine load (green idle → amber → red bog) or a fixed color you pick on the Machine tab.

**Range:** the board runs its Bluetooth at **full transmit power** for the best reach. If you need more, swap to an ESP32 with an **external antenna** (a WROOM-**32U** module + a 2.4 GHz whip) — same chip, same firmware, just drops into the header.

> **Connect time:** a **genuine** DS4/DualSense/Xbox pad connects in a couple of seconds. **Generic / clone** controllers have their own Bluetooth quirks and can take a while (or need a couple of tries) to pair — that's the clone, not the firmware. If one won't cooperate, a real first-party pad is the fix.

**Default dozer layout:**

| Control | Does |
|---------|------|
| **Left stick** | Drive + steer |
| **Right stick ↕** | Blade lift |
| **L2 / R2** | Ripper down / up |
| **D-pad ↕** | Throttle (hand-throttle — it holds where you leave it) |
| **✕** | Work lights (cycles off → front → rear → side) |
| **□** | Hi / Lo gear |
| **○** | Horn |
| **△** | Engine start / stop |

Nothing runs until you **press △ to start the engine**. Blade **tilt** and **angle** are unassigned by default — map them (to the bumpers or D-pad ◂▸) in **Controls → Output mapping**.

---

## Work lights

Press the **lights button** (✕ on a controller, or your lights channel on RC) to step through — each press **adds** the next set and they stay on:

**off → front → front + rear → front + rear + side → off**

**Where to plug them in.** The board has three LED-driver headers, each a simple on/off output that switches your LED to ground:

| Set | Board header | ESP32 pin |
|-----|--------------|-----------|
| **Front** (headlights) | **HEADL** | GPIO 3 |
| **Rear** (tail / work) | **FOGL** | GPIO 16 |
| **Side** (roof / beacon) | **ROOFL** | GPIO 5 |

Wire your LED light bar between the header's output and the board's **+** and **GND** for that channel (follow the board's silkscreen), and **always put a current-limiting resistor in series** with the LEDs unless your light bar already has one. These are low-current LED-driver outputs — for anything bigger than a few small LEDs, switch it through a transistor/MOSFET.

---

## Real hydraulics (Hydraulic mode)

Building an **actual hydraulic machine** — a real pump and valves, like a Burnie 3D-printed excavator — rather than the servo-driven sim? Flip on **Hydraulic mode** on the Machine tab and the board drives your **hydraulic pump ESC from the engine RPM**: it idles low and revs up with the throttle, so you **rev up to dig**, just like the real thing (and it's held at minimum at boot so a brushless ESC arms cleanly). Your **hydraulic control valves stay on the receiver's PWM**; the board runs the pump plus your electric **drive and swing** motors, all RPM-linked so the whole machine "wakes up" as you throttle on.

- **Pump signal** comes off the **ESC header (GPIO 33)** — wire your pump ESC there. Tracks stay on CH1/CH2, swing on the **32** header.
- Set the **idle pump speed** (`pumpIdlePercent`, ~25% default) high enough that a brushless motor spins smooth without cogging and holds a little pressure.
- Hydraulic mode is **RC only** — a Bluetooth pad can't run your valves — so turning it on automatically disables the gamepad option on the Controls tab.

---

## Sounds — use the Sound Technician

Each sound slot has a dropdown that shows **only the sounds that fit it** (the start slot lists start clips, idle lists idles, etc.). Hit **▶** to preview.

**Record your own machine!** Click **＋ Change → ⬆ Upload WAV** on any slot. A mono WAV (~22 kHz works best) gets converted and installed automatically.
- **Start** = one-shot crank.
- **Idle** and **Rev** should be **steady, loopable** recordings of the *same engine* at low vs high RPM (the sim crossfades and loops them).
- Name files with the keyword (…`Start.wav`, …`Idle.wav`, …`Rev.wav`) so they land in the right dropdown.

Delete clips you don't want with the **🗑** in the browser (you can't delete one that's currently in use — pick another for that slot first).

---

## Reversing an output that runs backwards

Motors and actuators get wired however they land, so any output might run the wrong way. In **Controls → Reverse output direction**, each of the 6 outputs has a **Reverse** toggle. If a track drives backward or an actuator extends when it should retract, flip its toggle, **Save**, and **Flash**. Once per output and it's set.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| **Flash can't find the board** | Try a different USB cable/port (some cables are charge-only). Install the CP2102 or CH340 USB driver for your board. |
| **No sound** | Check the speaker/amp wiring and the **Master volume** on the Levels tab. Make sure it's a **classic ESP32** (S3/C3 have no sound hardware). |
| **Engine won't start** | It doesn't auto-start — press **△** (controller) or your engine-toggle channel. |
| **A track drives the wrong way** | Flip that output in **Controls → Reverse output direction**. |
| **Reverse won't engage — it brakes until I let the stick back to neutral** | Your track **ESC is in Forward/Brake/Reverse mode** (the default on 1060-style ESCs — the first pull-back is a *brake*, and it needs to see neutral before it'll reverse). Best fix: set the ESC to **Forward/Reverse (no brake)** — usually a jumper (the **F/R** position, *not* "no jumper") or a throttle-stick program step. The firmware also holds a real neutral on each direction flip (`reverseArmMs`, 450 ms) to re-arm a brake-mode ESC for you; set it to `0` if your ESC is already no-brake. |
| **Engine dies on its own under heavy load** | That's the sim — it **overheated** (watch for the red lightbar flash; let it cool, then restart) or you **lugged it to a stall**. Ease off, or turn the systems sim down/off in the firmware (`simSystemsEnabled`). |
| **Controller drops out at range** | Bluetooth is already at full TX power. For more reach use an **external-antenna ESP32** (WROOM-32U + a 2.4 GHz whip). A genuine first-party pad also holds the link better than a clone at the fringe. |
| **Controller won't pair / slow to connect** | Re-do **SHARE + PS** until the light bar double-flashes; make sure you flashed the **Game controller** build and the pad isn't connected to a phone/PS4. **Generic/clone controllers can be slow or need a few tries** — a genuine DS4/DualSense pairs fastest. Hold **BOOT** ~3 s to wipe bonds and pair fresh. |
| **Faint hiss/Bluetooth noise from the speaker with the engine off** | This is the ESP32's Bluetooth radio coupling into the audio hardware — it's a board-layout trait, not a bug, and it's masked once the engine's running. If it bugs you, it's a hardware fix: a decoupling cap on the amp input, a ferrite on the speaker leads, or wire the amp's **mute/shutdown pin** so the firmware can cut it when idle (ask if your amp has one). |
| **Page stuck / looks wrong after an update** | Hard-refresh the browser: **Ctrl + Shift + R**. Only run **one** copy of the app at a time. |
| **"Nothing to save"** | You haven't changed anything since the last save. |

---

## FAQ

**Do I need to know how to code?** No. You never touch code — the app does everything.

**RC or a game controller — which is better?** RC if you already fly with a transmitter. A game controller is the easiest way to just pick it up and drive (Bluetooth, no receiver).

**Can I run more than one machine type?** The firmware builds for one machine at a time. Change it on the Machine tab and re-flash to switch.

**Which machines actually work?** All 7 build and run. The **dozer** is the most-tested; the others share the same proven core — bench-test each when you build it.

**Do I need the internet?** Only the very first Flash (to download the build tools once) and to grab your own sounds. After that it runs offline.

**Where do my settings live?** In the machine's config — saved when you press **Save**, and baked into the firmware when you **Flash**.

**How do I read the engine hours?** Two ways. Plugged in: on the **Flash** tab there's an **Engine hours** card — pick your port and hit **Read hours** (or **Reset to 0** for a fresh build). Trackside: **hold both bumpers + tap D-pad ◂** on a gamepad (or, on RC, engine off + hold the horn ~2 s) and it **blinks the hours out on the light** — long flash = tens, short = ones.

**Can I keep configs for several machines?** Yes — the **Machine tab → Vehicle Profiles** lets you save, export, and re-import a full config per machine. You still flash one at a time, but you don't have to re-enter everything.

---

Built on the open-source [TheDIYGuy999 sound engine](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32). Have fun building. 🚜
