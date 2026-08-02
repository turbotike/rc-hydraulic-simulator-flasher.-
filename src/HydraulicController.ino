/*
 * HydraulicController.ino
 * RC Construction Machine — Engine Sound & Hydraulic Simulator
 *
 * Based on TheDIYGuy999's Rc_Engine_Sound_ESP32 v9.15.0
 * Stripped to essentials: engine sound + hydraulic simulation + RC + ESC + servos
 *
 * ESP32 CPU must be 240 MHz!
 *
 * Audio: two 8-bit DAC channels driven by hardware timer ISRs
 *   DAC1 (GPIO 25): Engine idle/rev crossfade + turbo + fan + hydraulic pump (variable rate = RPM)
 *   DAC2 (GPIO 26): Diesel knock + horn + siren + air brake + hydraulic flow + track rattle + bucket (fixed ~22kHz)
 *
 * RC: SBUS / IBUS / SUMD / PPM / PWM on GPIO 36
 * ESC: MCPWM on GPIO 33
 * Servos: MCPWM on GPIO 13, 12, 14, 27
 */

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "soundpack.h"
#include "gamepad.h"

// ── Libraries ──────────────────────────────────────────────────
#if defined SBUS_COMMUNICATION && !defined EMBEDDED_SBUS
#include <SBUS.h>
#endif
#include <rcTrigger.h>
#include <IBusBM.h>
#include <ESP32AnalogRead.h>

// ── Included headers ───────────────────────────────────────────
#include "lib/curves.h"
#include "lib/helper.h"
#include "lib/SUMD.h"
#if defined EMBEDDED_SBUS
#include "lib/sbus.h"
#endif

// ── ESP-IDF drivers ────────────────────────────────────────────
#include "driver/uart.h"
#include "driver/rmt.h"
#include "driver/mcpwm.h"
#include "soc/rtc_wdt.h"
#include <Esp.h>

// ── Forward declarations ───────────────────────────────────────
void Task1code(void *parameters);
void readSbusCommands();
void readIbusCommands();
void readSumdCommands();
void readPpmCommands();
void readPwmSignals();
void processRawChannels();
void failsafeRcSignals();

// ════════════════════════════════════════════════════════════════
// PIN & GLOBAL STATE
// ════════════════════════════════════════════════════════════════

// PWM RC input
#define PWM_CHANNELS_NUM 6
const uint8_t PWM_CHANNELS[PWM_CHANNELS_NUM] = {1, 2, 3, 4, 5, 6};
const uint8_t PWM_PINS[PWM_CHANNELS_NUM] = {PWM_CH1_PIN, PWM_CH2_PIN, PWM_CH3_PIN, PWM_CH4_PIN, PWM_CH5_PIN, PWM_CH6_PIN};

// RC trigger objects
rcTrigger functionR100u(200);
rcTrigger functionR100d(100);
rcTrigger functionR75u(300);
rcTrigger functionR75d(300);
rcTrigger functionL100l(100);
rcTrigger functionL100r(100);

// Battery
ESP32AnalogRead battery;

// PWM RMT
#define RMT_TICK_PER_US 1
#define RMT_RX_CLK_DIV (80000000 / RMT_TICK_PER_US / 1000000)
#define RMT_RX_MAX_US 3500
volatile uint16_t pwmBuf[PWM_CHANNELS_NUM + 2] = {0};
uint32_t maxPwmRpmPercentage = 390;

// PPM
#define NUM_OF_PPM_CHL 8
#define NUM_OF_PPM_AVG 1
volatile int ppmInp[NUM_OF_PPM_CHL + 1] = {0};
volatile int ppmBuf[16] = {0};
volatile byte counter = NUM_OF_PPM_CHL;
volatile byte average = NUM_OF_PPM_AVG;
volatile boolean ready = false;
volatile unsigned long timelast;
unsigned long timelastloop;
uint32_t maxPpmRpmPercentage = 390;

// SBUS
#if defined SBUS_COMMUNICATION
  #if not defined EMBEDDED_SBUS
  SBUS sBus(Serial2);
  uint16_t SBUSchannels[16];
  bool SBUSfailSafe;
  bool SBUSlostFrame;
  #else
  bfs::SbusRx sBus(&Serial2);
  std::array<int16_t, bfs::SbusRx::NUM_CH()> SBUSchannels;
  #endif
  bool sbusInit;
#endif
uint32_t maxSbusRpmPercentage = 390;

// SUMD
HardwareSerial serial(2);
SUMD sumd(serial);
uint16_t SUMDchannels[16];
bool SUMD_failsafe;
bool SUMD_frame_lost;
bool SUMD_init;
uint32_t maxSumdRpmPercentage = 390;

// IBUS
IBusBM iBus;
bool ibusInit;
uint32_t maxIbusRpmPercentage = 320;

// RC channels
#define PULSE_ARRAY_SIZE 17
uint16_t pulseWidthRaw[PULSE_ARRAY_SIZE];
uint16_t pulseWidthRaw2[PULSE_ARRAY_SIZE];
uint16_t pulseWidthRaw3[PULSE_ARRAY_SIZE];
uint16_t pulseWidth[PULSE_ARRAY_SIZE];
int16_t pulseOffset[PULSE_ARRAY_SIZE];
uint16_t pulseMaxNeutral[PULSE_ARRAY_SIZE];
uint16_t pulseMinNeutral[PULSE_ARRAY_SIZE];
uint16_t pulseMax[PULSE_ARRAY_SIZE];
uint16_t pulseMin[PULSE_ARRAY_SIZE];
uint16_t pulseMaxLimit[PULSE_ARRAY_SIZE];
uint16_t pulseMinLimit[PULSE_ARRAY_SIZE];
uint16_t pulseZero[PULSE_ARRAY_SIZE];
uint16_t pulseLimit = 1100;
uint16_t pulseMinValid = 700;
uint16_t pulseMaxValid = 2300;
bool autoZeroDone;
#define NONE 0

volatile boolean failSafe = false;

// Control flags
boolean mode1;
boolean mode2;
boolean momentary1;
boolean hazard;
boolean left;
boolean right;

// Sound state
volatile boolean engineOn = false;  // Engine OFF at power-up — user cranks it (CH9 / Triangle). No auto-start.
volatile boolean engineStart = false;
volatile boolean engineRunning = false;
volatile boolean tracksAreRotating = false;
volatile boolean engineStop = false;
volatile boolean jakeBrakeRequest = false;
volatile boolean engineJakeBraking = false;
volatile boolean wastegateTrigger = false;
volatile boolean blowoffTrigger = false;
volatile boolean dieselKnockTrigger = false;
volatile boolean trackRattle2Trigger = false;
volatile boolean dieselKnockTriggerFirst = false;
volatile boolean airBrakeTrigger = false;
volatile boolean shiftingTrigger = false;
volatile boolean hornTrigger = false;
volatile boolean sirenTrigger = false;
volatile boolean sound1trigger = false;
volatile boolean couplingTrigger = false;
volatile boolean uncouplingTrigger = false;
volatile boolean bucketRattleTrigger = false;
volatile boolean indicatorSoundOn = false;
volatile boolean outOfFuelMessageTrigger = false;

// Lights state machine
// Modes: 0=off, 1=front lights, 2=work lights, 3=all
int8_t lightsMode = 0;
int8_t lightsState = 0;        // 0=off, 1=on (current mode active)
volatile boolean lightsOn = false;

volatile boolean hornLatch = false;
volatile boolean sirenLatch = false;

// Sound volumes
volatile uint16_t throttleDependentVolume = 0;
volatile uint16_t throttleDependentRevVolume = 0;
volatile uint16_t rpmDependentJakeBrakeVolume = 0;
volatile uint16_t throttleDependentKnockVolume = 0;
volatile uint16_t rpmDependentKnockVolume = 0;
volatile uint16_t throttleDependentTurboVolume = 0;
volatile uint16_t throttleDependentFanVolume = 0;
volatile uint16_t throttleDependentChargerVolume = 0;
volatile uint16_t rpmDependentWastegateVolume = 0;
volatile uint16_t tireSquealVolume = 0;

// Hydraulic volumes
volatile uint16_t hydraulicPumpVolume = 0;
volatile uint16_t hydraulicPumpVolumeArray[17];
volatile uint16_t hydraulicFlowVolume = 0;
volatile uint16_t trackRattleVolume = 0;
volatile uint16_t trackRattle2Volume = 0;
volatile uint16_t driveWhineVolume = 0;  // hydrostatic drive whine level/speed (0..100 from track speed)
volatile uint32_t trackRattle2TriggerInterval = 0;
volatile uint16_t hydraulicDependentKnockVolume = 100;
volatile uint16_t hydraulicLoad = 0;
volatile bool engineLugging = false; // dozer governor: engine bogged under heavy load
int16_t totalFlowDemand = 0;         // dozer: drive + implement pump demand (read by the governor)

volatile uint8_t dacOffset = 0;

// Throttle
int16_t currentThrottle = 0;
int16_t currentThrottleHydraulic = 0;
int16_t currentThrottleFaded = 0;

// Engine
const int16_t maxRpm = 500;
const int16_t minRpm = 0;
int32_t currentRpm = 0;
int16_t cmdTrackL = 0, cmdTrackR = 0;       // commanded track effort ±500 (declared early: read by the governor)
int16_t swashL = 0, swashR = 0;             // ramped swashplate state ±500
int32_t targetHydraulicRpm[17];
volatile uint8_t engineState = 0;
enum EngineState { OFF, STARTING, RUNNING, STOPPING };
int16_t engineLoad = 0;
volatile uint16_t engineSampleRate = 0;
int32_t speedLimit = maxRpm;

// Clutch
boolean clutchDisengaged = true;

// Transmission
uint8_t selectedGear = 1;
uint8_t selectedAutomaticGear = 1;
boolean gearUpShiftingInProgress;
boolean doubleClutchInProgress;
boolean gearDownShiftingInProgress;
boolean gearUpShiftingPulse;
boolean gearDownShiftingPulse;
volatile boolean neutralGear = false;

// ESC
volatile boolean escIsBraking = false;
volatile boolean escIsDriving = false;
volatile boolean escInReverse = false;
volatile boolean brakeDetect = false;
int8_t driveState = 0;
uint16_t escPulseMax = 2000;
uint16_t escPulseMin = 1000;
uint16_t escPulseMaxNeutral = 1500;
uint16_t escPulseMinNeutral = 1500;
uint16_t currentSpeed = 0;
volatile bool crawlerMode = false;

// Battery
float batteryCutoffvoltage;
float batteryVoltage;
uint8_t numberOfCells;
bool batteryProtection = false;
volatile bool lowBatteryTrigger = false; // fires the spoken "low battery" voice once
volatile bool lowBatteryLatch   = false; // set while that voice is playing out

// FreeRTOS
TaskHandle_t Task1;

// Timers
uint32_t maxSampleInterval = 4000000 / sampleRate;
uint32_t minSampleInterval = 4000000 / sampleRate * 100 / MAX_RPM_PERCENTAGE;

hw_timer_t *variableTimer = NULL;
portMUX_TYPE variableTimerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t variableTimerTicks = 4000000 / sampleRate;

hw_timer_t *fixedTimer = NULL;
portMUX_TYPE fixedTimerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t fixedTimerTicks = 4000000 / sampleRate;

SemaphoreHandle_t xPwmSemaphore;
SemaphoreHandle_t xRpmSemaphore;

uint16_t loopTime;

// ════════════════════════════════════════════════════════════════
// VARIABLE-RATE PLAYBACK ISR (Engine sounds → DAC1)
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR variablePlaybackTimer() {

#if defined GAMEPAD_MODE
  // Until a controller is connected there's nothing to hear (engine's off) — so hold the DAC at
  // center and skip all the sample reads. Those reads hit flash ~44k times/sec, and that flash-bus
  // traffic jitters the Bluetooth radio on the other core. Silencing it frees the bus so the pad
  // latches on fast/clean. Full audio resumes the instant the controller connects.
  if (!gamepadConnected) {
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, dacOffset, RTC_IO_PDAC1_DAC_S);
    return;
  }
