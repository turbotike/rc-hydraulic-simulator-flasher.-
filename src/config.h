// HydraulicController — config.h
// All user settings in one place. Pick your machine type, sounds, RC protocol, and tuning.
#pragma once
#include <Arduino.h>

// ============================================================================
// MACHINE TYPE — uncomment ONE
// ============================================================================
// #define EXCAVATOR_MODE          // Full excavator: boom/stick/bucket/swing + tracks
// #define LOADER_MODE          // Wheel loader: boom/bucket + drive
// #define CRANE_MODE           // Rough terrain crane: boom/extend/swing/rope + outriggers
#define DOZER_MODE           // Tracked dozer: blade/tilt/ripper + tracks
// #define SKIDSTEER_MODE       // Skid steer loader: boom/bucket + dual drive
// #define GRADER_MODE          // Motor grader: blade/circle/tilt + articulated steering

// ============================================================================
// CHANNEL MAPPING — defaults (overridden at runtime from NVS if saved)
// Change via web UI → "Push to ESP32" — no rebuild needed!
// ============================================================================
uint8_t CH_THROTTLE      = 5;   // Throttle stick (fwd/rev or fwd-only depending on mode)
uint8_t CH_HORN          = 8;   // Horn trigger (>1800µs = ON)
uint8_t CH_ENGINE_TOGGLE = 9;   // Engine on/off toggle (rising edge >1700µs)
uint8_t CH_HILO_TOGGLE   = 7;   // Hi/Lo range toggle (rising edge >1700µs)

// --- Excavator channels ---
uint8_t CH_EX_TRACK_R  = 1;   // Right track
uint8_t CH_EX_TRACK_L  = 2;   // Left track
uint8_t CH_EX_BOOM     = 3;   // Boom up/down
uint8_t CH_EX_STICK    = 4;   // Arm (stick/dipper) in/out
uint8_t CH_EX_BUCKET   = 5;   // Bucket curl/dump
uint8_t CH_EX_SWING    = 6;   // Cab swing

// --- Loader channels ---
uint8_t CH_LD_BUCKET   = 1;   // Bucket tilt
uint8_t CH_LD_BOOM     = 2;   // Boom lift

// --- Crane channels ---
uint8_t CH_CR_BOOM     = 1;   // Boom lift
uint8_t CH_CR_EXTEND   = 2;   // Boom extend/retract
uint8_t CH_CR_SWING    = 8;   // Cab swing

// --- Dozer channels ---
uint8_t CH_DZ_TRACK_R  = 1;   // Right track
uint8_t CH_DZ_TRACK_L  = 2;   // Left track
uint8_t CH_DZ_BLADE    = 3;   // Blade lift (up/down)
uint8_t CH_DZ_RIPPER   = 4;   // Ripper up/down
uint8_t CH_DZ_TILT     = 0;   // Blade tilt (left/right — S-blade or SU-blade)
uint8_t CH_DZ_ANGLE    = 0;   // Blade angle (angling the moldboard)
uint8_t CH_DZ_RIPPER_TILT = 0; // Ripper tilt

// --- Run the dozer on a game controller (PS4/PS5/Xbox) instead of an RC receiver ---
// Needs the esp32-bluepad32 board core — the flasher selects it for a gamepad build. When on,
// comment out the RC protocol below (one radio: Bluetooth OR the RC bus, not both).
// #define GAMEPAD_MODE

// --- Dozer drive input: choose ONE at flash time ---
#define DRIVE_SINGLE_STICK_MIX     // one stick: fwd/back + left/right, mixed to both tracks
// #define DRIVE_DUAL_STICK        // one stick per track (skid-steer style)
uint8_t CH_DZ_DRIVE    = 2;   // (mix mode) forward / reverse axis
uint8_t CH_DZ_STEER    = 1;   // (mix mode) left / right axis

// --- Hydrostatic drive tuning (dual-path pump→motor feel) ---
int16_t swashAccelRate  = 15;   // swashplate stroke toward command, per 20ms tick (±500 span)
int16_t swashDecelRate  = 25;   // destroke back toward neutral (faster than accel)
int16_t trackThrowScale = 100;  // % of servo throw the tracks use
int16_t counterRotScale = 80;   // % throw allowed while counter-rotating (spin on the spot)
int16_t driveDroopRefRpm = 350; // engine rpm at/above which full track speed is available

