// =======================================================================================================
// GAMEPAD (Bluepad32) — drive the JD 850P dozer on a PS4 / PS5 / Xbox controller
// =======================================================================================================
// Fills the same pulseWidthRaw[] array the RC receiver would, so the hydrostatic drive, proportional
// valves, relief model and sound all work unchanged. Everything here is driven by the flasher's
// "Controls" tab, which writes src/gamepad_config.h (button map, drive feel, per-output mapping). If that
// file is absent the #ifndef defaults below apply. Only compiled when GAMEPAD_MODE is defined (config.h);
// needs the esp32-bluepad32 board core (the flasher selects it automatically for a gamepad build). Because
// Bluepad32 uses Bluetooth, a gamepad build turns WiFi off (one radio).
//
// Default DS4 layout (single-joystick mix, the config.h default):
//   Left stick  Y  -> drive  (forward / reverse)        Right stick X -> steer
//   Right stick Y  -> blade lift                        R2 / L2       -> ripper up / down
//   Cross = lights   Square = Hi/Lo   Circle = horn      Triangle = engine start/stop
//   Blade tilt / angle are unassigned by default — map them on the Controls tab (they take GPIO32 / GPIO14).
#if defined GAMEPAD_MODE

#include <Bluepad32.h>

// The Controls tab writes these; included first so its #defines win over the defaults below.
#if defined __has_include
#  if __has_include("gamepad_config.h")
#    include "gamepad_config.h"
#  endif
#endif

// ── Drive-feel defaults (flasher overrides via gamepad_config.h) ──────────────────────────────────────
// Bluepad32 analog ranges: sticks -512..511, triggers 0..1023.
#ifndef GP_STEER_DEADZONE
#define GP_STEER_DEADZONE 60
#endif
#ifndef GP_THROTTLE_DEADZONE
#define GP_THROTTLE_DEADZONE 80
#endif
#ifndef GP_STEER_SOURCE
#define GP_STEER_SOURCE 0 // 0 = left stick X (normal: left stick drives + steers, right stick = blade), 1 = right stick X
#endif
#ifndef GP_STEER_INVERT
#define GP_STEER_INVERT 0
#endif
#ifndef GP_THROTTLE_INVERT
#define GP_THROTTLE_INVERT 0
#endif
#ifndef GP_RUMBLE
#define GP_RUMBLE 0 // 1 = engine-feel haptics (idle purr, load-follow, relief bump). Off = save pad battery.
#endif
// GP_TANKMIX is written by the flasher but the dozer always mixes in the firmware (driveMixer), keyed off
// the Machine tab's drive mode (DRIVE_SINGLE_STICK_MIX / DRIVE_DUAL_STICK), so it isn't used here.

// ── Per-output source ids (match the flasher's GP_SOURCES) ────────────────────────────────────────────
#define GP_SRC_NONE 0
#define GP_SRC_LX 1      // left stick X
#define GP_SRC_LY 2      // left stick Y (up = +)
#define GP_SRC_RX 3      // right stick X
#define GP_SRC_RY 4      // right stick Y (up = +)
#define GP_SRC_L2 5      // left trigger (0..max)
#define GP_SRC_R2 6      // right trigger (0..max)
#define GP_SRC_TRIG 7    // R2 - L2, centered
#define GP_SRC_BTN_MOM 8 // button: on while held
#define GP_SRC_BTN_TOG 9 // button: press to toggle