#endif

  static uint32_t attenuatorMillis = 0;
  static uint32_t curEngineSample = 0;
  static uint32_t curRevSample = 0;
  static uint32_t curTurboSample = 0;
  static uint32_t curFanSample = 0;
  static uint32_t curChargerSample = 0;
  static uint32_t curStartSample = 0;
  static uint32_t curJakeBrakeSample = 0;
  static uint32_t curHydraulicPumpSample = 0;
  static uint32_t curTrackRattleSample = 0;
  static uint32_t lastDieselKnockSample = 0;
  static uint16_t attenuator = 0;
  static uint16_t speedPercentage = 0;
  static int32_t a, a1, a2, a3, b, c, d, e = 0;
  static int32_t f = 0; // hydraulic pump
  static int32_t g = 0; // track rattle (steam loco — unused here but kept for compat)
  uint8_t a1Multi = 0;

  switch (engineState) {

  case OFF:
    variableTimerTicks = 4000000 / startSampleRate;
    timerAlarmWrite(variableTimer, variableTimerTicks, true);
    a = 0;
    if (engineOn && dacOffset >= 128) {  // Wait for DAC offset fade before starting
      engineState = STARTING;
      engineStart = true;
    }
    break;

  case STARTING:
    variableTimerTicks = 4000000 / startSampleRate;
    timerAlarmWrite(variableTimer, variableTimerTicks, true);

    if (curStartSample < startSampleCount - 1) {
      a = (startSamples[curStartSample] * startVolumePercentage / 100);
      curStartSample++;
    } else {
      curStartSample = 0;
      engineState = RUNNING;
      engineStart = false;
      engineRunning = true;
      airBrakeTrigger = true;
    }
    break;

  case RUNNING:
    // Variable sample rate (calculated in engineMassSimulation)
    variableTimerTicks = engineSampleRate;
    timerAlarmWrite(variableTimer, variableTimerTicks, true);

    // Idle sound
    if (curEngineSample < sampleCount - 1) {
      a1 = (samples[curEngineSample] * throttleDependentVolume / 100 * idleVolumePercentage / 100);
      curEngineSample++;

      // Knock trigger
      if (curEngineSample - lastDieselKnockSample > (sampleCount / dieselKnockInterval)) {
        dieselKnockTrigger = true;
        lastDieselKnockSample = curEngineSample;
      }
    } else {
      curEngineSample = 0;
      lastDieselKnockSample = 0;
      dieselKnockTriggerFirst = true;
    }
#ifdef REV_SOUND
    // Rev sound
    if (curRevSample < revSampleCount - 1) {
      a2 = (revSamples[curRevSample] * throttleDependentRevVolume / 100 * revVolumePercentage / 100);
      curRevSample++;
    } else {
      curRevSample = 0;
    }

    // Crossfade: idle ↔ rev based on RPM
    if (currentRpm > revSwitchPoint)
      a1Multi = map(currentRpm, idleEndPoint, revSwitchPoint, 0, idleVolumeProportionPercentage);
    else
      a1Multi = idleVolumeProportionPercentage;
    if (currentRpm > idleEndPoint)
      a1Multi = 0;

    a1 = a1 * a1Multi / 100;
    a2 = a2 * (100 - a1Multi) / 100;
    a = a1 + a2;
#else
    a = a1;
#endif

    // Throttle-dependent volume
    a = a * fullThrottleVolumePercentage / 100;

    // Turbo sound
    if (curTurboSample < turboSampleCount - 1) {
      b = (turboSamples[curTurboSample] * throttleDependentTurboVolume / 100 * turboVolumePercentage / 100);
      curTurboSample++;
    } else {
      curTurboSample = 0;
    }

    // Fan sound
    if (curFanSample < fanSampleCount - 1) {
      d = (fanSamples[curFanSample] * throttleDependentFanVolume / 100 * fanVolumePercentage / 100);
      curFanSample++;
    } else {
      curFanSample = 0;
    }

    // Supercharger
    if (curChargerSample < chargerSampleCount - 1) {
      e = (chargerSamples[curChargerSample] * throttleDependentChargerVolume / 100 * chargerVolumePercentage / 100);
      curChargerSample++;
    } else {
      curChargerSample = 0;
    }

    // Hydraulic pump sound
#if defined EXCAVATOR_MODE || defined CRANE_MODE || defined DOZER_MODE || defined GRADER_MODE || defined BACKHOE_MODE
    if (curHydraulicPumpSample < hydraulicPumpSampleCount - 1) {
      f = (hydraulicPumpSamples[curHydraulicPumpSample] * hydraulicPumpVolumePercentage / 100 * hydraulicPumpVolume / 100);
      curHydraulicPumpSample++;
    } else {
      curHydraulicPumpSample = 0;
    }
#endif

    if (!engineOn) {
      speedPercentage = 100;
      attenuator = 1;
      engineState = STOPPING;
      engineStop = true;
      engineRunning = false;
    }
    break;

  case STOPPING:
    variableTimerTicks = 4000000 / sampleRate * speedPercentage / 100;
    timerAlarmWrite(variableTimer, variableTimerTicks, true);

    if (curEngineSample < sampleCount - 1) {
      a = (samples[curEngineSample] * throttleDependentVolume / 100 * idleVolumePercentage / 100 / attenuator);
      curEngineSample++;
    } else {
      curEngineSample = 0;
    }

    if (millis() - attenuatorMillis > 100) {
      attenuatorMillis = millis();
      attenuator++;
      speedPercentage += 20;
    }

    if (attenuator >= 50 || speedPercentage >= 500) {
      a = 0;
      speedPercentage = 100;
      engineState = OFF;
      engineStop = false;
    }
    break;
  }

  // Mix & output to DAC1. Engine fully OFF → clean silence. Otherwise a soft LIMITER (past ±72,
  // compressed 8:1) so no volume setting can hard-clip/tear — quiet stuff stays linear.
  uint8_t value;
  if (engineState == OFF) {
    value = dacOffset;
  } else {
    // Rattle ducking: as the tracks speed up, pull the engine mix back so the rattle/whine (DAC2)
    // cut through instead of piling up. duck = 100 at rest → (100 - rattleDuckPercent) at full pace.
    int32_t duck = 100 - (int32_t)rattleDuckPercent * constrain((int16_t)trackRattleVolume, (int16_t)0, (int16_t)100) / 100;
    int32_t mix = (a * 8 / 10) + (b / 2) + (c / 5) + (d / 5) + (e / 5) + f + g;
    int32_t v = mix * duck / 100 * masterVolume / 100;
    if (v > 118)      v = 118 + (v - 118) / 2;
    else if (v < -118) v = -118 + (v + 118) / 2;
    value = (uint8_t)constrain(v + dacOffset, 0, 255);
  }
  SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, value, RTC_IO_PDAC1_DAC_S);
}

// ════════════════════════════════════════════════════════════════
// FIXED-RATE PLAYBACK ISR (Aux sounds → DAC2)
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR fixedPlaybackTimer() {

#if defined GAMEPAD_MODE
  // Same as the variable ISR: no flash sample reads until a pad is connected, so the BT radio has a
  // clean flash bus to latch onto during the connect window.
  if (!gamepadConnected) {
    SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, dacOffset, RTC_IO_PDAC2_DAC_S);
    return;
  }
#endif

  static uint32_t curHornSample = 0;
  static uint32_t curSirenSample = 0;
  static uint32_t curSound1Sample = 0;
  static uint32_t curReversingSample = 0;
  static uint32_t curIndicatorSample = 0;
  static uint32_t curWastegateSample = 0;
  static uint32_t curBrakeSample = 0;
  static uint32_t curShiftingSample = 0;
  static uint32_t curDieselKnockSample = 0;
  static uint32_t curCouplingSample = 0;
  static uint32_t curUncouplingSample = 0;
  static uint32_t curTrackRattleSample = 0;
  static uint32_t curTrackRattle2Sample = 0;
  static uint32_t curBucketRattleSample = 0;
  static int32_t a, a1, a2 = 0;
  static int32_t b, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9 = 0;
  static int32_t c, c2, c3, c4, c5 = 0;
  static int32_t d, d1, d2 = 0;
  static boolean knockSilent = 0;
  static boolean knockMedium = 0;
  static uint8_t curKnockCylinder = 0;

  // Horn
  if (hornTrigger || hornLatch) {
    if (curHornSample < hornSampleCount - 1) {
      a1 = (hornSamples[curHornSample] * hornVolumePercentage / 100);
      curHornSample++;
    } else {
      curHornSample = 0;
    }
    hornLatch = true;
  } else {
    a1 = 0;
    curHornSample = 0;
  }

  // Siren
  if (sirenTrigger || sirenLatch) {
    if (curSirenSample < sirenSampleCount - 1) {
      a2 = (sirenSamples[curSirenSample] * sirenVolumePercentage / 100);
      curSirenSample++;
    } else {
      curSirenSample = 0;
    }
    sirenLatch = true;
  } else {
    a2 = 0;
    curSirenSample = 0;
  }

  // Sound1
  if (sound1trigger) {
    if (curSound1Sample < sound1SampleCount - 1) {
      b0 = (sound1Samples[curSound1Sample] * sound1VolumePercentage / 100);
      curSound1Sample++;
    } else {
      sound1trigger = false;
    }
  } else {
    b0 = 0;
    curSound1Sample = 0;
  }

  // Reversing beep: 0 = off, 1 = reverse only, 2 = forward + reverse.
  boolean alarmActive = engineRunning &&
      ((reversingBeepMode == 1 && escInReverse) ||
       (reversingBeepMode == 2 && (escInReverse || escIsDriving)));
  if (alarmActive) {
    if (curReversingSample < reversingSampleCount - 1) {
      b1 = (reversingSamples[curReversingSample] * reversingVolumePercentage / 100);
      curReversingSample++;
    } else {
      curReversingSample = 0;
    }
  } else {
    b1 = 0;
  }

  // Indicator tick
  if (indicatorSoundOn) {
    if (curIndicatorSample < indicatorSampleCount - 1) {
      b2 = (indicatorSamples[curIndicatorSample] * indicatorVolumePercentage / 100);
      curIndicatorSample++;
    } else {
      indicatorSoundOn = false;
    }
  } else {
    b2 = 0;
    curIndicatorSample = 0;
  }

  // Wastegate
  if (wastegateTrigger) {
    if (curWastegateSample < wastegateSampleCount - 1) {
      b3 = (wastegateSamples[curWastegateSample] * rpmDependentWastegateVolume / 100 * wastegateVolumePercentage / 100);
      curWastegateSample++;
    } else {
      wastegateTrigger = false;
    }
  } else {
    b3 = 0;
    curWastegateSample = 0;
  }

  // Air brake
  if (airBrakeTrigger) {
    if (curBrakeSample < brakeSampleCount - 1) {
      b4 = (brakeSamples[curBrakeSample] * brakeVolumePercentage / 100);
      curBrakeSample++;
    } else {
      airBrakeTrigger = false;
    }
  } else {
    b4 = 0;
    curBrakeSample = 0;
  }

  // Shifting
  if (shiftingTrigger && engineRunning && !automatic && !doubleClutch) {
    if (curShiftingSample < shiftingSampleCount - 1) {
      b6 = (shiftingSamples[curShiftingSample] * shiftingVolumePercentage / 100);
      curShiftingSample++;
    } else {
      shiftingTrigger = false;
    }
  } else {
    b6 = 0;
    curShiftingSample = 0;
  }

  // Diesel knock
  if (dieselKnockTriggerFirst) {
    dieselKnockTriggerFirst = false;
    curKnockCylinder = 0;
  }

  if (dieselKnockTrigger) {
    dieselKnockTrigger = false;
    curKnockCylinder++;
    curDieselKnockSample = 0;
  }

  if (curDieselKnockSample < knockSampleCount) {
    // Knock volume depends on hydraulic load in construction modes
    b7 = (knockSamples[curDieselKnockSample] * dieselKnockVolumePercentage / 100 * throttleDependentKnockVolume / 100 * hydraulicDependentKnockVolume / 100);
    curDieselKnockSample++;
    if (knockSilent && !knockMedium) b7 = b7 * dieselKnockAdaptiveVolumePercentage / 100;
    if (knockMedium) b7 = b7 * dieselKnockAdaptiveVolumePercentage / 75;
  }

  // ── Hydraulic sounds (Group C) ────────────────────────────

  // (Hydraulic-flow / relief voice removed — no squeal/static when working the blade.)

  // Track rattle (continuous). Rate scales with track speed (~38% crawl → slider ceiling at full
  // pace), INTERPOLATED between samples so changing the rate doesn't alias into static/buzz.
  if (tracksAreRotating) {
    // Rate follows the ACTUAL track speed (which follows engine rpm × swash): ~38% crawl → 60% at
    // full motor speed (back to what it was before the slider). Motor speed itself is unchanged.
    int32_t lo = 38, hi = 60;
    int32_t spd = constrain((int32_t)trackRattleVolume, (int32_t)0, (int32_t)100);
    uint32_t rate = (uint32_t)(lo + spd * (hi - lo) / 100);       // % of native speed
    static uint32_t trkPhase = 0;                                 // Q8 fixed-point index into the buffer
    uint32_t idx = trkPhase >> 8, frac = trkPhase & 0xFF;
    if (idx >= trackRattleSampleCount) { idx = 0; trkPhase = 0; }
    int32_t s0 = trackRattleSamples[idx];
    int32_t s1 = trackRattleSamples[(idx + 1 < trackRattleSampleCount) ? idx + 1 : 0];
    int32_t interp = s0 + (s1 - s0) * (int32_t)frac / 256;        // linear interpolation → smooth
    c2 = (interp * trackRattleVolumePercentage / 100 * trackRattleVolume / 100);
    trkPhase += rate * 256 / 100;                                 // 100% = 1.0 sample/tick
    if ((trkPhase >> 8) >= trackRattleSampleCount) trkPhase -= (uint32_t)trackRattleSampleCount << 8;
    curTrackRattleSample = trkPhase >> 8;
  } else {
    curTrackRattleSample = 0; c2 = 0;
  }

#ifdef TRACK_RATTLE_2
  // Track rattle 2 (periodic clank)
  if (trackRattle2Trigger) {
    if (curTrackRattle2Sample < trackRattle2SampleCount - 1) {
      c4 = (trackRattle2Samples[curTrackRattle2Sample] * trackRattle2VolumePercentage / 100 * trackRattleVolume / 100);
      curTrackRattle2Sample++;
    } else {
      trackRattle2Trigger = false;
      curTrackRattle2Sample = 0;
    }
  }
#endif

  // Bucket rattle
  if (bucketRattleTrigger) {
    if (curBucketRattleSample < bucketRattleSampleCount - 1) {
      c3 = (bucketRattleSamples[curBucketRattleSample] * bucketRattleVolumePercentage / 100);
      curBucketRattleSample++;
    } else {
      bucketRattleTrigger = false;
    }
  } else {
    c3 = 0;
    curBucketRattleSample = 0;
  }

  // Hydrostatic drive whine — the cdc recording, pitched by TRACK SPEED (fixed-rate ISR, so it never
  // screeches with engine rpm). Interpolated so the rate change stays smooth. Only while driving.
  if (driveWhineVolume > 3 && cdcWhineSampleCount > 0) {
    static uint32_t whinePhase = 0;
    uint32_t widx = whinePhase >> 8, wfrac = whinePhase & 0xFF;
    if (widx >= cdcWhineSampleCount) { widx = 0; whinePhase = 0; }
    int32_t w0 = cdcWhineSamples[widx];
    int32_t w1 = cdcWhineSamples[(widx + 1 < cdcWhineSampleCount) ? widx + 1 : 0];
    int32_t winterp = w0 + (w1 - w0) * (int32_t)wfrac / 256;
    c5 = (winterp * hydrostaticWhineVolumePercentage / 100 * driveWhineVolume / 100);
    uint32_t wrate = 55 + driveWhineVolume * 85 / 100;   // ~55% low → ~140% at full pace (rises with speed)
    whinePhase += wrate * 256 / 100;
    if ((whinePhase >> 8) >= cdcWhineSampleCount) whinePhase -= (uint32_t)cdcWhineSampleCount << 8;
  } else {
    c5 = 0;
  }

  // Spoken "Low battery" — fired once (then every 45s) when the pack trips protection. Plays even
  // with the engine off (you might check the pack before starting) and ducks the aux mix so it's clear.
  static uint32_t curLowBatterySample = 0;
  int32_t lb = 0;
  if (lowBatteryTrigger || lowBatteryLatch) {
    lowBatteryLatch = true;
    lowBatteryTrigger = false;
    if (curLowBatterySample < lowBatterySampleCount - 1) {
      lb = (lowBatterySamples[curLowBatterySample] * lowBatteryVolumePercentage / 100);
      curLowBatterySample++;
    } else {
      curLowBatterySample = 0;
      lowBatteryLatch = false;
    }
  } else {
    curLowBatterySample = 0;
  }

  // Mix & output to DAC2
  a = a1 + a2;
  b = b0 * 5 + b1 + b2 / 2 + b3 + b4 + b5 + b6 + b7 + b8 + b9;
  c = c2 + c3 + c4 + c5;
  d = d1 + d2;

  // Engine fully OFF → mute the aux DAC (so frozen/looping voices don't buzz), UNLESS the low-battery
  // voice is playing — that one always gets through.
  uint8_t value;
  if (engineState == OFF && !lowBatteryLatch) {
    value = dacOffset;
  } else {
    int32_t aux = (a * 8 / 10) + (b * 2 / 10) + c + d;
    if (lowBatteryLatch) aux /= 4;                 // duck the aux so the spoken warning cuts through
    int32_t v = (aux + lb) * masterVolume / 100;
    // Soft LIMITER: everything past ±118 is compressed 2:1 so the aux voices (rattle + whine) stay
    // clean and never hard-clip/tear, even with the Levels cranked up. Normal-loud stuff stays linear.
    if (v > 118)      v = 118 + (v - 118) / 2;
    else if (v < -118) v = -118 + (v + 118) / 2;
    value = (uint8_t)constrain(v + dacOffset, 0, 255);
  }
  SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, value, RTC_IO_PDAC2_DAC_S);
}

