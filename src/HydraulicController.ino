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
volatile uint32_t trackRattle2TriggerInterval = 0;
volatile uint16_t hydraulicDependentKnockVolume = 100;
volatile uint16_t hydraulicLoad = 0;
volatile bool engineLugging = false; // dozer governor: engine bogged under heavy load
int16_t totalFlowDemand = 0;         // dozer: drive + implement pump demand (read by the governor)
volatile bool reliefActive = false;  // dozer: relief valve open — read by the audio ISR

volatile uint8_t dacOffset = 0;

// Throttle
int16_t currentThrottle = 0;
int16_t currentThrottleHydraulic = 0;
int16_t currentThrottleFaded = 0;

// Engine
const int16_t maxRpm = 500;
const int16_t minRpm = 0;
int32_t currentRpm = 0;
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

  // Mix & output to DAC1
  uint8_t value = constrain(((a * 8 / 10) + (b / 2) + (c / 5) + (d / 5) + (e / 5) + f + g) * masterVolume / 100 + dacOffset, 0, 255);
  SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, value, RTC_IO_PDAC1_DAC_S);
}

// ════════════════════════════════════════════════════════════════
// FIXED-RATE PLAYBACK ISR (Aux sounds → DAC2)
// ════════════════════════════════════════════════════════════════