// ── Implement output defaults: BLADE(lift) / TILT / ANGLE / RIPPER (flasher overrides) ────────────────
// SRC 0 = unassigned -> that implement stays centered.
#ifndef GP_BLADE_SRC
#define GP_BLADE_SRC 4   // right stick Y
#endif
#ifndef GP_BLADE_BTN
#define GP_BLADE_BTN 0x0000
#endif
#ifndef GP_BLADE_MIN
#define GP_BLADE_MIN 1000
#endif
#ifndef GP_BLADE_CENTER
#define GP_BLADE_CENTER 1500
#endif
#ifndef GP_BLADE_MAX
#define GP_BLADE_MAX 2000
#endif
#ifndef GP_TILT_SRC
#define GP_TILT_SRC 0
#endif
#ifndef GP_TILT_BTN
#define GP_TILT_BTN 0x0000
#endif
#ifndef GP_TILT_MIN
#define GP_TILT_MIN 1000
#endif
#ifndef GP_TILT_CENTER
#define GP_TILT_CENTER 1500
#endif
#ifndef GP_TILT_MAX
#define GP_TILT_MAX 2000
#endif
#ifndef GP_ANGLE_SRC
#define GP_ANGLE_SRC 0
#endif
#ifndef GP_ANGLE_BTN
#define GP_ANGLE_BTN 0x0000
#endif
#ifndef GP_ANGLE_MIN
#define GP_ANGLE_MIN 1000
#endif
#ifndef GP_ANGLE_CENTER
#define GP_ANGLE_CENTER 1500
#endif
#ifndef GP_ANGLE_MAX
#define GP_ANGLE_MAX 2000
#endif
#ifndef GP_RIPPER_SRC
#define GP_RIPPER_SRC 7  // triggers (R2 - L2)
#endif
#ifndef GP_RIPPER_BTN
#define GP_RIPPER_BTN 0x0000
#endif
#ifndef GP_RIPPER_MIN
#define GP_RIPPER_MIN 1000
#endif
#ifndef GP_RIPPER_CENTER
#define GP_RIPPER_CENTER 1500
#endif
#ifndef GP_RIPPER_MAX
#define GP_RIPPER_MAX 2000
#endif

// ── Digital-function button masks (Bluepad32 buttons() bitfield) ──────────────────────────────────────
#ifndef GP_BTN_HORN
#define GP_BTN_HORN 0x0002 // Circle / B
#endif
#ifndef GP_BTN_ENGINE
#define GP_BTN_ENGINE 0x0008 // Triangle / Y
#endif
#ifndef GP_BTN_LIGHTS
#define GP_BTN_LIGHTS 0x0001 // Cross / A
#endif
#ifndef GP_BTN_HILO
#define GP_BTN_HILO 0x0004 // Square / X
#endif

extern uint16_t pulseWidthRaw[]; // channel array (declared in the .ino); CH_* indices come from config.h

ControllerPtr gpController = nullptr;
volatile bool gamepadConnected = false;

static void gpOnConnect(ControllerPtr ctl)
{
  gpController = ctl;
  gamepadConnected = true;
  Serial.printf("Gamepad connected: %s\n", ctl->getModelName().c_str());
}
static void gpOnDisconnect(ControllerPtr ctl)
{
  if (ctl == gpController)
  {
    gpController = nullptr;
    gamepadConnected = false;
    Serial.println("Gamepad disconnected");
  }
}

void setupGamepad()
{
  Serial.printf("GAMEPAD_MODE — Bluepad32 %s, BT MAC ", BP32.firmwareVersion());
  const uint8_t *a = BP32.localBdAddress();
  Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x\n", a[0], a[1], a[2], a[3], a[4], a[5]);
  BP32.setup(&gpOnConnect, &gpOnDisconnect);
  BP32.enableVirtualDevice(false);
  Serial.println("Put your controller in pairing mode to connect...");
}