// ════════════════════════════════════════════════════════════════
// PWM RC SIGNAL READ (RMT interrupt)
// ════════════════════════════════════════════════════════════════

static void IRAM_ATTR rmt_isr_handler(void *arg) {
  uint32_t intr_st = RMT.int_st.val;
  static uint32_t lastFrameTime = millis();

  if (millis() - lastFrameTime > 20) {
    if (xSemaphoreTake(xPwmSemaphore, portMAX_DELAY)) {
      for (uint8_t i = 0; i < PWM_CHANNELS_NUM; i++) {
        uint8_t channel = PWM_CHANNELS[i];
        uint32_t channel_mask = BIT(channel * 3 + 1);
        if (!(intr_st & channel_mask)) continue;

        RMT.conf_ch[channel].conf1.rx_en = 0;
        RMT.conf_ch[channel].conf1.mem_owner = RMT_MEM_OWNER_TX;
        volatile rmt_item32_t *item = RMTMEM.chan[channel].data32;
        if (item) {
          pwmBuf[i + 1] = item->duration0;
        }
        RMT.conf_ch[channel].conf1.mem_wr_rst = 1;
        RMT.conf_ch[channel].conf1.mem_owner = RMT_MEM_OWNER_RX;
        RMT.conf_ch[channel].conf1.rx_en = 1;
        RMT.int_clr.val = channel_mask;
      }
      lastFrameTime = millis();
      xSemaphoreGive(xPwmSemaphore);
    } else {
      xSemaphoreGive(xPwmSemaphore);
    }
  }
}

// ════════════════════════════════════════════════════════════════
// PPM READ (GPIO interrupt)
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR readPpm() {
  unsigned long timenow = micros();
  unsigned long timeDiff = timenow - timelast;
  timelast = timenow;

  if (timeDiff > 4000) {
    counter = 0;
  } else if (counter < NUM_OF_PPM_CHL) {
    ppmBuf[counter] = timeDiff;
    counter++;
    if (counter == NUM_OF_PPM_CHL) {
      for (int i = 0; i < NUM_OF_PPM_CHL; i++) {
        ppmInp[i + 1] = ppmBuf[i];
      }
      ready = true;
    }
  }
}

// ════════════════════════════════════════════════════════════════
// RC COMMAND READERS
// ════════════════════════════════════════════════════════════════

void readPwmSignals() {
#if defined PWM_COMMUNICATION
  if (xSemaphoreTake(xPwmSemaphore, portMAX_DELAY)) {
    for (int i = 0; i < PWM_CHANNELS_NUM; i++) {
      pulseWidthRaw[i + 1] = pwmBuf[i + 1];
    }
    xSemaphoreGive(xPwmSemaphore);
  }
#endif
}

void readPpmCommands() {
#if defined PPM_COMMUNICATION
  if (ready) {
    ready = false;
    for (int i = 0; i < NUM_OF_PPM_CHL; i++) {
      pulseWidthRaw[i + 1] = ppmInp[i + 1];
    }
  }
#endif
}

void readSbusCommands() {
#if defined SBUS_COMMUNICATION
  #if defined EMBEDDED_SBUS
  if (sBus.read()) {
    SBUSchannels = sBus.ch();
    for (int i = 0; i < 16; i++) {
      pulseWidthRaw[i + 1] = map(SBUSchannels[i], 172, 1811, 1000, 2000);
    }
    failSafe = sBus.failsafe();
  }
  #else
  if (sBus.read(&SBUSchannels[0], &SBUSfailSafe, &SBUSlostFrame)) {
    for (int i = 0; i < 16; i++) {
      pulseWidthRaw[i + 1] = map(SBUSchannels[i], 172, 1811, 1000, 2000);
    }
    failSafe = SBUSfailSafe;
  }
  #endif
#endif
}

void readIbusCommands() {
#if defined IBUS_COMMUNICATION
  // Must call iBus.loop() to parse incoming serial data before reading channels
  static unsigned long lastIbusRead;
  static uint16_t iBusReadCycles;
  if (millis() - lastIbusRead > 5) {
    lastIbusRead = millis();
    iBus.loop();
    if (iBusReadCycles < 100) iBusReadCycles++;
    else if (!ibusInit) ibusInit = true;
  }
  if (!ibusInit) return; // Wait for buffer to fill before reading
  for (int i = 0; i < 14; i++) {
    uint16_t val = iBus.readChannel(i);
    if (val > 0) pulseWidthRaw[i + 1] = val;
  }
#endif
}

void readSumdCommands() {
#if defined SUMD_COMMUNICATION
  if (sumd.read(SUMDchannels, &SUMD_failsafe, &SUMD_frame_lost)) {
    for (int i = 0; i < 12; i++) {
      pulseWidthRaw[i + 1] = SUMDchannels[i] / 8;
    }
    failSafe = SUMD_failsafe;
  }
#endif
}

// ════════════════════════════════════════════════════════════════
// PROCESS RAW RC CHANNELS
// ════════════════════════════════════════════════════════════════

void processRawChannels() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 20) return;
  lastFrame = millis();

  // Auto-zero calibration
  if (!autoZeroDone) {
    static uint8_t zeroCount = 0;
    zeroCount++;
    if (zeroCount > 10) {
      for (int i = 1; i < PULSE_ARRAY_SIZE; i++) {
        if (pulseWidthRaw[i] >= 1400 && pulseWidthRaw[i] <= 1600) {
          pulseOffset[i] = 1500 - pulseWidthRaw[i];
        }
        pulseZero[i] = 1500;
        pulseMaxNeutral[i] = pulseZero[i] + pulseNeutral;
        pulseMinNeutral[i] = pulseZero[i] - pulseNeutral;
        pulseMax[i] = pulseZero[i] + pulseSpan;
        pulseMin[i] = pulseZero[i] - pulseSpan;
        pulseMaxLimit[i] = pulseZero[i] + pulseLimit;
        pulseMinLimit[i] = pulseZero[i] - pulseLimit;
      }
      autoZeroDone = true;
    }
  }

  // Apply offset, constrain, reverse, average
  for (int i = 1; i < PULSE_ARRAY_SIZE; i++) {
    if (pulseWidthRaw[i] > pulseMinValid && pulseWidthRaw[i] < pulseMaxValid) {
      pulseWidthRaw2[i] = pulseWidthRaw[i] + pulseOffset[i];
      pulseWidthRaw2[i] = constrain(pulseWidthRaw2[i], pulseMinLimit[i], pulseMaxLimit[i]);
    }
    // Channel reverse
    if (channelReversed[i]) {
      pulseWidthRaw2[i] = map(pulseWidthRaw2[i], 1000, 2000, 2000, 1000);
    }
    // Channel disable — force neutral if not enabled
    if (!channelEnabled[i]) {
      pulseWidthRaw2[i] = 1500;
    }
    // Simple averaging
    pulseWidthRaw3[i] = (pulseWidthRaw3[i] + pulseWidthRaw2[i]) / 2;
    pulseWidth[i] = pulseWidthRaw3[i];
  }
}

void failsafeRcSignals() {
  if (failSafe) {
    for (int i = 1; i < PULSE_ARRAY_SIZE; i++) {
      pulseWidth[i] = 1500;
    }
  }
}

// ════════════════════════════════════════════════════════════════
// THROTTLE MAPPING
// ════════════════════════════════════════════════════════════════

// Expo curve: blend linear and cubic response. expoPct 0 = linear, 100 = full cubic.
// v is a magnitude in [0, span]; small inputs are softened while full throw still reaches span.
int16_t applyExpo(int16_t v, int16_t span, int16_t expoPct) {
  if (expoPct <= 0 || span <= 0 || v <= 0) return v;
  if (expoPct > 100) expoPct = 100;
  float n = (float)v / (float)span;                 // 0..1
  float e = (float)expoPct / 100.0f;
  float out = n * (1.0f - e) + (n * n * n) * e;      // linear ↔ cubic blend
  return (int16_t)(out * (float)span + 0.5f);
}

// Signed expo for a centred axis (±span), e.g. the drive stick: softens around centre, keeps sign.
int16_t expoSigned(int32_t v, int16_t span, int16_t expoPct) {
  int16_t sign = (v < 0) ? -1 : 1;
  int16_t mag = (int16_t)constrain((int32_t)labs(v), (int32_t)0, (int32_t)span);
  return sign * applyExpo(mag, span, expoPct);
}

void mapThrottle() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 4) return;
  lastFrame = millis();

  // ── Engine on/off control ──
  // CH9 toggle ONLY — no auto-start
  {
    static boolean engineToggle = false;
    if (CH_ENGINE_TOGGLE > 0 && pulseWidth[CH_ENGINE_TOGGLE] > 1700 && !engineToggle) {
      engineOn = !engineOn;
      engineToggle = true;
    }
    if (CH_ENGINE_TOGGLE == 0 || pulseWidth[CH_ENGINE_TOGGLE] < 1600) engineToggle = false;
  }

  // ── Throttle mapping (mode-specific) ──
#if defined EXCAVATOR_MODE || defined DOZER_MODE || defined CRANE_MODE
  // Forward-only throttle (hand throttle sets RPM)
  if (pulseWidth[CH_THROTTLE] > pulseMaxNeutral[CH_THROTTLE]) {
    currentThrottle = map(pulseWidth[CH_THROTTLE], pulseMaxNeutral[CH_THROTTLE], pulseMax[CH_THROTTLE], 0, 500);
  } else {
    currentThrottle = 0;
  }
  currentThrottle = constrain(currentThrottle, 0, 500);

#elif defined LOADER_MODE || defined SKIDSTEER_MODE || defined GRADER_MODE || defined BACKHOE_MODE
  // Bidirectional throttle (stick controls speed + direction)
  if (pulseWidth[CH_THROTTLE] > pulseMaxNeutral[CH_THROTTLE]) {
    currentThrottle = map(pulseWidth[CH_THROTTLE], pulseMaxNeutral[CH_THROTTLE], pulseMax[CH_THROTTLE], 0, 500);
  } else if (pulseWidth[CH_THROTTLE] < pulseMinNeutral[CH_THROTTLE]) {
    currentThrottle = map(pulseWidth[CH_THROTTLE], pulseMinNeutral[CH_THROTTLE], pulseMin[CH_THROTTLE], 0, 500);
  } else {
    currentThrottle = 0;
  }
  currentThrottle = constrain(currentThrottle, 0, 500);
#endif

  // ── Auto idle-down: drop to base idle when no function has been touched for a while,
  //    and snap straight back to commanded rpm the instant any function moves. ──
  if (autoIdleEnabled) {
    static unsigned long lastActivity = 0;
    static uint16_t thrRef = 1500; static uint32_t thrRefMs = 0;

    // Any enabled channel (except the throttle itself) off centre = a function is being worked.
    boolean activity = false;
    for (uint8_t ch = 1; ch < PULSE_ARRAY_SIZE; ch++) {
      if (ch == CH_THROTTLE || !channelEnabled[ch]) continue;
      if (abs((int)pulseWidth[ch] - 1500) > 80) { activity = true; break; }
    }
    // Watch the RAW throttle INPUT (the D-pad hand-throttle / throttle stick) over a ~200ms window —
    // so a slow rev-up/down still counts as activity, and the idle-down's own output change (which
    // does NOT touch this pulse) can never re-trigger it. This is what lets you rev back up past the
    // idle level: the moment you work the throttle, idle-down backs off.
    if (CH_THROTTLE > 0 && abs((int)pulseWidth[CH_THROTTLE] - (int)thrRef) > 15) activity = true;
    if (millis() - thrRefMs > 200) { thrRef = (CH_THROTTLE > 0) ? pulseWidth[CH_THROTTLE] : 1500; thrRefMs = millis(); }

    if (activity) lastActivity = millis();

    // Parked and untouched past the delay → ease the throttle DOWN to the idle-down level (a fast
    // idle), and snap back to your throttle the instant activity returns.
    int16_t idleFloor = (int16_t)constrain((int32_t)autoIdleThrottlePercent * 500 / 100, (int32_t)0, (int32_t)500);
    if (currentThrottle > idleFloor && !activity && millis() - lastActivity > autoIdleDelayMs) {
      currentThrottle = idleFloor;
    }
  }

  // Throttle smoothing
  static int16_t lastThrottle = 0;
  if (currentThrottle > lastThrottle) lastThrottle += 3;
  else if (currentThrottle < lastThrottle) lastThrottle -= 3;
  currentThrottleFaded = constrain(lastThrottle, 0, 500);

  // Volume calculations based on throttle
  throttleDependentVolume = map(currentThrottleFaded, 0, 500, engineIdleVolumePercentage, fullThrottleVolumePercentage);
  throttleDependentRevVolume = map(currentThrottleFaded, 0, 500, engineRevVolumePercentage, 100);
  throttleDependentKnockVolume = map(currentThrottleFaded, 0, 500, dieselKnockIdleVolumePercentage, 100);
  throttleDependentTurboVolume = map(currentThrottleFaded, 0, 500, turboIdleVolumePercentage, 100);
  throttleDependentFanVolume = map(currentThrottleFaded, 0, 500, fanIdleVolumePercentage, 100);
  throttleDependentChargerVolume = map(currentThrottleFaded, 0, 500, chargerIdleVolumePercentage, 100);
  rpmDependentWastegateVolume = map(currentRpm, 0, 500, wastegateIdleVolumePercentage, 100);

  // Horn trigger
  hornTrigger = (pulseWidth[CH_HORN] > 1800);
  if (!hornTrigger) hornLatch = false;

  // Work lights: each press of the lights button steps the cycle
  //   off → FRONT → REAR → ALL (front + rear) → off
  {
    static boolean lightsToggleLock = false;

    if (pulseWidth[CH_LIGHTS] > 1700 && !lightsToggleLock) {
      lightsToggleLock = true;

      // Cycle (additive): off → front → +rear → +side (all on) → off
      lightsMode++;
      if (lightsMode > 3) lightsMode = 0;
      lightsOn = (lightsMode > 0);
      lightsState = lightsOn ? 1 : 0;
    }
    if (pulseWidth[CH_LIGHTS] < 1600) lightsToggleLock = false;
  }

  // ── Work-light GPIO output ── each press adds the next set; they all stay on until "off".
  // lightsMode: 0=off, 1=front, 2=front+rear, 3=front+rear+side
  digitalWrite(FRONT_WORKLIGHT_PIN, (lightsMode >= 1) ? HIGH : LOW);
  digitalWrite(REAR_WORKLIGHT_PIN,  (lightsMode >= 2) ? HIGH : LOW);
  digitalWrite(SIDE_LIGHT_PIN,      (lightsMode >= 3) ? HIGH : LOW);
}

