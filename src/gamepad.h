// =======================================================================================================
// GAMEPAD (Bluepad32) — drive the dozer on a PS4 / PS5 / Xbox controller
// =======================================================================================================
// Fills the same pulseWidthRaw[] array the RC receiver would, so the hydrostatic drive, valves, relief
// and sound all work unchanged. Only compiled when GAMEPAD_MODE is defined (config.h). Needs the
// esp32-bluepad32 board core — the flasher selects it automatically for a gamepad build. Because it uses
// Bluetooth, a gamepad build turns any WiFi off (one radio).
#if defined GAMEPAD_MODE

#include <Bluepad32.h>

extern uint16_t pulseWidthRaw[];
ControllerPtr gpController = nullptr;

static void gpOnConnect(ControllerPtr c) { gpController = c; Serial.printf("Gamepad: %s\n", c->getModelName().c_str()); }
static void gpOnDisconnect(ControllerPtr c) { if (c == gpController) gpController = nullptr; }

void setupGamepad() {
  Serial.printf("GAMEPAD_MODE — Bluepad32 %s\n", BP32.firmwareVersion());
  BP32.setup(&gpOnConnect, &gpOnDisconnect);
  BP32.enableVirtualDevice(false);
  Serial.println("Put the controller in pairing mode to connect...");
}

static uint16_t gpAxisUs(int v) {                 // stick -512..511 -> 1000..2000us
  long p = 1500 + (long)v * 500 / 512;
  return (uint16_t)constrain(p, 1000, 2000);
}

// Default JD 850P pad map (single-joystick-mix drive):
//   Left stick   X/Y -> steer (CH1) / drive (CH2)     Right stick Y -> blade lift (CH3)
//   Triggers R2/L2   -> ripper up/down (CH4)          Right stick X -> blade angle (CH10, if assigned)
//   D-pad L/R        -> blade tilt (CH11, if assigned) Cross=lights(6) Square=Hi/Lo(7) Circle=horn(8) Triangle=engine(9)
void readGamepadCommands() {
  BP32.update();
  if (!gpController || !gpController->isConnected() || !gpController->isGamepad()) {
    for (int i = 1; i <= 12; i++) pulseWidthRaw[i] = 1500; // failsafe: neutral
    pulseWidthRaw[5] = 1000;                                // throttle idle
    return;
  }
  ControllerPtr c = gpController;
  pulseWidthRaw[1] = gpAxisUs(c->axisX());    // steer
  pulseWidthRaw[2] = gpAxisUs(-c->axisY());   // drive (up = forward)
  pulseWidthRaw[3] = gpAxisUs(-c->axisRY());  // blade lift
  long rip = 1500 + (long)(c->throttle() - c->brake()) * 500 / 1023; // R2 up / L2 down
  pulseWidthRaw[4] = (uint16_t)constrain(rip, 1000, 2000);            // ripper
  pulseWidthRaw[5] = 2000;                     // dozer throttle (forward-only, governor handles rpm)
  uint16_t b = c->buttons();
  pulseWidthRaw[6] = (b & 0x0001) ? 2000 : 1500; // Cross   -> lights
  pulseWidthRaw[7] = (b & 0x0004) ? 2000 : 1500; // Square  -> Hi/Lo
  pulseWidthRaw[8] = (b & 0x0002) ? 2000 : 1500; // Circle  -> horn
  pulseWidthRaw[9] = (b & 0x0008) ? 2000 : 1500; // Triangle-> engine on/off
  pulseWidthRaw[10] = gpAxisUs(c->axisRX());     // blade angle (assign CH_DZ_ANGLE = 10)
  uint8_t dp = c->dpad();
  pulseWidthRaw[11] = (dp & 0x08) ? 1000 : ((dp & 0x04) ? 2000 : 1500); // tilt (assign CH_DZ_TILT = 11)
}

#endif // GAMEPAD_MODE