// --- Implement (proportional valve) model: [lift, tilt, angle, ripper] ---
int16_t cylStroke[4]      = {1000, 800, 800, 900}; // cylinder travel units (for end-stop detection)
int16_t cylSpeed[4]       = {30, 40, 40, 30};      // position units per 20ms per full spool
uint8_t implFlowWeight[4] = {40, 25, 25, 35};      // % pump load each function draws at full flow

// --- Relief valve / pump capacity ---
int16_t driveFlowWeight   = 60;  // how hard the tracks load the pump/engine
int16_t pumpFlowCapacity  = 140; // total flow the engine can supply at ref rpm
int16_t reliefSquealVol   = 150; // relief-valve squeal volume
uint16_t reliefHoldMs     = 140; // debounce so the relief cue is steady, not stuttering

// --- Engine governor (dozer): holds rpm, sags under load, lugs when bogged ---
int16_t sagAttack       = 6;   // rpm/tick the governor bogs under load (fast)
int16_t sagRecovery     = 3;   // rpm/tick it claws rpm back (slower)
int16_t maxSagRpm       = 220; // max rpm droop at full pump demand
int16_t lugRpmThreshold = 150; // below this rpm under load → lugging (heavy strain)
int16_t lugKnockBoost   = 40;  // extra engine strain when lugging

// --- Grader channels ---
uint8_t CH_GR_BLADE    = 1;   // Blade lift (raise/lower moldboard)
uint8_t CH_GR_CIRCLE   = 2;   // Circle rotation (moldboard angle)
uint8_t CH_GR_TILT     = 7;   // Blade tilt (left/right lean)
uint8_t CH_GR_ARTICULATION = 8; // Frame articulation (steering)

// --- Skid Steer channels ---
uint8_t CH_SS_BUCKET   = 1;   // Bucket curl/dump
uint8_t CH_SS_BOOM     = 2;   // Boom up/down

// --- Lights channel ---
uint8_t CH_LIGHTS      = 6;   // Lights toggle (long press >1700µs cycles states)

// ============================================================================
// CHANNEL REVERSE — per-channel inversion (overridden from NVS if saved)
// true = reversed (1000↔2000 swapped), false = normal
// ============================================================================
boolean channelReversed[17] = {
    false, // CH0 (unused)
    false, // CH1
    false, // CH2
    false, // CH3
    false, // CH4
    false, // CH5
    false, // CH6
    false, // CH7
    false, // CH8
    false, // CH9
    false, // CH10
    false, // CH11
    false, // CH12
    false, // CH13
    false, // CH14
    false, // CH15
    false // CH16
};

// ============================================================================
// CHANNEL ENABLE — per-channel on/off (overridden from NVS if saved)
// false = disabled (pulse forced to 1500 centre), true = active
// ============================================================================
boolean channelEnabled[17] = {
    true, // CH0 (unused)
    true, // CH1
    true, // CH2
    true, // CH3
    true, // CH4
    true, // CH5
    true, // CH6
    true, // CH7
    true, // CH8
    true, // CH9
    true, // CH10
    true, // CH11
    true, // CH12
    true, // CH13
    true, // CH14
    true, // CH15
    true // CH16
};

// ============================================================================
// RC PROTOCOL — uncomment ONE
// ============================================================================
// #define SBUS_COMMUNICATION      // Futaba / FrSky SBUS (inverted UART on GPIO 36)
#define IBUS_COMMUNICATION   // FlySky IBUS
// #define SUMD_COMMUNICATION   // Graupner SUMD
// #define PPM_COMMUNICATION    // PPM sum signal
// #define PWM_COMMUNICATION    // Individual PWM per channel (max 6ch)

#if defined SBUS_COMMUNICATION
#define EMBEDDED_SBUS           // Use embedded SBUS decoder (recommended)
boolean sbusInverted = true;    // true = standard (non-inverted) SBUS signal
uint32_t sbusBaud = 100000;     // Standard 100000. Some receivers need 163863
#endif