void IRAM_ATTR fixedPlaybackTimer() {

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
  static uint32_t curHydraulicFlowSample = 0;
  static uint32_t curTrackRattleSample = 0;
  static uint32_t curTrackRattle2Sample = 0;
  static uint32_t curBucketRattleSample = 0;
  static int32_t a, a1, a2 = 0;
  static int32_t b, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9 = 0;
  static int32_t c, c1, c2, c3, c4, c5 = 0;
  static uint32_t curReliefSample = 0;
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

  // Travel alarm (reversing beep — or both directions)
  boolean alarmActive = engineRunning && (escInReverse || (travelAlarmBothDirections && escIsDriving));
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

  // Hydraulic fluid flow
  if (engineRunning && curHydraulicFlowSample < hydraulicFlowSampleCount - 1) {
    c1 = (hydraulicFlowSamples[curHydraulicFlowSample] * hydraulicFlowVolumePercentage / 100 * hydraulicFlowVolume / 100);
    curHydraulicFlowSample++;
  } else {
    curHydraulicFlowSample = 0;
  }

  // Track rattle (continuous)
  if (tracksAreRotating && curTrackRattleSample < trackRattleSampleCount - 1) {
    c2 = (trackRattleSamples[curTrackRattleSample] * trackRattleVolumePercentage / 100 * trackRattleVolume / 100);
    curTrackRattleSample++;
  } else {
    curTrackRattleSample = 0;
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

  // Relief-valve squeal (loops while the relief is active)
  if (reliefActive && curReliefSample < reliefSquealSampleCount - 1) {
    c5 = (reliefSquealSamples[curReliefSample] * reliefSquealVol / 100);
    curReliefSample++;
  } else {
    c5 = 0;
    curReliefSample = 0;
  }

  // Mix & output to DAC2
  a = a1 + a2;
  b = b0 * 5 + b1 + b2 / 2 + b3 + b4 + b5 + b6 + b7 + b8 + b9;
  c = c1 + c2 + c3 + c4 + c5;
  d = d1 + d2;

  uint8_t value = constrain(((a * 8 / 10) + (b * 2 / 10) + c + d) * masterVolume / 100 + dacOffset, 0, 255);
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

  // ── Auto idle-down: if throttle is up but nothing is moving, idle down after 4s ──
  {
    static unsigned long lastActivity = 0;
    static boolean idledDown = false;
    static int16_t prevThrottle = 0;

    // Detect any control activity (tracks, blade, ripper, or throttle change)
    boolean activity = false;
#if defined DOZER_MODE
    activity = abs((int)pulseWidth[CH_DZ_TRACK_L] - 1500) > 80
            || abs((int)pulseWidth[CH_DZ_TRACK_R] - 1500) > 80
            || abs((int)pulseWidth[CH_DZ_BLADE] - 1500) > 80
            || abs((int)pulseWidth[CH_DZ_RIPPER] - 1500) > 80;
#elif defined EXCAVATOR_MODE
    activity = abs((int)pulseWidth[CH_EX_TRACK_L] - 1500) > 80
            || abs((int)pulseWidth[CH_EX_TRACK_R] - 1500) > 80
            || abs((int)pulseWidth[CH_EX_BOOM] - 1500) > 80
            || abs((int)pulseWidth[CH_EX_STICK] - 1500) > 80
            || abs((int)pulseWidth[CH_EX_BUCKET] - 1500) > 80;
#endif

    // Throttle stick movement also counts as activity (cycling throttle snaps out of idle-down)
    if (abs(currentThrottle - prevThrottle) > 30) activity = true;
    prevThrottle = currentThrottle;

    if (activity) {
      lastActivity = millis();
      idledDown = false;
    }

    // After 4s of no activity, drop to 25% of commanded throttle
    if (currentThrottle > 0 && !activity && millis() - lastActivity > 4000) {
      currentThrottle = currentThrottle / 4;
      idledDown = true;
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

  // Lights: simple on/off toggle, 2 fast flicks = cycle mode
  // Single flick: toggle on/off (using current mode, default front lights)
  // Double flick (2 toggles within 500ms): cycle mode: front → work → all → front
  {
    static boolean lightsToggleLock = false;

    if (pulseWidth[CH_LIGHTS] > 1700 && !lightsToggleLock) {
      lightsToggleLock = true;

      // Cycle: off → headlights → work lights → both → off
      lightsMode++;
      if (lightsMode > 3) lightsMode = 0;
      lightsOn = (lightsMode > 0);
      lightsState = lightsOn ? 1 : 0;
    }
    if (pulseWidth[CH_LIGHTS] < 1600) lightsToggleLock = false;
  }

  // ── Light GPIO output ──
  // lightsMode: 0=off, 1=front only, 2=work only, 3=all
  if (lightsOn && lightsState) {
    digitalWrite(HEADLIGHT_PIN, (lightsMode == 1 || lightsMode == 3) ? HIGH : LOW);
    digitalWrite(WORKLIGHT_PIN, (lightsMode == 2 || lightsMode == 3) ? HIGH : LOW);
  } else {
    digitalWrite(HEADLIGHT_PIN, LOW);
    digitalWrite(WORKLIGHT_PIN, LOW);
  }
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

  // Calculate sample rate from RPM
  engineSampleRate = map(currentRpm, minRpm, maxRpm, maxSampleInterval, minSampleInterval);

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
int16_t cmdTrackL = 0, cmdTrackR = 0;       // commanded track effort ±500
int16_t swashL = 0, swashR = 0;             // ramped swashplate state ±500
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
  // Dozer single-stick mix: one stick fwd/back + left/right, blended into both tracks.
  int32_t drive = (int32_t)pulseWidth[CH_DZ_DRIVE] - 1500; // ±500
  int32_t steer = (int32_t)pulseWidth[CH_DZ_STEER] - 1500;
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
  cmdTrackL = (int16_t)pulseWidth[outDriveL] - 1500;
  cmdTrackR = (int16_t)pulseWidth[outDriveR] - 1500;
  #endif
  if (!engineRunning) { cmdTrackL = 0; cmdTrackR = 0; }
#else
  // Wheeled: a drive motor + a steering servo, no tank mix. cmdTrackR = drive, cmdTrackL = steer.
  cmdTrackR = (int16_t)pulseWidth[outDriveR] - 1500; // drive
  cmdTrackL = (int16_t)pulseWidth[outDriveL] - 1500; // steer (servo passes through, engine or not)
  if (!engineRunning) cmdTrackR = 0;
#endif
}

// Swashplate ramp + engine-load speed droop → actual track speed. 20ms tick.
void hydrostaticModel() {
  static unsigned long last = 0;
  if (millis() - last < 20) return;
  last = millis();
#if MACHINE_TRACKED
  swashL = rampToward(swashL, cmdTrackL, swashAccelRate, swashDecelRate);
  swashR = rampToward(swashR, cmdTrackR, swashAccelRate, swashDecelRate);
  int32_t droop = constrain((int32_t)currentRpm * 100 / max((int16_t)1, driveDroopRefRpm), 0, 100);
  actualTrackL = swashL * droop / 100; // tracks slow as the engine bogs, recover as rpm returns
  actualTrackR = swashR * droop / 100;
  driveFlowDemand = (int16_t)(((int32_t)abs(swashL) + abs(swashR)) * driveFlowWeight / 1000);
#else
  // Wheeled: drive motor gets an accel/decel ramp; the steer servo passes straight through.
  swashR = rampToward(swashR, cmdTrackR, swashAccelRate, swashDecelRate);
  actualTrackR = swashR;      // drive
  actualTrackL = cmdTrackL;   // steer (direct)
  driveFlowDemand = (int16_t)((int32_t)abs(swashR) * driveFlowWeight / 1000);
#endif
}

// ── Proportional implement valves: lift/tilt/angle/ripper + cylinder position ──
int16_t valveCmd[4] = {0, 0, 0, 0};        // ramped spool command ±500 (0=lift,1=tilt,2=angle,3=ripper)
int32_t cylPos[4]   = {0, 0, 0, 0};        // integrated cylinder position [0..cylStroke]
bool    cylAtEndstop[4] = {false, false, false, false};
int16_t implFlowDemand = 0;                // total implement pump load (0..~100)
bool    systemRelief = false;              // demand exceeds engine-limited pump capacity
bool    functionRelief[4] = {false, false, false, false};

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
    cylPos[i] += (int32_t)valveCmd[i] * cylSpeed[i] / 500;              // integrate travel
    cylAtEndstop[i] = false;
    if (cylPos[i] < 0)            { cylPos[i] = 0;            if (valveCmd[i] < 0) cylAtEndstop[i] = true; }
    if (cylPos[i] > cylStroke[i]) { cylPos[i] = cylStroke[i]; if (valveCmd[i] > 0) cylAtEndstop[i] = true; }
    demand += (int32_t)abs(valveCmd[i]) * implFlowWeight[i] / 100;
  }
  implFlowDemand = (int16_t)(demand / 5); // scale to ~0..100 range
}

// ── Relief valve: a function at its end-stop, or total demand beyond the (engine-limited)
//    pump capacity, "relieves" — the engine strains, the flow sound spikes, RPM lugs. ──
void reliefLogic() {
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
  totalFlowDemand = driveFlowDemand + implFlowDemand + digLoad;
  int32_t cap = (int32_t)pumpFlowCapacity * max((int32_t)1, (int32_t)currentRpm)
              / max((int16_t)1, driveDroopRefRpm); // capacity falls as the engine bogs
  systemRelief = totalFlowDemand > cap;

  static uint32_t reliefLastMs = 0;
  bool any = systemRelief;
  for (int i = 0; i < 4; i++) {
    functionRelief[i] = cylAtEndstop[i] || (systemRelief && abs(valveCmd[i]) > hydraulicDeadZone);
    if (functionRelief[i]) any = true;
  }
  if (any) reliefLastMs = millis();
  reliefActive = (millis() - reliefLastMs < reliefHoldMs);

  if (reliefActive) {
    if (hydraulicLoad < 80) hydraulicLoad = 80; // engine strains when the relief is open
  }

  // The hydraulic-FLOW voice is the RELIEF sound now — it plays only when the relief cracks over
  // pressure, nothing else (no flow hiss on normal implement/track movement).
  hydraulicFlowVolume = reliefActive ? 100 : 0;

  // Pump whine still follows drive load (augments the implement-driven pump from the control fn).
  uint16_t drivePump = (uint16_t)constrain(driveFlowDemand, (int16_t)0, (int16_t)100);
  if (hydraulicPumpVolume < drivePump) hydraulicPumpVolume = drivePump;
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

  static uint32_t lastMs = 0;
  static bool prevStrain = false;
  bool strain = reliefActive || engineLugging;

  // One immediate hard bump the instant the relief cracks / the engine bogs.
  if (strain && !prevStrain) {
    prevStrain = strain;
    gpController->playDualRumble(0, 130, 60, 255);
    lastMs = millis();
    return;
  }
  prevStrain = strain;

  if (millis() - lastMs < 120) return; // ~8 Hz refresh
  lastMs = millis();

  uint8_t weak = 0, strong = 0;
  if (engineState == STARTING) { weak = 90; strong = 140; } // cranking shudder
  else if (engineRunning) {
    int load = constrain(totalFlowDemand, (int16_t)0, (int16_t)100); // 0..100 pump demand
    weak = 26 + (uint8_t)(load * 30 / 100);                          // idle purr + buzz under load
    strong = (uint8_t)(load * 200 / 100);                           // load -> up to ~200
    if (strain) { weak = 60; strong = 255; }                        // sustained strain = full bump
  }
  gpController->playDualRumble(0, 160, weak, strong); // duration > refresh -> continuous
#endif
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
  im0 = constrain(1500 + valveCmd[0], servoMin[2], servoMax[2]);
  im1 = constrain(1500 + valveCmd[1], servoMin[2], servoMax[2]);
  im2 = constrain(1500 + valveCmd[2], servoMin[2], servoMax[2]);
  im3 = constrain(1500 + valveCmd[3], servoMin[3], servoMax[3]);

  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, driveA); // GPIO13
  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, driveB); // GPIO12
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_0, MCPWM_OPR_A, im0);    // GPIO33
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_0, MCPWM_OPR_B, im1);    // GPIO32
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_1, MCPWM_OPR_A, im2);    // GPIO14
  mcpwm_set_duty_in_us(MCPWM_UNIT_1, MCPWM_TIMER_1, MCPWM_OPR_B, im3);    // GPIO27
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
  // ── Track rattle (from track stick movement) ──
  static boolean trackLisRotating = false, trackRisRotating = false;

  uint16_t trackLVol = 0;
  if (pulseWidth[CH_DZ_TRACK_L] > pulseMaxNeutral[CH_DZ_TRACK_L]) {
    trackLVol = map(pulseWidth[CH_DZ_TRACK_L], pulseMaxNeutral[CH_DZ_TRACK_L], pulseMax[CH_DZ_TRACK_L], 0, 100);
    trackLisRotating = true;
  } else if (pulseWidth[CH_DZ_TRACK_L] < pulseMinNeutral[CH_DZ_TRACK_L]) {
    trackLVol = map(pulseWidth[CH_DZ_TRACK_L], pulseMinNeutral[CH_DZ_TRACK_L], pulseMin[CH_DZ_TRACK_L], 0, 100);
    trackLisRotating = true;
  } else {
    trackLisRotating = false;
  }

  uint16_t trackRVol = 0;
  if (pulseWidth[CH_DZ_TRACK_R] > pulseMaxNeutral[CH_DZ_TRACK_R]) {
    trackRVol = map(pulseWidth[CH_DZ_TRACK_R], pulseMaxNeutral[CH_DZ_TRACK_R], pulseMax[CH_DZ_TRACK_R], 0, 100);
    trackRisRotating = true;
  } else if (pulseWidth[CH_DZ_TRACK_R] < pulseMinNeutral[CH_DZ_TRACK_R]) {
    trackRVol = map(pulseWidth[CH_DZ_TRACK_R], pulseMinNeutral[CH_DZ_TRACK_R], pulseMin[CH_DZ_TRACK_R], 0, 100);
    trackRisRotating = true;
  } else {
    trackRVol = 0;
    trackRisRotating = false;
  }

  tracksAreRotating = trackLisRotating || trackRisRotating;
  if (engineRunning) {
    trackRattleVolume = constrain(trackLVol + trackRVol, 0, 100);
  } else {
    trackRattleVolume = 0;
  }

  // ── Dozer reverse detection (for travel alarm) ──
  // Both tracks moving backward = reversing
  boolean trackLreverse = (pulseWidth[CH_DZ_TRACK_L] < pulseMinNeutral[CH_DZ_TRACK_L]);
  boolean trackRreverse = (pulseWidth[CH_DZ_TRACK_R] < pulseMinNeutral[CH_DZ_TRACK_R]);
  escInReverse = trackLreverse && trackRreverse;
  escIsDriving = tracksAreRotating;

  // ── Blade / tilt / ripper → hydraulic pump volume ──
  uint16_t bladeVol = 0;
  if (pulseWidth[CH_DZ_BLADE] < pulseMinNeutral[CH_DZ_BLADE])
    bladeVol = map(pulseWidth[CH_DZ_BLADE], pulseMinNeutral[CH_DZ_BLADE], pulseMin[CH_DZ_BLADE], 0, 60);
  else if (pulseWidth[CH_DZ_BLADE] > pulseMaxNeutral[CH_DZ_BLADE])
    bladeVol = map(pulseWidth[CH_DZ_BLADE], pulseMaxNeutral[CH_DZ_BLADE], pulseMax[CH_DZ_BLADE], 0, 40);

  uint16_t tiltVol = 0;
  if (CH_DZ_TILT > 0) {
    if (pulseWidth[CH_DZ_TILT] < pulseMinNeutral[CH_DZ_TILT])
      tiltVol = map(pulseWidth[CH_DZ_TILT], pulseMinNeutral[CH_DZ_TILT], pulseMin[CH_DZ_TILT], 0, 30);
    else if (pulseWidth[CH_DZ_TILT] > pulseMaxNeutral[CH_DZ_TILT])
      tiltVol = map(pulseWidth[CH_DZ_TILT], pulseMaxNeutral[CH_DZ_TILT], pulseMax[CH_DZ_TILT], 0, 30);
  }

  uint16_t ripperVol = 0;
  if (pulseWidth[CH_DZ_RIPPER] < pulseMinNeutral[CH_DZ_RIPPER])
    ripperVol = map(pulseWidth[CH_DZ_RIPPER], pulseMinNeutral[CH_DZ_RIPPER], pulseMin[CH_DZ_RIPPER], 0, 40);
  else if (pulseWidth[CH_DZ_RIPPER] > pulseMaxNeutral[CH_DZ_RIPPER])
    ripperVol = map(pulseWidth[CH_DZ_RIPPER], pulseMaxNeutral[CH_DZ_RIPPER], pulseMax[CH_DZ_RIPPER], 0, 30);

  hydraulicPumpVolume = constrain(bladeVol + tiltVol + ripperVol, 0, 100);
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
// SETUP
// ════════════════════════════════════════════════════════════════