// ════════════════════════════════════════════════════════════════
// ENGINE MASS SIMULATION
// ════════════════════════════════════════════════════════════════

void engineMassSimulation() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 4) return;
  lastFrame = millis();

  int32_t targetRpm = 0;

#if defined DOZER_MODE
  // Governor: setpoint = throttle; RPM sags under TOTAL hydraulic demand (drive + implements)
  // with fast attack / slow recovery, and lugs when bogged under heavy load.
  static int16_t rpmSag = 0;
  int16_t targetSag = map(constrain(totalFlowDemand, 0, pumpFlowCapacity),
                          0, max((int16_t)1, pumpFlowCapacity), 0, maxSagRpm);
  if (rpmSag < targetSag)      { rpmSag += sagAttack;   if (rpmSag > targetSag) rpmSag = targetSag; }
  else if (rpmSag > targetSag) { rpmSag -= sagRecovery; if (rpmSag < targetSag) rpmSag = targetSag; }
  // Engine rpm = your THROTTLE setting (minus load sag). rpm is the pump's flow rate; the drive stick
  // is the swashplate displacement — track speed = flow = rpm × displacement (done in hydrostaticModel).
  targetRpm = constrain((int32_t)currentThrottle - rpmSag, 0, 500);
  engineLugging = (currentRpm < lugRpmThreshold && totalFlowDemand > pumpFlowCapacity / 2);

#elif defined EXCAVATOR_MODE || defined BACKHOE_MODE
  // Excavator / backhoe: throttle sets target RPM, hydraulic load drops it
  targetRpm = currentThrottle - hydraulicLoad;
  targetRpm = constrain(targetRpm, 0, 500);

#elif defined LOADER_MODE || defined CRANE_MODE || defined SKIDSTEER_MODE || defined GRADER_MODE
  // Loader/Crane/SkidSteer/Grader: RPM follows max of throttle or hydraulic demand
  targetRpm = currentThrottle;
  if (targetHydraulicRpm[0] > targetRpm) targetRpm = targetHydraulicRpm[0];
  targetRpm = constrain(targetRpm, 0, 500);
#endif

  // Acceleration / deceleration with mass
  if (engineRunning) {
    if (currentRpm < targetRpm) {
      currentRpm += acc;
      if (currentRpm > targetRpm) currentRpm = targetRpm;
    } else if (currentRpm > targetRpm) {
      currentRpm -= dec;
      if (currentRpm < targetRpm) currentRpm = targetRpm;
    }
  } else {
    currentRpm = 0;
  }

  currentRpm = constrain(currentRpm, minRpm, maxRpm);

  // Calculate sample rate from RPM. Idle is unchanged, BUT the engine NOTE is allowed to bog a little
  // below idle under load — so working the pump at idle gives an audible dip/lug even though the real
  // rpm is already floored at 0. This is sound-only; currentRpm (tracks/drive) is not touched.
  int32_t soundRpm = currentRpm;
#if defined DOZER_MODE
  int32_t belowIdle = (int32_t)rpmSag - currentThrottle; // >0 when load exceeds the idle rpm headroom
  if (belowIdle > 0) soundRpm -= belowIdle;              // dip the pitch below idle = bog
  if (soundRpm < -(int32_t)maxSagRpm) soundRpm = -(int32_t)maxSagRpm;
#endif
  engineSampleRate = map(soundRpm, minRpm, maxRpm, maxSampleInterval, minSampleInterval);

  // Wastegate trigger on rapid throttle drop
  static int32_t lastRpm = 0;
  if (lastRpm - currentRpm > 60 && currentRpm > 200) {
    wastegateTrigger = true;
  }
  lastRpm = currentRpm;
}

// ════════════════════════════════════════════════════════════════
// ESC OUTPUT (5-state machine)
// ════════════════════════════════════════════════════════════════

int8_t pulse() {
  if (currentThrottle > 50) return 1;
  if (currentThrottle < -50) return -1;
  return 0;
}

// Hi/Lo range state (rabbit mode / 2-speed)
boolean hiLoIsHigh = hiLoDefaultHigh;

void esc() {
  static unsigned long lastFrame = millis();
  uint16_t escRampTime = hiLoIsHigh ? escRampTimeHigh : escRampTimeLow;

  if (millis() - lastFrame < escRampTime) return;
  lastFrame = millis();

  // Hi/Lo range toggle
  #ifdef HILO_ENABLED
  static boolean hiLoToggle = false;
  if (pulseWidth[CH_HILO_TOGGLE] > 1700 && !hiLoToggle) {
    hiLoIsHigh = !hiLoIsHigh;
    hiLoToggle = true;
  }
  if (pulseWidth[CH_HILO_TOGGLE] < 1600) hiLoToggle = false;
  #endif

  // Compute effective ESC limits (narrowed in low range)
  uint16_t effectiveMax = escPulseMax;
  uint16_t effectiveMin = escPulseMin;
  #ifdef HILO_ENABLED
  if (!hiLoIsHigh) {
    uint16_t fwdRange = (escPulseMax - 1500) * hiLoRatioPercent / 100;
    uint16_t revRange = (1500 - escPulseMin) * hiLoRatioPercent / 100;
    effectiveMax = 1500 + fwdRange;
    effectiveMin = 1500 - revRange;
  }
  #endif

  static uint16_t escPulseWidth = 1500;
  uint16_t target;

  switch (driveState) {
  case 0: // Standing still
    escPulseWidth = 1500;
#ifndef DOZER_MODE
    escIsBraking = false;
    escIsDriving = false;
    escInReverse = false;
#endif
    if (pulse() == 1) driveState = 1;
    if (pulse() == -1) driveState = 3;
    break;

  case 1: // Forward
    target = map(currentSpeed, 0, 500, 1500, effectiveMax);
    if (escPulseWidth < target) escPulseWidth += escAccelerationSteps;
    if (escPulseWidth > target) escPulseWidth = target;
#ifndef DOZER_MODE
    escIsDriving = true;
    escIsBraking = false;
    escInReverse = false;
#endif
    if (pulse() == 0) driveState = 2;
    if (pulse() == -1) driveState = 2;
    break;

  case 2: // Braking (forward)
    if (escPulseWidth > 1500) escPulseWidth -= escBrakeSteps;
    if (escPulseWidth <= 1500) { escPulseWidth = 1500; driveState = 0; }
#ifndef DOZER_MODE
    escIsBraking = true;
#endif
    break;

  case 3: // Reverse
    target = map(currentSpeed, 0, 500, 1500, effectiveMin);
    if (escPulseWidth > target) escPulseWidth -= escAccelerationSteps;
    if (escPulseWidth < target) escPulseWidth = target;
#ifndef DOZER_MODE
    escIsDriving = true;
    escIsBraking = false;
    escInReverse = true;
#endif
    if (pulse() == 0) driveState = 4;
    if (pulse() == 1) driveState = 4;
    break;

  case 4: // Braking (reverse)
    if (escPulseWidth < 1500) escPulseWidth += escBrakeSteps;
    if (escPulseWidth >= 1500) { escPulseWidth = 1500; driveState = 0; }
#ifndef DOZER_MODE
    escIsBraking = true;
#endif
    break;
  }

  escPulseWidth = constrain(escPulseWidth, effectiveMin, effectiveMax);
  // GPIO33 is now a native implement output on EVERY machine, driven by mcpwmOutput(). The ESC
  // state machine only survives here to feed the engine-sound speed below — it no longer drives a
  // pin (drive goes out through the generic 6-output path).

  // Calculate speed for sound engine
  currentSpeed = abs(escPulseWidth - 1500);
}

// ════════════════════════════════════════════════════════════════
// DRIVE + IMPLEMENTS — hydrostatic tank drive (tracked) or drive+steer (wheeled),
// plus the shared proportional-valve implement model. Uses the active machine output map
// (MACHINE_TRACKED / outImpl[] / outDriveR / outDriveL) defined in config.h.
// ════════════════════════════════════════════════════════════════
int16_t actualTrackL = 0, actualTrackR = 0; // after droop, ±500 → servo
int16_t driveFlowDemand = 0;                // track pump load (0..~60), set in hydrostaticModel

static int16_t rampToward(int16_t cur, int16_t target, int16_t accel, int16_t decel) {
  int16_t rate = (abs(target) > abs(cur)) ? accel : decel; // stroking vs destroking
  if (cur < target) { cur += rate; if (cur > target) cur = target; }
  else if (cur > target) { cur -= rate; if (cur < target) cur = target; }
  return cur;
}

// Read the sticks → commanded drive effort (±500). Tracked = left/right track; wheeled = drive/steer.
void driveMixer() {
#if MACHINE_TRACKED
  #if defined DOZER_MODE && defined DRIVE_SINGLE_STICK_MIX
  // Dozer single-stick mix: one stick fwd/back + left/right, blended into both tracks. Expo on both
  // axes of the drive stick so small moves are gentle for fine control.
  int32_t drive = expoSigned((int32_t)pulseWidth[CH_DZ_DRIVE] - 1500, 500, driveExpo); // ±500
  int32_t steer = expoSigned((int32_t)pulseWidth[CH_DZ_STEER] - 1500, 500, driveExpo);
  if (drive < 0) steer = -steer; // reverse: flip steer so it turns the way the stick points (like a car)
  int32_t l = drive + steer;
  int32_t r = drive - steer;
  int32_t m = max(labs(l), labs(r));
  if (m > 500) { l = l * 500 / m; r = r * 500 / m; } // normalize, keep turn ratio
  if (l != 0 && r != 0 && ((l > 0) != (r > 0))) {     // counter-rotating pivot
    l = l * counterRotScale / 100;
    r = r * counterRotScale / 100;
  }
  cmdTrackL = l;
  cmdTrackR = r;
  #else // dual-track: one channel per track (excavator, skid steer, dozer dual-stick)
  cmdTrackL = expoSigned((int32_t)pulseWidth[outDriveL] - 1500, 500, driveExpo);
  cmdTrackR = expoSigned((int32_t)pulseWidth[outDriveR] - 1500, 500, driveExpo);
  #endif
  if (!engineRunning) { cmdTrackL = 0; cmdTrackR = 0; }
#else
  // Wheeled: a drive motor + a steering servo, no tank mix. cmdTrackR = drive, cmdTrackL = steer.
  cmdTrackR = expoSigned((int32_t)pulseWidth[outDriveR] - 1500, 500, driveExpo); // drive (expo for fine control)
  cmdTrackL = (int16_t)pulseWidth[outDriveL] - 1500; // steer (servo passes through linear, engine or not)
  if (!engineRunning) cmdTrackR = 0;
#endif
}

// Swashplate ramp + engine-load speed droop → actual track speed. 20ms tick.
void hydrostaticModel() {
  static unsigned long last = 0;
  if (millis() - last < 20) return;
  last = millis();
#if MACHINE_TRACKED
  // Swashplate ramps straight from forward through neutral to reverse — no dwell. (The old ESC
  // re-arm neutral-hold only helps brake/reverse ESCs; with no-brake ESCs it just stalled reverse
  // until you re-centered. No-brake ESCs reverse instantly, so let the ramp flow through.)
  swashL = rampToward(swashL, cmdTrackL, swashAccelRate, swashDecelRate);
  swashR = rampToward(swashR, cmdTrackR, swashAccelRate, swashDecelRate);
  // Full hydrostat: track speed = pump flow = swashplate displacement (swash) × engine-rpm fraction.
  // rpm is the flow RATE (set by throttle), swash is the displacement (drive stick). So idle rpm =
  // crawl even at full stick, throttle up = faster, and load sag drops rpm → flow → tracks slow (bog).
  int32_t rpmFrac = constrain((int32_t)currentRpm * 100 / max((int16_t)1, driveDroopRefRpm), 0, 100);
  if (rpmFrac < driveIdleCreepPercent) rpmFrac = driveIdleCreepPercent; // idle creep: still moves at idle throttle
  actualTrackL = swashL * rpmFrac / 100;
  actualTrackR = swashR * rpmFrac / 100;
  driveFlowDemand = (int16_t)(((int32_t)abs(swashL) + abs(swashR)) * driveFlowWeight / 1000);
#else
  // Wheeled: drive motor gets an accel/decel ramp; the steer servo passes straight through.
  swashR = rampToward(swashR, cmdTrackR, swashAccelRate, swashDecelRate);
  actualTrackR = swashR;      // drive
  actualTrackL = cmdTrackL;   // steer (direct)
  driveFlowDemand = (int16_t)((int32_t)abs(swashR) * driveFlowWeight / 1000);
#endif
}

// ── Proportional implement valves: lift/tilt/angle/ripper ──
int16_t valveCmd[4] = {0, 0, 0, 0};        // ramped spool command ±500 (0=lift,1=tilt,2=angle,3=ripper)
int16_t implFlowDemand = 0;                // total implement pump load (0..~100)

void implementControl() {
  static unsigned long last = 0;
  if (millis() - last < 20) return;
  last = millis();

  const uint8_t *chans = outImpl; // the active machine's 4 implement channels (0 = unused)
  int16_t rampStep = (int16_t)(500L * 20 / max((uint16_t)20, hydraulicRampTime)); // span per 20ms
  int32_t demand = 0;
  for (int i = 0; i < 4; i++) {
    int16_t raw = 0;
    if (chans[i] > 0 && engineRunning) {
      raw = (int16_t)pulseWidth[chans[i]] - 1500;
      if (abs(raw) < hydraulicDeadZone) raw = 0;                        // deadzone
      else raw = (raw > 0) ? raw - hydraulicDeadZone : raw + hydraulicDeadZone;
    }
    valveCmd[i] = rampToward(valveCmd[i], constrain(raw, -500, 500), rampStep, rampStep);
    demand += (int32_t)abs(valveCmd[i]) * implFlowWeight[i] / 100;
  }
  implFlowDemand = (int16_t)(demand / 5); // scale to ~0..100 range
}