// ============================================================================
// PIN ASSIGNMENTS (match the DIYGuy999 v1.2 board by default)
// ============================================================================
#define RC_INPUT_PIN        36  // Main RC input (SBUS/IBUS/SUMD/PPM/PWM CH1)
#define COMMAND_RX          36  // Alias for RC input
#define COMMAND_TX          UART_PIN_NO_CHANGE // TX not used
#define ESC_OUT_PIN         33  // ESC signal output (MCPWM)
#define DAC1_PIN            25  // DAC1 — engine / hydraulic pump sound
#define DAC2_PIN            26  // DAC2 — knock / horn / aux sounds
#define SERVO_CH1_PIN       13  // Steering / Left track
#define SERVO_CH2_PIN       12  // Shifting / Right track
#define SERVO_CH3_PIN       14  // Boom / Winch
#define SERVO_CH4_PIN       27  // Bucket / Blade

// Dozer 6-output map (native pins, no PCA9685). Tracks on MCPWM Unit0; the 6-way PAT
// blade + ripper on MCPWM Unit1 (the ESC pin is free in DOZER_MODE).
#define TRACK_R_PIN         13  // MCPWM U0 T0 A
#define TRACK_L_PIN         12  // MCPWM U0 T0 B
#define BLADE_LIFT_PIN      33  // MCPWM U1 T0 A  (freed ESC pin)
#define BLADE_TILT_PIN      32  // MCPWM U1 T0 B
#define BLADE_ANGLE_PIN     14  // MCPWM U1 T1 A
#define RIPPER_PIN          27  // MCPWM U1 T1 B

// PWM-only RC input pins (only used if PWM_COMMUNICATION defined)
#define PWM_CH1_PIN         36
#define PWM_CH2_PIN         22
#define PWM_CH3_PIN         17
#define PWM_CH4_PIN         16
#define PWM_CH5_PIN         39
#define PWM_CH6_PIN         34

// Battery voltage sense
#define BATTERY_DETECT_PIN  39  // Voltage divider on VN

// Lights output
#define HEADLIGHT_PIN        3  // Front headlights (GPIO 3)
#define WORKLIGHT_PIN       22  // Work lights / cab lights (GPIO 22, free when not using PWM RC)

// Status LED (active low on some boards)
#define STATUS_LED_PIN       0

// ============================================================================
// ENGINE SOUND FILES — change #includes to swap sounds
// ============================================================================

// --- Start sound ---
volatile int startVolumePercentage = 140;
#include "sounds/D6TrackRattle.h"
// Alias: auto-generated variable mapping
const signed char* startSamples = trackRattle2Samples;
const unsigned int startSampleCount = trackRattle2SampleCount;
const unsigned int startSampleRate = trackRattle2SampleRate;

// --- Idle sound ---
volatile int idleVolumePercentage = 80;
volatile int engineIdleVolumePercentage = 80;
volatile int fullThrottleVolumePercentage = 140;
#include "sounds/idle.h"
// Alias: auto-generated variable mapping
const signed char* samples = idle;
const unsigned int sampleCount = idle_sampleCount;
const unsigned int sampleRate = idle_sampleRate;

// --- Rev sound ---
#define REV_SOUND
volatile int revVolumePercentage = 110;
volatile int engineRevVolumePercentage = 50;
volatile const uint16_t revSwitchPoint = 100;
volatile const uint16_t idleEndPoint = 400;
volatile const uint16_t idleVolumeProportionPercentage = 90;
#ifdef REV_SOUND
#include "sounds/idle.h"
// Alias: auto-generated variable mapping
const signed char* revSamples = idle;
const unsigned int revSampleCount = idle_sampleCount;
const unsigned int revSampleRate = idle_sampleRate;
#endif

// --- Diesel knock ---
volatile int dieselKnockVolumePercentage = 600;
volatile int dieselKnockIdleVolumePercentage = 10;
volatile int dieselKnockInterval = 6;
volatile int dieselKnockStartPoint = 110;
volatile int dieselKnockAdaptiveVolumePercentage = 50;
#include "sounds/Caterpillar323Knock.h"

// --- Turbo ---
volatile int turboVolumePercentage = 90;
volatile int turboIdleVolumePercentage = 30;
#include "sounds/TurboWhistle.h"

// --- Supercharger (set to 0 to disable) ---
volatile int chargerVolumePercentage = 0;
volatile int chargerIdleVolumePercentage = 10;
volatile int chargerStartPoint = 10;
#include "sounds/supercharger.h"

// --- Wastegate ---
volatile int wastegateVolumePercentage = 50;
volatile int wastegateIdleVolumePercentage = 1;
#include "sounds/UnimogU1000TurboWastegate.h"