void setup() {

  Serial.begin(115200);
  Serial.println("HydraulicController starting...");

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

  // Lights GPIO
  pinMode(HEADLIGHT_PIN, OUTPUT);
  pinMode(WORKLIGHT_PIN, OUTPUT);
  digitalWrite(HEADLIGHT_PIN, LOW);
  digitalWrite(WORKLIGHT_PIN, LOW);

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

  // RC input setup
#if defined GAMEPAD_MODE
  setupGamepad();
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

  // Wait for RC signal
  Serial.println("Waiting for RC signal...");
  uint32_t rcTimeout = millis();
  while (!autoZeroDone && millis() - rcTimeout < 5000) {
#if defined GAMEPAD_MODE
    readGamepadCommands();
#else
    readSbusCommands();
    readIbusCommands();
    readSumdCommands();
    readPpmCommands();
    readPwmSignals();
#endif
    processRawChannels();
    delay(20);
  }
  Serial.println(autoZeroDone ? "RC calibrated!" : "RC timeout — using defaults");

  // Audio timers
  variableTimer = timerBegin(0, 20, true);
  timerAttachInterrupt(variableTimer, &variablePlaybackTimer, true);
  timerAlarmWrite(variableTimer, variableTimerTicks, true);
  timerAlarmEnable(variableTimer);

  fixedTimer = timerBegin(1, 20, true);
  timerAttachInterrupt(fixedTimer, &fixedPlaybackTimer, true);
  timerAlarmWrite(fixedTimer, variableTimerTicks, true);
  timerAlarmEnable(fixedTimer);

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
  implementControl();  // proportional implement valves + cylinder position

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

  reliefLogic();       // relief-valve strain — augments the sound set just above (must run after)

  // Servo output
  mcpwmOutput();

#if defined GAMEPAD_MODE
  updateGamepadRumble(); // engine-feel haptics (only if GP_RUMBLE)
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