// ── Engine load model: total pump demand (drive + implements + dig load) feeds the governor so the
//    engine bogs/lugs under load. No relief valve — the flow voice stays silent. ──
void loadModel() {
  // Dig load (blade machines): if the blade is lowered AND you're driving forward, infer you're
  // cutting and add engine load proportional to depth × forward push. Heuristic, not physics — it
  // just makes "drop the blade and push" bog the machine the way it should.
  int16_t digLoad = 0;
#if defined DOZER_MODE || defined LOADER_MODE || defined GRADER_MODE || defined SKIDSTEER_MODE
  if (digLoadGain > 0) {
  #if MACHINE_TRACKED
    int16_t fwd = (actualTrackL + actualTrackR) / 2;   // tracked: both tracks (+ = forward)
  #else
    int16_t fwd = actualTrackR;                        // wheeled: drive only (L is steer)
  #endif
    int16_t bladeDown = digBladeDownSign * valveCmd[0]; // > 0 when the blade is lowered
    if (fwd > 40 && bladeDown > 40) {
      int32_t dig = (int32_t)fwd * bladeDown / 500;     // depth × push, 0..~500
      dig = dig * digLoadGain / 100;
      digLoad = (int16_t)constrain(dig, (int32_t)0, (int32_t)digLoadCap);
    }
  }
#endif
  totalFlowDemand = driveFlowDemand + implFlowDemand + digLoad; // governor reads this → engine bogs

  hydraulicFlowVolume = 0; // relief removed — the flow voice never plays
  // NOTE: the pump-whine voice pitches with engine rpm, so feeding it the DRIVE flow made it scream
  // high when you throttled up to drive (the "drive whine" static). Pump whine now stays implement-
  // only (set in dozerControl); driving does NOT trigger it.
}

#if defined GAMEPAD_MODE
// ── Gamepad engine-feel rumble ────────────────────────────────────────────────────────────────
// Feel the dozer through the pad: an idle purr while running, the strong motor follows total
// hydraulic load (drive + implements), and a hard bump the moment the relief cracks or the engine
// lugs under a heavy push. Self-throttled to ~8 Hz so it doesn't flood Bluetooth. GP_RUMBLE off =
// save the controller's battery.
void updateGamepadRumble() {
#if GP_RUMBLE
  if (!gpController || !gpController->isConnected() || !gpController->isGamepad()) return;
  if (!gpRumbleOn) { gpController->playDualRumble(0, 0, 0, 0); return; } // vibration toggled off

  static uint32_t lastMs = 0;
  static bool prevStrain = false;
  bool strain = engineLugging;

  // One immediate hard bump the instant the engine bogs.
  if (strain && !prevStrain) {
    prevStrain = strain;
    gpController->playDualRumble(0, 130, 60, 255);
    lastMs = millis();
    return;
  }
  prevStrain = strain;

  if (millis() - lastMs < 60) return; // ~16 Hz refresh — smooth enough to PULSE the tracks
  lastMs = millis();

  if (batteryProtection) {                          // LOW BATTERY: short warning buzz every second
    bool buzz = (millis() % 1000) < 200;
    gpController->playDualRumble(0, 90, buzz ? 200 : 0, buzz ? 255 : 0);
    return;
  }

  uint8_t weak = 0, strong = 0;
  if (engineState == STARTING) { weak = 90; strong = 140; } // cranking shudder
  else if (engineRunning) {
    int implLoad = constrain((int)implFlowDemand, 0, 100);                        // implements → smooth HYDRAULIC rumble
    int trkSpd   = constrain((abs((int)actualTrackL) + abs((int)actualTrackR)) / 10, 0, 100); // 0..100 track speed

    // Hydraulic rumble: smooth, follows implement load (kept as-is).
    int hydW = (implLoad > 3) ? 40 + implLoad * 40 / 100 : 0;
    int hydS = implLoad * 170 / 100;

    // Track vibration: PULSE in rhythm with track speed — faster tracks = faster thumps (like the
    // track links passing). At a standstill it falls back to a gentle idle lope.
    int trkW = 0, trkS = 0;
    if (trkSpd > 4) {
      uint32_t period = (uint32_t)map(trkSpd, 4, 100, 300, 140);  // ms between thumps: slow → fast
      bool beat = (millis() % period) < (period / 2);
      if (beat) { trkS = 70 + trkSpd * 120 / 100; trkW = 45; }    // thump strength grows with speed
    } else {
      bool beat = (millis() % 430) < 160;                         // parked idle lope (gentle)
      trkW = beat ? 88 : 22;
      trkS = beat ? 40 : 0;
    }

    weak   = (uint8_t)constrain(max(hydW, trkW), 0, 255);
    strong = (uint8_t)constrain(max(hydS, trkS), 0, 255);
    if (strain) { weak = 60; strong = 255; }                       // sustained strain = full bump
  }
  gpController->playDualRumble(0, 90, weak, strong); // duration > refresh -> continuous
#endif
}

// ── Controller lightbar. ledColorMode: 0 = reactive (green idle → amber load → red bog), else a
//    fixed colour you pick in the flasher. ──
void updateGamepadLED() {
  if (!gpController || !gpController->isConnected() || !gpController->isGamepad()) return;
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 200) return; // 5 Hz — don't flood Bluetooth
  lastMs = millis();

  if (batteryProtection) {                         // LOW BATTERY: hard red flash, overrides every mode
    bool on = (millis() % 600) < 300;
    gpController->setColorLED(on ? 255 : 0, 0, 0);
    return;
  }

  uint8_t r = 0, g = 0, b = 0;
  switch (ledColorMode) {
    case 1: r = 0;   g = 0;   b = 255; break; // blue
    case 2: r = 0;   g = 255; b = 0;   break; // green
    case 3: r = 255; g = 0;   b = 0;   break; // red
    case 4: r = 255; g = 110; b = 0;   break; // amber
    case 5: r = 255; g = 255; b = 255; break; // white
    case 6: r = 0;   g = 200; b = 255; break; // cyan
    case 7: r = 200; g = 0;   b = 255; break; // purple
    case 8: r = 0;   g = 0;   b = 0;   break; // off
    default:                                   // 0 = reactive
      if (engineState == OFF) { b = 6; }
      else if (engineLugging) { r = 255; }
      else {
        int load = constrain(totalFlowDemand, (int16_t)0, (int16_t)100);
        r = (uint8_t)(load * 255 / 100);
        g = (uint8_t)(200 - load * 200 / 100); if (g < 30) g = 30;
      }
      break;
  }
  gpController->setColorLED(r, g, b);
}
#endif // GAMEPAD_MODE

// ════════════════════════════════════════════════════════════════
// SERVO OUTPUT (MCPWM)
// ════════════════════════════════════════════════════════════════

void mcpwmOutput() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 10) return;
  lastFrame = millis();

  // ── Every machine drives 6 MCPWM outputs: 2 drive (GPIO13/12) + 4 implements (GPIO33/32/14/27).
  //    Tracked: drive A/B = right/left track from the hydrostatic model. Wheeled: A = drive motor,
  //    B = steering servo. The 4 implements come from the proportional-valve model. ──
  uint16_t driveA, driveB, im0, im1, im2, im3;
  int16_t aR = actualTrackR, aL = actualTrackL;
#if MACHINE_TRACKED
  #ifdef HILO_ENABLED
  if (!hiLoIsHigh) { aR = aR * hiLoRatioPercent / 100; aL = aL * hiLoRatioPercent / 100; }
  #endif
  driveA = constrain(1500 + aR * trackThrowScale / 100, servoMin[0], servoMax[0]); // right track
  driveB = constrain(1500 + aL * trackThrowScale / 100, servoMin[1], servoMax[1]); // left track
#else
  driveA = constrain(1500 + aR * trackThrowScale / 100, servoMin[0], servoMax[0]); // drive motor
  driveB = constrain(1500 + aL, servoMin[1], servoMax[1]);                          // steer servo (direct)
#endif
  // Implements: proportional valve model (deadzoned + ramped in implementControl()); 0-channel = center.
  // Hydraulics ride PUMP FLOW too — the same pump feeds the cylinders, so implement speed scales with
  // engine rpm (slow at idle, quick when revved), with a floor so they still creep at idle. Same idea
  // as the track droop. (Best for linear actuators; for a position servo this also trims its throw.)
  int32_t hydFrac = constrain((int32_t)currentRpm * 100 / max((int16_t)1, driveDroopRefRpm), 0, 100);
  if (hydFrac < hydIdleFlowPercent) hydFrac = hydIdleFlowPercent;
  im0 = constrain(1500 + (int)((int32_t)valveCmd[0] * hydFrac / 100), servoMin[2], servoMax[2]);
  im1 = constrain(1500 + (int)((int32_t)valveCmd[1] * hydFrac / 100), servoMin[2], servoMax[2]);
  im2 = constrain(1500 + (int)((int32_t)valveCmd[2] * hydFrac / 100), servoMin[2], servoMax[2]);
  im3 = constrain(1500 + (int)((int32_t)valveCmd[3] * hydFrac / 100), servoMin[3], servoMax[3]);

  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, driveA); // GPIO13
  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, driveB); // GPIO12
  // Per-output direction flip (mirror around 1500) — for motors/actuators wired backwards.
  // Index: 0=CH1 driveA, 1=CH2 driveB, 2=ESC im0, 3=CH4 im1, 4=CH3 im2, 5="32" im3.
  if (outputReversed[0]) driveA = 3000 - driveA;
  if (outputReversed[1]) driveB = 3000 - driveB;
  if (outputReversed[2]) im0 = 3000 - im0;
  if (outputReversed[3]) im1 = 3000 - im1;
  if (outputReversed[4]) im2 = 3000 - im2;
  if (outputReversed[5]) im3 = 3000 - im3;

  // Implement slots (valveCmd) → board headers: [0]=ESC(33), [1]=CH4(27), [2]=CH3(14), [3]=32.
  // im1 (slot 2, tilt) and im3 (slot 4, ripper) are swapped onto GPIO27/GPIO32 so the ripper
  // lands on the GPIO32 header and the three blade functions sit on ESC/CH3/CH4.
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_0, MCPWM_OPR_A, im0);    // GPIO33  (ESC hdr)
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_0, MCPWM_OPR_B, im3);    // GPIO32  (ripper)
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_1, MCPWM_OPR_A, im2);    // GPIO14  (CH3 hdr)
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_1, MCPWM_OPR_B, im1);    // GPIO27  (CH4 hdr)
}

// ════════════════════════════════════════════════════════════════
// HYDRAULIC CONTROL (machine-type specific)
// ════════════════════════════════════════════════════════════════

#if defined EXCAVATOR_MODE
void excavatorControl() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 4) return;
  lastFrame = millis();

  static uint16_t hydraulicPumpVolumeInternal[17] = {0};
  static uint16_t hydraulicPumpVolumeInternalUndelayed = 0;
  static uint16_t hydraulicFlowVolumeInternalUndelayed = 0;
  static uint16_t trackRattleVolumeInternal[17] = {0};
  static uint16_t trackRattleVolumeInternalUndelayed = 0;
  static boolean trackLisRotating = false, trackRisRotating = false;

  // Boom
  if (pulseWidth[CH_EX_BOOM] < pulseMinNeutral[CH_EX_BOOM])
    hydraulicPumpVolumeInternal[5] = map(pulseWidth[CH_EX_BOOM], pulseMinNeutral[CH_EX_BOOM], pulseMin[CH_EX_BOOM], 0, 50);
  else if (pulseWidth[CH_EX_BOOM] > pulseMaxNeutral[CH_EX_BOOM])
    hydraulicPumpVolumeInternal[5] = map(pulseWidth[CH_EX_BOOM], pulseMaxNeutral[CH_EX_BOOM], pulseMax[CH_EX_BOOM], 0, 30);
  else
    hydraulicPumpVolumeInternal[5] = 0;

  // Stick / Dipper
  if (pulseWidth[CH_EX_STICK] < pulseMinNeutral[CH_EX_STICK])
    hydraulicPumpVolumeInternal[6] = map(pulseWidth[CH_EX_STICK], pulseMinNeutral[CH_EX_STICK], pulseMin[CH_EX_STICK], 0, 50);
  else if (pulseWidth[CH_EX_STICK] > pulseMaxNeutral[CH_EX_STICK])
    hydraulicPumpVolumeInternal[6] = map(pulseWidth[CH_EX_STICK], pulseMaxNeutral[CH_EX_STICK], pulseMax[CH_EX_STICK], 0, 30);
  else
    hydraulicPumpVolumeInternal[6] = 0;

  // Bucket
  if (pulseWidth[CH_EX_BUCKET] < pulseMinNeutral[CH_EX_BUCKET])
    hydraulicPumpVolumeInternal[1] = map(pulseWidth[CH_EX_BUCKET], pulseMinNeutral[CH_EX_BUCKET], pulseMin[CH_EX_BUCKET], 0, 30);
  else if (pulseWidth[CH_EX_BUCKET] > pulseMaxNeutral[CH_EX_BUCKET])
    hydraulicPumpVolumeInternal[1] = map(pulseWidth[CH_EX_BUCKET], pulseMaxNeutral[CH_EX_BUCKET], pulseMax[CH_EX_BUCKET], 0, 30);
  else
    hydraulicPumpVolumeInternal[1] = 0;

  // Swing
  if (pulseWidth[CH_EX_SWING] < pulseMinNeutral[CH_EX_SWING])
    hydraulicPumpVolumeInternal[2] = map(pulseWidth[CH_EX_SWING], pulseMinNeutral[CH_EX_SWING], pulseMin[CH_EX_SWING], 0, 40);
  else if (pulseWidth[CH_EX_SWING] > pulseMaxNeutral[CH_EX_SWING])
    hydraulicPumpVolumeInternal[2] = map(pulseWidth[CH_EX_SWING], pulseMaxNeutral[CH_EX_SWING], pulseMax[CH_EX_SWING], 0, 40);
  else
    hydraulicPumpVolumeInternal[2] = 0;

  // Sum pump volumes
  if (engineRunning) {
    hydraulicPumpVolumeInternalUndelayed = constrain(
      hydraulicPumpVolumeInternal[1] + hydraulicPumpVolumeInternal[2] +
      hydraulicPumpVolumeInternal[5] + hydraulicPumpVolumeInternal[6], 0, 100) *
      map(currentRpm, 0, 500, 30, 100) / 100;
  } else {
    hydraulicPumpVolumeInternalUndelayed = 0;
  }

  // Smooth ramp
  if (hydraulicPumpVolumeInternalUndelayed < hydraulicPumpVolume) hydraulicPumpVolume--;
  if (hydraulicPumpVolumeInternalUndelayed > hydraulicPumpVolume) hydraulicPumpVolume++;

  // Hydraulic flow (boom lowering)
  if (engineRunning && pulseWidth[CH_EX_BOOM] > pulseMaxNeutral[CH_EX_BOOM])
    hydraulicFlowVolumeInternalUndelayed = map(pulseWidth[CH_EX_BOOM], pulseMaxNeutral[CH_EX_BOOM], pulseMax[CH_EX_BOOM] - 200, 0, 100);
  else
    hydraulicFlowVolumeInternalUndelayed = 0;

  if (hydraulicFlowVolumeInternalUndelayed < hydraulicFlowVolume) hydraulicFlowVolume--;
  if (hydraulicFlowVolumeInternalUndelayed > hydraulicFlowVolume) hydraulicFlowVolume++;

  // Track rattle from left track (CH7) and right track (CH8)
  // (Only relevant for tracked excavators with dual stick drive)
  if (pulseWidth[CH_EX_TRACK_L] > 1570) {
    trackRattleVolumeInternal[7] = map(pulseWidth[CH_EX_TRACK_L], 1570, 1800, 0, 100);
    trackLisRotating = true;
  } else if (pulseWidth[CH_EX_TRACK_L] < 1430) {
    trackRattleVolumeInternal[7] = map(pulseWidth[CH_EX_TRACK_L], 1430, 1200, 0, 100);
    trackLisRotating = true;
  } else {
    trackRattleVolumeInternal[7] = 0;
    trackLisRotating = false;
  }

  if (pulseWidth[CH_EX_TRACK_R] > 1570) {
    trackRattleVolumeInternal[8] = map(pulseWidth[CH_EX_TRACK_R], 1570, 1800, 0, 100);
    trackRisRotating = true;
  } else if (pulseWidth[CH_EX_TRACK_R] < 1430) {
    trackRattleVolumeInternal[8] = map(pulseWidth[CH_EX_TRACK_R], 1430, 1200, 0, 100);
    trackRisRotating = true;
  } else {
    trackRattleVolumeInternal[8] = 0;
    trackRisRotating = false;
  }

  tracksAreRotating = trackLisRotating || trackRisRotating;

  if (engineRunning) {
    trackRattleVolumeInternalUndelayed = constrain(trackRattleVolumeInternal[7] + trackRattleVolumeInternal[8], 0, 100) * map(currentRpm, 0, 500, 100, 150) / 100;
  } else {
    trackRattleVolumeInternalUndelayed = 0;
  }

  if (trackRattleVolumeInternalUndelayed < trackRattleVolume) trackRattleVolume--;
  if (trackRattleVolumeInternalUndelayed > trackRattleVolume) trackRattleVolume++;

  // Hydraulic load → knock volume & RPM drop
  hydraulicDependentKnockVolume = map(hydraulicPumpVolume, 0, 100, 50, 100);
  hydraulicLoad = map(hydraulicPumpVolume, 0, 100, 0, 40);

  // Bucket rattle on fast stick movement
  if (engineRunning && currentRpm > 400) {
    static uint16_t lastBucket = 1500, lastDipper = 1500;
    if (abs(pulseWidth[CH_EX_BUCKET] - lastBucket) > 100) bucketRattleTrigger = true;
    lastBucket = pulseWidth[CH_EX_BUCKET];
    if (abs(pulseWidth[CH_EX_SWING] - lastDipper) > 100) bucketRattleTrigger = true;
    lastDipper = pulseWidth[CH_EX_SWING];
  }

  #ifdef TRACK_RATTLE_2
  static uint32_t lastTrackRattle2Time = millis();
  uint32_t interval = constrain(max(trackRattleVolumeInternal[7], trackRattleVolumeInternal[8]), (uint16_t)0, (uint16_t)100);
  interval = map(interval, 0, 100, trackRattleIntervalMax, trackRattleIntervalMin);
  if (millis() - lastTrackRattle2Time > interval) {
    trackRattle2Trigger = true;
    lastTrackRattle2Time = millis();
  }
  #endif
}
#endif