// --- Fan (set to 0 to disable) ---
volatile int fanVolumePercentage = 0;
volatile int fanIdleVolumePercentage = 0;
volatile int fanStartPoint = 0;
#include "sounds/GenericFan.h"

// --- Horn ---
volatile int hornVolumePercentage = 140;
#include "sounds/CarHorn.h"

// --- Siren (set volume to 0 to disable) ---
volatile int sirenVolumePercentage = 100;
#include "sounds/sirenDummy.h"

// --- Air brake ---
volatile int brakeVolumePercentage = 150;
#include "sounds/AirBrakeDummy.h"



// --- Shifting sound ---
volatile int shiftingVolumePercentage = 100;
#include "sounds/AirShiftingDummy.h"

// --- Misc sound1 (door, etc.) ---
volatile int sound1VolumePercentage = 100;
#include "sounds/door.h"

// --- Travel Alarm (Reversing beep) ---
volatile int reversingVolumePercentage = 205;
const boolean travelAlarmBothDirections = true; // true = beep in fwd + rev
#include "sounds/TruckReversingBeep.h"

// --- Indicator tick ---
volatile int indicatorVolumePercentage = 100;
const uint16_t indicatorOn = 300;
const boolean INDICATOR_DIR = true;
#include "sounds/Indicator.h"

// --- Coupling sound (optional) ---
#define COUPLING_SOUND
volatile int couplingVolumePercentage = 100;
#ifdef COUPLING_SOUND
#include "sounds/coupling.h"
#include "sounds/uncoupling.h"
#endif

// ============================================================================
// HYDRAULIC SOUND FILES
// ============================================================================

// --- Hydraulic pump ---
volatile int hydraulicPumpVolumePercentage = 120;
#include "sounds/Caterpillar323Hydraulic2.h"

// --- Hydraulic fluid flow ---
volatile int hydraulicFlowVolumePercentage = 0;
#include "sounds/Caterpillar323HydraulicFlow.h"
#include "sounds/reliefSqueal.h"

// --- Track rattle ---
volatile int trackRattleVolumePercentage = 175;
#include "sounds/Caterpillar323TrackRattle.h"

// --- Track rattle 2 (periodic clank, for tracked machines) ---
#define TRACK_RATTLE_2
#if defined TRACK_RATTLE_2
volatile int trackRattle2VolumePercentage = 150;
// These control the periodic chain-link clank interval
uint16_t pwmStrokeChainDriveTopSpeed = 255;
uint16_t pwmStrokeChainDriveStartRotation = 70;
uint16_t trackRattleIntervalMin = 90;   // ms at max speed
uint16_t trackRattleIntervalMax = 305;  // ms at min speed
#endif

// --- Bucket rattle ---
volatile int bucketRattleVolumePercentage = 160;
#include "sounds/Caterpillar323BucketRattle.h"

// Auto-injected for parkingBrakeSound (was missing) — using idle sound
const signed char* parkingBrakeSamples = samples;
const unsigned int parkingBrakeSampleCount = sampleCount;
const unsigned int parkingBrakeSampleRate = sampleRate;

// ============================================================================
// RC SIGNAL TUNING
// ============================================================================
const uint16_t pulseNeutral = 30;  // 1500 +/- this = neutral dead zone
const uint16_t pulseSpan = 400;    // 1500 +/- this = full range (theory = 500)

// ============================================================================
// ENGINE TUNING
// ============================================================================
uint32_t MAX_RPM_PERCENTAGE = 200;  // Max RPM as % of idle (200 = big diesel, 400 = gas)
int8_t acc = 2;                     // Acceleration step (1=slow loco, 9=fast trophy truck)
int8_t dec = 1;                     // Deceleration step
boolean autoEngineStart = false;     // true = engine auto-starts on stick input, false = use CH_ENGINE_TOGGLE
uint16_t clutchEngagingPoint = 500; // Engine RPM couples to ESC above this

// ============================================================================
// ESC TUNING
// ============================================================================
uint8_t escRampTimeLow = 20;        // ESC ramp rate in low range (ms per step)
uint8_t escRampTimeHigh = 50;       // ESC ramp rate in high range (ms per step)
uint8_t escBrakeSteps = 30;
uint8_t escAccelerationSteps = 6;