// signed analog (-range..range) -> 1000..2000us (1500 center), with deadzone
static uint16_t gpAxisToPulse(int val, int range, int deadzone)
{
  if (val > -deadzone && val < deadzone) return 1500;
  long p = 1500 + (long)val * 500 / range;
  return (uint16_t)constrain(p, 1000, 2000);
}
// centered analog (-512..511) -> min..center..max endpoints, with deadzone
static uint16_t gpMapCentered(int v, uint16_t mn, uint16_t ct, uint16_t mx, int dz)
{
  if (v > -dz && v < dz) return ct;
  long p = (v >= 0) ? (long)ct + (long)v * ((int)mx - (int)ct) / 512
                    : (long)ct + (long)v * ((int)ct - (int)mn) / 512;
  long lo = min(mn, mx), hi = max(mn, mx);
  return (uint16_t)constrain(p, lo, hi);
}
// unipolar (0..1023, e.g. a trigger) -> min..max
static uint16_t gpMapUni(int v, uint16_t mn, uint16_t mx)
{
  long p = (long)mn + (long)v * ((int)mx - (int)mn) / 1023;
  long lo = min(mn, mx), hi = max(mn, mx);
  return (uint16_t)constrain(p, lo, hi);
}
// resolve one mapped implement to servo microseconds. idx (0..3) keys the per-output toggle state.
static uint16_t gpResolve(ControllerPtr c, uint8_t idx, uint8_t src, uint16_t btnMask,
                          uint16_t mn, uint16_t ct, uint16_t mx)
{
  static bool tog[4] = {false, false, false, false};
  static bool prev[4] = {false, false, false, false};
  uint16_t btn = c->buttons();
  switch (src)
  {
  case GP_SRC_LX:   return gpMapCentered(c->axisX(), mn, ct, mx, 40);
  case GP_SRC_LY:   return gpMapCentered(-c->axisY(), mn, ct, mx, 40);
  case GP_SRC_RX:   return gpMapCentered(c->axisRX(), mn, ct, mx, 40);
  case GP_SRC_RY:   return gpMapCentered(-c->axisRY(), mn, ct, mx, 40);
  case GP_SRC_L2:   return gpMapUni(c->brake(), mn, mx);
  case GP_SRC_R2:   return gpMapUni(c->throttle(), mn, mx);
  case GP_SRC_TRIG: return gpMapCentered((c->throttle() - c->brake()) * 512 / 1023, mn, ct, mx, 20);
  case GP_SRC_BTN_MOM: return (btn & btnMask) ? mx : mn;
  case GP_SRC_BTN_TOG:
  {
    bool pressed = (btn & btnMask) != 0;
    if (pressed && !prev[idx]) tog[idx] = !tog[idx];
    prev[idx] = pressed;
    return tog[idx] ? mx : mn;
  }
  default: return ct;
  }
}