#if defined LOADER_MODE
void loaderControl() {
  // Boom up → pump RPM demand
  if (pulseWidth[CH_LD_BOOM] < pulseMinNeutral[CH_LD_BOOM])
    targetHydraulicRpm[2] = map(pulseWidth[CH_LD_BOOM], pulseMinNeutral[CH_LD_BOOM], pulseMin[CH_LD_BOOM], 0, 300);
  else
    targetHydraulicRpm[2] = 0;

  // Bucket up → pump RPM demand
  if (pulseWidth[CH_LD_BUCKET] < pulseMinNeutral[CH_LD_BUCKET])
    targetHydraulicRpm[1] = map(pulseWidth[CH_LD_BUCKET], pulseMinNeutral[CH_LD_BUCKET], pulseMin[CH_LD_BUCKET], 0, 150);
  else
    targetHydraulicRpm[1] = 0;

  targetHydraulicRpm[0] = targetHydraulicRpm[1] + targetHydraulicRpm[2];
  currentThrottleHydraulic = targetHydraulicRpm[0];

  // Boom lowering → flow sound
  if (pulseWidth[CH_LD_BOOM] > pulseMaxNeutral[CH_LD_BOOM])
    hydraulicFlowVolume = map(pulseWidth[CH_LD_BOOM], pulseMaxNeutral[CH_LD_BOOM], pulseMax[CH_LD_BOOM] - 200, 0, 100);
  else
    hydraulicFlowVolume = 0;
}
#endif

#if defined CRANE_MODE
void hydraulicSound(int i, int rpm, int rpmRev, int vol, int volRev) {
  if (pulseWidth[i] < pulseMinNeutral[i]) {
    targetHydraulicRpm[i] = map(pulseWidth[i], pulseMinNeutral[i], pulseMin[i], 0, rpm);
    hydraulicPumpVolumeArray[i] = map(pulseWidth[i], pulseMinNeutral[i], pulseMin[i], 0, vol);
  } else if (pulseWidth[i] > pulseMaxNeutral[i]) {
    targetHydraulicRpm[i] = map(pulseWidth[i], pulseMaxNeutral[i], pulseMax[i], 0, rpmRev);
    hydraulicPumpVolumeArray[i] = map(pulseWidth[i], pulseMaxNeutral[i], pulseMax[i], 0, volRev);
  } else {
    targetHydraulicRpm[i] = 0;
    hydraulicPumpVolumeArray[i] = 0;
  }
}

void craneControl() {
  hydraulicSound(CH_CR_BOOM, 300, 0, 50, 0);      // Boom lift
  hydraulicSound(CH_CR_EXTEND, 300, 300, 50, 50);  // Boom extension
  hydraulicSound(CH_CR_SWING, 250, 250, 50, 50);   // Swing

  targetHydraulicRpm[0] = targetHydraulicRpm[CH_CR_BOOM] + targetHydraulicRpm[CH_CR_EXTEND] + targetHydraulicRpm[CH_CR_SWING];
  currentThrottleHydraulic = targetHydraulicRpm[0];
  hydraulicPumpVolume = hydraulicPumpVolumeArray[CH_CR_BOOM] + hydraulicPumpVolumeArray[CH_CR_EXTEND] + hydraulicPumpVolumeArray[CH_CR_SWING];

  // Boom lowering → flow sound
  if (pulseWidth[CH_CR_BOOM] > pulseMaxNeutral[CH_CR_BOOM])
    hydraulicFlowVolume = map(pulseWidth[CH_CR_BOOM], pulseMaxNeutral[CH_CR_BOOM], pulseMax[CH_CR_BOOM] - 200, 0, 100);
  else
    hydraulicFlowVolume = 0;

  hydraulicDependentKnockVolume = map(targetHydraulicRpm[0], 0, 100, 50, 100);
}
#endif

#if defined DOZER_MODE
void dozerControl() {
  // ── Track rattle + reverse from the ACTUAL track speed (after the hydrostat droop), so the rattle
  //    speeds up and slows with how fast the tracks are really moving — crawls at idle rpm, lugs down
  //    under load — instead of jumping to full the instant you touch the stick. ──
  int16_t trkSpd = (int16_t)constrain((abs((int)actualTrackL) + abs((int)actualTrackR)) / 10, 0, 100); // 0..100
  tracksAreRotating = (trkSpd > 3);
  trackRattleVolume = engineRunning ? (uint16_t)trkSpd : 0;
  driveWhineVolume  = engineRunning ? (uint16_t)trkSpd : 0; // hydrostatic whine rides track speed too

  escInReverse = (actualTrackL < -20 && actualTrackR < -20); // both tracks driven backward
  escIsDriving = tracksAreRotating;

  // ── Implement pump volume from the actual valve commands (all 4: lift/tilt/angle/ripper) ──
  // Driven off valveCmd, so it's the same in RC and gamepad — blade tilt and angle make the pump
  // sound just like the blade lift does (each function drives the pump up to ~60).
  uint16_t implVol = 0;
  for (int i = 0; i < 4; i++) implVol += (uint16_t)(abs(valveCmd[i]) * 60 / 500);
  hydraulicPumpVolume = constrain(implVol, 0, 100);
  hydraulicDependentKnockVolume = map(hydraulicPumpVolume, 0, 100, 50, 100);
  hydraulicLoad = map(hydraulicPumpVolume, 0, 100, 0, 40);

  // Flow sound when blade is lowering
  if (engineRunning && pulseWidth[CH_DZ_BLADE] > pulseMaxNeutral[CH_DZ_BLADE])
    hydraulicFlowVolume = map(pulseWidth[CH_DZ_BLADE], pulseMaxNeutral[CH_DZ_BLADE], pulseMax[CH_DZ_BLADE] - 200, 0, 100);
  else
    hydraulicFlowVolume = 0;

  // Periodic track chain clank (speed-dependent interval)
  #ifdef TRACK_RATTLE_2
  if (tracksAreRotating && engineRunning) {
    static uint32_t lastTrackRattle2Time = millis();
    uint32_t trkSpd = constrain(trackRattleVolume, (uint16_t)0, (uint16_t)100);
    uint32_t interval = map(trkSpd, 0, 100, trackRattleIntervalMax, trackRattleIntervalMin);
    if (millis() - lastTrackRattle2Time > interval) {
      trackRattle2Trigger = true;
      lastTrackRattle2Time = millis();
    }
  }
  #endif
}
#endif

#if defined GRADER_MODE
void graderControl() {
  // Blade lift → pump volume (heaviest function)
  uint16_t bladeVol = 0;
  if (pulseWidth[CH_GR_BLADE] < pulseMinNeutral[CH_GR_BLADE])
    bladeVol = map(pulseWidth[CH_GR_BLADE], pulseMinNeutral[CH_GR_BLADE], pulseMin[CH_GR_BLADE], 0, 60);
  else if (pulseWidth[CH_GR_BLADE] > pulseMaxNeutral[CH_GR_BLADE])
    bladeVol = map(pulseWidth[CH_GR_BLADE], pulseMaxNeutral[CH_GR_BLADE], pulseMax[CH_GR_BLADE], 0, 40);

  // Circle rotation → pump volume (moldboard angle)
  uint16_t circleVol = 0;
  if (pulseWidth[CH_GR_CIRCLE] < pulseMinNeutral[CH_GR_CIRCLE])
    circleVol = map(pulseWidth[CH_GR_CIRCLE], pulseMinNeutral[CH_GR_CIRCLE], pulseMin[CH_GR_CIRCLE], 0, 40);
  else if (pulseWidth[CH_GR_CIRCLE] > pulseMaxNeutral[CH_GR_CIRCLE])
    circleVol = map(pulseWidth[CH_GR_CIRCLE], pulseMaxNeutral[CH_GR_CIRCLE], pulseMax[CH_GR_CIRCLE], 0, 40);

  // Blade tilt → pump volume (lean left/right)
  uint16_t tiltVol = 0;
  if (pulseWidth[CH_GR_TILT] < pulseMinNeutral[CH_GR_TILT])
    tiltVol = map(pulseWidth[CH_GR_TILT], pulseMinNeutral[CH_GR_TILT], pulseMin[CH_GR_TILT], 0, 30);
  else if (pulseWidth[CH_GR_TILT] > pulseMaxNeutral[CH_GR_TILT])
    tiltVol = map(pulseWidth[CH_GR_TILT], pulseMaxNeutral[CH_GR_TILT], pulseMax[CH_GR_TILT], 0, 30);

  // Articulation steering → pump volume
  uint16_t articulationVol = 0;
  if (pulseWidth[CH_GR_ARTICULATION] < pulseMinNeutral[CH_GR_ARTICULATION])
    articulationVol = map(pulseWidth[CH_GR_ARTICULATION], pulseMinNeutral[CH_GR_ARTICULATION], pulseMin[CH_GR_ARTICULATION], 0, 30);
  else if (pulseWidth[CH_GR_ARTICULATION] > pulseMaxNeutral[CH_GR_ARTICULATION])
    articulationVol = map(pulseWidth[CH_GR_ARTICULATION], pulseMaxNeutral[CH_GR_ARTICULATION], pulseMax[CH_GR_ARTICULATION], 0, 30);

  // Sum as RPM demand for pump sound
  targetHydraulicRpm[0] = bladeVol * 3 + circleVol * 3 + tiltVol * 2 + articulationVol * 2;
  currentThrottleHydraulic = targetHydraulicRpm[0];
  hydraulicPumpVolume = constrain(bladeVol + circleVol + tiltVol + articulationVol, 0, 100);

  // Blade lowering → flow sound (gravity return)
  if (pulseWidth[CH_GR_BLADE] > pulseMaxNeutral[CH_GR_BLADE])
    hydraulicFlowVolume = map(pulseWidth[CH_GR_BLADE], pulseMaxNeutral[CH_GR_BLADE], pulseMax[CH_GR_BLADE] - 200, 0, 100);
  else
    hydraulicFlowVolume = 0;

  hydraulicDependentKnockVolume = map(targetHydraulicRpm[0], 0, 100, 50, 100);
}
#endif

#if defined SKIDSTEER_MODE
void skidSteerControl() {
  // Bucket → pump RPM demand
  if (pulseWidth[CH_SS_BUCKET] < pulseMinNeutral[CH_SS_BUCKET])
    targetHydraulicRpm[1] = map(pulseWidth[CH_SS_BUCKET], pulseMinNeutral[CH_SS_BUCKET], pulseMin[CH_SS_BUCKET], 0, 150);
  else
    targetHydraulicRpm[1] = 0;

  // Boom → pump RPM demand
  if (pulseWidth[CH_SS_BOOM] < pulseMinNeutral[CH_SS_BOOM])
    targetHydraulicRpm[2] = map(pulseWidth[CH_SS_BOOM], pulseMinNeutral[CH_SS_BOOM], pulseMin[CH_SS_BOOM], 0, 300);
  else
    targetHydraulicRpm[2] = 0;

  targetHydraulicRpm[0] = targetHydraulicRpm[1] + targetHydraulicRpm[2];
  currentThrottleHydraulic = targetHydraulicRpm[0];

  // Boom lowering → flow sound
  if (pulseWidth[CH_SS_BOOM] > pulseMaxNeutral[CH_SS_BOOM])
    hydraulicFlowVolume = map(pulseWidth[CH_SS_BOOM], pulseMaxNeutral[CH_SS_BOOM], pulseMax[CH_SS_BOOM] - 200, 0, 100);
  else
    hydraulicFlowVolume = 0;
}
#endif