// ============================================================================
// TRANSMISSION
// ============================================================================
const boolean automatic = false;
#define NumberOfAutomaticGears 3
const boolean doubleClutch = false;
const boolean shiftingAutoThrottle = true;

// Hi/Lo 2-speed range (rabbit mode) — CH7 toggles, or set hiLoDefaultHigh
#define HILO_ENABLED                    // Comment out to disable hi/lo
const uint8_t hiLoRatioPercent = 60;    // Low range = this % of full speed
const boolean hiLoDefaultHigh = false;  // Start in high range?

// ============================================================================
// SERVO ENDPOINTS & RAMP
// ============================================================================
const uint16_t servoMin[5] = {1000, 1000, 1000, 1000, 1000};
const uint16_t servoMax[5] = {2000, 2000, 2000, 2000, 2000};
const uint16_t servoCenter[5] = {1500, 1500, 1500, 1500, 1500};

// Excavator-specific: hydraulic valve dead zone & ramp time (ms to reach full throw)
const uint16_t hydraulicRampTime = 200;  // Smooth valve response
const uint16_t hydraulicDeadZone = 50;   // µs dead zone around center

// ============================================================================
// MASTER VOLUME
// ============================================================================
volatile int masterVolume = 260;  // 0-200%

// ============================================================================
// DEBUG
// ============================================================================
#define DEBUG_RC       // Print RC channel values
#define DEBUG_ESC      // Print ESC state machine
#define DEBUG_SOUND    // Print sound engine stats
#define DEBUG_HYDRAULIC // Print hydraulic volumes

// ============================================================================
// RUNTIME SOUND INDIRECTION
// ============================================================================
// Mutable pointers initialized to compiled-in defaults.
// The sound pack loader (soundpack.h) can redirect these to memory-mapped
// data from the flash sound partition — no recompile needed.
// The #define macros below transparently redirect all ISR array accesses
// through these pointers, so ZERO ISR code changes are needed.

// --- Pointer variables (defaults → compiled-in arrays) ---
const int8_t*  volatile _rt_startSamples       = startSamples;
volatile uint32_t       _rt_startSampleCount    = startSampleCount;
volatile uint32_t       _rt_startSampleRate     = startSampleRate;

const int8_t*  volatile _rt_idleSamples         = samples;
volatile uint32_t       _rt_idleSampleCount     = sampleCount;
volatile uint32_t       _rt_idleSampleRate      = sampleRate;

const int8_t*  volatile _rt_revSamples          = revSamples;
volatile uint32_t       _rt_revSampleCount      = revSampleCount;

const int8_t*  volatile _rt_knockSamples        = knockSamples;
volatile uint32_t       _rt_knockSampleCount    = knockSampleCount;

const int8_t*  volatile _rt_turboSamples        = turboSamples;
volatile uint32_t       _rt_turboSampleCount    = turboSampleCount;

const int8_t*  volatile _rt_fanSamples          = fanSamples;
volatile uint32_t       _rt_fanSampleCount      = fanSampleCount;

const int8_t*  volatile _rt_chargerSamples      = chargerSamples;
volatile uint32_t       _rt_chargerSampleCount  = chargerSampleCount;

const int8_t*  volatile _rt_wastegateSamples    = wastegateSamples;
volatile uint32_t       _rt_wastegateSampleCount = wastegateSampleCount;

const int8_t*  volatile _rt_hornSamples         = hornSamples;
volatile uint32_t       _rt_hornSampleCount     = hornSampleCount;

const int8_t*  volatile _rt_sirenSamples        = sirenSamples;
volatile uint32_t       _rt_sirenSampleCount    = sirenSampleCount;

const int8_t*  volatile _rt_brakeSamples        = brakeSamples;
volatile uint32_t       _rt_brakeSampleCount    = brakeSampleCount;

const int8_t*  volatile _rt_shiftingSamples     = shiftingSamples;
volatile uint32_t       _rt_shiftingSampleCount = shiftingSampleCount;

const int8_t*  volatile _rt_sound1Samples       = sound1Samples;
volatile uint32_t       _rt_sound1SampleCount   = sound1SampleCount;

const int8_t*  volatile _rt_reversingSamples    = reversingSamples;
volatile uint32_t       _rt_reversingSampleCount = reversingSampleCount;