void readGamepadCommands()
{
  BP32.update();

  // No pad -> hold everything neutral, idle the engine (failsafe).
  if (!gpController || !gpController->isConnected() || !gpController->isGamepad())
  {
    for (uint8_t i = 1; i < 17; i++) pulseWidthRaw[i] = 1500;
    if (CH_THROTTLE > 0) pulseWidthRaw[CH_THROTTLE] = 1000; // idle
    return;
  }

  ControllerPtr c = gpController;
  for (uint8_t i = 1; i < 17; i++) pulseWidthRaw[i] = 1500; // start neutral, override below

  int lx = c->axisX();   // left stick  X (-512..511)
  int ly = c->axisY();   // left stick  Y (up is negative)
  int rx = c->axisRX();  // right stick X
  int ry = c->axisRY();  // right stick Y
  uint16_t btn = c->buttons();

  // --- Engine throttle: a HELD hand-throttle on the D-pad. Up = more rpm, down = less; it stays put
  //     when you let go. Starts at idle — throttle up to move. Lower rpm = slower tracks (drive droop). ---
  static int gpThrottle = 1500;      // held between frames; starts at idle (you bring it up)
  static uint32_t lastThrMs = 0;
  uint8_t dpadT = c->dpad();          // DS4 bits: up=0x01 down=0x02 right=0x04 left=0x08
  if (millis() - lastThrMs > 25) {    // ~40 Hz so a hold ramps smoothly
    lastThrMs = millis();
    if (dpadT & 0x01) gpThrottle += 15; // D-pad up
    if (dpadT & 0x02) gpThrottle -= 15; // D-pad down
    gpThrottle = constrain(gpThrottle, 1500, 2000); // 1500 = idle, 2000 = full working rpm
  }
  if (CH_THROTTLE > 0) pulseWidthRaw[CH_THROTTLE] = gpThrottle;

  // --- Drive: feed the channels driveMixer expects for the active machine (tracked vs wheeled). ---
  int steerRaw = GP_STEER_SOURCE ? rx : lx;
#if GP_STEER_INVERT
  steerRaw = -steerRaw;
#endif
  int thrRaw = -ly; // left stick up = forward
#if GP_THROTTLE_INVERT
  thrRaw = -thrRaw;
#endif
#if MACHINE_TRACKED
 #if defined DOZER_MODE && defined DRIVE_SINGLE_STICK_MIX
  // Dozer single-stick mix: throttle axis + steer axis, firmware mixes into both tracks.
  if (CH_DZ_DRIVE > 0) pulseWidthRaw[CH_DZ_DRIVE] = gpAxisToPulse(thrRaw, 512, GP_THROTTLE_DEADZONE);
  if (CH_DZ_STEER > 0) pulseWidthRaw[CH_DZ_STEER] = gpAxisToPulse(steerRaw, 512, GP_STEER_DEADZONE);
 #else
  // Tracked dual-stick: left stick Y = left track, right stick Y = right track (tank).
  int lTrk = -ly, rTrk = -ry;
  #if GP_THROTTLE_INVERT
  lTrk = -lTrk; rTrk = -rTrk;
  #endif
  if (outDriveL > 0) pulseWidthRaw[outDriveL] = gpAxisToPulse(lTrk, 512, GP_THROTTLE_DEADZONE);
  if (outDriveR > 0) pulseWidthRaw[outDriveR] = gpAxisToPulse(rTrk, 512, GP_THROTTLE_DEADZONE);
 #endif
#else
  // Wheeled: drive motor from the throttle axis, steer servo from the steer axis.
  if (outDriveR > 0) pulseWidthRaw[outDriveR] = gpAxisToPulse(thrRaw, 512, GP_THROTTLE_DEADZONE);
  if (outDriveL > 0) pulseWidthRaw[outDriveL] = gpAxisToPulse(steerRaw, 512, GP_STEER_DEADZONE);
#endif

  // --- Implements: the 4 assignable gamepad outputs drive the active machine's 4 implement
  //     channels (outImpl[0..3]). If a slot's channel is 0 (e.g. an unused dozer tilt/angle),
  //     borrow a free channel index so the valve model still picks it up. ---
#if GP_BLADE_SRC
  if (outImpl[0] == 0) outImpl[0] = 10;
  pulseWidthRaw[outImpl[0]] = gpResolve(c, 0, GP_BLADE_SRC, GP_BLADE_BTN, GP_BLADE_MIN, GP_BLADE_CENTER, GP_BLADE_MAX);
#endif
#if GP_TILT_SRC
  if (outImpl[1] == 0) outImpl[1] = 11;
  pulseWidthRaw[outImpl[1]] = gpResolve(c, 1, GP_TILT_SRC, GP_TILT_BTN, GP_TILT_MIN, GP_TILT_CENTER, GP_TILT_MAX);
#endif
#if GP_ANGLE_SRC
  if (outImpl[2] == 0) outImpl[2] = 12;
  pulseWidthRaw[outImpl[2]] = gpResolve(c, 2, GP_ANGLE_SRC, GP_ANGLE_BTN, GP_ANGLE_MIN, GP_ANGLE_CENTER, GP_ANGLE_MAX);
#endif
#if GP_RIPPER_SRC
  if (outImpl[3] == 0) outImpl[3] = 13;
  pulseWidthRaw[outImpl[3]] = gpResolve(c, 3, GP_RIPPER_SRC, GP_RIPPER_BTN, GP_RIPPER_MIN, GP_RIPPER_CENTER, GP_RIPPER_MAX);
#endif

  // --- Digital functions -> the toggle/level channels the firmware reads (held = 2000, released = 1000). ---
  if (CH_HORN > 0)          pulseWidthRaw[CH_HORN]          = (btn & GP_BTN_HORN)   ? 2000 : 1000;
  if (CH_ENGINE_TOGGLE > 0) pulseWidthRaw[CH_ENGINE_TOGGLE] = (btn & GP_BTN_ENGINE) ? 2000 : 1000;
  if (CH_HILO_TOGGLE > 0)   pulseWidthRaw[CH_HILO_TOGGLE]   = (btn & GP_BTN_HILO)   ? 2000 : 1000;
  if (CH_LIGHTS > 0)        pulseWidthRaw[CH_LIGHTS]        = (btn & GP_BTN_LIGHTS) ? 2000 : 1000;
}

#endif // GAMEPAD_MODE