#if defined BACKHOE_MODE
// Backhoe loader: rear boom / dipper / bucket / swing hydraulics + drive. Sound only — the drive
// and the 4 implements are output through the shared generic path (driveMixer/implementControl).
void backhoeControl() {
  const uint8_t fns[4] = {CH_BH_BOOM, CH_BH_DIPPER, CH_BH_BUCKET, CH_BH_SWING};
  const int     wt[4]  = {35, 30, 25, 25}; // % pump demand each function draws at full stroke

  // Summed proportional pump whine, scaled by rpm; smooth ramp like the other machines.
  int demand = 0;
  for (int i = 0; i < 4; i++) {
    int off = abs((int)pulseWidth[fns[i]] - 1500);
    if (off > 40) demand += map(constrain(off, 40, 500), 40, 500, 0, wt[i]);
  }
  int target = engineRunning ? constrain(demand, 0, 100) * map(currentRpm, 0, 500, 30, 100) / 100 : 0;
  if (target < hydraulicPumpVolume) hydraulicPumpVolume--;
  if (target > hydraulicPumpVolume) hydraulicPumpVolume++;

  // Boom lowering → flow hiss.
  int flow = (engineRunning && pulseWidth[CH_BH_BOOM] > pulseMaxNeutral[CH_BH_BOOM])
    ? map(pulseWidth[CH_BH_BOOM], pulseMaxNeutral[CH_BH_BOOM], pulseMax[CH_BH_BOOM] - 200, 0, 100) : 0;
  if (flow < hydraulicFlowVolume) hydraulicFlowVolume--;
  if (flow > hydraulicFlowVolume) hydraulicFlowVolume++;

  // Load → diesel knock + rpm sag feed (same coupling the excavator uses).
  hydraulicDependentKnockVolume = map(hydraulicPumpVolume, 0, 100, 50, 100);
  hydraulicLoad = map(hydraulicPumpVolume, 0, 100, 0, 40);
}
#endif

// ════════════════════════════════════════════════════════════════
// DAC OFFSET FADE (prevents pop on startup)
// ════════════════════════════════════════════════════════════════

void dacOffsetFade() {
  static unsigned long lastFrame = millis();
  if (millis() - lastFrame < 10) return;
  lastFrame = millis();

  if (dacOffset < 128) dacOffset++;
}

// ════════════════════════════════════════════════════════════════
// CORE 0 TASK — Audio engine, ESC, mass sim, shaker
// ════════════════════════════════════════════════════════════════

void Task1code(void *parameters) {
  while (true) {
    rtc_wdt_feed();
    dacOffsetFade();
    engineMassSimulation();
    esc();
    vTaskDelay(1); // feed watchdog
  }
}

// ════════════════════════════════════════════════════════════════
// NVS CHANNEL MAPPING — runtime-configurable, no rebuild needed
// ════════════════════════════════════════════════════════════════

Preferences nvsPrefs;

// Table mapping variable names to their pointers
struct ChMapEntry { const char* name; uint8_t* ptr; };
const ChMapEntry CH_MAP[] = {
  {"CH_THROTTLE",      &CH_THROTTLE},
  {"CH_HORN",          &CH_HORN},
  {"CH_ENGINE_TOGGLE", &CH_ENGINE_TOGGLE},
  {"CH_HILO_TOGGLE",   &CH_HILO_TOGGLE},
  {"CH_EX_BUCKET",     &CH_EX_BUCKET},
  {"CH_EX_SWING",      &CH_EX_SWING},
  {"CH_EX_BOOM",       &CH_EX_BOOM},
  {"CH_EX_STICK",      &CH_EX_STICK},
  {"CH_EX_TRACK_L",    &CH_EX_TRACK_L},
  {"CH_EX_TRACK_R",    &CH_EX_TRACK_R},
  {"CH_LD_BUCKET",     &CH_LD_BUCKET},
  {"CH_LD_BOOM",       &CH_LD_BOOM},
  {"CH_CR_BOOM",       &CH_CR_BOOM},
  {"CH_CR_EXTEND",     &CH_CR_EXTEND},
  {"CH_CR_SWING",      &CH_CR_SWING},
  {"CH_DZ_BLADE",      &CH_DZ_BLADE},
  {"CH_DZ_TILT",       &CH_DZ_TILT},
  {"CH_DZ_RIPPER",     &CH_DZ_RIPPER},
  {"CH_SS_BUCKET",     &CH_SS_BUCKET},
  {"CH_SS_BOOM",       &CH_SS_BOOM},
  {"CH_GR_BLADE",      &CH_GR_BLADE},
  {"CH_GR_CIRCLE",     &CH_GR_CIRCLE},
  {"CH_GR_TILT",       &CH_GR_TILT},
  {"CH_GR_ARTICULATION", &CH_GR_ARTICULATION},
  {"CH_LIGHTS",          &CH_LIGHTS},
};
const int CH_MAP_COUNT = sizeof(CH_MAP) / sizeof(CH_MAP[0]);

// ── Runtime settings table (volatile int volumes only) ──
struct SettingEntry { const char* name; volatile int* ptr; int minVal; int maxVal; };
const SettingEntry SETTINGS_MAP[] = {
  {"masterVolume",                  &masterVolume,                    0, 200},
  {"idleVolumePercentage",          &idleVolumePercentage,            0, 300},
  {"dieselKnockVolumePercentage",   &dieselKnockVolumePercentage,     0, 1000},
  {"turboVolumePercentage",         &turboVolumePercentage,           0, 300},
  {"hornVolumePercentage",          &hornVolumePercentage,            0, 300},
  {"brakeVolumePercentage",         &brakeVolumePercentage,           0, 300},
  {"hydraulicPumpVolumePercentage", &hydraulicPumpVolumePercentage,   0, 300},
  {"hydraulicFlowVolumePercentage", &hydraulicFlowVolumePercentage,   0, 300},
  {"trackRattleVolumePercentage",   &trackRattleVolumePercentage,     0, 300},
  {"bucketRattleVolumePercentage",  &bucketRattleVolumePercentage,    0, 300},
  {"reversingVolumePercentage",     &reversingVolumePercentage,       0, 300},
  {"startVolumePercentage",         &startVolumePercentage,           0, 300},
};

void loadChannelsFromNVS() {
  nvsPrefs.begin("chmap", true);  // read-only
  for (int i = 0; i < CH_MAP_COUNT; i++) {
    // NVS key max 15 chars — use short key (index)
    char key[4];
    snprintf(key, sizeof(key), "c%d", i);
    if (nvsPrefs.isKey(key)) {
      *CH_MAP[i].ptr = nvsPrefs.getUChar(key, *CH_MAP[i].ptr);
    }
  }
  // Load channel reverse flags
  for (int i = 1; i <= 16; i++) {
    char key[4];
    snprintf(key, sizeof(key), "r%d", i);
    if (nvsPrefs.isKey(key)) {
      channelReversed[i] = nvsPrefs.getBool(key, false);
    }
  }
  // Load channel enable flags
  for (int i = 1; i <= 16; i++) {
    char key[4];
    snprintf(key, sizeof(key), "e%d", i);
    if (nvsPrefs.isKey(key)) {
      channelEnabled[i] = nvsPrefs.getBool(key, true);
    }
  }
  nvsPrefs.end();
}

void saveChannelsToNVS() {
  nvsPrefs.begin("chmap", false);  // read-write
  for (int i = 0; i < CH_MAP_COUNT; i++) {
    char key[4];
    snprintf(key, sizeof(key), "c%d", i);
    nvsPrefs.putUChar(key, *CH_MAP[i].ptr);
  }
  // Save channel reverse flags
  for (int i = 1; i <= 16; i++) {
    char key[4];
    snprintf(key, sizeof(key), "r%d", i);
    nvsPrefs.putBool(key, channelReversed[i]);
  }
  // Save channel enable flags
  for (int i = 1; i <= 16; i++) {
    char key[4];
    snprintf(key, sizeof(key), "e%d", i);
    nvsPrefs.putBool(key, channelEnabled[i]);
  }
  nvsPrefs.end();
}

const int SETTINGS_MAP_COUNT = sizeof(SETTINGS_MAP) / sizeof(SETTINGS_MAP[0]);

void loadSettingsFromNVS() {
  nvsPrefs.begin("settings", true);
  for (int i = 0; i < SETTINGS_MAP_COUNT; i++) {
    char key[4];
    snprintf(key, sizeof(key), "s%d", i);
    if (nvsPrefs.isKey(key)) {
      *SETTINGS_MAP[i].ptr = nvsPrefs.getInt(key, *SETTINGS_MAP[i].ptr);
    }
  }
  // Small-type settings
  if (nvsPrefs.isKey("acc"))   acc = nvsPrefs.getChar("acc", acc);
  if (nvsPrefs.isKey("dec"))   dec = nvsPrefs.getChar("dec", dec);
  if (nvsPrefs.isKey("rl"))    escRampTimeLow = nvsPrefs.getUChar("rl", escRampTimeLow);
  if (nvsPrefs.isKey("rh"))    escRampTimeHigh = nvsPrefs.getUChar("rh", escRampTimeHigh);
  if (nvsPrefs.isKey("bs"))    escBrakeSteps = nvsPrefs.getUChar("bs", escBrakeSteps);
  if (nvsPrefs.isKey("as"))    escAccelerationSteps = nvsPrefs.getUChar("as", escAccelerationSteps);
  if (nvsPrefs.isKey("autoSt")) autoEngineStart = nvsPrefs.getBool("autoSt", autoEngineStart);
  nvsPrefs.end();
}

void saveSettingsToNVS() {
  nvsPrefs.begin("settings", false);
  for (int i = 0; i < SETTINGS_MAP_COUNT; i++) {
    char key[4];
    snprintf(key, sizeof(key), "s%d", i);
    nvsPrefs.putInt(key, *SETTINGS_MAP[i].ptr);
  }
  nvsPrefs.putChar("acc", acc);
  nvsPrefs.putChar("dec", dec);
  nvsPrefs.putUChar("rl", escRampTimeLow);
  nvsPrefs.putUChar("rh", escRampTimeHigh);
  nvsPrefs.putUChar("bs", escBrakeSteps);
  nvsPrefs.putUChar("as", escAccelerationSteps);
  nvsPrefs.putBool("autoSt", autoEngineStart);
  nvsPrefs.end();
}

// Serial command buffer
static char serialBuf[128];
static uint8_t serialBufIdx = 0;

void processSerialCommand(const char* cmd) {
  // CH:NAME=VALUE — set a channel mapping
  if (strncmp(cmd, "CH:", 3) == 0) {
    const char* eq = strchr(cmd + 3, '=');
    if (!eq) { Serial.println("ERR:bad format"); return; }
    
    // Extract name
    int nameLen = eq - (cmd + 3);
    if (nameLen <= 0 || nameLen > 20) { Serial.println("ERR:bad name"); return; }
    char name[24];
    strncpy(name, cmd + 3, nameLen);
    name[nameLen] = '\0';
    
    // Extract value (0 = unassigned/none)
    int val = atoi(eq + 1);
    if (val < 0 || val > 16) { Serial.println("ERR:ch 0-16"); return; }
    
    // Find and set
    for (int i = 0; i < CH_MAP_COUNT; i++) {
      if (strcmp(name, CH_MAP[i].name) == 0) {
        *CH_MAP[i].ptr = (uint8_t)val;
        Serial.printf("OK:%s=%d\n", name, val);
        return;
      }
    }
    Serial.printf("ERR:unknown %s\n", name);
  }
  // SET:NAME=VALUE — set a runtime setting (volume, acc, etc.)
  else if (strncmp(cmd, "SET:", 4) == 0) {
    const char* eq = strchr(cmd + 4, '=');
    if (!eq) { Serial.println("ERR:bad format"); return; }

    int nameLen = eq - (cmd + 4);
    if (nameLen <= 0 || nameLen > 40) { Serial.println("ERR:bad name"); return; }
    char name[44];
    strncpy(name, cmd + 4, nameLen);
    name[nameLen] = '\0';

    int val = atoi(eq + 1);

    // Check boolean settings first
    if (strcmp(name, "autoEngineStart") == 0) {
      autoEngineStart = (val != 0);
      Serial.printf("OK:%s=%d\n", name, autoEngineStart ? 1 : 0);
      return;
    }

    // Small-type settings (int8_t / uint8_t — can't use pointer table)
    #define SET_SMALL(N, VAR, LO, HI) if(strcmp(name,N)==0){VAR=constrain(val,LO,HI);Serial.printf("OK:%s=%d\n",name,(int)VAR);return;}
    SET_SMALL("acc",                  acc,                  1, 9)
    SET_SMALL("dec",                  dec,                  1, 9)
    SET_SMALL("escRampTimeLow",       escRampTimeLow,       5, 200)
    SET_SMALL("escRampTimeHigh",      escRampTimeHigh,      5, 200)
    SET_SMALL("escBrakeSteps",        escBrakeSteps,        1, 100)
    SET_SMALL("escAccelerationSteps", escAccelerationSteps, 1, 20)
    #undef SET_SMALL

    // Check numeric settings (volatile int volumes)
    for (int i = 0; i < SETTINGS_MAP_COUNT; i++) {
      if (strcmp(name, SETTINGS_MAP[i].name) == 0) {
        val = constrain(val, SETTINGS_MAP[i].minVal, SETTINGS_MAP[i].maxVal);
        *SETTINGS_MAP[i].ptr = val;
        Serial.printf("OK:%s=%d\n", name, val);
        return;
      }
    }
    Serial.printf("ERR:unknown %s\n", name);
  }
  // REV:CH=0|1 — set channel reverse flag
  else if (strncmp(cmd, "REV:", 4) == 0) {
    const char* eq = strchr(cmd + 4, '=');
    if (!eq) { Serial.println("ERR:bad format"); return; }
    int ch = atoi(cmd + 4);
    int val = atoi(eq + 1);
    if (ch < 1 || ch > 16) { Serial.println("ERR:ch 1-16"); return; }
    channelReversed[ch] = (val != 0);
    Serial.printf("OK:REV:%d=%d\n", ch, channelReversed[ch] ? 1 : 0);
  }
  // EN:CH=0|1 — set channel enable flag
  else if (strncmp(cmd, "EN:", 3) == 0) {
    const char* eq = strchr(cmd + 3, '=');
    if (!eq) { Serial.println("ERR:bad format"); return; }
    int ch = atoi(cmd + 3);
    int val = atoi(eq + 1);
    if (ch < 1 || ch > 16) { Serial.println("ERR:ch 1-16"); return; }
    channelEnabled[ch] = (val != 0);
    Serial.printf("OK:EN:%d=%d\n", ch, channelEnabled[ch] ? 1 : 0);
  }
  // SAVE — persist current channel mapping + settings to NVS
  else if (strcmp(cmd, "SAVE") == 0) {
    saveChannelsToNVS();
    saveSettingsToNVS();
    Serial.println("OK:SAVED");
  }
  // DUMP — print all current channel mappings + settings
  else if (strcmp(cmd, "DUMP") == 0) {
    for (int i = 0; i < CH_MAP_COUNT; i++) {
      Serial.printf("CH:%s=%d\n", CH_MAP[i].name, *CH_MAP[i].ptr);
    }
    for (int i = 0; i < SETTINGS_MAP_COUNT; i++) {
      Serial.printf("SET:%s=%d\n", SETTINGS_MAP[i].name, *SETTINGS_MAP[i].ptr);
    }
    Serial.printf("SET:acc=%d\n", (int)acc);
    Serial.printf("SET:dec=%d\n", (int)dec);
    Serial.printf("SET:escRampTimeLow=%d\n", (int)escRampTimeLow);
    Serial.printf("SET:escRampTimeHigh=%d\n", (int)escRampTimeHigh);
    Serial.printf("SET:escBrakeSteps=%d\n", (int)escBrakeSteps);
    Serial.printf("SET:escAccelerationSteps=%d\n", (int)escAccelerationSteps);
    Serial.printf("SET:autoEngineStart=%d\n", autoEngineStart ? 1 : 0);
    for (int i = 1; i <= 16; i++) {
      Serial.printf("REV:%d=%d\n", i, channelReversed[i] ? 1 : 0);
    }
    for (int i = 1; i <= 16; i++) {
      Serial.printf("EN:%d=%d\n", i, channelEnabled[i] ? 1 : 0);
    }
    Serial.println("OK:DUMP");
  }
  // PING — heartbeat
  else if (strcmp(cmd, "PING") == 0) {
    Serial.println("OK:PONG");
  }
  else {
    Serial.printf("ERR:unknown cmd '%s'\n", cmd);
  }
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBufIdx > 0) {
        serialBuf[serialBufIdx] = '\0';
        processSerialCommand(serialBuf);
        serialBufIdx = 0;
      }
    } else if (serialBufIdx < sizeof(serialBuf) - 1) {
      serialBuf[serialBufIdx++] = c;
    }
  }
}