const int8_t*  volatile _rt_indicatorSamples    = indicatorSamples;
volatile uint32_t       _rt_indicatorSampleCount = indicatorSampleCount;

#ifdef COUPLING_SOUND
const int8_t*  volatile _rt_couplingSamples     = couplingSamples;
volatile uint32_t       _rt_couplingSampleCount = couplingSampleCount;
const int8_t*  volatile _rt_uncouplingSamples   = uncouplingSamples;
volatile uint32_t       _rt_uncouplingSampleCount = uncouplingSampleCount;
#endif

const int8_t*  volatile _rt_hydraulicPumpSamples    = hydraulicPumpSamples;
volatile uint32_t       _rt_hydraulicPumpSampleCount = hydraulicPumpSampleCount;

const int8_t*  volatile _rt_hydraulicFlowSamples    = hydraulicFlowSamples;
volatile uint32_t       _rt_hydraulicFlowSampleCount = hydraulicFlowSampleCount;

const int8_t*  volatile _rt_reliefSquealSamples     = reliefSquealSamples;
volatile uint32_t       _rt_reliefSquealSampleCount  = reliefSquealSampleCount;

const int8_t*  volatile _rt_trackRattleSamples      = trackRattleSamples;
volatile uint32_t       _rt_trackRattleSampleCount  = trackRattleSampleCount;

const int8_t*  volatile _rt_bucketRattleSamples     = bucketRattleSamples;
volatile uint32_t       _rt_bucketRattleSampleCount = bucketRattleSampleCount;

// --- Shadow macros (redirect all subsequent code to use pointers) ---
// After this point, "startSamples" resolves to "_rt_startSamples" etc.
// The ISR code is unchanged — it just reads through these pointers.
#define startSamples        _rt_startSamples
#define startSampleCount    _rt_startSampleCount
#define startSampleRate     _rt_startSampleRate
#define samples             _rt_idleSamples
#define sampleCount         _rt_idleSampleCount
#define sampleRate          _rt_idleSampleRate
#define revSamples          _rt_revSamples
#define revSampleCount      _rt_revSampleCount
#define knockSamples        _rt_knockSamples
#define knockSampleCount    _rt_knockSampleCount
#define turboSamples        _rt_turboSamples
#define turboSampleCount    _rt_turboSampleCount
#define fanSamples          _rt_fanSamples
#define fanSampleCount      _rt_fanSampleCount
#define chargerSamples      _rt_chargerSamples
#define chargerSampleCount  _rt_chargerSampleCount
#define wastegateSamples    _rt_wastegateSamples
#define wastegateSampleCount _rt_wastegateSampleCount
#define hornSamples         _rt_hornSamples
#define hornSampleCount     _rt_hornSampleCount
#define sirenSamples        _rt_sirenSamples
#define sirenSampleCount    _rt_sirenSampleCount
#define brakeSamples        _rt_brakeSamples
#define brakeSampleCount    _rt_brakeSampleCount
#define shiftingSamples     _rt_shiftingSamples
#define shiftingSampleCount _rt_shiftingSampleCount
#define sound1Samples       _rt_sound1Samples
#define sound1SampleCount   _rt_sound1SampleCount
#define reversingSamples    _rt_reversingSamples
#define reversingSampleCount _rt_reversingSampleCount
#define indicatorSamples    _rt_indicatorSamples
#define indicatorSampleCount _rt_indicatorSampleCount
#ifdef COUPLING_SOUND
#define couplingSamples     _rt_couplingSamples
#define couplingSampleCount _rt_couplingSampleCount
#define uncouplingSamples   _rt_uncouplingSamples
#define uncouplingSampleCount _rt_uncouplingSampleCount
#endif
#define hydraulicPumpSamples    _rt_hydraulicPumpSamples
#define hydraulicPumpSampleCount _rt_hydraulicPumpSampleCount
#define hydraulicFlowSamples    _rt_hydraulicFlowSamples
#define hydraulicFlowSampleCount _rt_hydraulicFlowSampleCount
#define reliefSquealSamples     _rt_reliefSquealSamples
#define reliefSquealSampleCount _rt_reliefSquealSampleCount
#define trackRattleSamples      _rt_trackRattleSamples
#define trackRattleSampleCount  _rt_trackRattleSampleCount
#define bucketRattleSamples     _rt_bucketRattleSamples
#define bucketRattleSampleCount _rt_bucketRattleSampleCount