// ════════════════════════════════════════════════════════════════
// BATTERY MONITOR — voltage divider on VN, spoken low-battery warning
// ════════════════════════════════════════════════════════════════

// Averaged pack voltage. calibration = (Rtop+Rbottom)/Rbottom + diode trim.
float batteryVolts() {
  static float raw[6];
  static bool initDone = false;
  float cal = (batteryRtop + batteryRbottom) / batteryRbottom + batteryDiodeDrop;
  if (!initDone) { for (uint8_t i = 0; i < 6; i++) raw[i] = battery.readVoltage(); initDone = true; }
  raw[5] = raw[4]; raw[4] = raw[3]; raw[3] = raw[2]; raw[2] = raw[1]; raw[1] = raw[0];
  raw[0] = battery.readVoltage();
  return (raw[0] + raw[1] + raw[2] + raw[3] + raw[4] + raw[5]) / 6.0f * cal;
}

// Detect how many LiPo cells are in series from a plausible pack voltage.
void detectCells(float v) {
  if (batteryCellsOverride > 0) { numberOfCells = batteryCellsOverride; }
  else {
    float setpoint = cellCutoffVoltage - (cellFullVoltage - cellCutoffVoltage) / 2.0f; // midpoint-ish per cell
    numberOfCells = 1;
    if (v > setpoint * 2) numberOfCells = 2;
    if (v > setpoint * 3) numberOfCells = 3;
    if (v > cellFullVoltage * 3) numberOfCells = 4;
  }
  batteryCutoffvoltage = cellCutoffVoltage * numberOfCells;
  Serial.printf("Battery: %.2f V, %dS pack, cutoff %.2f V\n", v, numberOfCells, batteryCutoffvoltage);
}

// Poll the pack, flag protection, and fire the spoken warning + re-announce while low.
void updateBattery() {
  if (!batteryMonitorEnabled) return;
  static uint32_t last = 0;
  if (millis() - last < 300) return;
  last = millis();

  batteryVoltage = batteryVolts();

  // Under ~5.5 V means we're on USB / no pack connected — don't nag or false-alarm on the bench.
  if (batteryVoltage < 5.5f) { batteryProtection = false; numberOfCells = 0; return; }

  if (numberOfCells == 0) detectCells(batteryVoltage); // a pack just came up — size it

  static uint32_t lastAnnounce = 0;
  if (batteryVoltage < batteryCutoffvoltage) {
    if (!batteryProtection) {                       // just crossed the threshold
      batteryProtection = true;
      lowBatteryTrigger = true;                     // say "low battery" once now
      lastAnnounce = millis();
      Serial.printf("LOW BATTERY %.2f V (< %.2f V) — bring it home!\n", batteryVoltage, batteryCutoffvoltage);
    } else if (millis() - lastAnnounce > 45000) {   // keep reminding every 45 s while low
      lowBatteryTrigger = true;
      lastAnnounce = millis();
    }
  } else if (batteryVoltage > batteryCutoffvoltage + 0.25f * numberOfCells) {
    batteryProtection = false;                       // recovered (hysteresis)
  }
}

// Start the two DAC playback timers (the 44 kHz audio interrupt engine). On gamepad builds this is
// held off until a controller connects — a silent, interrupt-free chip pairs FAR more reliably.
bool audioTimersStarted = false;
void startAudioTimers() {
  if (audioTimersStarted) return;
  audioTimersStarted = true;
  variableTimer = timerBegin(0, 20, true);
  timerAttachInterrupt(variableTimer, &variablePlaybackTimer, true);
  timerAlarmWrite(variableTimer, variableTimerTicks, true);
  timerAlarmEnable(variableTimer);

  fixedTimer = timerBegin(1, 20, true);
  timerAttachInterrupt(fixedTimer, &fixedPlaybackTimer, true);
  timerAlarmWrite(fixedTimer, variableTimerTicks, true);
  timerAlarmEnable(fixedTimer);
}

// ════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════

void setup() {

  Serial.begin(115200);
  Serial.println("HydraulicController starting...");

#if defined GAMEPAD_MODE
  // Bring Bluetooth up FIRST — before the sound-pack load and all the other init — so the stack is
  // ready and a bonded controller can reconnect the instant the battery comes up (fastest connect).
  setupGamepad();
#endif

  // Load channel mapping from NVS (overrides defaults in config.h)
  loadChannelsFromNVS();
  loadSettingsFromNVS();
  Serial.println("Channel mapping + settings loaded from NVS");

  // Try to load sound pack from flash partition (overrides compiled-in sounds)
  if (!loadSoundPack()) {
    Serial.println("Using compiled-in default sounds");
  }

  // Semaphores
  xPwmSemaphore = xSemaphoreCreateMutex();
  xRpmSemaphore = xSemaphoreCreateMutex();

  // DAC — init both channels with zero output
  dacWrite(DAC1_PIN, 0); // GPIO 25
  dacWrite(DAC2_PIN, 0); // GPIO 26

  // Work-light GPIO (front / rear / side)
  pinMode(FRONT_WORKLIGHT_PIN, OUTPUT);
  pinMode(REAR_WORKLIGHT_PIN, OUTPUT);
  pinMode(SIDE_LIGHT_PIN, OUTPUT);
  digitalWrite(FRONT_WORKLIGHT_PIN, LOW);
  digitalWrite(REAR_WORKLIGHT_PIN, LOW);
  digitalWrite(SIDE_LIGHT_PIN, LOW);

  // Servo outputs — all six on MCPWM (hardware PWM, zero ISR load, can't starve the DAC ISRs).
  mcpwm_config_t servo_config;
  servo_config.frequency = 50;
  servo_config.cmpr_a = 0;
  servo_config.cmpr_b = 0;
  servo_config.counter_mode = MCPWM_UP_COUNTER;
  servo_config.duty_mode = MCPWM_DUTY_MODE_0;

  // Every machine now drives the same 6 native MCPWM outputs: 2 drive on Unit0 Timer0, 4 implements
  // on Unit1 (Timer0 A/B + Timer1 A/B). GPIO33 is a native output for all machines (no separate ESC).
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, TRACK_R_PIN);     // GPIO13  drive A (right track / drive motor)
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, TRACK_L_PIN);     // GPIO12  drive B (left track / steer servo)
  mcpwm_gpio_init(MCPWM_UNIT_1, MCPWM0A, BLADE_LIFT_PIN);  // GPIO33  implement 1
  mcpwm_gpio_init(MCPWM_UNIT_1, MCPWM0B, BLADE_TILT_PIN);  // GPIO32  implement 2
  mcpwm_gpio_init(MCPWM_UNIT_1, MCPWM1A, BLADE_ANGLE_PIN); // GPIO14  implement 3
  mcpwm_gpio_init(MCPWM_UNIT_1, MCPWM1B, RIPPER_PIN);      // GPIO27  implement 4
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &servo_config);  // drive
  mcpwm_init(MCPWM_UNIT_1, MCPWM_TIMER_0, &servo_config);  // implements 1 + 2
  mcpwm_init(MCPWM_UNIT_1, MCPWM_TIMER_1, &servo_config);  // implements 3 + 4

  // RC input setup (gamepad already started at the top of setup() for the fastest connect)
#if defined GAMEPAD_MODE
  // nothing here — setupGamepad() ran first thing
#elif defined SBUS_COMMUNICATION
  #if defined EMBEDDED_SBUS
    sBus.begin(COMMAND_RX, COMMAND_TX, sbusInverted, sbusBaud);
  #else
    sBus.begin();
  #endif
#elif defined IBUS_COMMUNICATION
  iBus.begin(Serial2, IBUSBM_NOTIMER, COMMAND_RX, COMMAND_TX);
#elif defined SUMD_COMMUNICATION
  sumd.begin(COMMAND_RX);
#elif defined PPM_COMMUNICATION
  pinMode(COMMAND_RX, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COMMAND_RX), readPpm, RISING);
#elif defined PWM_COMMUNICATION
  // RMT setup for PWM reading
  rmt_config_t rmt_rx;
  rmt_rx.rmt_mode = RMT_MODE_RX;
  rmt_rx.clk_div = RMT_RX_CLK_DIV;
  rmt_rx.mem_block_num = 1;
  rmt_rx.rx_config.filter_en = true;
  rmt_rx.rx_config.filter_ticks_thresh = 100;
  rmt_rx.rx_config.idle_threshold = RMT_RX_MAX_US * RMT_TICK_PER_US;

  for (uint8_t i = 0; i < PWM_CHANNELS_NUM; i++) {
    rmt_rx.channel = (rmt_channel_t)PWM_CHANNELS[i];
    rmt_rx.gpio_num = (gpio_num_t)PWM_PINS[i];
    rmt_config(&rmt_rx);
    rmt_driver_install(rmt_rx.channel, 0, 0);
    rmt_set_rx_intr_en(rmt_rx.channel, true);
    rmt_rx_start(rmt_rx.channel, true);
  }
  rmt_isr_register(rmt_isr_handler, NULL, 0, NULL);
#endif

#if !defined GAMEPAD_MODE
  // Wait for the RC signal to auto-zero. (Skipped on gamepad builds — there's no RC bus to wait for,
  // and blocking here just starves the Bluetooth stack right when you're trying to pair.)
  Serial.println("Waiting for RC signal...");
  uint32_t rcTimeout = millis();
  while (!autoZeroDone && millis() - rcTimeout < 5000) {
    readSbusCommands();
    readIbusCommands();
    readSumdCommands();
    readPpmCommands();
    readPwmSignals();
    processRawChannels();
    rtc_wdt_feed();
    delay(20);
  }
  Serial.println(autoZeroDone ? "RC calibrated!" : "RC timeout — using defaults");
#endif

  // Battery monitor: attach the ADC and size the pack (if one's connected).
  if (batteryMonitorEnabled) {
    battery.attach(BATTERY_DETECT_PIN);
    float v = batteryVolts();
    if (v > 5.5f) detectCells(v); else Serial.println("Battery: on USB / no pack — monitor idle.");
  }

  // Audio timers. On RC builds, start now. On gamepad builds, HOLD OFF until a controller connects
  // (see loop) so the 44 kHz interrupt storm doesn't jitter the Bluetooth pairing/handshake.
#if !defined GAMEPAD_MODE
  startAudioTimers();
#endif

  // Start Core 0 task
  xTaskCreatePinnedToCore(Task1code, "Task1", 8192, NULL, 1, &Task1, 0);

  Serial.println("HydraulicController ready!");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// ════════════════════════════════════════════════════════════════
// MAIN LOOP (Core 1) — RC input + hydraulic control + servos
// ════════════════════════════════════════════════════════════════

void loop() {
  rtc_wdt_feed();

  // Handle serial config commands (channel mapping, etc.)
  handleSerialCommands();

  // Read RC (or the gamepad — it fills the same channels)
#if defined GAMEPAD_MODE
  readGamepadCommands();
  gamepadRepairCheck();   // hold BOOT ~3s to forget bonds and re-pair a new controller
  // Fire up the audio engine only once a controller is actually connected — keeps the chip quiet
  // and interrupt-free during pairing, which is when Bluetooth is most easily disrupted.
  if (gamepadConnected && !audioTimersStarted) startAudioTimers();
#else
  readSbusCommands();
  readIbusCommands();
  readSumdCommands();
  readPpmCommands();
  readPwmSignals();
#endif
  processRawChannels();
  failsafeRcSignals();

  // Throttle & sound triggers
  mapThrottle();

  // Drive + implements — shared by every machine (generic 6-output path).
  driveMixer();        // sticks → commanded drive effort (tank mix or drive/steer)
  hydrostaticModel();  // swashplate ramp + speed droop (tracked) / drive ramp (wheeled)
  implementControl();  // proportional implement valves

  // Machine-specific SOUND control (pump/flow/rattle voicing from that machine's channels).
#if defined EXCAVATOR_MODE
  excavatorControl();
#elif defined LOADER_MODE
  loaderControl();
#elif defined CRANE_MODE
  craneControl();
#elif defined DOZER_MODE
  dozerControl();
#elif defined SKIDSTEER_MODE
  skidSteerControl();
#elif defined GRADER_MODE
  graderControl();
#elif defined BACKHOE_MODE
  backhoeControl();
#endif

  loadModel();         // total pump demand → governor bog (must run after the sound set above)

  updateBattery();     // poll pack voltage → low-battery warning (voice + controller)

  // Servo output
  mcpwmOutput();

#if defined GAMEPAD_MODE
  updateGamepadRumble(); // engine-feel haptics (only if GP_RUMBLE)
  updateGamepadLED();    // lightbar: idle green → load amber → bog red
#endif


#ifdef DEBUG_RC
  static unsigned long lastDebug = millis();
  if (millis() - lastDebug > 500) {
    lastDebug = millis();
    Serial.printf("CH1:%d CH2:%d CH3:%d CH4:%d CH5:%d CH6:%d | RPM:%d THR:%d | Pump:%d Flow:%d Track:%d\n",
      pulseWidth[1], pulseWidth[2], pulseWidth[3], pulseWidth[4], pulseWidth[5], pulseWidth[6],
      currentRpm, currentThrottle, hydraulicPumpVolume, hydraulicFlowVolume, trackRattleVolume);
  }
#endif
}
