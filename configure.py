#!/usr/bin/env python3
"""
HydraulicController Web Configurator
Serves a web UI to edit src/config.h, build, and flash.
Usage: python configure.py [--port 8080] [--no-browser]
"""

import http.server
import json
import os
import re
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import webbrowser
import urllib.parse
import glob
import time

PORT = 8080
# When frozen (PyInstaller --onefile), the bundle's src/web/libraries sit next to the
# .exe, not in the temp _MEIPASS extraction dir — so resolve paths from the executable.
if getattr(sys, "frozen", False):
    PROJECT_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(PROJECT_DIR, "src", "config.h")
SOUNDS_DIR = os.path.join(PROJECT_DIR, "src", "sounds")
VEHICLES_DIR = os.path.join(PROJECT_DIR, "vehicles")
os.makedirs(VEHICLES_DIR, exist_ok=True)

# ─── Config Parser ───────────────────────────────────────────────────────────

def list_sound_files():
    """Return sorted list of .h files in sounds/ (no path, no extension)."""
    files = []
    for f in sorted(glob.glob(os.path.join(SOUNDS_DIR, "*.h"))):
        files.append(os.path.basename(f))
    return files


def parse_sound_header(filepath):
    """Parse a PCM .h file and return {sampleRate, sampleCount, samples:[int]}."""
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    rate_m = re.search(r'(?:unsigned\s+int|int)\s+\w*[Ss]ample[Rr]ate\s*=\s*(\d+)', text)
    count_m = re.search(r'(?:unsigned\s+int|int)\s+\w*[Ss]ample[Cc]ount\s*=\s*(\d+)', text)
    arr_m = re.search(r'(?:signed\s+char|char)\s+\w+\[\]\s*=\s*\{([^}]+)\}', text, re.DOTALL)
    rate = int(rate_m.group(1)) if rate_m else 22050
    count = int(count_m.group(1)) if count_m else 0
    samples = []
    if arr_m:
        for tok in arr_m.group(1).replace('\n', ',').split(','):
            tok = tok.strip().split('//')[0].strip()
            if tok and tok.lstrip('-').isdigit():
                samples.append(max(-128, min(127, int(tok))))
    return {"sampleRate": rate, "sampleCount": count or len(samples), "samples": samples}


SOUND_CATEGORY_KEYWORDS = {
    'idle': ['idle', 'Idle'],
    'reversing': ['revers', 'Revers', 'Beep'],
    'rev': ['rev', 'Rev'],
    'start': ['start', 'Start'],
    'knock': ['knock', 'Knock'],
    'turbo': ['turbo', 'Turbo', 'Whistle'],
    'wastegate': ['wastegate', 'Wastegate'],
    'fan': ['fan', 'Fan'],
    'horn': ['horn', 'Horn'],
    'siren': ['siren', 'Siren'],
    'brake': ['brake', 'Brake'],
    'parking': ['parking', 'Parking'],
    'shifting': ['shift', 'Shift'],
    'indicator': ['indicator', 'Indicator'],
    'coupling': ['coupl', 'Coupl'],
    'hydraulic': ['hydraulic', 'Hydraulic'],
    'track': ['track', 'Track', 'rattle', 'Rattle'],
    'supercharger': ['supercharger', 'charger'],
    'door': ['door'],
}


def scan_all_sounds():
    """Scan sounds dir and return [{file, label, category}]."""
    result = []
    for fn in sorted(os.listdir(SOUNDS_DIR)):
        if not fn.endswith('.h'):
            continue
        label = fn.replace('.h', '')
        cat = 'other'
        for category, keywords in SOUND_CATEGORY_KEYWORDS.items():
            if any(kw in fn for kw in keywords):
                cat = category
                break
        result.append({"file": fn, "label": label, "category": cat})
    return result


# Expected variable names per sound slot (array, count, rate).
# These are what the ISR / runtime indirection expects.
SLOT_EXPECTED_VARS = {
    "startSound":          ("startSamples",         "startSampleCount",         "startSampleRate"),
    "idleSound":           ("samples",              "sampleCount",              "sampleRate"),
    "revSound":            ("revSamples",           "revSampleCount",           "revSampleRate"),
    "knockSound":          ("knockSamples",         "knockSampleCount",         "knockSampleRate"),
    "turboSound":          ("turboSamples",         "turboSampleCount",         "turboSampleRate"),
    "chargerSound":        ("chargerSamples",       "chargerSampleCount",       "chargerSampleRate"),
    "wastegateSound":      ("wastegateSamples",     "wastegateSampleCount",     "wastegateSampleRate"),
    "fanSound":            ("fanSamples",           "fanSampleCount",           "fanSampleRate"),
    "hornSound":           ("hornSamples",          "hornSampleCount",          "hornSampleRate"),
    "sirenSound":          ("sirenSamples",         "sirenSampleCount",         "sirenSampleRate"),
    "brakeSound":          ("brakeSamples",         "brakeSampleCount",         "brakeSampleRate"),
    "parkingBrakeSound":   ("parkingBrakeSamples",  "parkingBrakeSampleCount",  "parkingBrakeSampleRate"),
    "shiftingSound":       ("shiftingSamples",      "shiftingSampleCount",      "shiftingSampleRate"),
    "sound1Sound":         ("sound1Samples",        "sound1SampleCount",        "sound1SampleRate"),
    "reversingSound":      ("reversingSamples",     "reversingSampleCount",     "reversingSampleRate"),
    "indicatorSound":      ("indicatorSamples",     "indicatorSampleCount",     "indicatorSampleRate"),
    "couplingSound":       ("couplingSamples",      "couplingSampleCount",      "couplingSampleRate"),
    "uncouplingSound":     ("uncouplingSamples",    "uncouplingSampleCount",    "uncouplingSampleRate"),
    "hydraulicPumpSound":  ("hydraulicPumpSamples", "hydraulicPumpSampleCount", "hydraulicPumpSampleRate"),
    "hydraulicFlowSound":  ("hydraulicFlowSamples", "hydraulicFlowSampleCount", "hydraulicFlowSampleRate"),
    "trackRattleSound":    ("trackRattleSamples",    "trackRattleSampleCount",    "trackRattleSampleRate"),
    "trackRattle2Sound":   ("trackRattle2Samples",   "trackRattle2SampleCount",   "trackRattle2SampleRate"),
    "bucketRattleSound":   ("bucketRattleSamples",  "bucketRattleSampleCount",  "bucketRattleSampleRate"),
}


def detect_sound_vars(filepath):
    """Detect the array name, count name, and rate name defined in a sound .h file."""
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except FileNotFoundError:
        return None, None, None
    arr_m = re.search(r'(?:const\s+)?(?:signed\s+)?char\s+(\w+)\s*\[\]', text)
    cnt_m = re.search(r'(?:const\s+)?(?:unsigned\s+)?int\s+(\w*[Ss]ample[Cc]ount)\s*=', text)
    rate_m = re.search(r'(?:const\s+)?(?:unsigned\s+)?int\s+(\w*[Ss]ample[Rr]ate)\s*=', text)
    arr_name = arr_m.group(1) if arr_m else None
    cnt_name = cnt_m.group(1) if cnt_m else None
    rate_name = rate_m.group(1) if rate_m else None
    return arr_name, cnt_name, rate_name


def make_aliases(slot, sound_file):
    """Generate C alias lines if the sound file's variable names don't match what the slot expects."""
    if slot not in SLOT_EXPECTED_VARS:
        return []
    exp_arr, exp_cnt, exp_rate = SLOT_EXPECTED_VARS[slot]
    fpath = os.path.join(SOUNDS_DIR, sound_file)
    act_arr, act_cnt, act_rate = detect_sound_vars(fpath)
    if act_arr is None:
        return []
    aliases = []
    if act_arr != exp_arr:
        aliases.append(f"const signed char* {exp_arr} = {act_arr};")
    if act_cnt and act_cnt != exp_cnt:
        aliases.append(f"const unsigned int {exp_cnt} = {act_cnt};")
    if act_rate and act_rate != exp_rate:
        aliases.append(f"const unsigned int {exp_rate} = {act_rate};")
    return aliases

def read_config():
    """Parse config.h into a dict of current settings."""
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        text = f.read()

    cfg = {}

    # Machine type
    machine_modes = ["EXCAVATOR_MODE", "LOADER_MODE", "CRANE_MODE", "DOZER_MODE", "SKIDSTEER_MODE", "GRADER_MODE", "BACKHOE_MODE"]
    cfg["machineType"] = "EXCAVATOR_MODE"  # default
    for m in machine_modes:
        # active = line starts with #define (not //)
        pat = re.compile(r'^#define\s+' + m, re.MULTILINE)
        if pat.search(text):
            cfg["machineType"] = m

    # RC protocol
    rc_protos = ["SBUS_COMMUNICATION", "IBUS_COMMUNICATION", "SUMD_COMMUNICATION",
                 "PPM_COMMUNICATION", "PWM_COMMUNICATION"]
    cfg["rcProtocol"] = "SBUS_COMMUNICATION"
    for p in rc_protos:
        pat = re.compile(r'^#define\s+' + p, re.MULTILINE)
        if pat.search(text):
            cfg["rcProtocol"] = p
    # Gamepad (Bluepad32) is a mutually-exclusive input source — Bluetooth instead of an RC bus.
    if re.search(r'^#define\s+GAMEPAD_MODE\b', text, re.MULTILINE):
        cfg["rcProtocol"] = "GAMEPAD_MODE"

    # Dozer drive mode
    cfg["driveMode"] = "DRIVE_DUAL_STICK"
    if re.search(r'^#define\s+DRIVE_SINGLE_STICK_MIX\b', text, re.MULTILINE):
        cfg["driveMode"] = "DRIVE_SINGLE_STICK_MIX"

    # Auto idle-down switch
    m = re.search(r'boolean\s+autoIdleEnabled\s*=\s*(true|false)', text)
    cfg["autoIdleEnabled"] = m.group(1) if m else "true"

    # SBUS settings
    m = re.search(r'boolean\s+sbusInverted\s*=\s*(true|false)', text)
    cfg["sbusInverted"] = m.group(1) if m else "true"
    m = re.search(r'uint32_t\s+sbusBaud\s*=\s*(\d+)', text)
    cfg["sbusBaud"] = int(m.group(1)) if m else 100000

    # Volumes (volatile int name = value)
    vol_pattern = re.compile(r'volatile\s+(?:int|int16_t)\s+(\w+)\s*=\s*(-?\d+)')
    for match in vol_pattern.finditer(text):
        cfg[match.group(1)] = int(match.group(2))

    # Const uint16_t / uint8_t / int8_t / uint32_t values
    const_pattern = re.compile(r'(?:volatile\s+)?(?:const\s+)?(?:uint32_t|uint16_t|uint8_t|int8_t|int16_t|int)\s+(\w+)\s*=\s*(-?\d+)')
    for match in const_pattern.finditer(text):
        name = match.group(1)
        if name not in cfg:
            cfg[name] = int(match.group(2))

    # Boolean consts
    bool_pattern = re.compile(r'(?:const\s+)?boolean\s+(\w+)\s*=\s*(true|false)')
    for match in bool_pattern.finditer(text):
        cfg[match.group(1)] = match.group(2)

    # Channel reverse array: boolean channelReversed[17] = { false, true, ... };
    rev_match = re.search(r'boolean\s+channelReversed\s*\[\s*\d+\s*\]\s*=\s*\{([^}]+)\}', text, re.DOTALL)
    if rev_match:
        vals = [v.strip() for v in rev_match.group(1).split(',')]
        rev_dict = {}
        for i in range(1, min(len(vals), 17)):
            rev_dict[i] = (vals[i].split('//')[0].strip() == 'true')
        cfg["channelReversed"] = rev_dict
    else:
        cfg["channelReversed"] = {i: False for i in range(1, 17)}

    # Channel enable array: boolean channelEnabled[17] = { true, true, ... };
    en_match = re.search(r'boolean\s+channelEnabled\s*\[\s*\d+\s*\]\s*=\s*\{([^}]+)\}', text, re.DOTALL)
    if en_match:
        vals = [v.strip() for v in en_match.group(1).split(',')]
        en_dict = {}
        for i in range(1, min(len(vals), 17)):
            en_dict[i] = (vals[i].split('//')[0].strip() != 'false')
        cfg["channelEnabled"] = en_dict
    else:
        cfg["channelEnabled"] = {i: True for i in range(1, 17)}

    # Sound file includes: #include "sounds/XXX.h"
    # Map them by their preceding comment or variable context
    sound_slots = {
        "startSound": None, "idleSound": None, "revSound": None, "knockSound": None,
        "turboSound": None, "chargerSound": None, "wastegateSound": None, "fanSound": None,
        "hornSound": None, "sirenSound": None, "brakeSound": None, "parkingBrakeSound": None,
        "shiftingSound": None, "sound1Sound": None, "reversingSound": None, "indicatorSound": None,
        "couplingSound": None, "uncouplingSound": None,
        "hydraulicPumpSound": None, "hydraulicFlowSound": None,
        "trackRattleSound": None, "trackRattle2Sound": None, "bucketRattleSound": None,
    }

    # Parse includes in order — map by the variable that precedes them
    include_pat = re.compile(r'(?://\s*)?#include\s+"sounds/([^"]+)"')
    lines = text.split('\n')
    slot_map = {
        "startVolumePercentage": "startSound",
        "idleVolumePercentage": "idleSound",
        "revVolumePercentage": "revSound",
        "dieselKnockVolumePercentage": "knockSound",
        "turboVolumePercentage": "turboSound",
        "chargerVolumePercentage": "chargerSound",
        "wastegateVolumePercentage": "wastegateSound",
        "fanVolumePercentage": "fanSound",
        "hornVolumePercentage": "hornSound",
        "sirenVolumePercentage": "sirenSound",
        "brakeVolumePercentage": "brakeSound",
        "parkingBrakeVolumePercentage": "parkingBrakeSound",
        "shiftingVolumePercentage": "shiftingSound",
        "sound1VolumePercentage": "sound1Sound",
        "reversingVolumePercentage": "reversingSound",
        "indicatorVolumePercentage": "indicatorSound",
        "couplingVolumePercentage": "couplingSound",
        "hydraulicPumpVolumePercentage": "hydraulicPumpSound",
        "hydraulicFlowVolumePercentage": "hydraulicFlowSound",
        "trackRattleVolumePercentage": "trackRattleSound",
        "trackRattle2VolumePercentage": "trackRattle2Sound",
        "bucketRattleVolumePercentage": "bucketRattleSound",
    }

    current_slot = None
    for line in lines:
        for var, slot in slot_map.items():
            if var in line and '=' in line:
                current_slot = slot
        im = include_pat.search(line)
        if im and current_slot:
            sound_slots[current_slot] = im.group(1)
            if current_slot == "couplingSound":
                current_slot = "uncouplingSound"  # next include is uncoupling
            else:
                current_slot = None

    # Pick up any slots from previous "Auto-injected" fixup blocks so we don't
    # lose track on round-trip
    for slot in sound_slots:
        if sound_slots[slot] is None:
            inj = re.search(
                r'// Auto-injected for ' + re.escape(slot) + r' \(was missing\).*?\n#include\s+"sounds/([^"]+)"',
                text, re.DOTALL)
            if inj:
                sound_slots[slot] = inj.group(1)
            else:
                # No #include in the auto-injected block, but the alias points to a
                # variable from another file that's already included. Detect via alias.
                exp = SLOT_EXPECTED_VARS.get(slot)
                if exp:
                    exp_arr = exp[0]
                    am = re.search(
                        r'const\s+signed\s+char\*\s+' + re.escape(exp_arr) + r'\s*=\s*(\w+)\s*;',
                        text)
                    if am:
                        # Find which sound file declares that base var
                        base_var = am.group(1)
                        for fn in os.listdir(SOUNDS_DIR):
                            if not fn.endswith('.h'):
                                continue
                            arr_name, _, _ = detect_sound_vars(os.path.join(SOUNDS_DIR, fn))
                            if arr_name == base_var:
                                sound_slots[slot] = fn
                                break

    cfg["sounds"] = sound_slots

    # REV_SOUND enabled?
    cfg["revSoundEnabled"] = bool(re.search(r'^#define\s+REV_SOUND', text, re.MULTILINE))

    # TRACK_RATTLE_2 enabled?
    cfg["trackRattle2Enabled"] = bool(re.search(r'^#define\s+TRACK_RATTLE_2', text, re.MULTILINE))

    # COUPLING_SOUND enabled?
    cfg["couplingSoundEnabled"] = bool(re.search(r'^#define\s+COUPLING_SOUND', text, re.MULTILINE))

    # Debug flags
    cfg["debugRc"] = bool(re.search(r'^#define\s+DEBUG_RC', text, re.MULTILINE))
    cfg["debugEsc"] = bool(re.search(r'^#define\s+DEBUG_ESC', text, re.MULTILINE))
    cfg["debugSound"] = bool(re.search(r'^#define\s+DEBUG_SOUND', text, re.MULTILINE))
    cfg["debugHydraulic"] = bool(re.search(r'^#define\s+DEBUG_HYDRAULIC', text, re.MULTILINE))

    # Servo arrays
    sm = re.search(r'servoMin\[5\]\s*=\s*\{([^}]+)\}', text)
    cfg["servoMin"] = [int(x.strip()) for x in sm.group(1).split(',')] if sm else [1000]*5
    sm = re.search(r'servoMax\[5\]\s*=\s*\{([^}]+)\}', text)
    cfg["servoMax"] = [int(x.strip()) for x in sm.group(1).split(',')] if sm else [2000]*5
    sm = re.search(r'servoCenter\[5\]\s*=\s*\{([^}]+)\}', text)
    cfg["servoCenter"] = [int(x.strip()) for x in sm.group(1).split(',')] if sm else [1500]*5

    return cfg


def write_config(cfg):
    """Write settings back to config.h."""
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        text = f.read()

    # Strip ALL previous auto-injected blocks before regenerating, so we don't
    # accumulate duplicates across saves. A block is the comment line plus all
    # following alias / include lines until the next blank line.
    # The trailing \n* also eats each block's trailing blank line, otherwise one
    # blank line leaked per slot on every save and the file grew unbounded.
    text = re.sub(
        r'^// Auto-injected for \w+ \(was missing\)[^\n]*\n(?:(?:#include[^\n]*|const[^\n]*)\n)*\n*',
        '', text, flags=re.MULTILINE)

    # Machine type
    machine_modes = ["EXCAVATOR_MODE", "LOADER_MODE", "CRANE_MODE", "DOZER_MODE", "SKIDSTEER_MODE", "GRADER_MODE", "BACKHOE_MODE"]
    for m in machine_modes:
        if m == cfg.get("machineType"):
            text = re.sub(r'^(//\s*)?#define\s+' + m + r'(.*)', '#define ' + m + r'\2', text, flags=re.MULTILINE)
        else:
            text = re.sub(r'^(//\s*)?#define\s+' + m + r'(.*)', '// #define ' + m + r'\2', text, flags=re.MULTILINE)

    # RC protocol / input source — GAMEPAD_MODE is mutually exclusive with the RC buses.
    rc_protos = ["SBUS_COMMUNICATION", "IBUS_COMMUNICATION", "SUMD_COMMUNICATION",
                 "PPM_COMMUNICATION", "PWM_COMMUNICATION", "GAMEPAD_MODE"]
    for p in rc_protos:
        if p == cfg.get("rcProtocol"):
            text = re.sub(r'^(//\s*)?#define\s+' + p + r'\b(.*)', '#define ' + p + r'\2', text, flags=re.MULTILINE)
        else:
            text = re.sub(r'^(//\s*)?#define\s+' + p + r'\b(.*)', '// #define ' + p + r'\2', text, flags=re.MULTILINE)

    # Dozer drive mode (single-joystick mix vs dual-stick)
    for d in ("DRIVE_SINGLE_STICK_MIX", "DRIVE_DUAL_STICK"):
        if d == cfg.get("driveMode"):
            text = re.sub(r'^(//\s*)?#define\s+' + d + r'\b(.*)', '#define ' + d + r'\2', text, flags=re.MULTILINE)
        else:
            text = re.sub(r'^(//\s*)?#define\s+' + d + r'\b(.*)', '// #define ' + d + r'\2', text, flags=re.MULTILINE)

    # SBUS settings
    if "sbusInverted" in cfg:
        text = re.sub(r'(boolean\s+sbusInverted\s*=\s*)(true|false)', r'\g<1>' + cfg["sbusInverted"], text)
    if "sbusBaud" in cfg:
        text = re.sub(r'(uint32_t\s+sbusBaud\s*=\s*)\d+', r'\g<1>' + str(cfg["sbusBaud"]), text)

    # Simple numeric values (volatile int, const uint*, int8_t etc.)
    simple_vars = [
        "startVolumePercentage", "idleVolumePercentage", "engineIdleVolumePercentage",
        "fullThrottleVolumePercentage", "revVolumePercentage", "engineRevVolumePercentage",
        "revSwitchPoint", "idleEndPoint", "idleVolumeProportionPercentage",
        "dieselKnockVolumePercentage", "dieselKnockIdleVolumePercentage",
        "dieselKnockInterval", "dieselKnockStartPoint", "dieselKnockAdaptiveVolumePercentage",
        "turboVolumePercentage", "turboIdleVolumePercentage",
        "chargerVolumePercentage", "chargerIdleVolumePercentage", "chargerStartPoint",
        "wastegateVolumePercentage", "wastegateIdleVolumePercentage",
        "fanVolumePercentage", "fanIdleVolumePercentage", "fanStartPoint",
        "hornVolumePercentage", "sirenVolumePercentage",
        "brakeVolumePercentage",
        "shiftingVolumePercentage", "sound1VolumePercentage",
        "reversingVolumePercentage", "indicatorVolumePercentage",
        "couplingVolumePercentage",
        "hydraulicPumpVolumePercentage", "hydraulicFlowVolumePercentage",
        "trackRattleVolumePercentage", "trackRattle2VolumePercentage",
        "trackRattleIntervalMin", "trackRattleIntervalMax",
        "pwmStrokeChainDriveTopSpeed", "pwmStrokeChainDriveStartRotation",
        "bucketRattleVolumePercentage",
        "pulseNeutral", "pulseSpan",
        "MAX_RPM_PERCENTAGE", "acc", "dec", "clutchEngagingPoint",
        "escRampTimeLow", "escRampTimeHigh",
        "escBrakeSteps", "escAccelerationSteps",
        "hydraulicRampTime", "hydraulicDeadZone",
        "autoIdleDelayMs", "driveExpo", "hydrostaticWhineVolumePercentage",
        "masterVolume", "indicatorOn",
        "hiLoRatioPercent",
        # Channel mapping
        "CH_THROTTLE", "CH_HORN", "CH_ENGINE_TOGGLE", "CH_HILO_TOGGLE",
        "CH_EX_BUCKET", "CH_EX_SWING", "CH_EX_BOOM", "CH_EX_STICK", "CH_EX_TRACK_L", "CH_EX_TRACK_R",
        "CH_LD_BUCKET", "CH_LD_BOOM",
        "CH_CR_BOOM", "CH_CR_EXTEND", "CH_CR_SWING",
        "CH_DZ_BLADE", "CH_DZ_TILT", "CH_DZ_RIPPER", "CH_DZ_TRACK_L", "CH_DZ_TRACK_R",
        "CH_DZ_ANGLE", "CH_DZ_RIPPER_TILT",
        "CH_SS_BUCKET", "CH_SS_BOOM",
        "CH_GR_BLADE", "CH_GR_CIRCLE", "CH_GR_TILT", "CH_GR_ARTICULATION",
        "CH_LIGHTS",
    ]
    for var in simple_vars:
        if var in cfg:
            text = re.sub(
                r'(\b' + var + r'\s*=\s*)-?\d+',
                r'\g<1>' + str(cfg[var]),
                text
            )

    # Boolean consts
    bool_vars = ["automatic", "doubleClutch", "shiftingAutoThrottle", "INDICATOR_DIR",
                 "hiLoEnabled", "hiLoDefaultHigh", "autoEngineStart",
                 "travelAlarmBothDirections", "autoIdleEnabled"]
    for var in bool_vars:
        if var in cfg:
            # Normalize: accept bool, string, or int → always "true"/"false"
            raw = cfg[var]
            if isinstance(raw, bool):
                val = "true" if raw else "false"
            elif isinstance(raw, int):
                val = "true" if raw else "false"
            elif isinstance(raw, str):
                val = "true" if raw.lower() in ("true", "1") else "false"
            else:
                val = str(raw).lower()
            text = re.sub(
                r'(\b' + var + r'\s*=\s*)(true|false)',
                r'\g<1>' + val,
                text
            )

    # Channel reverse array
    if "channelReversed" in cfg:
        rev = cfg["channelReversed"]
        vals = ["false"]  # CH0 (unused)
        for i in range(1, 17):
            v = rev.get(i, rev.get(str(i), False))
            vals.append("true" if v else "false")
        lines = []
        for j in range(17):
            suffix = ' (unused)' if j == 0 else ''
            comma = ',' if j < 16 else ''
            lines.append(vals[j] + comma + ' // CH' + str(j) + suffix)
        text = re.sub(
            r'(boolean\s+channelReversed\s*\[\s*\d+\s*\]\s*=\s*\{)[^}]+(\})',
            r'\g<1>\n    ' + '\n    '.join(lines) + '\n\g<2>',
            text,
            flags=re.DOTALL
        )

    # Channel enable array
    if "channelEnabled" in cfg:
        en = cfg["channelEnabled"]
        vals = ["true"]  # CH0 (unused)
        for i in range(1, 17):
            v = en.get(i, en.get(str(i), True))
            vals.append("true" if v else "false")
        lines = []
        for j in range(17):
            suffix = ' (unused)' if j == 0 else ''
            comma = ',' if j < 16 else ''
            lines.append(vals[j] + comma + ' // CH' + str(j) + suffix)
        text = re.sub(
            r'(boolean\s+channelEnabled\s*\[\s*\d+\s*\]\s*=\s*\{)[^}]+(\})',
            r'\g<1>\n    ' + '\n    '.join(lines) + '\n\g<2>',
            text,
            flags=re.DOTALL
        )

    # Sound file swaps
    sounds = cfg.get("sounds", {})
    include_pat = re.compile(r'((?://\s*)?#include\s+"sounds/)([^"]+)(")')
    slot_map_rev = {}
    for var, slot in {
        "startVolumePercentage": "startSound",
        "idleVolumePercentage": "idleSound",
        "revVolumePercentage": "revSound",
        "dieselKnockVolumePercentage": "knockSound",
        "turboVolumePercentage": "turboSound",
        "chargerVolumePercentage": "chargerSound",
        "wastegateVolumePercentage": "wastegateSound",
        "fanVolumePercentage": "fanSound",
        "hornVolumePercentage": "hornSound",
        "sirenVolumePercentage": "sirenSound",
        "brakeVolumePercentage": "brakeSound",
        "parkingBrakeVolumePercentage": "parkingBrakeSound",
        "shiftingVolumePercentage": "shiftingSound",
        "sound1VolumePercentage": "sound1Sound",
        "reversingVolumePercentage": "reversingSound",
        "indicatorVolumePercentage": "indicatorSound",
        "couplingVolumePercentage": "couplingSound",
        "hydraulicPumpVolumePercentage": "hydraulicPumpSound",
        "hydraulicFlowVolumePercentage": "hydraulicFlowSound",
        "trackRattleVolumePercentage": "trackRattleSound",
        "trackRattle2VolumePercentage": "trackRattle2Sound",
        "bucketRattleVolumePercentage": "bucketRattleSound",
    }.items():
        slot_map_rev[slot] = var

    # Replace includes line by line, tracking which slot we're in
    # Also strip old alias lines (generated by previous saves) and regenerate them.
    # We also track which sample variables are already declared (either by an
    # included sound file or by an earlier alias) so we never emit a redefinition.
    alias_line_pat = re.compile(r'^(?:// Alias:.*|// (?:Already included|Skipped duplicate|Skipped: sounds/).*|const\s+(?:signed\s+char\*|unsigned\s+int)\s+\w+\s*=\s*\w+;)$')

    def render_include(line, sound_file, skip):
        """Return the include line — commented out if skip, active otherwise.
        Strips leading `// ` if not skipping (re-enabling a previously skipped one)."""
        # Normalize: ensure exactly one form. First strip any leading `//`.
        active_line = re.sub(r'^\s*//\s*(#include)', r'\1', line)
        active_line = re.sub(r'(#include\s+"sounds/)([^"]+)(")', r'\1' + sound_file + r'\3', active_line)
        if skip:
            return '// ' + active_line.lstrip()
        return active_line
    declared_vars = set()
    included_files = set()

    # PRE-PASS: determine the final set of #include files (after slot
    # substitutions) and collect ALL variables those files will declare. This
    # way the alias-generation pass knows up-front which symbols will be
    # provided natively by an include and won't emit a colliding alias.
    # We also detect when two DIFFERENT sound files would declare the SAME
    # global variable (e.g. CAT730Rev.h and Caterpillar323Rev.h both declare
    # `revSamples`). In that case we keep only the first file's #include and
    # mark subsequent ones as "skip" — their slots will reuse the existing
    # variables, which keeps the build green even if the audio is shared.
    pre_lines = text.split('\n')
    pre_slot = None
    skip_includes = set()  # set of (line_index, filename) tuples to drop
    pre_idx = -1
    for line in pre_lines:
        pre_idx += 1
        if alias_line_pat.match(line.strip()):
            continue
        for var, slot in {
            "startVolumePercentage": "startSound",
            "idleVolumePercentage": "idleSound",
            "revVolumePercentage": "revSound",
            "dieselKnockVolumePercentage": "knockSound",
            "turboVolumePercentage": "turboSound",
            "chargerVolumePercentage": "chargerSound",
            "wastegateVolumePercentage": "wastegateSound",
            "fanVolumePercentage": "fanSound",
            "hornVolumePercentage": "hornSound",
            "sirenVolumePercentage": "sirenSound",
            "brakeVolumePercentage": "brakeSound",
            "parkingBrakeVolumePercentage": "parkingBrakeSound",
            "shiftingVolumePercentage": "shiftingSound",
            "sound1VolumePercentage": "sound1Sound",
            "reversingVolumePercentage": "reversingSound",
            "indicatorVolumePercentage": "indicatorSound",
            "couplingVolumePercentage": "couplingSound",
            "hydraulicPumpVolumePercentage": "hydraulicPumpSound",
            "hydraulicFlowVolumePercentage": "hydraulicFlowSound",
            "trackRattleVolumePercentage": "trackRattleSound",
            "trackRattle2VolumePercentage": "trackRattle2Sound",
            "bucketRattleVolumePercentage": "bucketRattleSound",
        }.items():
            if var in line and '=' in line:
                pre_slot = slot
        im = include_pat.search(line)
        if im:
            if pre_slot and pre_slot in sounds and sounds[pre_slot]:
                fn = sounds[pre_slot]
            else:
                fn = im.group(2)
            arr_n, cnt_n, rate_n = detect_sound_vars(os.path.join(SOUNDS_DIR, fn))
            # Conflict with already-declared vars from a DIFFERENT file?
            conflict = (
                (arr_n and arr_n in declared_vars) or
                (cnt_n and cnt_n in declared_vars) or
                (rate_n and rate_n in declared_vars)
            )
            if fn in included_files:
                # Same file referenced twice: pragma once handles it, no skip needed.
                pass
            elif conflict:
                # Different file but it would redefine an existing global. Skip it.
                skip_includes.add(pre_idx)
            else:
                included_files.add(fn)
                if arr_n: declared_vars.add(arr_n)
                if cnt_n: declared_vars.add(cnt_n)
                if rate_n: declared_vars.add(rate_n)
            if pre_slot == "couplingSound":
                pre_slot = "uncouplingSound"
            else:
                pre_slot = None

    def alias_lines_safe(slot, sound_file):
        """Build alias lines for a slot, skipping any whose target is already declared."""
        if slot not in SLOT_EXPECTED_VARS:
            return []
        exp_arr, exp_cnt, exp_rate = SLOT_EXPECTED_VARS[slot]
        fpath = os.path.join(SOUNDS_DIR, sound_file)
        act_arr, act_cnt, act_rate = detect_sound_vars(fpath)
        if act_arr is None:
            return []
        out = []
        # Only emit alias if the target name is NOT already declared natively
        # by any included file (and the source name IS declared somewhere).
        if act_arr != exp_arr and exp_arr not in declared_vars:
            out.append(f"const signed char* {exp_arr} = {act_arr};")
            declared_vars.add(exp_arr)
        if act_cnt and act_cnt != exp_cnt and exp_cnt not in declared_vars:
            out.append(f"const unsigned int {exp_cnt} = {act_cnt};")
            declared_vars.add(exp_cnt)
        if act_rate and act_rate != exp_rate and exp_rate not in declared_vars:
            out.append(f"const unsigned int {exp_rate} = {act_rate};")
            declared_vars.add(exp_rate)
        return out

    def register_include(sound_file):
        # Already populated in pre-pass; keep no-op for compatibility
        return

    new_lines = []
    current_slot = None
    slot_map_fwd = {v: k for k, v in slot_map_rev.items()}
    lines = text.split('\n')
    for line_idx, line in enumerate(lines):
        # Skip old alias / dedupe-comment lines (will be regenerated)
        if alias_line_pat.match(line.strip()):
            continue

        for var, slot in {
            "startVolumePercentage": "startSound",
            "idleVolumePercentage": "idleSound",
            "revVolumePercentage": "revSound",
            "dieselKnockVolumePercentage": "knockSound",
            "turboVolumePercentage": "turboSound",
            "chargerVolumePercentage": "chargerSound",
            "wastegateVolumePercentage": "wastegateSound",
            "fanVolumePercentage": "fanSound",
            "hornVolumePercentage": "hornSound",
            "sirenVolumePercentage": "sirenSound",
            "brakeVolumePercentage": "brakeSound",
            "parkingBrakeVolumePercentage": "parkingBrakeSound",
            "shiftingVolumePercentage": "shiftingSound",
            "sound1VolumePercentage": "sound1Sound",
            "reversingVolumePercentage": "reversingSound",
            "indicatorVolumePercentage": "indicatorSound",
            "couplingVolumePercentage": "couplingSound",
            "hydraulicPumpVolumePercentage": "hydraulicPumpSound",
            "hydraulicFlowVolumePercentage": "hydraulicFlowSound",
            "trackRattleVolumePercentage": "trackRattleSound",
            "trackRattle2VolumePercentage": "trackRattle2Sound",
            "bucketRattleVolumePercentage": "bucketRattleSound",
        }.items():
            if var in line and '=' in line:
                current_slot = slot

        im = include_pat.search(line)

        if im and current_slot and current_slot in sounds and sounds[current_slot]:
            sound_file = sounds[current_slot]
            skip = line_idx in skip_includes
            new_lines.append(render_include(line, sound_file, skip))
            if not skip:
                aliases = alias_lines_safe(current_slot, sound_file)
                if aliases:
                    new_lines.append("// Alias: auto-generated variable mapping")
                    new_lines.extend(aliases)
            if current_slot == "couplingSound":
                current_slot = "uncouplingSound"
            else:
                current_slot = None
        elif im and current_slot == "uncouplingSound" and "uncouplingSound" in sounds and sounds["uncouplingSound"]:
            sound_file = sounds["uncouplingSound"]
            skip = line_idx in skip_includes
            new_lines.append(render_include(line, sound_file, skip))
            if not skip:
                aliases = alias_lines_safe("uncouplingSound", sound_file)
                if aliases:
                    new_lines.append("// Alias: auto-generated variable mapping")
                    new_lines.extend(aliases)
            current_slot = None
        elif im and current_slot:
            sound_file = im.group(2)
            skip = line_idx in skip_includes
            new_lines.append(render_include(line, sound_file, skip))
            if not skip:
                aliases = alias_lines_safe(current_slot, sound_file)
                if aliases:
                    new_lines.append("// Alias: auto-generated variable mapping")
                    new_lines.extend(aliases)
            if current_slot == "couplingSound":
                current_slot = "uncouplingSound"
            else:
                current_slot = None
        else:
            if im:
                sound_file = im.group(2)
                skip = line_idx in skip_includes
                new_lines.append(render_include(line, sound_file, skip))
            else:
                new_lines.append(line)

    text = '\n'.join(new_lines)

    # Toggle defines
    def set_define(name, enabled):
        nonlocal text
        if enabled:
            text = re.sub(r'^//\s*#define\s+' + name, '#define ' + name, text, flags=re.MULTILINE)
        else:
            text = re.sub(r'^#define\s+' + name + r'(\s|$)', '// #define ' + name + r'\1', text, flags=re.MULTILINE)

    if "revSoundEnabled" in cfg:
        set_define("REV_SOUND", cfg["revSoundEnabled"])
    if "trackRattle2Enabled" in cfg:
        set_define("TRACK_RATTLE_2", cfg["trackRattle2Enabled"])
    if "couplingSoundEnabled" in cfg:
        set_define("COUPLING_SOUND", cfg["couplingSoundEnabled"])

    # Debug flags
    for flag, define in [("debugRc", "DEBUG_RC"), ("debugEsc", "DEBUG_ESC"),
                         ("debugSound", "DEBUG_SOUND"), ("debugHydraulic", "DEBUG_HYDRAULIC")]:
        if flag in cfg:
            set_define(define, cfg[flag])

    # Servo arrays
    for arr_name in ["servoMin", "servoMax", "servoCenter"]:
        if arr_name in cfg:
            vals = ", ".join(str(v) for v in cfg[arr_name])
            text = re.sub(
                r'(' + arr_name + r'\[5\]\s*=\s*\{)[^}]+(})',
                r'\g<1>' + vals + r'\2',
                text
            )

    # Fix-up pass: if any slot's expected variables aren't declared yet
    # (e.g. include line was lost in a previous bad save), alias them to the
    # idle sound's variables. We deliberately DO NOT inject a new #include
    # because that can cause namespace conflicts with other slots aliasing
    # variables from that same file. The idle sound is always present.
    fixup_lines = []
    for slot in sounds:
        if slot not in SLOT_EXPECTED_VARS or slot == "idleSound":
            continue
        exp_arr, exp_cnt, exp_rate = SLOT_EXPECTED_VARS[slot]
        # Already declared (by an include or by an emitted alias)?
        if exp_arr in declared_vars:
            continue
        fixup_lines.append(f'// Auto-injected for {slot} (was missing) — using idle sound')
        fixup_lines.append(f'const signed char* {exp_arr} = samples;')
        fixup_lines.append(f'const unsigned int {exp_cnt} = sampleCount;')
        fixup_lines.append(f'const unsigned int {exp_rate} = sampleRate;')
        fixup_lines.append('')
        declared_vars.add(exp_arr)
        declared_vars.add(exp_cnt)
        declared_vars.add(exp_rate)
    if fixup_lines:
        # Insert before the "RC SIGNAL TUNING" section header (or before MASTER VOLUME)
        marker = re.search(r'^//\s*=+\s*\n//\s*RC SIGNAL TUNING', text, re.MULTILINE)
        if not marker:
            marker = re.search(r'^//\s*=+\s*\n//\s*MASTER VOLUME', text, re.MULTILINE)
        injection = '\n'.join(fixup_lines) + '\n'
        if marker:
            text = text[:marker.start()] + injection + text[marker.start():]
        else:
            rt_marker = re.search(r'^//\s*=+\s*\n//\s*RUNTIME SOUND INDIRECTION', text, re.MULTILINE)
            if rt_marker:
                text = text[:rt_marker.start()] + injection + text[rt_marker.start():]
            else:
                text += '\n' + injection

    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        f.write(text)


# ─── Vehicle Profiles ────────────────────────────────────────────────────────

# Track which vehicle is currently loaded (None = unsaved)
_current_vehicle_file = None

def list_vehicles():
    """Return list of saved vehicle profile names (without .json)."""
    files = []
    for f in sorted(glob.glob(os.path.join(VEHICLES_DIR, "*.json"))):
        files.append(os.path.splitext(os.path.basename(f))[0])
    return files

def save_vehicle(name, cfg):
    """Save config dict as a vehicle profile JSON."""
    global _current_vehicle_file
    fn = re.sub(r'[^a-zA-Z0-9_ -]', '', name).strip()
    if not fn:
        raise ValueError("Invalid vehicle name")
    path = os.path.join(VEHICLES_DIR, fn + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    _current_vehicle_file = fn
    return fn

def load_vehicle(name):
    """Load a vehicle profile and apply it to config.h."""
    global _current_vehicle_file
    fn = re.sub(r'[^a-zA-Z0-9_ -]', '', name).strip()
    path = os.path.join(VEHICLES_DIR, fn + ".json")
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Vehicle '{fn}' not found")
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    write_config(cfg)
    _current_vehicle_file = fn
    return cfg

def delete_vehicle(name):
    """Delete a vehicle profile."""
    global _current_vehicle_file
    fn = re.sub(r'[^a-zA-Z0-9_ -]', '', name).strip()
    path = os.path.join(VEHICLES_DIR, fn + ".json")
    if os.path.isfile(path):
        os.remove(path)
        if _current_vehicle_file == fn:
            _current_vehicle_file = None


# ─── Build / Flash ───────────────────────────────────────────────────────────

build_log = []
build_running = False

def find_pio():
    """Find platformio CLI."""
    home = os.path.expanduser("~")
    pio = os.path.join(home, ".platformio", "penv", "Scripts", "pio.exe")
    if os.path.exists(pio):
        return pio
    pio2 = os.path.join(home, ".platformio", "penv", "bin", "pio")
    if os.path.exists(pio2):
        return pio2
    return "pio"

# ── No-install build path: arduino-cli (no PlatformIO / VS Code required) ──────
def find_arduino_cli():
    import shutil
    p = shutil.which("arduino-cli")
    if p:
        return p
    la = os.environ.get("LOCALAPPDATA", "")
    for c in [
        os.path.join(la, "Programs", "Arduino IDE", "resources", "app", "lib", "backend", "resources", "arduino-cli.exe"),
        r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
        "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli",
    ]:
        if os.path.isfile(c):
            return c
    return None


def get_build_libs():
    """Bundled libraries (repo/libraries) so a fresh clone builds with nothing installed;
    falls back to PlatformIO's .pio/libdeps on a dev machine."""
    for base in [os.path.join(PROJECT_DIR, "libraries"),
                 os.path.join(PROJECT_DIR, ".pio", "libdeps", "esp32")]:
        if os.path.isdir(base):
            dirs = [os.path.join(base, e) for e in os.listdir(base)
                    if os.path.isdir(os.path.join(base, e))]
            if dirs:
                return dirs
    return []


def is_gamepad_build():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return bool(re.search(r'^#define\s+GAMEPAD_MODE\b', f.read(), re.MULTILINE))
    except Exception:
        return False


def stage_build_dir():
    """arduino-cli needs the sketch folder to match the .ino name, only compiles .cpp in the
    root + a src/ subfolder, and rejects UTF-8 BOMs. So stage a clean copy: rename to
    HydraulicController/, strip BOMs, flatten lib/ into the root and fix its includes."""
    import shutil, glob
    bd_root = os.path.join(PROJECT_DIR, ".arduino_build")
    bd = os.path.join(bd_root, "HydraulicController")
    shutil.rmtree(bd_root, ignore_errors=True)
    os.makedirs(bd, exist_ok=True)
    src = os.path.join(PROJECT_DIR, "src")
    for item in os.listdir(src):
        s, d = os.path.join(src, item), os.path.join(bd, item)
        shutil.copytree(s, d) if os.path.isdir(s) else shutil.copy2(s, d)
    libdir = os.path.join(bd, "lib")
    if os.path.isdir(libdir):
        for f in glob.glob(os.path.join(libdir, "*")):
            shutil.move(f, os.path.join(bd, os.path.basename(f)))
        os.rmdir(libdir)
    for f in glob.glob(os.path.join(bd, "**", "*.*"), recursive=True):
        if f.lower().endswith((".h", ".cpp", ".c", ".ino")):
            b = open(f, "rb").read()
            if b[:3] == b"\xef\xbb\xbf":
                b = b[3:]
            t = re.sub(r'#include\s+"lib/', '#include "', b.decode("utf-8", "replace"))
            open(f, "w", encoding="utf-8", newline="").write(t)
    return bd


def run_build_cli(cli, upload=False, port=None):
    global build_log
    gamepad = is_gamepad_build()
    core, ver = ("esp32-bluepad32:esp32", "4.1.0") if gamepad else ("esp32:esp32", "1.0.6")
    fqbn = ("esp32-bluepad32:esp32:esp32:PartitionScheme=huge_app" if gamepad
            else "esp32:esp32:esp32:PartitionScheme=huge_app")
    url = ("https://raw.githubusercontent.com/ricardoquesada/esp32-arduino-lib-builder/master/bluepad32_files/package_esp32_bluepad32_index.json"
           if gamepad else
           "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json")

    def run(args):
        p = subprocess.Popen([cli] + args, cwd=PROJECT_DIR, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, bufsize=1, shell=(os.name == "nt"))
        for line in p.stdout:
            build_log.append(line.rstrip())
        p.wait()
        return p.returncode

    build_log.append("Building with arduino-cli (no PlatformIO / VS Code needed)...")
    subprocess.run([cli, "config", "add", "board_manager.additional_urls", url],
                   capture_output=True, shell=(os.name == "nt"))
    listed = subprocess.run([cli, "core", "list"], capture_output=True, text=True,
                            shell=(os.name == "nt")).stdout
    if not (core in listed and ver in listed):
        build_log.append(f"Installing {core}@{ver} (first time only, a few minutes)...")
        run(["core", "update-index"])
        run(["core", "install", f"{core}@{ver}"])
    bd = stage_build_dir()
    cmd = ["compile", "--fqbn", fqbn]
    for lp in get_build_libs():
        cmd += ["--library", lp]
    if upload and port:
        cmd += ["--upload", "--port", port]
    cmd.append(bd)
    build_log.append(f"Uploading to {port}..." if (upload and port) else "Compiling firmware...")
    return run(cmd)


def run_build(upload=False, port=None):
    global build_log, build_running
    build_log = []
    build_running = True
    try:
        cli = find_arduino_cli()
        if cli:
            rc = run_build_cli(cli, upload, port)
        else:
            # Fallback: PlatformIO (gamepad uses its own Bluepad32 env)
            pio = find_pio()
            env = "gamepad" if is_gamepad_build() else "esp32"
            cmd = [pio, "run", "-e", env]
            if upload:
                cmd.append("--target=upload")
                if port:
                    cmd.extend(["--upload-port", port])
            proc = subprocess.Popen(cmd, cwd=PROJECT_DIR, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, bufsize=1)
            for line in proc.stdout:
                build_log.append(line.rstrip())
            proc.wait()
            rc = proc.returncode
        build_log.append(f"\n--- Exit code: {rc} ---")
    except Exception as e:
        build_log.append(f"ERROR: {e}")
    build_running = False


# ─── Sound Pack Builder ──────────────────────────────────────────────────────

SNDPACK_MAGIC = 0x504E4453      # "SNDP" little-endian
SNDPACK_VERSION = 1
SOUNDS_PARTITION_OFFSET = 0x190000

def find_esptool():
    """Find esptool.py from PlatformIO packages."""
    home = os.path.expanduser("~")
    for sub in [
        os.path.join(".platformio", "packages", "tool-esptoolpy", "esptool.py"),
        os.path.join(".platformio", "packages", "tool-esptoolpy", "esptool", "__main__.py"),
    ]:
        p = os.path.join(home, sub)
        if os.path.exists(p):
            return p
    return None

def find_pio_python():
    """Find PlatformIO's bundled Python interpreter."""
    home = os.path.expanduser("~")
    for sub in [
        os.path.join(".platformio", "penv", "Scripts", "python.exe"),
        os.path.join(".platformio", "penv", "bin", "python"),
    ]:
        p = os.path.join(home, sub)
        if os.path.exists(p):
            return p
    return sys.executable

def build_soundpack_binary(slot_sounds):
    """Build SNDP binary from {slot_name: sound_filename} dict. Returns bytes."""
    slots_data = []
    for slot_name, filename in slot_sounds.items():
        fpath = os.path.join(SOUNDS_DIR, filename)
        if not os.path.isfile(fpath):
            raise FileNotFoundError(f"Sound file not found: {filename}")
        parsed = parse_sound_header(fpath)
        raw = bytes(s & 0xFF for s in parsed["samples"])
        slots_data.append({
            "name": slot_name,
            "rate": parsed["sampleRate"],
            "count": len(raw),
            "data": raw,
        })

    num_slots = len(slots_data)
    header = struct.pack('<IHH', SNDPACK_MAGIC, SNDPACK_VERSION, num_slots)

    slot_table = b''
    data_section = b''
    data_offset = 0
    for sd in slots_data:
        name_bytes = sd["name"].encode('utf-8')[:31]
        slot_entry = struct.pack('<32sIIII', name_bytes, data_offset, sd["count"], sd["rate"], 0)
        slot_table += slot_entry
        data_section += sd["data"]
        data_offset += sd["count"]

    return header + slot_table + data_section

def flash_soundpack(port, soundpack_data):
    """Write soundpack binary to ESP32 sounds partition via esptool."""
    global build_log, build_running
    build_log = []
    build_running = True

    esptool = find_esptool()
    python = find_pio_python()
    if not esptool:
        build_log.append("ERROR: esptool.py not found in PlatformIO packages")
        build_running = False
        return

    tmp = os.path.join(tempfile.gettempdir(), "soundpack.bin")
    with open(tmp, 'wb') as f:
        f.write(soundpack_data)

    build_log.append(f"Sound pack: {len(soundpack_data):,} bytes ({len(soundpack_data)//1024}KB)")
    build_log.append(f"Writing to partition at 0x{SOUNDS_PARTITION_OFFSET:X} on {port}...")

    cmd = [python, esptool, '--chip', 'esp32', '--port', port, '--baud', '921600',
           'write_flash', hex(SOUNDS_PARTITION_OFFSET), tmp]
    try:
        proc = subprocess.Popen(cmd, cwd=PROJECT_DIR,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        for line in proc.stdout:
            build_log.append(line.rstrip())
        proc.wait()
        build_log.append(f"\n--- Exit code: {proc.returncode} ---")
    except Exception as e:
        build_log.append(f"ERROR: {e}")
    build_running = False


# ─── Serial Push (no-rebuild channel config) ─────────────────────────────────

def list_serial_ports():
    """List available COM ports."""
    ports = []
    try:
        import serial.tools.list_ports
        for p in serial.tools.list_ports.comports():
            ports.append({"port": p.device, "desc": p.description})
    except ImportError:
        # Fallback: scan COM1-20 on Windows
        if sys.platform == 'win32':
            for i in range(1, 21):
                name = f"COM{i}"
                try:
                    import serial as _ser
                    s = _ser.Serial(name, timeout=0.1)
                    s.close()
                    ports.append({"port": name, "desc": name})
                except Exception:
                    pass
        if not ports:
            ports.append({"port": "", "desc": "pyserial not installed — run: pip install pyserial"})
    return ports


def push_channels_serial(port, channels, settings=None, reversed_chs=None, enabled_chs=None):
    """Send channel mappings, settings, reverse flags, and enable flags to ESP32 over serial, then SAVE to NVS."""
    try:
        import serial
    except ImportError:
        return {"ok": False, "error": "pyserial not installed. Run: pip install pyserial"}

    log = []
    try:
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(0.1)  # let ESP32 settle
        ser.reset_input_buffer()

        # Send each channel mapping
        for name, val in channels.items():
            val = int(val)
            if val < 1 or val > 16:
                continue
            cmd = f"CH:{name}={val}\n"
            ser.write(cmd.encode())
            time.sleep(0.05)
            resp = ser.readline().decode(errors='replace').strip()
            log.append(f"{name}={val} → {resp}")

        # Send runtime settings
        if settings:
            for name, val in settings.items():
                cmd = f"SET:{name}={val}\n"
                ser.write(cmd.encode())
                time.sleep(0.05)
                resp = ser.readline().decode(errors='replace').strip()
                log.append(f"{name}={val} → {resp}")

        # Send channel reverse flags
        if reversed_chs:
            for ch_str, val in reversed_chs.items():
                ch = int(ch_str)
                rv = 1 if val else 0
                cmd = f"REV:{ch}={rv}\n"
                ser.write(cmd.encode())
                time.sleep(0.05)
                resp = ser.readline().decode(errors='replace').strip()
                log.append(f"REV CH{ch}={rv} → {resp}")

        # Send channel enable flags
        if enabled_chs:
            for ch_str, val in enabled_chs.items():
                ch = int(ch_str)
                ev = 1 if val else 0
                cmd = f"EN:{ch}={ev}\n"
                ser.write(cmd.encode())
                time.sleep(0.05)
                resp = ser.readline().decode(errors='replace').strip()
                log.append(f"EN CH{ch}={ev} → {resp}")

        # Save to NVS
        ser.write(b"SAVE\n")
        time.sleep(0.1)
        resp = ser.readline().decode(errors='replace').strip()
        log.append(f"SAVE → {resp}")

        ser.close()
        return {"ok": True, "log": log}
    except Exception as e:
        return {"ok": False, "error": str(e), "log": log}


# ─── SPA adapter: serve the DIYGuy web/ SPA against this single-config firmware ─
WEB_DIR = os.path.join(PROJECT_DIR, "web")

# Volume sliders shown on the Levels tab — construction-machine relevant only, in a sensible
# order. Anything not here (coupler, siren, gearshift, indicators, supercharger, cooling fan…)
# is hidden from the UI; its config.h value is left untouched.
CONSTRUCTION_LEVELS = [
    "startVolumePercentage", "idleVolumePercentage", "revVolumePercentage",
    "fullThrottleVolumePercentage", "turboVolumePercentage",
    "hydraulicPumpVolumePercentage", "hydrostaticWhineVolumePercentage",
    "hydraulicFlowVolumePercentage",
    "trackRattleVolumePercentage", "bucketRattleVolumePercentage",
    "hornVolumePercentage", "reversingVolumePercentage",
]
# A few Levels sliders get a clearer label than the auto-prettified variable name.
LEVEL_LABELS = {
    "hydraulicFlowVolumePercentage": "Relief squeal",   # flow voice is the relief cue now
    "hydraulicPumpVolumePercentage": "Hydraulic pump",
    "hydrostaticWhineVolumePercentage": "Drive whine",
}

def spa_schema():
    """Build the app.js /api/schema from config.h (via read_config)."""
    cfg = read_config()

    def sel(name, label, value, opts, desc=""):
        return {"name": name, "label": label, "desc": desc, "control": "select",
                "saveKind": "select", "value": value,
                "options": [{"value": v, "label": l} for v, l in opts]}

    def sld(name, label, value, mn, mx, step=1, suffix="", desc=""):
        return {"name": name, "label": label, "desc": desc, "control": "slider",
                "saveKind": "num", "value": value, "min": mn, "max": mx,
                "step": step, "suffix": suffix}

    def pretty(k):
        s = k.replace("VolumePercentage", "").replace("Volume", "")
        s = re.sub(r'([a-z0-9])([A-Z])', r'\1 \2', s).strip()
        return (s[:1].upper() + s[1:]) or k

    machine = {"file": "config.h", "id": "machine", "label": "Machine", "controls": [
        sel("machineType", "Machine type", cfg.get("machineType"),
            [("EXCAVATOR_MODE", "Excavator"), ("LOADER_MODE", "Loader"), ("CRANE_MODE", "Crane"),
             ("DOZER_MODE", "Dozer"), ("SKIDSTEER_MODE", "Skid Steer"), ("GRADER_MODE", "Grader"),
                    ("BACKHOE_MODE", "Backhoe Loader")],
            "Which machine this firmware drives."),
        sel("driveMode", "Dozer drive", cfg.get("driveMode"),
            [("DRIVE_SINGLE_STICK_MIX", "Single joystick (mixed to both tracks)"),
             ("DRIVE_DUAL_STICK", "Dual stick (one per track)")],
            "How the tracks are driven."),
        sel("autoIdleEnabled", "Auto idle-down", cfg.get("autoIdleEnabled"),
            [("true", "On — settle to idle when parked"), ("false", "Off — hold throttle")],
            "Eases the engine down to low idle after a few seconds with nothing touched, and snaps "
            "back to your set rpm the instant you move a stick or function."),
        sld("autoIdleDelayMs", "Idle-down delay", cfg.get("autoIdleDelayMs", 3000),
            500, 10000, 250, " ms", "How long with no input before it idles down."),
        sld("driveExpo", "Drive stick expo", cfg.get("driveExpo", 30),
            0, 100, 5, "%", "Softens the drive stick around centre so small moves are gentle for "
            "fine control; full throw still reaches max. 0 = linear."),
    ]}

    # Levels tab: only the volumes that matter on construction equipment. The engine sound packs
    # carry a lot of on-road-truck extras (coupler, siren, gearshift, indicators, supercharger…);
    # they'd just clutter a set-and-go dozer, so we show a curated construction list in order.
    lvl = []
    if isinstance(cfg.get("masterVolume"), int):
        lvl.append(sld("masterVolume", "Master volume", cfg["masterVolume"], 0, 300, 5, "%"))
    for k in CONSTRUCTION_LEVELS:
        v = cfg.get(k)
        if isinstance(v, int):
            lvl.append(sld(k, LEVEL_LABELS.get(k, pretty(k)), v, 0, 300, 5, "%"))
    levels = {"file": "config.h", "id": "levels", "label": "Levels", "controls": lvl}

    # Sound Forge choosers — construction-relevant slots only (no siren/coupler/gearshift/etc.).
    sounds = cfg.get("sounds", {}) or {}
    sopts = [{"file": s["file"], "label": s["label"]} for s in scan_all_sounds()]
    # Swappable engine/effect sounds. Diesel knock is disabled (not listed).
    # (Relief squeal / hydraulicFlowSound stays LOCKED — not listed here.)
    slot_titles = [("startSound", "Engine start"), ("idleSound", "Engine idle"),
                   ("revSound", "Engine rev"), ("turboSound", "Turbo"),
                   ("hydraulicPumpSound", "Hydraulic pump whine"),
                   ("trackRattleSound", "Track rattle"), ("bucketRattleSound", "Bucket rattle"),
                   ("hornSound", "Horn"), ("reversingSound", "Reversing beep")]
    # Each slot's dropdown shows only sounds that fit it (by filename keyword). (include, exclude)
    slot_filters = {
        "startSound":         (("start", "crank"), ()),
        "idleSound":          (("idle",), ()),
        "revSound":           (("rev",), ("revers", "beep")),  # "rev" but not "reversing"
        "turboSound":         (("turbo", "whistle", "charger", "wastegate", "blow"), ()),
        "hydraulicPumpSound": (("hydraulic", "pump"), ("flow",)),
        "hydraulicFlowSound": (("hydraulic", "flow", "relief", "squeal", "hiss"), ()),
        "trackRattleSound":   (("track",), ()),
        "bucketRattleSound":  (("bucket",), ()),
        "hornSound":          (("horn",), ()),
        "reversingSound":     (("revers", "beep"), ()),
    }

    def slot_opts(slot, selected):
        f = slot_filters.get(slot)
        if not f:
            return sopts
        incl, excl = f
        out = [o for o in sopts
               if any(k in o["file"].lower() for k in incl)
               and not any(x in o["file"].lower() for x in excl)]
        if selected and not any(o["file"] == selected for o in out):  # always keep the current pick
            sel = next((o for o in sopts if o["file"] == selected), None)
            if sel:
                out = [sel] + out
        return out or sopts  # if nothing matched, fall back to the whole library

    sound_choices = [{"key": slot, "title": title, "options": slot_opts(slot, sounds.get(slot)),
                      "selected": sounds.get(slot), "category": "", "varPrefix": ""}
                     for slot, title in slot_titles if sounds.get(slot)]

    return {"vehicles": [], "currentVehicle": None, "vehicleTab": None,
            "tabs": [machine, levels], "soundChoices": sound_choices,
            "sounds": sounds, "presets": [],
            "levels": {k: cfg[k] for k in (
                "masterVolume", "idleVolumePercentage", "revVolumePercentage",
                "fullThrottleVolumePercentage", "turboVolumePercentage",
                "hydraulicPumpVolumePercentage", "hydraulicFlowVolumePercentage",
                "trackRattleVolumePercentage", "bucketRattleVolumePercentage",
                "hornVolumePercentage", "reversingVolumePercentage") if isinstance(cfg.get(k), int)}}

def spa_save(payload):
    """Translate app.js's {file:{name:{kind,value|enabled}}} into a merged write_config()."""
    full = read_config()
    for _file, fields in (payload or {}).items():
        for name, p in (fields or {}).items():
            if name.startswith("__sound__"):          # Sound Forge slot change
                full.setdefault("sounds", {})[name[len("__sound__"):]] = p.get("value")
            elif name.startswith("__"):
                continue
            elif "value" in p:
                full[name] = p["value"]
            elif "enabled" in p:
                full[name] = "true" if p["enabled"] else "false"
    write_config(full)


# ─── Gamepad control-mapping (ported from the DIYGuy flasher, adapted to the dozer) ───
GP_CONFIG_PATH = os.path.join(PROJECT_DIR, "src", "gamepad_config.h")
GP_BUTTON_CHOICES = [
    ["0x0001", "Cross / A"], ["0x0002", "Circle / B"], ["0x0004", "Square / X"],
    ["0x0008", "Triangle / Y"], ["0x0010", "L1 / LB"], ["0x0020", "R1 / RB"],
    ["0x0040", "L2 / LT (click)"], ["0x0080", "R2 / RT (click)"],
    ["0x0100", "L3 (left stick click)"], ["0x0200", "R3 (right stick click)"],
]
# Digital functions on the dozer: (#define, label, default mask)
GP_FUNCTIONS = [
    ["GP_BTN_HORN", "Horn", "0x0002"],
    ["GP_BTN_ENGINE", "Engine start / stop", "0x0008"],
    ["GP_BTN_LIGHTS", "Lights", "0x0001"],
    ["GP_BTN_HILO", "Hi / Lo range", "0x0004"],
]
# Freely-mappable implement outputs. The 4 slot KEYS (BLADE/TILT/ANGLE/RIPPER) are fixed — they map
# to the firmware's outImpl[0..3] / GPIO33/32/14/27 — but the LABELS follow the selected machine.
GP_OUTPUTS = [
    ["BLADE", "Implement 1 (ESC hdr)"],
    ["TILT", "Implement 2 (CH4)"],
    ["ANGLE", "Implement 3 (CH3)"],
    ["RIPPER", "Implement 4 (32)"],
]
# Per-machine implement names for the 4 slots (fewer than 4 = the rest are hidden in the UI).
MACHINE_OUTPUTS = {
    "DOZER_MODE":     [["BLADE", "Blade lift (ESC hdr)"], ["TILT", "Blade tilt (CH4)"],
                       ["ANGLE", "Blade angle (CH3)"], ["RIPPER", "Ripper (32)"]],
    "EXCAVATOR_MODE": [["BLADE", "Boom (ESC hdr)"], ["TILT", "Stick / arm (CH4)"],
                       ["ANGLE", "Bucket (CH3)"], ["RIPPER", "Swing (32)"]],
    "LOADER_MODE":    [["BLADE", "Boom lift (ESC hdr)"], ["TILT", "Bucket (CH4)"]],
    "CRANE_MODE":     [["BLADE", "Boom lift (ESC hdr)"], ["TILT", "Extend (CH4)"],
                       ["ANGLE", "Swing (CH3)"], ["RIPPER", "Winch (32)"]],
    "GRADER_MODE":    [["BLADE", "Blade lift (ESC hdr)"], ["TILT", "Circle (CH4)"],
                       ["ANGLE", "Blade tilt (CH3)"]],
    "SKIDSTEER_MODE": [["BLADE", "Boom (ESC hdr)"], ["TILT", "Bucket (CH4)"]],
    "BACKHOE_MODE":   [["BLADE", "Boom (ESC hdr)"], ["TILT", "Dipper (CH4)"],
                       ["ANGLE", "Bucket (CH3)"], ["RIPPER", "Swing (32)"]],
}
# The two drive outputs (GPIO13/12) per machine: tracked = two tracks, wheeled = drive + steer.
MACHINE_DRIVE = {
    "DOZER_MODE":     [["CH1", "Right track"], ["CH2", "Left track"]],
    "EXCAVATOR_MODE": [["CH1", "Right track"], ["CH2", "Left track"]],
    "SKIDSTEER_MODE": [["CH1", "Right track"], ["CH2", "Left track"]],
    "LOADER_MODE":    [["CH1", "Drive motor (ESC)"], ["CH2", "Steer servo"]],
    "GRADER_MODE":    [["CH1", "Drive motor (ESC)"], ["CH2", "Articulation steer"]],
    "CRANE_MODE":     [["CH1", "Drive motor (ESC)"], ["CH2", "Steer servo"]],
    "BACKHOE_MODE":   [["CH1", "Drive motor (ESC)"], ["CH2", "Steer servo"]],
}
GP_SOURCES = [
    [0, "Unassigned"], [1, "Left stick — left/right"], [2, "Left stick — up/down"],
    [3, "Right stick — left/right"], [4, "Right stick — up/down"], [5, "L2 trigger"],
    [6, "R2 trigger"], [7, "Triggers (R2 − L2)"],
    [10, "Bumpers (R1 − L1)"], [11, "D-pad ◂ ▸"],
]
GP_SERVO_N = 4  # CH1..CH4 endpoints shown in the Controls tab (map to servoMin/Center/Max[0..3])


def _gp_norm_mask(v, default="0x0000"):
    try:
        return "0x%04X" % (int(str(v).strip(), 0) & 0xFFFF)
    except Exception:
        return default


def _read_gp_defines():
    out = {}
    if os.path.isfile(GP_CONFIG_PATH):
        with open(GP_CONFIG_PATH, encoding="utf-8", errors="replace") as f:
            for m in re.finditer(r"^\s*#define\s+(GP_\w+)\s+(\S+)", f.read(), re.MULTILINE):
                out[m.group(1)] = m.group(2)
    return out


def _read_servo_array(text, name):
    m = re.search(name + r"\s*\[\s*\d*\s*\]\s*=\s*\{([^}]+)\}", text)
    return [int(x) for x in re.findall(r"\d+", m.group(1))] if m else []


def read_servo_endpoints():
    with open(CONFIG_PATH, encoding="utf-8", errors="replace") as f:
        text = f.read()
    lo = _read_servo_array(text, "servoMin")
    ce = _read_servo_array(text, "servoCenter")
    hi = _read_servo_array(text, "servoMax")
    vals = {}
    for n in range(GP_SERVO_N):
        if n < len(lo): vals["CH%dL" % (n + 1)] = lo[n]
        if n < len(ce): vals["CH%dC" % (n + 1)] = ce[n]
        if n < len(hi): vals["CH%dR" % (n + 1)] = hi[n]
    return vals


def write_servo_endpoints(vals):
    with open(CONFIG_PATH, encoding="utf-8", errors="replace") as f:
        text = f.read()
    lo = _read_servo_array(text, "servoMin")
    ce = _read_servo_array(text, "servoCenter")
    hi = _read_servo_array(text, "servoMax")
    for n in range(GP_SERVO_N):
        for suffix, arr in (("L", lo), ("C", ce), ("R", hi)):
            k = "CH%d%s" % (n + 1, suffix)
            if k in vals and n < len(arr):
                arr[n] = max(500, min(2500, int(vals[k])))
    for name, arr in (("servoMin", lo), ("servoCenter", ce), ("servoMax", hi)):
        if arr:
            text = re.sub(r"(" + name + r"\s*\[\s*\d*\s*\]\s*=\s*\{)[^}]+(\})",
                          lambda m, a=arr: m.group(1) + ", ".join(str(x) for x in a) + m.group(2), text)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        f.write(text)


def read_output_reversed():
    with open(CONFIG_PATH, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r"bool\s+outputReversed\s*\[\s*\d*\s*\]\s*=\s*\{([^}]+)\}", text)
    if not m:
        return [False] * 6
    vals = [v.split("//")[0].strip() == "true" for v in m.group(1).split(",")]
    return (vals + [False] * 6)[:6]


def write_output_reversed(lst):
    with open(CONFIG_PATH, encoding="utf-8", errors="replace") as f:
        text = f.read()
    arr = ["true" if (i < len(lst) and lst[i]) else "false" for i in range(6)]
    text = re.sub(r"(bool\s+outputReversed\s*\[\s*\d*\s*\]\s*=\s*\{)[^}]+(\})",
                  lambda m: m.group(1) + ", ".join(arr) + m.group(2), text)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        f.write(text)


def read_gamepad_config():
    d = _read_gp_defines()
    cfg = read_config()
    buttons = {name: _gp_norm_mask(d.get(name, dflt), _gp_norm_mask(dflt))
               for name, _l, dflt in GP_FUNCTIONS}
    output_list = MACHINE_OUTPUTS.get(cfg.get("machineType"), GP_OUTPUTS)  # labels follow the machine
    _rc_buses = ("IBUS_COMMUNICATION", "SBUS_COMMUNICATION", "PWM_COMMUNICATION",
                 "SUMD_COMMUNICATION", "PPM_COMMUNICATION")
    _rcp = cfg.get("rcProtocol")
    _rc_bus = _rcp if _rcp in _rc_buses else "IBUS_COMMUNICATION"
    return {
        "mode": "gamepad" if cfg.get("rcProtocol") == "GAMEPAD_MODE" else "webui",
        "prevComm": _rc_bus, "rcProtocol": _rc_bus,   # the chosen RC bus (Controls tab picks it)
        "machineType": cfg.get("machineType"),
        "tankmix": int(d.get("GP_TANKMIX", "1")) != 0,
        "rumble": int(d.get("GP_RUMBLE", "0")) != 0,
        "outputs": {name: {
            "src": int(d.get("GP_%s_SRC" % name, "0")),
            "btn": _gp_norm_mask(d.get("GP_%s_BTN" % name, "0x0000"), "0x0000"),
            "min": int(d.get("GP_%s_MIN" % name, "1000")),
            "center": int(d.get("GP_%s_CENTER" % name, "1500")),
            "max": int(d.get("GP_%s_MAX" % name, "2000")),
        } for name, _l in output_list},
        "sourceChoices": GP_SOURCES, "outputList": output_list,
        "driveOutputs": MACHINE_DRIVE.get(cfg.get("machineType"), MACHINE_DRIVE["DOZER_MODE"]),
        "steerSource": 1 if int(d.get("GP_STEER_SOURCE", "0")) else 0,
        "steerInvert": int(d.get("GP_STEER_INVERT", "0")) != 0,
        "throttleInvert": int(d.get("GP_THROTTLE_INVERT", "0")) != 0,
        "steerDeadzone": int(d.get("GP_STEER_DEADZONE", "60")),
        "throttleDeadzone": int(d.get("GP_THROTTLE_DEADZONE", "80")),
        "outputReversed": read_output_reversed(),
        "buttons": buttons, "servos": read_servo_endpoints(), "servoProfile": "config.h",
        "buttonChoices": GP_BUTTON_CHOICES, "functions": GP_FUNCTIONS,
    }


def write_gamepad_config(req):
    def i(key, default):
        try:
            return int(req.get(key, default))
        except Exception:
            return default
    buttons = req.get("buttons") or {}
    outs = req.get("outputs") or {}
    lines = [
        "// AUTO-GENERATED by the Controls tab — do not edit by hand. Included by src/gamepad.h.",
        "#define GP_TANKMIX %d" % (1 if req.get("tankmix") else 0),
        "#define GP_RUMBLE %d" % (1 if req.get("rumble") else 0),
        "#define GP_STEER_SOURCE %d" % (1 if i("steerSource", 0) else 0),
        "#define GP_STEER_INVERT %d" % (1 if req.get("steerInvert") else 0),
        "#define GP_THROTTLE_INVERT %d" % (1 if req.get("throttleInvert") else 0),
        "#define GP_STEER_DEADZONE %d" % i("steerDeadzone", 60),
        "#define GP_THROTTLE_DEADZONE %d" % i("throttleDeadzone", 80),
        "",
    ]
    for name, _l, dflt in GP_FUNCTIONS:
        lines.append("#define %s %s" % (name, _gp_norm_mask(buttons.get(name, dflt), dflt)))
    lines.append("")
    for name, _l in GP_OUTPUTS:
        o = outs.get(name) or {}
        def oi(k, dv):
            try:
                return int(o.get(k, dv))
            except Exception:
                return dv
        lines += [
            "#define GP_%s_SRC %d" % (name, oi("src", 0)),
            "#define GP_%s_BTN %s" % (name, _gp_norm_mask(o.get("btn", "0x0000"), "0x0000")),
            "#define GP_%s_MIN %d" % (name, oi("min", 1000)),
            "#define GP_%s_CENTER %d" % (name, oi("center", 1500)),
            "#define GP_%s_MAX %d" % (name, oi("max", 2000)),
            "",
        ]
    with open(GP_CONFIG_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    # Mode + RC bus: the Controls tab picks gamepad OR which RC protocol (IBUS/SBUS/PWM/…).
    _rc_buses = ("IBUS_COMMUNICATION", "SBUS_COMMUNICATION", "PWM_COMMUNICATION",
                 "SUMD_COMMUNICATION", "PPM_COMMUNICATION")
    rc_bus = req.get("rcProtocol") or req.get("prevComm") or "IBUS_COMMUNICATION"
    if rc_bus not in _rc_buses:
        rc_bus = "IBUS_COMMUNICATION"
    full = read_config()
    full["rcProtocol"] = "GAMEPAD_MODE" if req.get("mode") == "gamepad" else rc_bus
    write_config(full)

    servos = {k: v for k, v in (req.get("servos") or {}).items() if re.match(r"CH\d[LCR]$", k)}
    if servos:
        write_servo_endpoints(servos)

    if isinstance(req.get("outputReversed"), list):
        write_output_reversed([bool(x) for x in req["outputReversed"]])


# ─── HTTP Handler ────────────────────────────────────────────────────────────

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silence request logs

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache")

    def _json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _html(self, html):
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, fpath, ctype):
        if not os.path.isfile(fpath):
            self.send_error(404); return
        with open(fpath, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self._cors(); self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        # ── DIYGuy SPA (CAT-yellow) ──
        if path == "/" or path == "/index.html":
            self._serve_file(os.path.join(WEB_DIR, "index.html"), "text/html; charset=utf-8")
        elif path == "/classic":
            self._html(HTML_PAGE)  # original embedded UI, kept as fallback
        elif path.startswith("/web/"):
            fn = os.path.basename(path)
            ctype = {"js": "text/javascript", "css": "text/css", "png": "image/png",
                     "html": "text/html"}.get(fn.rsplit(".", 1)[-1], "application/octet-stream")
            self._serve_file(os.path.join(WEB_DIR, fn), ctype)
        elif path == "/api/schema":
            try:
                self._json({"ok": True, "schema": spa_schema()})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
        elif path == "/gamepad_config":
            try:
                self._json({"ok": True, "config": read_gamepad_config()})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
        elif path == "/ping":
            self._json({"ok": True})
        elif path == "/native_ports":
            self._json({"ports": [{"address": p.get("port"), "likely": False}
                                  for p in list_serial_ports()]})
        elif path == "/get_volume":
            v = read_config().get("masterVolume", 100)
            self._json({"ok": True, "volume": v})
        elif path == "/all_sounds":
            self._json({"ok": True, "sounds": scan_all_sounds()})
        elif path.startswith("/sound_pcm/"):
            fn = os.path.basename(urllib.parse.unquote(path[len("/sound_pcm/"):]))
            fp = os.path.join(SOUNDS_DIR, fn)
            if os.path.isfile(fp):
                d = parse_sound_header(fp)
                self._json({"ok": True, "samples": d["samples"], "sampleRate": d["sampleRate"]})
            else:
                self._json({"ok": False, "error": "not found"}, 404)
        elif path.startswith("/sound_text/"):
            fn = os.path.basename(urllib.parse.unquote(path[len("/sound_text/"):]))
            fp = os.path.join(SOUNDS_DIR, fn)
            if os.path.isfile(fp):
                self._json({"ok": True, "text": open(fp, encoding="utf-8", errors="replace").read()})
            else:
                self._json({"ok": False, "error": "not found"}, 404)
        elif path == "/api/config":
            cfg = read_config()
            cfg["soundFiles"] = list_sound_files()
            cfg["_currentVehicle"] = _current_vehicle_file
            cfg["_vehicles"] = list_vehicles()
            self._json(cfg)
        elif path == "/api/build-log":
            self._json({"log": build_log, "running": build_running})
        elif path == "/api/all_sounds":
            self._json(scan_all_sounds())
        elif path == "/api/serial_ports":
            self._json(list_serial_ports())
        elif path == "/api/vehicles":
            self._json({"ok": True, "vehicles": list_vehicles(), "current": _current_vehicle_file})
        elif path == "/logo.png":
            logo_path = os.path.join(PROJECT_DIR, "logo.png")
            if os.path.isfile(logo_path):
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "no-cache")
                with open(logo_path, "rb") as f:
                    data = f.read()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_error(404)
        elif path.startswith("/api/sound_pcm/"):
            fn = os.path.basename(urllib.parse.unquote(path[len("/api/sound_pcm/"):]))
            fpath = os.path.join(SOUNDS_DIR, fn)
            if os.path.isfile(fpath):
                data = parse_sound_header(fpath)
                self._json({"ok": True, "file": fn, "sampleRate": data["sampleRate"],
                            "sampleCount": data["sampleCount"], "samples": data["samples"]})
            else:
                self._json({"ok": False, "error": "File not found"}, 404)
        else:
            self.send_error(404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length else "{}"

        # ── DIYGuy SPA endpoints ──
        if path == "/save":
            try:
                spa_save(json.loads(body)); self._json({"ok": True})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
            return
        if path == "/gamepad_config":
            try:
                write_gamepad_config(json.loads(body))
                self._json({"ok": True, "config": read_gamepad_config()})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
            return
        if path == "/set_volume":
            try:
                full = read_config(); full["masterVolume"] = int(json.loads(body).get("volume", 100))
                write_config(full); self._json({"ok": True})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
            return
        if path == "/quit":
            self._json({"ok": True})
            threading.Timer(0.3, lambda: os._exit(0)).start()
            return
        if path == "/install_header":
            try:
                d = json.loads(body)
                fn = os.path.basename(d.get("filename", ""))
                text = d.get("text", "")
                if not re.match(r'^[a-zA-Z0-9_]+\.h$', fn) or not text:
                    self._json({"ok": False, "error": "bad filename/content"}, 400); return
                with open(os.path.join(SOUNDS_DIR, fn), "w", encoding="utf-8") as f:
                    f.write(text)
                self._json({"ok": True, "file": fn})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)
            return
        if path == "/run":
            global build_running
            data = json.loads(body) if body.strip() else {}
            upload = (data.get("cmd") == "flash")
            port = data.get("port") or None
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self._cors(); self.end_headers()

            def w(s):
                try:
                    self.wfile.write(s.encode("utf-8", "replace")); self.wfile.flush()
                except Exception:
                    pass

            th = threading.Thread(target=run_build, args=(upload, port), daemon=True)
            th.start()
            time.sleep(0.3)  # let run_build initialise build_log / build_running
            sent = 0
            while True:
                cur = list(build_log)
                if sent > len(cur):
                    sent = 0
                if sent < len(cur):
                    for line in cur[sent:]:
                        w(line + "\n")
                    sent = len(cur)
                if not build_running and sent >= len(cur):
                    break
                time.sleep(0.15)
            rc = 1
            for l in build_log[-8:]:
                m = re.search(r'Exit code:\s*(-?\d+)', l)
                if m:
                    rc = int(m.group(1))
            w("\n--- DONE (exit %d) ---\n" % (0 if rc == 0 else 1))
            return

        if path == "/api/save":
            try:
                data = json.loads(body)
                write_config(data)
                self._json({"ok": True})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/build":
            if build_running:
                self._json({"ok": False, "error": "Build already running"})
            else:
                threading.Thread(target=run_build, args=(False,), daemon=True).start()
                self._json({"ok": True})

        elif path == "/api/upload":
            if build_running:
                self._json({"ok": False, "error": "Build already running"})
            else:
                data = json.loads(body) if body.strip() else {}
                port = data.get("port", "") or None
                threading.Thread(target=run_build, args=(True, port), daemon=True).start()
                self._json({"ok": True})

        elif path == "/api/install_sound":
            try:
                data = json.loads(body)
                fn = os.path.basename(data.get("filename", ""))
                content = data.get("content", "")
                if not fn or not fn.endswith(".h") or not content:
                    self._json({"ok": False, "error": "Invalid filename or empty content"}, 400)
                    return
                # Sanitize: only allow alphanumeric, underscore, dot
                if not re.match(r'^[a-zA-Z0-9_]+\.h$', fn):
                    self._json({"ok": False, "error": "Invalid filename characters"}, 400)
                    return
                # Auto-increment filename if it already exists
                base = fn[:-2]  # strip ".h"
                fpath = os.path.join(SOUNDS_DIR, fn)
                counter = 1
                while os.path.exists(fpath):
                    fn = base + str(counter) + ".h"
                    fpath = os.path.join(SOUNDS_DIR, fn)
                    counter += 1
                # Update variable names inside the header content to match new filename
                if counter > 1:
                    new_var = fn[:-2]  # e.g. "MySound1"
                    content = re.sub(
                        r'(const\s+(?:unsigned\s+int|signed\s+char)\s+)(\w+)(Samples|SampleRate|SampleCount)',
                        lambda m: m.group(1) + new_var + m.group(3),
                        content
                    )
                with open(fpath, "w", encoding="utf-8") as f:
                    f.write(content)
                self._json({"ok": True, "file": fn})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/delete_sound":  # SPA: delete a sound file (refuses if it's in use)
            try:
                fn = os.path.basename(json.loads(body).get("filename", ""))
                if not re.match(r'^[A-Za-z0-9_]+\.h$', fn):
                    self._json({"ok": False, "error": "Invalid filename"}, 400); return
                in_use = [k[:-5] for k, v in (read_config().get("sounds") or {}).items() if v == fn]
                if in_use:
                    self._json({"ok": False, "error": "That sound is in use (" + ", ".join(in_use)
                                + "). Pick another for that slot first."}, 409); return
                fpath = os.path.join(SOUNDS_DIR, fn)
                if not os.path.isfile(fpath):
                    self._json({"ok": False, "error": "File not found"}, 404); return
                os.remove(fpath)
                self._json({"ok": True, "file": fn})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/delete_sound":
            try:
                data = json.loads(body)
                fn = os.path.basename(data.get("filename", ""))
                if not fn or not fn.endswith(".h"):
                    self._json({"ok": False, "error": "Invalid filename"}, 400)
                    return
                if not re.match(r'^[a-zA-Z0-9_]+\.h$', fn):
                    self._json({"ok": False, "error": "Invalid filename characters"}, 400)
                    return
                fpath = os.path.join(SOUNDS_DIR, fn)
                if not os.path.isfile(fpath):
                    self._json({"ok": False, "error": "File not found"}, 404)
                    return
                os.remove(fpath)
                self._json({"ok": True, "file": fn})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/push_channels":
            try:
                data = json.loads(body)
                port = data.get("port", "")
                channels = data.get("channels", {})
                settings = data.get("settings", {})
                reversed_chs = data.get("channelReversed", {})
                enabled_chs = data.get("channelEnabled", {})
                if not port or (not channels and not settings and not reversed_chs and not enabled_chs):
                    self._json({"ok": False, "error": "Missing port or channels/settings"}, 400)
                    return
                result = push_channels_serial(port, channels, settings, reversed_chs, enabled_chs)
                self._json(result)
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/upload_soundpack":
            try:
                data = json.loads(body)
                port = data.get("port", "")
                slots = data.get("slots", {})
                if not port:
                    self._json({"ok": False, "error": "No COM port specified"}, 400)
                    return
                if not slots:
                    self._json({"ok": False, "error": "No sound slots specified"}, 400)
                    return
                if build_running:
                    self._json({"ok": False, "error": "Build already running"})
                    return
                pack = build_soundpack_binary(slots)
                threading.Thread(target=flash_soundpack, args=(port, pack), daemon=True).start()
                self._json({"ok": True, "size": len(pack)})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/vehicle/save":
            try:
                data = json.loads(body)
                name = data.get("name", "").strip()
                cfg = data.get("config", {})
                if not name:
                    self._json({"ok": False, "error": "No vehicle name"}, 400)
                    return
                fn = save_vehicle(name, cfg)
                self._json({"ok": True, "name": fn})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/vehicle/load":
            try:
                data = json.loads(body)
                name = data.get("name", "").strip()
                if not name:
                    self._json({"ok": False, "error": "No vehicle name"}, 400)
                    return
                cfg = load_vehicle(name)
                cfg["soundFiles"] = list_sound_files()
                self._json({"ok": True, "config": cfg, "name": name})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        elif path == "/api/vehicle/delete":
            try:
                data = json.loads(body)
                name = data.get("name", "").strip()
                if not name:
                    self._json({"ok": False, "error": "No vehicle name"}, 400)
                    return
                delete_vehicle(name)
                self._json({"ok": True})
            except Exception as e:
                self._json({"ok": False, "error": str(e)}, 500)

        else:
            self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(200)
        self._cors()
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


# ─── HTML ────────────────────────────────────────────────────────────────────

HTML_PAGE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RC Hydraulic Controller</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800;900&display=swap" rel="stylesheet">
<style>
:root {
  --bg: #080808;
  --surface: #0e0e0e;
  --card: #141414;
  --border: #1e1e1e;
  --border-hl: #2a2a2a;
  --text: #d8d8d8;
  --dim: #606060;
  --accent: #ffcb05;
  --accent-hover: #ffe04a;
  --accent-dim: rgba(255,203,5,0.12);
  --danger: #ef4444;
  --success: #22c55e;
  --radius: 3px;
  --font: 'Inter', system-ui, -apple-system, sans-serif;
  --sidebar-w: 230px;
  --header-h: 80px;
}
* { margin:0; padding:0; box-sizing:border-box; }
body { background:var(--bg); color:var(--text); font:12px/1.5 var(--font); height:100vh; overflow:hidden; }

/* Header Bar (Cat ET style toolbar) */
.header-bar {
  height:var(--header-h); display:flex; align-items:center;
  background:var(--surface); border-bottom:3px solid var(--accent);
  padding:0 16px; gap:16px; z-index:100;
}
.logo {
  display:flex; align-items:center; height:var(--header-h); flex-shrink:0; overflow:hidden;
}
.logo img { height:60px; width:auto; display:block; object-fit:contain; }
.machine-name {
  font-size:18px; font-weight:700; color:var(--accent);
  padding:4px 14px; background:var(--accent-dim); border-radius:var(--radius);
  max-width:300px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
}
.header-sep { width:1px; height:24px; background:var(--border-hl); }
.header-actions { margin-left:auto; display:flex; gap:4px; align-items:center; }

/* App Layout — Cat ET two-panel */
.app-layout {
  display:flex; height:calc(100vh - var(--header-h) - 2px);
}
.sidebar {
  width:var(--sidebar-w); min-width:var(--sidebar-w);
  background:var(--surface); border-right:1px solid var(--border);
  overflow-y:auto; display:flex; flex-direction:column;
}
.sidebar::-webkit-scrollbar { width:4px; }
.sidebar::-webkit-scrollbar-thumb { background:var(--border-hl); border-radius:2px; }
.main-content {
  flex:1; overflow-y:auto; background:var(--bg);
}
.main-content::-webkit-scrollbar { width:6px; }
.main-content::-webkit-scrollbar-thumb { background:var(--border-hl); border-radius:3px; }

/* Sidebar Tree (Cat ET ECM tree style) */
.tree-header {
  padding:8px 10px; font-size:9px; font-weight:800;
  text-transform:uppercase; letter-spacing:1.5px;
  color:var(--accent); background:var(--card);
  border-bottom:1px solid var(--border);
  position:sticky; top:0; z-index:1;
}
.tree-node {
  display:flex; align-items:center; gap:6px;
  padding:8px 10px 8px 14px; font-size:11px; font-weight:600;
  color:var(--dim); cursor:pointer; border-left:3px solid transparent;
  transition:all 0.1s; user-select:none;
}
.tree-node:hover { color:var(--text); background:rgba(255,255,255,0.02); }
.tree-node.active {
  color:var(--accent); background:var(--accent-dim);
  border-left-color:var(--accent);
}
.tree-node .tree-icon { font-size:13px; width:16px; text-align:center; opacity:0.6; }
.tree-node.active .tree-icon { opacity:1; }
.tree-sep { height:1px; background:var(--border); margin:2px 0; }
.tree-status {
  margin-top:auto; padding:8px 10px; border-top:1px solid var(--border);
  font-size:9px; color:var(--dim); text-transform:uppercase; letter-spacing:0.5px;
}

/* Buttons */
.btn {
  display:inline-flex; align-items:center; gap:4px;
  padding:6px 14px; border-radius:var(--radius);
  font-size:11px; font-weight:700; font-family:var(--font);
  text-transform:uppercase; letter-spacing:0.5px;
  cursor:pointer; border:1px solid transparent; transition:all 0.15s; white-space:nowrap;
}
.btn-primary { background:var(--accent); color:#000; border-color:var(--accent); }
.btn-primary:hover { background:var(--accent-hover); box-shadow:0 2px 8px rgba(255,203,5,0.3); }
.btn-ghost { background:transparent; color:var(--dim); border-color:var(--border); }
.btn-ghost:hover { color:var(--text); border-color:var(--dim); }
.btn-danger { background:var(--danger); color:#fff; border-color:var(--danger); }
.btn-sm { padding:4px 10px; font-size:10px; }

.status-dot { width:10px; height:10px; border-radius:50%; display:inline-block; margin-right:6px; }
.status-dot.ok { background:var(--success); }
.status-dot.busy { background:var(--accent); animation:pulse 1s infinite; }
@keyframes pulse { 50% { opacity:0.4; } }

/* Panels */
.panel { display:none; padding:16px 20px; }
.panel.active { display:block; }

/* Section Cards */
.section-card {
  background:var(--surface); border:1px solid var(--border);
  border-radius:var(--radius); margin-bottom:12px; overflow:hidden;
}
.section-card > summary {
  list-style:none; display:flex; align-items:center; gap:10px;
  padding:12px 16px; cursor:pointer; font-weight:700; font-size:13px;
  text-transform:uppercase; letter-spacing:0.5px; color:var(--dim);
  background:var(--surface); border-bottom:1px solid transparent;
  transition:all 0.15s; user-select:none;
}
.section-card > summary:hover { color:var(--text); background:#252525; }
.section-card[open] > summary {
  color:var(--accent); background:var(--surface); border-bottom:2px solid var(--accent);
}
.section-card > summary::before { content:'\25B8'; width:12px; color:var(--dim); font-size:11px; transition:transform 0.15s; }
.section-card[open] > summary::before { content:'\25BE'; }
.section-body { padding:16px; }

/* Section Title — Cat ET style section header bar */
.section-title {
  font-size:10px; font-weight:700; color:var(--accent);
  margin:16px -20px 10px; padding:6px 20px;
  background:var(--accent-dim); border-left:3px solid var(--accent);
  text-transform:uppercase; letter-spacing:0.8px;
}
.section-title:first-child { margin-top:-16px; }

/* Fields */
.field { display:flex; align-items:center; gap:12px; margin:8px 0; flex-wrap:wrap; }
.field label { min-width:220px; font-size:11px; font-weight:700; text-transform:uppercase; letter-spacing:0.3px; color:var(--text); }
.field input[type=number], .field select {
  background:var(--surface); border:1px solid var(--border); color:var(--text);
  padding:6px 10px; border-radius:6px; font-size:13px; font-family:var(--font); width:120px;
}
.field input[type=number]:focus, .field select:focus {
  outline:none; border-color:var(--accent); box-shadow:0 0 0 2px rgba(255,203,5,0.15);
}
.field input[type=range] { flex:1; max-width:200px; accent-color:var(--accent); }
.field input[type=range]::-webkit-slider-thumb {
  -webkit-appearance:none; width:14px; height:14px; border-radius:50%;
  background:var(--accent); border:2px solid #b8920a; cursor:pointer;
  box-shadow:0 1px 3px rgba(0,0,0,0.5);
}
.field input[type=range]:hover::-webkit-slider-thumb { background:var(--accent-hover); }
.field .range-val { min-width:40px; text-align:right; font-size:12px; color:var(--accent); font-weight:700; }
.field select { width:280px; }

/* Hint text */
.hint { color:var(--dim); font-size:11px; }

/* Radio Group */
.radio-group { display:flex; gap:8px; flex-wrap:wrap; }
.radio-group label {
  min-width:auto; display:flex; align-items:center; gap:4px;
  padding:6px 14px; background:var(--surface); border:1px solid var(--border);
  border-radius:6px; cursor:pointer; font-size:11px; font-weight:700;
  text-transform:uppercase; letter-spacing:0.3px; transition:all 0.15s;
}
.radio-group label:has(input:checked) {
  border-color:var(--accent); background:rgba(255,203,5,0.1); color:var(--accent);
}
.radio-group input { display:none; }

/* Checkbox / Toggle */
.check-group { display:flex; gap:16px; flex-wrap:wrap; margin:8px 0; }
.check-group label { display:flex; align-items:center; gap:6px; font-size:11px; font-weight:600; cursor:pointer; text-transform:uppercase; }
.sw { position:relative; display:inline-block; width:40px; height:22px; }
.sw input { opacity:0; width:0; height:0; }
.sl {
  position:absolute; inset:0; background:#444; border-radius:22px; transition:0.2s; cursor:pointer;
}
.sl::before {
  content:''; position:absolute; width:16px; height:16px; left:3px; bottom:3px;
  background:#888; border-radius:50%; transition:0.2s;
}
input:checked + .sl { background:var(--accent); }
input:checked + .sl::before { transform:translateX(18px); background:#000; }

/* Servo Grid */
.servo-grid {
  display:grid; grid-template-columns:80px repeat(5,1fr);
  gap:4px 8px; font-size:11px; margin:8px 0;
}
.servo-grid .hdr { color:var(--accent); font-weight:700; text-align:center; text-transform:uppercase; letter-spacing:0.3px; font-size:10px; }
.servo-grid .lbl { color:var(--dim); font-weight:600; text-transform:uppercase; }
.servo-grid input {
  background:var(--surface); border:1px solid var(--border); color:var(--text);
  padding:4px; border-radius:4px; width:100%; text-align:center; font-size:12px; font-family:var(--font);
}
.servo-grid input:focus { outline:none; border-color:var(--accent); box-shadow:0 0 0 2px rgba(255,203,5,0.15); }

/* Build Output */
#buildOutput {
  background:#050505; border:1px solid var(--border); border-radius:var(--radius);
  padding:10px 12px; font-family:'Cascadia Code','Consolas',monospace;
  font-size:11px; line-height:1.4; max-height:calc(100vh - 180px); overflow-y:auto;
  white-space:pre-wrap; color:#8f8;
}

/* Toast */
.toast {
  position:fixed; bottom:20px; right:20px; padding:12px 20px;
  border-radius:var(--radius); font-weight:700; font-size:12px; z-index:999;
  transition:opacity 0.3s; text-transform:uppercase; letter-spacing:0.5px;
}
.toast.ok { background:var(--success); color:#fff; }
.toast.err { background:var(--danger); color:#fff; }
</style>
</head>
<body>

<div class="header-bar">
  <div class="logo"><img src="/logo.png?v=${Date.now()}" alt="Caterpillar Electro-Hydraulic Controller"></div>
  <div class="header-sep"></div>
  <div id="machineName" class="machine-name"></div>
  <div class="header-actions">
    <button class="btn btn-primary btn-sm" onclick="saveConfig()">&#128190; Save</button>
    <button class="btn btn-sm" onclick="saveAsNewVehicle()" title="Save as new vehicle profile">Save As</button>
    <button class="btn btn-sm" onclick="exportVehicle()" title="Export config as .json">&#128229;</button>
    <button class="btn btn-primary btn-sm" onclick="startBuild(false)">&#9881; Build</button>
    <button class="btn btn-primary btn-sm" onclick="startBuild(true)">&#9889; Flash</button>
  </div>
</div>

<div class="app-layout">
  <div class="sidebar" id="sidebar"></div>
  <div class="main-content" id="panels"></div>
</div>

<script>
const TABS = [
  { id:'machine',   label:'Machine',          icon:'\u2699' },
  { id:'sounds',    label:'Sounds',            icon:'\u266B' },
  { id:'rc',        label:'RC Input',          icon:'\u25CE' },
  { id:'esc',       label:'ESC / Drive',       icon:'\u26A1' },
  { id:'servos',    label:'Channels',          icon:'\u21C4' },
  { id:'soundtech', label:'Sound Technician',  icon:'\u2692' },
  { id:'build',     label:'Diagnostics',       icon:'\u25B6' },
];

// Machine-specific channel labels
const CHANNEL_LABELS = {
  'EXCAVATOR_MODE': ['Bucket', 'Swing', 'Throttle', 'Horn', 'Boom', 'Stick', 'Track L', 'Track R'],
  'LOADER_MODE':    ['Bucket', 'Boom', 'Throttle', 'Horn', 'Eng On/Off', 'Aux', 'Steer', 'Drive'],
  'CRANE_MODE':     ['Boom Lift', 'Boom Ext', 'Throttle', 'Horn', 'Eng On/Off', 'Rope', 'Aux', 'Swing'],
  'DOZER_MODE':     ['Blade', 'Ripper', 'Throttle', 'Horn', 'Eng On/Off', 'Aux', 'Tilt', 'Track'],
  'SKIDSTEER_MODE': ['Bucket', 'Boom', 'Throttle', 'Horn', 'Eng On/Off', 'Hi/Lo', 'Drive L', 'Drive R'],
  'GRADER_MODE':    ['Blade Lift', 'Circle', 'Throttle', 'Horn', 'Eng On/Off', 'Hi/Lo', 'Tilt', 'Articulation'],
};

// Channel mapping: which config variable maps to which function per machine type
// { varName: 'CH_XX_YYY', label: 'Function Name' }
const CH_MAP_COMMON = [
  { v: 'CH_THROTTLE',      label: 'Throttle' },
  { v: 'CH_HORN',          label: 'Horn' },
  { v: 'CH_ENGINE_TOGGLE', label: 'Engine On/Off' },
  { v: 'CH_HILO_TOGGLE',   label: 'Hi/Lo Toggle' },
  { v: 'CH_LIGHTS',        label: 'Lights' },
];
const CH_MAP_MACHINE = {
  'EXCAVATOR_MODE': [
    { v: 'CH_EX_TRACK_R', label: 'Track R' },
    { v: 'CH_EX_TRACK_L', label: 'Track L' },
    { v: 'CH_EX_BOOM',    label: 'Boom' },
    { v: 'CH_EX_STICK',   label: 'Arm' },
    { v: 'CH_EX_BUCKET',  label: 'Bucket' },
    { v: 'CH_EX_SWING',   label: 'Swing' },
  ],
  'LOADER_MODE': [
    { v: 'CH_LD_BUCKET', label: 'Bucket' },
    { v: 'CH_LD_BOOM',   label: 'Boom' },
  ],
  'CRANE_MODE': [
    { v: 'CH_CR_BOOM',   label: 'Boom Lift' },
    { v: 'CH_CR_EXTEND', label: 'Boom Extend' },
    { v: 'CH_CR_SWING',  label: 'Swing' },
  ],
  'DOZER_MODE': [
    { v: 'CH_DZ_TRACK_R',      label: 'Track R' },
    { v: 'CH_DZ_TRACK_L',      label: 'Track L' },
    { v: 'CH_DZ_BLADE',        label: 'Blade Lift' },
    { v: 'CH_DZ_RIPPER',       label: 'Ripper' },
    { v: 'CH_DZ_TILT',         label: 'Blade Tilt' },
    { v: 'CH_DZ_ANGLE',        label: 'Blade Angle' },
    { v: 'CH_DZ_RIPPER_TILT',  label: 'Ripper Tilt' },
  ],
  'SKIDSTEER_MODE': [
    { v: 'CH_SS_BUCKET', label: 'Bucket' },
    { v: 'CH_SS_BOOM',   label: 'Boom' },
  ],
  'GRADER_MODE': [
    { v: 'CH_GR_BLADE',        label: 'Blade Lift' },
    { v: 'CH_GR_CIRCLE',       label: 'Circle Rotation' },
    { v: 'CH_GR_TILT',         label: 'Blade Tilt' },
    { v: 'CH_GR_ARTICULATION', label: 'Articulation' },
  ],
};

const MACHINE_NAMES = {
  'EXCAVATOR_MODE': 'Excavator',
  'LOADER_MODE': 'Wheel Loader',
  'CRANE_MODE': 'Crane',
  'DOZER_MODE': 'Dozer',
  'SKIDSTEER_MODE': 'Skid Steer',
  'GRADER_MODE': 'Motor Grader',
};

let CFG = {};
let soundFiles = [];

async function init() {
  try {
  const resp = await fetch('/api/config');
  const data = await resp.json();
  soundFiles = data.soundFiles || [];
  delete data.soundFiles;
  CFG = data;
  _vehicleList = data._vehicles || [];
  _currentVehicle = data._currentVehicle || null;
  delete CFG._vehicles;
  delete CFG._currentVehicle;
  renderTabs();
  renderPanels();
  updateMachineName();
  activateTab('machine');
  } catch(e) { document.body.innerHTML = '<pre style="color:red;padding:20px;">INIT ERROR: ' + e.message + '\n\n' + e.stack + '</pre>'; }
}

function updateMachineName() {
  const el = document.getElementById('machineName');
  if (el) el.textContent = CFG.customMachineName || MACHINE_NAMES[CFG.machineType] || 'Unknown';
}

function renderTabs() {
  const sb = document.getElementById('sidebar');
  sb.innerHTML =
    '<div class="tree-header">\u25BC Available Modules</div>' +
    TABS.map(t =>
      `<div class="tree-node" data-tab="${t.id}" onclick="activateTab('${t.id}')">` +
      `<span class="tree-icon">${t.icon}</span>${t.label}</div>`
    ).join('') +
    '<div class="tree-status" id="treeStatus">Ready</div>';
}

function activateTab(id) {
  document.querySelectorAll('.tree-node').forEach(n => n.classList.toggle('active', n.dataset.tab === id));
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === 'p-'+id));
  if (id === 'build') { pollBuild(); refreshFlashPorts(); }
  if (id === 'soundtech') { stLoadBrowser(); if (typeof refreshSndpackPorts === 'function') refreshSndpackPorts(); }
  if (id === 'servos' && typeof refreshPorts === 'function') refreshPorts();
  const st = document.getElementById('treeStatus');
  if (st) { const t = TABS.find(t=>t.id===id); st.textContent = t ? t.label : id; }
}

function soundSelect(slot, current) {
  const opts = soundFiles.map(f =>
    `<option value="${f}" ${f===current?'selected':''}>${f.replace('.h','')}</option>`
  ).join('');
  return `<select data-sound="${slot}">${opts}</select>
    <button class="btn btn-ghost btn-sm" onclick="stPreviewCurrent('${slot}')" title="Preview sound" style="margin-left:4px;">&#9654;</button>
    <button class="btn btn-ghost btn-sm" onclick="sbStop()" title="Stop" style="margin-left:2px;">&#9632;</button>`;
}

function numField(label, key, min, max, step) {
  const v = CFG[key] ?? 0; step = step || 1;
  return `<div class="field">
    <label>${label}</label>
    <input type="range" min="${min}" max="${max}" step="${step}" value="${v}"
      oninput="CFG['${key}']=+this.value; this.nextElementSibling.textContent=this.value">
    <span class="range-val">${v}</span>
  </div>`;
}

function numInput(label, key, min, max) {
  const v = CFG[key] ?? 0;
  return `<div class="field">
    <label>${label}</label>
    <input type="number" min="${min}" max="${max}" value="${v}"
      onchange="CFG['${key}']=+this.value">
  </div>`;
}

function radioGroup(key, options, onChange) {
  const extra = onChange ? ` ${onChange}` : '';
  return `<div class="radio-group">${options.map(o =>
    `<label><input type="radio" name="${key}" value="${o.value}" ${CFG[key]===o.value?'checked':''}
      onchange="CFG['${key}']=this.value;${extra}"> ${o.label}</label>`
  ).join('')}</div>`;
}

function checkbox(label, key) {
  const v = CFG[key];
  return `<label class="sw"><input type="checkbox" ${v?'checked':''}
    onchange="CFG['${key}']=this.checked"><span class="sl"></span></label> ${label}`;
}

function getChannelLabels() {
  // Build labels for CH1-CH16 from actual CH_* config values
  const labels = [];
  for (let i = 0; i < 16; i++) labels.push('');
  const all = [...CH_MAP_COMMON, ...(CH_MAP_MACHINE[CFG.machineType] || [])];
  for (const m of all) {
    const ch = CFG[m.v];
    if (ch >= 1 && ch <= 16) labels[ch - 1] = m.label;
  }
  return labels;
}

function liveSlider(key, label, min, max, unit) {
  const val = CFG[key] || min;
  return `<div style="display:flex;align-items:center;gap:6px;margin-bottom:4px;">
    <span style="min-width:100px;font-size:11px;color:var(--dim);">${label}</span>
    <input type="range" min="${min}" max="${max}" value="${val}" style="flex:1;accent-color:var(--accent);"
      oninput="CFG['${key}']=+this.value;this.nextElementSibling.textContent=this.value+'${unit}'">
    <span style="min-width:45px;font-size:11px;color:var(--text);text-align:right;">${val}${unit}</span>
  </div>`;
}

function liveToggle(key, label, hint) {
  const v = (CFG[key] === 'true' || CFG[key] === true);
  return `<div style="display:flex;align-items:center;gap:8px;margin:6px 0;">
    <label class="sw"><input type="checkbox" ${v?'checked':''}
      onchange="CFG['${key}']=this.checked?'true':'false'"><span class="sl"></span></label>
    <span style="font-size:11px;color:var(--dim);">${label}${hint ? ' — <em>'+hint+'</em>' : ''}</span>
  </div>`;
}

function renderPanels() {
  document.getElementById('panels').innerHTML = `
    ${panelMachine()}
    ${panelSounds()}
    ${panelRC()}
    ${panelESC()}
    ${panelServos()}
    ${panelSoundTech()}
    ${panelBuild()}
  `;
}

function onMachineChange() {
  updateMachineName();
  // Re-render the entire Channels panel (labels + mapping change per machine)
  const servoPanel = document.getElementById('p-servos');
  if (servoPanel) {
    const wasActive = servoPanel.classList.contains('active');
    servoPanel.outerHTML = panelServos();
    if (wasActive) document.getElementById('p-servos').classList.add('active');
  }
}

function panelMachine() {
  return `<div class="panel" id="p-machine">
    <div class="section-title">Machine Type</div>
    ${radioGroup('machineType', [
      {value:'EXCAVATOR_MODE', label:'Excavator'},
      {value:'LOADER_MODE', label:'Wheel Loader'},
      {value:'CRANE_MODE', label:'Crane'},
      {value:'DOZER_MODE', label:'Dozer'},
      {value:'SKIDSTEER_MODE', label:'Skid Steer'},
      {value:'GRADER_MODE', label:'Motor Grader'},
    ], 'onMachineChange()')}
    <div class="section-title">Custom Machine Name</div>
    <div class="field">
      <label>Display Name</label>
      <input type="text" id="customMachineName" value="${CFG.customMachineName||''}"
        style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:13px;font-family:var(--font);width:280px;"
        oninput="CFG.customMachineName=this.value;updateMachineName()"
        placeholder="${MACHINE_NAMES[CFG.machineType]||''}">
    </div>
    <p class="hint">Override the display name shown in the top bar (leave blank for default)</p>

    <div class="section-title">Vehicle Profiles</div>
    <p class="hint" style="margin-bottom:6px;">Current: <strong id="currentVehicleName">${_currentVehicle || '(unsaved)'}</strong></p>
    <div style="display:flex;gap:4px;margin-bottom:10px;flex-wrap:wrap;">
      <button class="btn btn-primary btn-sm" onclick="saveAsNewVehicle()">&#128190; Save As New</button>
      <button class="btn btn-sm" onclick="exportVehicle()">&#128229; Export .json</button>
      <label class="btn btn-sm" style="cursor:pointer;">&#128228; Import .json
        <input type="file" accept=".json" onchange="importVehicle(this)" style="display:none;">
      </label>
    </div>
    <table style="width:100%;border-collapse:collapse;background:var(--surface);border:1px solid var(--border);border-radius:6px;overflow:hidden;">
      <tbody id="vehicleListBody">${_vehicleListRows()}</tbody>
    </table>
  </div>`;
}

function panelSounds() {
  const s = CFG.sounds || {};
  return `<div class="panel" id="p-sounds">

    <div class="section-title">Engine Start</div>
    <div class="field"><label>Sound File</label>${soundSelect('startSound', s.startSound)}</div>
    ${numField('Start Volume %', 'startVolumePercentage', 0, 500, 5)}

    <div class="section-title">Idle</div>
    <div class="field"><label>Sound File</label>${soundSelect('idleSound', s.idleSound)}</div>
    ${numField('Idle Volume %', 'idleVolumePercentage', 0, 500, 5)}
    ${numField('Engine Idle Volume %', 'engineIdleVolumePercentage', 0, 300, 5)}
    ${numField('Full Throttle Volume %', 'fullThrottleVolumePercentage', 0, 500, 5)}

    <div class="section-title">Rev Sound</div>
    <div class="check-group">${checkbox('Enable Rev Sound', 'revSoundEnabled')}</div>
    <div class="field"><label>Sound File</label>${soundSelect('revSound', s.revSound)}</div>
    ${numField('Rev Volume %', 'revVolumePercentage', 0, 500, 5)}
    ${numField('Engine Rev Volume %', 'engineRevVolumePercentage', 0, 300, 5)}
    ${numInput('Rev Switch Point', 'revSwitchPoint', 0, 500)}
    ${numInput('Idle End Point', 'idleEndPoint', 0, 500)}
    ${numField('Idle Volume Proportion %', 'idleVolumeProportionPercentage', 0, 100, 5)}

    <div class="section-title">Diesel Knock</div>
    <div class="field"><label>Sound File</label>${soundSelect('knockSound', s.knockSound)}</div>
    ${numField('Knock Volume %', 'dieselKnockVolumePercentage', 0, 1000, 10)}
    ${numField('Knock Idle Volume %', 'dieselKnockIdleVolumePercentage', 0, 100, 1)}
    ${numInput('Knock Interval (cylinders)', 'dieselKnockInterval', 1, 12)}
    ${numInput('Knock Start Point', 'dieselKnockStartPoint', 0, 500)}
    ${numField('Knock Adaptive Volume %', 'dieselKnockAdaptiveVolumePercentage', 0, 100, 5)}

    <div class="section-title">Turbo</div>
    <div class="field"><label>Sound File</label>${soundSelect('turboSound', s.turboSound)}</div>
    ${numField('Turbo Volume %', 'turboVolumePercentage', 0, 200, 5)}
    ${numField('Turbo Idle Volume %', 'turboIdleVolumePercentage', 0, 100, 5)}

    <div class="section-title">Supercharger</div>
    <div class="field"><label>Sound File</label>${soundSelect('chargerSound', s.chargerSound)}</div>
    ${numField('Charger Volume % (0=off)', 'chargerVolumePercentage', 0, 200, 5)}
    ${numField('Charger Idle Volume %', 'chargerIdleVolumePercentage', 0, 100, 5)}
    ${numInput('Charger Start Point', 'chargerStartPoint', 0, 500)}

    <div class="section-title">Wastegate</div>
    <div class="field"><label>Sound File</label>${soundSelect('wastegateSound', s.wastegateSound)}</div>
    ${numField('Wastegate Volume %', 'wastegateVolumePercentage', 0, 200, 5)}
    ${numField('Wastegate Idle Volume %', 'wastegateIdleVolumePercentage', 0, 100, 5)}

    <div class="section-title">Fan</div>
    <div class="field"><label>Sound File</label>${soundSelect('fanSound', s.fanSound)}</div>
    ${numField('Fan Volume % (0=off)', 'fanVolumePercentage', 0, 200, 5)}
    ${numField('Fan Idle Volume %', 'fanIdleVolumePercentage', 0, 100, 5)}
    ${numInput('Fan Start Point', 'fanStartPoint', 0, 500)}

    <div class="section-title">Engine Tuning</div>
    ${numField('Max RPM % (200=diesel, 400=gas)', 'MAX_RPM_PERCENTAGE', 100, 500, 10)}
    ${numField('Acceleration (1=slow, 9=fast)', 'acc', 1, 9, 1)}
    ${numField('Deceleration (1=slow, 9=fast)', 'dec', 1, 9, 1)}
    ${numInput('Clutch Engaging Point', 'clutchEngagingPoint', 0, 500)}

    <div class="section-title" style="margin-top:24px;">Hydraulic Pump</div>
    <div class="field"><label>Sound File</label>${soundSelect('hydraulicPumpSound', s.hydraulicPumpSound)}</div>
    ${numField('Pump Volume %', 'hydraulicPumpVolumePercentage', 0, 500, 5)}

    <div class="section-title">Hydraulic Flow</div>
    <div class="field"><label>Sound File</label>${soundSelect('hydraulicFlowSound', s.hydraulicFlowSound)}</div>
    ${numField('Flow Volume %', 'hydraulicFlowVolumePercentage', 0, 500, 5)}

    <div class="section-title">Track Rattle</div>
    <div class="field"><label>Sound File</label>${soundSelect('trackRattleSound', s.trackRattleSound)}</div>
    ${numField('Track Rattle Volume %', 'trackRattleVolumePercentage', 0, 500, 5)}
    ${numInput('Rattle Interval Min (ms at max speed)', 'trackRattleIntervalMin', 10, 2000)}
    ${numInput('Rattle Interval Max (ms at min speed)', 'trackRattleIntervalMax', 50, 5000)}
    ${numInput('Chain Drive Top Speed PWM', 'pwmStrokeChainDriveTopSpeed', 1, 255)}
    ${numInput('Chain Drive Start Rotation', 'pwmStrokeChainDriveStartRotation', 0, 255)}

    <div class="section-title">Track Rattle 2</div>
    <div class="field"><label>Sound File</label>${soundSelect('trackRattle2Sound', s.trackRattle2Sound)}</div>
    ${numField('Track Rattle 2 Volume %', 'trackRattle2VolumePercentage', 0, 500, 5)}

    <div class="section-title">Bucket Rattle</div>
    <div class="field"><label>Sound File</label>${soundSelect('bucketRattleSound', s.bucketRattleSound)}</div>
    ${numField('Bucket Rattle Volume %', 'bucketRattleVolumePercentage', 0, 500, 5)}

    <div class="section-title">Hydraulic Response</div>
    ${numInput('Ramp Time (ms)', 'hydraulicRampTime', 50, 1000)}
    ${numInput('Dead Zone (us)', 'hydraulicDeadZone', 0, 200)}

    <div class="section-title" style="margin-top:24px;">Horn</div>
    <div class="field"><label>Sound File</label>${soundSelect('hornSound', s.hornSound)}</div>
    ${numField('Horn Volume %', 'hornVolumePercentage', 0, 500, 5)}

    <div class="section-title">Air Brake</div>
    <div class="field"><label>Sound File</label>${soundSelect('brakeSound', s.brakeSound)}</div>
    ${numField('Brake Volume %', 'brakeVolumePercentage', 0, 500, 5)}

    <div class="section-title">Travel Alarm (Reversing Beep)</div>
    <div class="field"><label>Sound File</label>${soundSelect('reversingSound', s.reversingSound)}</div>
    ${numField('Alarm Volume %', 'reversingVolumePercentage', 0, 300, 5)}
    <div class="check-group">${checkbox('Alarm in both directions (fwd + rev)', 'travelAlarmBothDirections')}</div>
    <p class="hint">When enabled, the travel alarm beeps whenever the machine is moving in any direction. When disabled, it only beeps in reverse.</p>
  </div>`;
}

function panelRC() {
  return `<div class="panel" id="p-rc">
    <div class="section-title">Input Source</div>
    ${radioGroup('rcProtocol', [
      {value:'SBUS_COMMUNICATION', label:'SBUS'},
      {value:'IBUS_COMMUNICATION', label:'IBUS'},
      {value:'SUMD_COMMUNICATION', label:'SUMD'},
      {value:'PPM_COMMUNICATION', label:'PPM'},
      {value:'PWM_COMMUNICATION', label:'PWM'},
      {value:'GAMEPAD_MODE', label:'🎮 Gamepad (PS4/PS5/Xbox)'},
    ])}
    <p class="hint" style="color:var(--dim);font-size:12px;margin:6px 0 0">
      Gamepad drives over Bluetooth (Bluepad32) — the flasher builds it on the Bluepad32 core
      automatically. One radio: pick Gamepad <b>or</b> an RC bus, not both.</p>
    <div class="section-title" style="margin-top:22px">Dozer Drive</div>
    ${radioGroup('driveMode', [
      {value:'DRIVE_SINGLE_STICK_MIX', label:'Single joystick (mixed to both tracks)'},
      {value:'DRIVE_DUAL_STICK', label:'Dual stick (one per track)'},
    ])}
    <div class="section-title">SBUS Settings</div>
    <div class="field">
      <label>Signal Inverted</label>
      <select onchange="CFG.sbusInverted=this.value">
        <option value="true" ${CFG.sbusInverted==='true'?'selected':''}>true (standard)</option>
        <option value="false" ${CFG.sbusInverted==='false'?'selected':''}>false</option>
      </select>
    </div>
    ${numInput('SBUS Baud Rate', 'sbusBaud', 90000, 200000)}
    <div class="section-title">Signal Range</div>
    ${numInput('Neutral Dead Zone (us)', 'pulseNeutral', 10, 100)}
    ${numInput('Pulse Span (us)', 'pulseSpan', 200, 500)}
  </div>`;
}

function panelESC() {
  return `<div class="panel" id="p-esc">
    <div class="section-title">Hi/Lo Range (2-Speed / Rabbit Mode)</div>
    <div class="check-group">
      ${checkbox('Enable Hi/Lo Range', 'hiLoEnabled')}
    </div>
    ${numField('Low Range Speed % (of full)', 'hiLoRatioPercent', 10, 100, 5)}
    <p class="hint">CH6 toggle switches between High and Low range. Low range limits top speed to the percentage above. Great for fine jobsite maneuvering.</p>
    <div class="section-title">ESC Response</div>
    ${numField('Acceleration Steps', 'escAccelerationSteps', 1, 20, 1)}
  </div>`;
}

function panelServos() {
  const min = CFG.servoMin || [1000,1000,1000,1000,1000];
  const max = CFG.servoMax || [2000,2000,2000,2000,2000];
  const ctr = CFG.servoCenter || [1500,1500,1500,1500,1500];
  const labels = getChannelLabels();

  // Build all available functions for this machine mode (machine-specific first, then common)
  const allFuncs = [...(CH_MAP_MACHINE[CFG.machineType] || []), ...CH_MAP_COMMON];

  function chSelect(varName, label) {
    const val = CFG[varName] || 0;
    const opts = Array.from({length:17}, (_,n) =>
      '<option value="' + n + '"' + (n===val?' selected':'') + '>' + (n===0 ? 'OFF' : 'CH'+n) + '</option>'
    ).join('');
    return '<div style="display:flex;align-items:center;gap:8px;margin-bottom:4px;">' +
      '<span style="min-width:130px;font-size:12px;color:var(--dim);">' + label + '</span>' +
      '<select style="background:var(--surface);border:1px solid var(--border);color:var(--text);' +
        'padding:4px 8px;border-radius:6px;font-size:12px;font-family:var(--font);width:80px;"' +
        ' onchange="CFG[\'' + varName + '\']=+this.value;onMachineChange()">' +
        opts +
      '</select>' +
    '</div>';
  }

  return `<div class="panel" id="p-servos">
    <div class="section-title">Channel Mapping &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">${MACHINE_NAMES[CFG.machineType] || ''} mode</span></div>
    <p class="hint" style="margin-bottom:10px;">Assign each function to a receiver channel. <strong>OFF</strong> = disabled. Use <strong>Push to ESP32</strong> to apply — no rebuild needed!</p>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:4px 32px;margin-bottom:16px;">
      ${allFuncs.map(m => chSelect(m.v, m.label)).join('')}
    </div>

    <div class="section-title">Channel Reverse</div>
    <p class="hint" style="margin-bottom:8px;">Flip the direction of individual channels. Useful when a servo or stick moves the wrong way.</p>
    <div style="display:grid;grid-template-columns:repeat(4, 1fr);gap:4px 16px;margin-bottom:16px;">
      ${Array.from({length:16},(_,i)=>i+1).map(ch => {
        const rev = CFG.channelReversed && CFG.channelReversed[ch];
        return '<div style="display:flex;align-items:center;gap:6px;padding:3px 0;">' +
          '<label class="sw"><input type="checkbox" ' + (rev ? 'checked' : '') +
          ' onchange="if(!CFG.channelReversed)CFG.channelReversed={};CFG.channelReversed[' + ch + ']=this.checked"><span class="sl"></span></label>' +
          '<span style="font-size:11px;color:var(--dim);">CH' + ch + ' ' + (labels[ch-1]||'') + '</span></div>';
      }).join('')}
    </div>

    <div class="section-title">&#9889; Push to ESP32 &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">No rebuild needed!</span></div>
    <p class="hint" style="margin-bottom:8px;">Send channel mapping + live settings directly to the connected ESP32 over serial. Saved to flash — survives reboots.</p>
    <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap;">
      <select id="serialPort" style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:12px;font-family:var(--font);min-width:160px;">
        <option value="">Select COM port...</option>
      </select>
      <button class="btn btn-ghost btn-sm" onclick="refreshPorts()">&#8635; Refresh</button>
      <button class="btn btn-primary btn-sm" onclick="pushChannels()" style="font-weight:700;">&#9889; Push All to ESP32</button>
      <span id="pushStatus" style="font-size:11px;color:var(--dim);"></span>
    </div>
    <pre id="pushLog" style="display:none;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:8px 12px;font-size:11px;color:var(--text);max-height:200px;overflow-y:auto;margin-top:6px;white-space:pre-wrap;"></pre>

    <div class="section-title">Servo Endpoints (&micro;s)</div>
    <div class="servo-grid">
      <div class="hdr"></div>${[0,1,2,3,4].map(i=>`<div class="hdr ch-hdr">${labels[i] || 'CH'+(i+1)}</div>`).join('')}
      <div class="lbl">Min</div>${min.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoMin[${i}]=+this.value">`).join('')}
      <div class="lbl">Center</div>${ctr.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoCenter[${i}]=+this.value">`).join('')}
      <div class="lbl">Max</div>${max.map((v,i)=>`<input type="number" value="${v}" min="500" max="2500" onchange="CFG.servoMax[${i}]=+this.value">`).join('')}
    </div>
  </div>`;
}

function panelBuild() {
  return `<div class="panel" id="p-build">
    <div class="section-title">&#9881; Build &amp; Flash</div>
    <div style="display:flex; gap:8px; margin-bottom:8px; align-items:center; flex-wrap:wrap;">
      <select id="flashPort" style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:6px;font-size:12px;font-family:var(--font);min-width:180px;">
        <option value="">Auto-detect port...</option>
      </select>
      <button class="btn btn-ghost btn-sm" onclick="refreshFlashPorts()">&#8635; Refresh</button>
    </div>
    <div style="display:flex; gap:8px; margin-bottom:12px; align-items:center">
      <button class="btn btn-primary btn-sm" onclick="startBuild(false)">&#9881; Build</button>
      <button class="btn btn-primary btn-sm" onclick="startBuild(true)">&#9889; Build &amp; Flash</button>
      <span id="buildStatus"></span>
    </div>
    <div id="buildOutput">Click Build to compile firmware...</div>

    <div class="section-title" style="margin-top:20px;">&#128296; Debug Output</div>
    <p class="hint" style="margin-bottom:8px;">Enable serial debug streams for troubleshooting.</p>
    <div class="check-group">
      ${checkbox('RC Channels', 'debugRc')}
      ${checkbox('ESC State', 'debugEsc')}
      ${checkbox('Sound Stats', 'debugSound')}
      ${checkbox('Hydraulic', 'debugHydraulic')}
    </div>

    <div class="section-title" style="margin-top:20px;">&#128269; Config Dump</div>
    <p class="hint" style="margin-bottom:8px;">Current in-memory configuration (useful for debugging). Click to refresh.</p>
    <button class="btn btn-ghost btn-sm" onclick="document.getElementById('cfgDump').textContent=JSON.stringify(CFG,null,2)" style="margin-bottom:6px;">&#8635; Refresh Config Dump</button>
    <pre id="cfgDump" style="background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:8px 12px;font-size:11px;color:var(--text);max-height:400px;overflow-y:auto;white-space:pre-wrap;">(click Refresh to load)</pre>
  </div>`;
}

// ── Live Sound Builder ──────────────────────────
let _stAllSounds = [];
let _stFilterCat = 'all';
let _sbAudioCtx = null;
let _sbSource = null;
let _sbGain = null;
let _sbBuffer = null;
let _sbPlaying = false;
let _sbCurrentFile = '';
let _sbLoopBuf = null;
let _sbLoopStartTime = 0;
let _sbSwapTimer = null;
let _sbHighPass = null;
let _sbLowPass = null;
let _sbRawSamples = null;
let _sbRawRate = 22050;

function _getAudioCtx() {
  if (!_sbAudioCtx) _sbAudioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return _sbAudioCtx;
}

function sbUpdateFilters() {
  const ctx = _getAudioCtx();
  if (_sbHighPass) {
    const hp = parseInt(document.getElementById('sbHighPass').value) || 0;
    const hpVal = hp > 0 ? hp : 10;
    _sbHighPass.frequency.setValueAtTime(_sbHighPass.frequency.value, ctx.currentTime);
    _sbHighPass.frequency.linearRampToValueAtTime(hpVal, ctx.currentTime + 0.1);
  }
  if (_sbLowPass) {
    const lp = parseInt(document.getElementById('sbLowPass').value) || 11025;
    _sbLowPass.frequency.setValueAtTime(_sbLowPass.frequency.value, ctx.currentTime);
    _sbLowPass.frequency.linearRampToValueAtTime(lp, ctx.currentTime + 0.1);
  }
}

function _sbConnectChain(source, ctx) {
  const hp = parseInt(document.getElementById('sbHighPass').value) || 0;
  const lp = parseInt(document.getElementById('sbLowPass').value) || 11025;
  _sbHighPass = ctx.createBiquadFilter();
  _sbHighPass.type = 'highpass';
  _sbHighPass.frequency.value = hp > 0 ? hp : 10;
  _sbHighPass.Q.value = 0.7;
  _sbLowPass = ctx.createBiquadFilter();
  _sbLowPass.type = 'lowpass';
  _sbLowPass.frequency.value = lp;
  _sbLowPass.Q.value = 0.7;
  const volSlider = document.getElementById('sbVolSlider');
  _sbGain = ctx.createGain();
  _sbGain.gain.value = (volSlider ? parseInt(volSlider.value) : 100) / 100;
  source.connect(_sbHighPass);
  _sbHighPass.connect(_sbLowPass);
  _sbLowPass.connect(_sbGain);
  _sbGain.connect(ctx.destination);
  return _sbGain;
}

function resampleLinear(floatData, fromRate, toRate) {
  if (fromRate === toRate) return floatData;
  const ratio = fromRate / toRate;
  const outLen = Math.max(1, Math.floor(floatData.length / ratio));
  const out = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    const pos = i * ratio;
    const idx = Math.floor(pos);
    const frac = pos - idx;
    const a = floatData[idx] || 0;
    const b = floatData[Math.min(floatData.length - 1, idx + 1)] || a;
    out[i] = a + (b - a) * frac;
  }
  return out;
}

function crossfadeLoop(pcm8arr, fadeSamples) {
  if (!fadeSamples || fadeSamples < 2 || pcm8arr.length < fadeSamples * 2) return pcm8arr;
  const out = new Int8Array(pcm8arr.length - fadeSamples);
  for (let i = fadeSamples; i < pcm8arr.length - fadeSamples; i++) out[i] = pcm8arr[i];
  for (let i = 0; i < fadeSamples; i++) {
    const t = i / fadeSamples;
    const fromEnd = pcm8arr[pcm8arr.length - fadeSamples + i];
    const fromStart = pcm8arr[i];
    out[i] = Math.max(-128, Math.min(127, Math.round(fromEnd * (1 - t) + fromStart * t)));
  }
  return out;
}

function compressPcm8(pcm8arr, amount) {
  if (!amount || amount <= 0 || pcm8arr.length < 100) return pcm8arr;
  const len = pcm8arr.length;
  const winSize = Math.max(32, Math.min(512, Math.floor(len / 50)));
  const half = Math.floor(winSize / 2);
  const env = new Float32Array(len);
  let sumSq = 0;
  for (let i = 0; i < Math.min(winSize, len); i++) sumSq += (pcm8arr[i] / 128.0) ** 2;
  for (let i = 0; i < len; i++) {
    const addIdx = i + half;
    const remIdx = i - half - 1;
    if (addIdx < len) sumSq += (pcm8arr[addIdx] / 128.0) ** 2;
    if (remIdx >= 0) sumSq -= (pcm8arr[remIdx] / 128.0) ** 2;
    if (sumSq < 0) sumSq = 0;
    const cnt = Math.min(addIdx + 1, len) - Math.max(remIdx + 1, 0);
    env[i] = Math.sqrt(sumSq / cnt);
  }
  let totalSq = 0;
  for (let i = 0; i < len; i++) totalSq += (pcm8arr[i] / 128.0) ** 2;
  const targetRms = Math.sqrt(totalSq / len);
  if (targetRms < 0.001) return pcm8arr;
  const out = new Int8Array(len);
  for (let i = 0; i < len; i++) {
    const localRms = Math.max(env[i], 0.005);
    const gain = targetRms / localRms;
    const clampedGain = Math.min(gain, 4.0);
    const finalGain = 1.0 + (clampedGain - 1.0) * amount;
    const sample = pcm8arr[i] * finalGain;
    out[i] = Math.max(-128, Math.min(127, Math.round(sample)));
  }
  return out;
}

function _sbResample(samples, factor) {
  if (Math.abs(factor - 1.0) < 0.001) return samples;
  const newLen = Math.max(4, Math.round(samples.length * factor));
  const out = new Int8Array(newLen);
  for (let i = 0; i < newLen; i++) {
    const srcPos = i / factor;
    const idx = Math.floor(srcPos);
    const frac = srcPos - idx;
    const s0 = idx < samples.length ? samples[idx] : 0;
    const s1 = (idx + 1) < samples.length ? samples[idx + 1] : s0;
    out[i] = Math.max(-128, Math.min(127, Math.round(s0 * (1 - frac) + s1 * frac)));
  }
  return out;
}

function _sbBuildPreviewBuf(ctx) {
  if (!_sbRawSamples) return null;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0');
  const le = parseFloat(endSlider ? endSlider.value : '1');
  const startIdx = Math.max(0, Math.floor(ls * _sbRawSamples.length));
  const endIdx = Math.min(_sbRawSamples.length, Math.floor(le * _sbRawSamples.length));
  if (endIdx - startIdx < 4) return null;
  let slice = _sbRawSamples.slice(startIdx, endIdx);
  const cfPct = parseInt(document.getElementById('sbCrossfade').value) || 0;
  const minFade = 32;
  if (slice.length > 200) {
    const fadeSamples = Math.max(minFade, Math.floor(slice.length * cfPct / 100));
    if (slice.length > fadeSamples * 2) slice = crossfadeLoop(slice, fadeSamples);
  }
  const pitchSt = parseFloat(document.getElementById('sbPitch') ? document.getElementById('sbPitch').value : '0');
  const pitchLock = !!(document.getElementById('sbPitchLock') && document.getElementById('sbPitchLock').checked);
  let pitchFactor = Math.pow(2, -pitchSt / 12);
  if (pitchLock) {
    const rpm = parseFloat(document.getElementById('sbRpmSlider') ? document.getElementById('sbRpmSlider').value : '1');
    pitchFactor *= (1 / rpm);
  }
  if (Math.abs(pitchFactor - 1.0) > 0.001) slice = _sbResample(slice, pitchFactor);
  const buf = ctx.createBuffer(1, slice.length, _sbRawRate);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < slice.length; i++) ch[i] = slice[i] / 128.0;
  return buf;
}

function sbStop() {
  if (_sbSource) { try { _sbSource.stop(); } catch(e) {} _sbSource = null; }
  if (_sbHighPass) { try { _sbHighPass.disconnect(); } catch(e) {} _sbHighPass = null; }
  if (_sbLowPass) { try { _sbLowPass.disconnect(); } catch(e) {} _sbLowPass = null; }
  if (_sbGain) { try { _sbGain.disconnect(); } catch(e) {} _sbGain = null; }
  _sbPlaying = false;
  updatePlayBtn();
}

function sbPlayStop() { if (_sbPlaying) sbStop(); else sbPlay(); }

function updatePlayBtn() {
  const btn = document.getElementById('sbPlayBtn');
  if (btn) btn.innerHTML = _sbPlaying ? '&#9724; Stop' : '&#9654; Play';
}

function sbPlay() {
  if (!_sbBuffer) return;
  sbStop();
  const ctx = _getAudioCtx();
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0') * dur;
  const le = parseFloat(endSlider ? endSlider.value : '1') * dur;
  const doLoop = !!(document.getElementById('sbLoop') && document.getElementById('sbLoop').checked);
  if (doLoop) {
    const previewBuf = _sbBuildPreviewBuf(ctx);
    if (previewBuf) {
      _sbLoopBuf = previewBuf;
      _sbSource = ctx.createBufferSource();
      _sbSource.buffer = previewBuf;
      _sbSource.loop = true;
      _sbSource.loopStart = 0;
      _sbSource.loopEnd = previewBuf.duration;
      const rpmSlider = document.getElementById('sbRpmSlider');
      _sbSource.playbackRate.value = rpmSlider ? parseFloat(rpmSlider.value) : 1.0;
      _sbConnectChain(_sbSource, ctx);
      _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
      _sbSource.start(0);
      _sbLoopStartTime = ctx.currentTime;
      _sbPlaying = true;
      updatePlayBtn();
      return;
    }
  }
  _sbSource = ctx.createBufferSource();
  _sbSource.buffer = _sbBuffer;
  _sbSource.loop = doLoop;
  _sbSource.loopStart = ls;
  _sbSource.loopEnd = le;
  const rpmSlider = document.getElementById('sbRpmSlider');
  _sbSource.playbackRate.value = rpmSlider ? parseFloat(rpmSlider.value) : 1.0;
  _sbConnectChain(_sbSource, ctx);
  _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
  _sbSource.start(0, ls);
  _sbLoopStartTime = ctx.currentTime;
  _sbPlaying = true;
  updatePlayBtn();
}

function sbUpdateRpm(val) {
  const v = parseFloat(val);
  const label = document.getElementById('sbRpmLabel');
  let desc = 'idle';
  if (v >= 2.5) desc = 'redline';
  else if (v >= 2.0) desc = 'high RPM';
  else if (v >= 1.5) desc = 'mid-high';
  else if (v >= 1.1) desc = 'mid RPM';
  else if (v >= 0.7) desc = 'idle';
  else desc = 'very low';
  if (label) label.textContent = v.toFixed(2) + 'x (' + desc + ')';
  if (_sbSource && _sbPlaying) {
    const ctx = _getAudioCtx();
    _sbSource.playbackRate.setValueAtTime(_sbSource.playbackRate.value, ctx.currentTime);
    _sbSource.playbackRate.linearRampToValueAtTime(v, ctx.currentTime + 0.15);
    const pitchLock = !!(document.getElementById('sbPitchLock') && document.getElementById('sbPitchLock').checked);
    if (pitchLock) {
      if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
      _sbSwapTimer = setTimeout(_sbHotSwap, 200);
    }
  }
}

function sbUpdateVol(val) {
  const v = parseInt(val);
  const label = document.getElementById('sbVolLabel');
  if (label) label.textContent = v + '%';
  if (_sbGain) {
    const ctx = _getAudioCtx();
    _sbGain.gain.setValueAtTime(_sbGain.gain.value, ctx.currentTime);
    _sbGain.gain.linearRampToValueAtTime(v / 100, ctx.currentTime + 0.08);
  }
}

function sbUpdateLoopPoints() {
  if (!_sbBuffer) return;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const startLabel = document.getElementById('sbLoopStartLabel');
  const endLabel = document.getElementById('sbLoopEndLabel');
  const selInfo = document.getElementById('sbSelectionInfo');
  let ls = parseFloat(startSlider.value) * dur;
  let le = parseFloat(endSlider.value) * dur;
  if (le <= ls) le = Math.min(ls + 0.001, dur);
  if (startLabel) startLabel.textContent = ls.toFixed(3) + 's';
  if (endLabel) endLabel.textContent = le.toFixed(3) + 's';
  const selSamples = Math.round((le - ls) * _sbRawRate);
  const selKB = Math.round(selSamples / 1024);
  if (selInfo) selInfo.textContent = 'Selection: ' + (le - ls).toFixed(3) + 's, ~' + selSamples + ' samples, ~' + selKB + ' KB';
  if (_sbPlaying && _sbSource) {
    if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
    _sbSwapTimer = setTimeout(_sbHotSwap, 120);
  }
}

function _sbHotSwap() {
  if (!_sbPlaying || !_sbSource || !_sbBuffer) return;
  const ctx = _getAudioCtx();
  const newBuf = _sbBuildPreviewBuf(ctx);
  if (!newBuf) return;
  const rate = _sbSource.playbackRate.value;
  const elapsed = (ctx.currentTime - _sbLoopStartTime) * rate;
  const oldDur = _sbLoopBuf ? _sbLoopBuf.duration : _sbBuffer.duration;
  const phase = elapsed % oldDur;
  const offset = Math.min(phase / oldDur * newBuf.duration, newBuf.duration - 0.001);
  const fadeTime = 0.03;
  const volSlider = document.getElementById('sbVolSlider');
  const vol = (volSlider ? parseInt(volSlider.value) : 100) / 100;
  const oldSource = _sbSource;
  const oldGain = _sbGain;
  oldSource.onended = null;
  oldGain.gain.setValueAtTime(oldGain.gain.value, ctx.currentTime);
  oldGain.gain.linearRampToValueAtTime(0, ctx.currentTime + fadeTime);
  try { oldSource.stop(ctx.currentTime + fadeTime + 0.01); } catch(e) {}
  setTimeout(function(){ try { oldGain.disconnect(); } catch(e){} }, 100);
  _sbLoopBuf = newBuf;
  _sbSource = ctx.createBufferSource();
  _sbSource.buffer = newBuf;
  _sbSource.loop = true;
  _sbSource.loopStart = 0;
  _sbSource.loopEnd = newBuf.duration;
  _sbSource.playbackRate.value = rate;
  _sbConnectChain(_sbSource, ctx);
  _sbGain.gain.setValueAtTime(0, ctx.currentTime);
  _sbGain.gain.linearRampToValueAtTime(vol, ctx.currentTime + fadeTime);
  _sbSource.onended = function() { _sbPlaying = false; updatePlayBtn(); };
  _sbSource.start(0, offset > 0 ? offset : 0);
  _sbLoopStartTime = ctx.currentTime - (offset / (rate || 1));
}

function sbUpdatePitch() {
  const pitchSlider = document.getElementById('sbPitch');
  const pitchLabel = document.getElementById('sbPitchLabel');
  const v = parseFloat(pitchSlider ? pitchSlider.value : '0');
  if (pitchLabel) pitchLabel.textContent = (v >= 0 ? '+' : '') + v.toFixed(1) + ' st';
  if (_sbPlaying) {
    if (_sbSwapTimer) clearTimeout(_sbSwapTimer);
    _sbSwapTimer = setTimeout(_sbHotSwap, 120);
  }
}

async function stPreviewCurrent(slot) {
  const sel = document.querySelector('[data-sound="' + slot + '"]');
  if (!sel) return;
  const file = sel.value;
  if (!file) return;
  sbStop();
  try {
    const r = await fetch('/api/sound_pcm/' + encodeURIComponent(file));
    const j = await r.json();
    if (!j.ok) return;
    const ctx = _getAudioCtx();
    const buf = ctx.createBuffer(1, j.samples.length, j.sampleRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128.0;
    _sbBuffer = buf;
    _sbRawSamples = j.samples;
    _sbRawRate = j.sampleRate;
    _sbCurrentFile = file;
    _sbSource = ctx.createBufferSource();
    _sbSource.buffer = buf;
    _sbSource.loop = true;
    _sbGain = ctx.createGain();
    _sbGain.gain.value = 1.0;
    _sbSource.connect(_sbGain);
    _sbGain.connect(ctx.destination);
    _sbSource.onended = function() { _sbPlaying = false; };
    _sbSource.start(0);
    _sbPlaying = true;
  } catch(e) { console.error('stPreviewCurrent error:', e); }
}

async function stLoadBrowser() {
  try {
    const r = await fetch('/api/all_sounds');
    _stAllSounds = await r.json();
  } catch(e) { _stAllSounds = []; }
  stRenderBrowser();
}

function stRenderBrowser() {
  const search = (document.getElementById('stSearch')?.value || '').toLowerCase();
  const filtered = _stAllSounds.filter(s => {
    if (_stFilterCat !== 'all' && s.category !== _stFilterCat) return false;
    if (search && !s.label.toLowerCase().includes(search)) return false;
    return true;
  });
  const tbody = document.getElementById('stBrowserBody');
  if (!tbody) return;
  const countEl = document.getElementById('stCount');
  if (countEl) countEl.textContent = filtered.length + ' / ' + _stAllSounds.length;
  const catColors = {idle:'#4ade80',rev:'#f87171',start:'#fbbf24',knock:'#f97316',
    horn:'#60a5fa',siren:'#f472b6',brake:'#94a3b8',turbo:'#22d3ee',wastegate:'#a78bfa',
    track:'#a3e635',hydraulic:'#22d3ee',other:'#666'};
  tbody.innerHTML = filtered.map(s => {
    const color = catColors[s.category] || '#888';
    const hl = s.file === _sbCurrentFile ? 'background:rgba(255,203,5,0.08);' : '';
    return '<tr style="border-bottom:1px solid var(--border);cursor:pointer;' + hl + '" onclick="sbLoadSound(\'' + s.file.replace(/'/g, "\\'") + '\')">' +
      '<td style="padding:5px 8px;font-size:12px;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:220px" title="' + s.label + '">' +
      (s.file === _sbCurrentFile ? '&#9654; ' : '') + s.label + '</td>' +
      '<td style="padding:5px 4px;text-align:center"><span style="color:' + color + ';font-size:10px;background:var(--surface);padding:1px 6px;border-radius:8px">' + s.category + '</span></td>' +
      '<td style="padding:5px 4px;text-align:center"><button class="btn btn-ghost btn-sm" onclick="event.stopPropagation();sbLoadAndPlay(\'' + s.file.replace(/'/g, "\\'") + '\')" title="Load & Play">&#9654;</button></td>' +
      '<td style="padding:5px 4px;text-align:center"><button class="btn btn-ghost btn-sm" style="color:var(--danger)" onclick="event.stopPropagation();stDeleteSound(\'' + s.file.replace(/'/g, "\\'") + '\')" title="Delete">&#128465;</button></td></tr>';
  }).join('');
}

async function sbLoadSound(filename) {
  sbStop();
  _sbCurrentFile = filename;
  const nameEl = document.getElementById('sbNowPlaying');
  if (nameEl) nameEl.textContent = 'Loading ' + filename + '...';
  const info = document.getElementById('sbSoundInfo');
  if (info) info.textContent = '';
  try {
    const r = await fetch('/api/sound_pcm/' + encodeURIComponent(filename));
    const j = await r.json();
    if (!j.ok) { if (nameEl) nameEl.textContent = 'Error: ' + (j.error || '?'); return; }
    const ctx = _getAudioCtx();
    const buf = ctx.createBuffer(1, j.samples.length, j.sampleRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128.0;
    _sbBuffer = buf;
    _sbRawSamples = j.samples;
    _sbRawRate = j.sampleRate;
    const dur = (j.samples.length / j.sampleRate).toFixed(3);
    if (nameEl) nameEl.textContent = filename.replace('.h', '');
    if (info) info.textContent = j.sampleRate + ' Hz, ' + j.samples.length + ' samples, ' + dur + 's';
    const startSlider = document.getElementById('sbLoopStart');
    const endSlider = document.getElementById('sbLoopEnd');
    if (startSlider) startSlider.value = 0;
    if (endSlider) endSlider.value = 1;
    sbUpdateLoopPoints();
    stRenderBrowser();
  } catch(e) { if (nameEl) nameEl.textContent = 'Error loading: ' + e; }
}

async function sbLoadAndPlay(filename) {
  await sbLoadSound(filename);
  sbPlay();
}

async function stDeleteSound(filename) {
  if (!confirm('Delete ' + filename + '? This cannot be undone.')) return;
  try {
    const r = await fetch('/api/delete_sound', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ filename: filename })
    });
    const d = await r.json();
    if (d.ok) {
      toast('Deleted ' + filename, true);
      if (_sbCurrentFile === filename) { _sbCurrentFile = null; _sbBuffer = null; _sbRawSamples = null; }
      stLoadBrowser();
    } else {
      toast('Delete failed: ' + (d.error || '?'), false);
    }
  } catch(e) { toast('Delete error: ' + e, false); }
}

// Import a WAV file from the user's computer into the Live Sound Editor
async function sbImportWav(fileInput) {
  if (!fileInput.files || !fileInput.files[0]) return;
  const file = fileInput.files[0];
  const nameEl = document.getElementById('sbNowPlaying');
  const info = document.getElementById('sbSoundInfo');
  if (nameEl) nameEl.textContent = 'Importing ' + file.name + '...';

  sbStop();

  try {
    const arrayBuf = await file.arrayBuffer();
    const ctx = _getAudioCtx();
    const decoded = await ctx.decodeAudioData(arrayBuf);

    // Mix down to mono if stereo
    let monoData;
    if (decoded.numberOfChannels === 1) {
      monoData = decoded.getChannelData(0);
    } else {
      const ch0 = decoded.getChannelData(0);
      const ch1 = decoded.getChannelData(1);
      monoData = new Float32Array(ch0.length);
      for (let i = 0; i < ch0.length; i++) monoData[i] = (ch0[i] + ch1[i]) * 0.5;
    }

    // Resample to a reasonable rate if the source is very high (>44100)
    const srcRate = decoded.sampleRate;
    let targetRate = srcRate;
    let samples = monoData;
    if (srcRate > 44100) {
      targetRate = 22050;
      const ratio = srcRate / targetRate;
      const newLen = Math.floor(monoData.length / ratio);
      const resampled = new Float32Array(newLen);
      for (let i = 0; i < newLen; i++) {
        const srcIdx = i * ratio;
        const lo = Math.floor(srcIdx);
        const hi = Math.min(lo + 1, monoData.length - 1);
        const frac = srcIdx - lo;
        resampled[i] = monoData[lo] * (1 - frac) + monoData[hi] * frac;
      }
      samples = resampled;
    }

    // Convert float [-1, 1] to int8 [-128, 127] for internal representation
    const int8Samples = new Array(samples.length);
    for (let i = 0; i < samples.length; i++) {
      int8Samples[i] = Math.max(-128, Math.min(127, Math.round(samples[i] * 128)));
    }

    // Create AudioBuffer at the target rate
    const buf = ctx.createBuffer(1, samples.length, targetRate);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < samples.length; i++) ch[i] = int8Samples[i] / 128.0;

    _sbBuffer = buf;
    _sbRawSamples = int8Samples;
    _sbRawRate = targetRate;
    _sbCurrentFile = file.name.replace(/\.wav$/i, '.h');

    const dur = (samples.length / targetRate).toFixed(3);
    if (nameEl) nameEl.textContent = file.name.replace(/\.wav$/i, '') + ' (imported)';
    if (info) info.textContent = targetRate + ' Hz, ' + samples.length + ' samples, ' + dur + 's (from ' + srcRate + ' Hz WAV)';

    // Reset loop sliders
    const startSlider = document.getElementById('sbLoopStart');
    const endSlider = document.getElementById('sbLoopEnd');
    if (startSlider) startSlider.value = 0;
    if (endSlider) endSlider.value = 1;
    sbUpdateLoopPoints();

    toast('Imported ' + file.name + ' \u2014 use Export .h or Install when ready', true);
  } catch(e) {
    if (nameEl) nameEl.textContent = 'Import error: ' + e.message;
    toast('Failed to import WAV: ' + e.message, false);
  }
  // Reset file input so same file can be re-imported
  fileInput.value = '';
}

function sbGetLoopRegion() {
  if (!_sbBuffer) return null;
  const dur = _sbBuffer.duration;
  const startSlider = document.getElementById('sbLoopStart');
  const endSlider = document.getElementById('sbLoopEnd');
  const ls = parseFloat(startSlider ? startSlider.value : '0') * dur;
  const le = parseFloat(endSlider ? endSlider.value : '1') * dur;
  return { start: ls, end: le };
}

function sbProcessSlice() {
  const region = sbGetLoopRegion();
  const startIdx = Math.max(0, Math.floor(region.start * _sbRawRate));
  const endIdx = Math.min(_sbRawSamples.length, Math.floor(region.end * _sbRawRate));
  if (endIdx <= startIdx) return null;
  let slice = _sbRawSamples.slice(startIdx, endIdx);
  const speed = parseFloat(document.getElementById('sbRpmSlider').value) || 1;
  let outRate = parseInt(document.getElementById('sbExportRate').value) || 22050;
  if (speed !== 1) {
    const floats = new Float32Array(slice.length);
    for (let i = 0; i < slice.length; i++) floats[i] = slice[i] / 128.0;
    const resampled = resampleLinear(floats, _sbRawRate * speed, _sbRawRate);
    slice = new Int8Array(resampled.length);
    for (let i = 0; i < resampled.length; i++) slice[i] = Math.max(-128, Math.min(127, Math.round(resampled[i] * 128)));
  }
  if (outRate !== _sbRawRate) {
    const floats = new Float32Array(slice.length);
    for (let i = 0; i < slice.length; i++) floats[i] = slice[i] / 128.0;
    const resampled = resampleLinear(floats, _sbRawRate, outRate);
    slice = new Int8Array(resampled.length);
    for (let i = 0; i < resampled.length; i++) slice[i] = Math.max(-128, Math.min(127, Math.round(resampled[i] * 128)));
  }
  const smoothPct = parseInt(document.getElementById('sbSmooth').value) || 0;
  if (smoothPct > 0) slice = compressPcm8(slice, smoothPct / 100);
  if (document.getElementById('sbExportNorm').checked) {
    let peak = 0;
    for (let i = 0; i < slice.length; i++) peak = Math.max(peak, Math.abs(slice[i]));
    if (peak > 0 && peak < 127) {
      const gain = 127 / peak;
      const normed = new Int8Array(slice.length);
      for (let i = 0; i < slice.length; i++) normed[i] = Math.max(-128, Math.min(127, Math.round(slice[i] * gain)));
      slice = normed;
    }
  }
  const cfPct = parseInt(document.getElementById('sbCrossfade').value) || 0;
  if (cfPct > 0 && slice.length > 200) {
    const fadeSamples = Math.max(2, Math.floor(slice.length * cfPct / 100));
    slice = crossfadeLoop(slice, fadeSamples);
  }
  return { slice: slice, rate: outRate, region: region, speed: speed };
}

function sbBuildHeader(varName, slice, rate, region, speed) {
  const lines = [];
  lines.push('// Exported from Live Sound Builder');
  lines.push('// Loop region: ' + region.start.toFixed(3) + 's - ' + region.end.toFixed(3) + 's');
  if (speed !== 1) lines.push('// Export speed: ' + speed + 'x');
  lines.push('#pragma once');
  lines.push('const unsigned int ' + varName + '_sampleRate = ' + rate + ';');
  lines.push('const unsigned int ' + varName + '_sampleCount = ' + slice.length + ';');
  lines.push('const signed char ' + varName + '[] = {');
  let row = '  ';
  for (let i = 0; i < slice.length; i++) {
    row += slice[i].toString();
    if (i !== slice.length - 1) row += ', ';
    if ((i + 1) % 20 === 0 && i !== slice.length - 1) { lines.push(row); row = '  '; }
  }
  if (row.trim()) lines.push(row);
  lines.push('};');
  return lines.join('\n');
}

function sbExportSelection() {
  if (!_sbRawSamples || !_sbBuffer) { toast('Load a sound first', false); return; }
  const p = sbProcessSlice();
  if (!p) { toast('Invalid selection', false); return; }
  const varName = _sbCurrentFile.replace('.h', '').replace(/[^a-zA-Z0-9_]/g, '');
  const text = sbBuildHeader(varName, p.slice, p.rate, p.region, p.speed);
  const blob = new Blob([text], {type: 'text/plain'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = varName + '.h';
  a.click();
  URL.revokeObjectURL(a.href);
  const sizeKB = Math.round(p.slice.length / 1024);
  const dur = (p.slice.length / p.rate).toFixed(2);
  toast('Exported ' + varName + '.h \\u2014 ' + p.slice.length + ' samples (' + dur + 's @ ' + p.rate + 'Hz), ~' + sizeKB + ' KB', true);
}

async function sbInstallSelection() {
  if (!_sbRawSamples || !_sbBuffer) { toast('Load a sound first', false); return; }
  const p = sbProcessSlice();
  if (!p) { toast('Invalid selection', false); return; }
  const varName = _sbCurrentFile.replace('.h', '').replace(/[^a-zA-Z0-9_]/g, '');
  const filename = varName + '.h';
  const text = sbBuildHeader(varName, p.slice, p.rate, p.region, p.speed);
  try {
    const resp = await fetch('/api/install_sound', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ filename: filename, content: text })
    });
    const data = await resp.json();
    if (data.ok) {
      toast('Installed ' + (data.file || filename) + ' to sounds folder', true);
      stLoadBrowser();
    } else {
      toast('Install failed: ' + (data.error || '?'), false);
    }
  } catch(e) { toast('Install error: ' + e, false); }
}

function panelSoundTech() {
  const s = CFG.sounds || {};

  const cats = ['all', ...new Set(_stAllSounds.map(s=>s.category))].sort();
  const catOpts = cats.map(c => `<option value="${c}">${c==='all'?'All Categories':c}</option>`).join('');

  return `<div class="panel" id="p-soundtech">
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;">
      <!-- Sound Browser (left) -->
      <div>
        <div class="section-title">Sound Browser</div>
        <p class="hint" style="margin-bottom:8px;">Browse and preview all available sounds. Click <strong>Load</strong> to open in the Live Sound Editor.</p>
        <div style="display:flex;gap:6px;margin-bottom:10px;align-items:center;">
          <select id="stCatFilter" style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:5px 8px;border-radius:6px;font-size:11px;font-family:var(--font);"
            onchange="_stFilterCat=this.value;stRenderBrowser()">
            ${catOpts}
          </select>
          <input type="text" id="stSearch" placeholder="Search..."
            style="background:var(--surface);border:1px solid var(--border);color:var(--text);padding:5px 8px;border-radius:6px;font-size:11px;font-family:var(--font);flex:1;"
            oninput="stRenderBrowser()">
          <button class="btn btn-ghost btn-sm" onclick="sbStop()">&#9632; Stop</button>
        </div>
        <div style="max-height:560px;overflow-y:auto;border:1px solid var(--border);border-radius:var(--radius);">
          <table style="width:100%;border-collapse:collapse;">
            <thead><tr style="background:var(--surface);position:sticky;top:0;">
              <th style="padding:6px 8px;text-align:left;font-size:10px;color:var(--accent);text-transform:uppercase;">Name</th>
              <th style="padding:6px 8px;text-align:left;font-size:10px;color:var(--accent);text-transform:uppercase;">Category</th>
              <th style="padding:6px 8px;text-align:center;font-size:10px;color:var(--accent);text-transform:uppercase;">Actions</th>
            </tr></thead>
            <tbody id="stBrowserBody"></tbody>
          </table>
        </div>
      </div>

      <!-- Live Sound Editor (right) -->
      <div>
        <div class="section-title">&#127925; Live Sound Editor</div>
        <div style="padding:8px;background:var(--surface);border-radius:var(--radius);border:1px solid var(--border)">
          <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
            <span style="color:var(--accent);font-size:13px;font-weight:bold" id="sbNowPlaying">No sound loaded</span>
            <button type="button" class="btn btn-primary btn-sm" onclick="sbPlayStop()" id="sbPlayBtn" style="min-width:60px">&#9654; Play</button>
            <label style="font-size:12px;color:var(--dim)"><input id="sbLoop" type="checkbox" checked> Loop</label>
            <button type="button" class="btn btn-ghost btn-sm" onclick="document.getElementById('wavFileInput').click()" title="Import a .wav file from your computer">&#128194; Import WAV</button>
            <input type="file" id="wavFileInput" accept=".wav,audio/wav" style="display:none" onchange="sbImportWav(this)">
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:8px;flex-wrap:wrap">
            <label style="color:var(--dim);font-size:12px;white-space:nowrap">RPM:</label>
            <input id="sbRpmSlider" type="range" min="0.3" max="3.0" step="0.05" value="1.0"
              style="flex:1;min-width:150px;accent-color:var(--accent)"
              oninput="sbUpdateRpm(this.value)">
            <span id="sbRpmLabel" style="color:var(--text);font-size:13px;min-width:70px">1.00x (idle)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--dim);font-size:12px;white-space:nowrap">Volume:</label>
            <input id="sbVolSlider" type="range" min="0" max="200" step="5" value="100"
              style="width:100px;accent-color:var(--accent)"
              oninput="sbUpdateVol(this.value)">
            <span id="sbVolLabel" style="color:var(--text);font-size:12px">100%</span>
            <span style="color:var(--dim);font-size:11px;margin-left:auto" id="sbSoundInfo"></span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:8px;flex-wrap:wrap">
            <label style="color:#4ade80;font-size:12px;white-space:nowrap">Loop Start:</label>
            <input id="sbLoopStart" type="range" min="0" max="1" step="0.001" value="0"
              style="flex:1;min-width:120px;accent-color:#4ade80"
              oninput="sbUpdateLoopPoints()">
            <span id="sbLoopStartLabel" style="color:#4ade80;font-size:11px;min-width:50px">0.000s</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:4px;flex-wrap:wrap">
            <label style="color:#f87171;font-size:12px;white-space:nowrap">Loop End:</label>
            <input id="sbLoopEnd" type="range" min="0" max="1" step="0.001" value="1"
              style="flex:1;min-width:120px;accent-color:#f87171"
              oninput="sbUpdateLoopPoints()">
            <span id="sbLoopEndLabel" style="color:#f87171;font-size:11px;min-width:50px">1.000s</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--text);font-size:12px;white-space:nowrap">Smooth:</label>
            <input id="sbSmooth" type="range" min="0" max="100" step="5" value="0"
              style="flex:1;min-width:100px;accent-color:var(--accent)"
              oninput="document.getElementById('sbSmoothLabel').textContent=this.value+'%'; if(_sbPlaying){if(_sbSwapTimer)clearTimeout(_sbSwapTimer);_sbSwapTimer=setTimeout(_sbHotSwap,150);}">
            <span id="sbSmoothLabel" style="color:var(--text);font-size:11px;min-width:30px">0%</span>
            <span style="color:var(--dim);font-size:10px">(evens out loud &amp; quiet spots)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#f472b6;font-size:12px;white-space:nowrap">Crossfade:</label>
            <input id="sbCrossfade" type="range" min="0" max="100" step="1" value="0"
              style="flex:1;min-width:100px;accent-color:#f472b6"
              oninput="document.getElementById('sbCrossfadeLabel').textContent=this.value+'%'; if(_sbPlaying){if(_sbSwapTimer)clearTimeout(_sbSwapTimer);_sbSwapTimer=setTimeout(_sbHotSwap,150);}">
            <span id="sbCrossfadeLabel" style="color:#f472b6;font-size:11px;min-width:30px">0%</span>
            <span style="color:var(--dim);font-size:10px">(blends end&rarr;start for seamless loop)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#60a5fa;font-size:12px;white-space:nowrap">Low Cut:</label>
            <input id="sbHighPass" type="range" min="0" max="2000" step="10" value="0"
              style="flex:1;min-width:100px;accent-color:#60a5fa"
              oninput="document.getElementById('sbHighPassLabel').textContent=this.value+'Hz'; sbUpdateFilters();">
            <span id="sbHighPassLabel" style="color:#60a5fa;font-size:11px;min-width:40px">0Hz</span>
            <span style="color:var(--dim);font-size:10px">(removes rumble)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#a78bfa;font-size:12px;white-space:nowrap">High Cut:</label>
            <input id="sbLowPass" type="range" min="500" max="11025" step="25" value="11025"
              style="flex:1;min-width:100px;accent-color:#a78bfa"
              oninput="document.getElementById('sbLowPassLabel').textContent=this.value+'Hz'; sbUpdateFilters();">
            <span id="sbLowPassLabel" style="color:#a78bfa;font-size:11px;min-width:50px">11025Hz</span>
            <span style="color:var(--dim);font-size:10px">(removes hiss)</span>
          </div>
          <div style="display:flex;align-items:center;gap:10px;margin-top:6px;flex-wrap:wrap">
            <label style="color:#e879f9;font-size:12px;white-space:nowrap">Pitch:</label>
            <input id="sbPitch" type="range" min="-12" max="12" step="0.5" value="0"
              style="flex:1;min-width:100px;accent-color:#e879f9"
              oninput="sbUpdatePitch()">
            <span id="sbPitchLabel" style="color:#e879f9;font-size:11px;min-width:50px">0 st</span>
            <label style="font-size:11px;color:var(--dim);white-space:nowrap" title="When locked, pitch stays constant regardless of RPM speed"><input id="sbPitchLock" type="checkbox" onchange="sbUpdatePitch()"> Lock</label>
            <span style="color:var(--dim);font-size:10px">(semitones)</span>
          </div>
          <p style="color:var(--dim);font-size:11px;margin:8px 0 0;padding-top:8px;border-top:1px solid var(--border)">Export bakes in your current RPM, loop points, and pitch settings.</p>
          <div style="display:flex;align-items:center;gap:8px;margin-top:6px;flex-wrap:wrap">
            <label style="color:var(--accent);font-size:12px;white-space:nowrap">Rate:</label>
            <select id="sbExportRate" style="width:80px;background:var(--surface);border:1px solid var(--border);color:var(--text);padding:4px 6px;border-radius:6px;font-size:12px;font-family:var(--font);" title="Output sample rate">
              <option value="8000">8 kHz</option>
              <option value="11025">11 kHz</option>
              <option value="16000">16 kHz</option>
              <option value="22050" selected>22 kHz</option>
            </select>
            <label style="font-size:12px;color:var(--dim)"><input id="sbExportNorm" type="checkbox" checked> Normalize</label>
          </div>
          <div style="display:flex;align-items:center;gap:8px;margin-top:8px;flex-wrap:wrap">
            <button type="button" class="btn btn-primary btn-sm" onclick="sbExportSelection()" title="Export the selected loop region as a .h header file">&#128229; Export .h</button>
            <button type="button" class="btn btn-primary btn-sm" onclick="sbInstallSelection()" title="Export and install to sounds folder">&#9654; Install</button>
            <span id="sbSelectionInfo" style="color:var(--dim);font-size:11px"></span>
          </div>
        </div>
      </div>
    </div>

    <div class="section-title" style="margin-top:20px;">&#127911; Live Tuning &mdash; <span style="color:var(--dim);font-weight:400;text-transform:none;font-size:11px">Runtime-configurable (pushed via serial)</span></div>
    <p class="hint" style="margin-bottom:10px;">Adjust volumes, engine response, and ESC tuning. Push to ESP32 to apply instantly.</p>

    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px 24px;margin-bottom:16px;">
      <div>
        <div style="font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;margin-bottom:6px;">Engine</div>
        ${liveSlider('masterVolume', 'Master Volume', 0, 200, '%')}
        ${liveSlider('idleVolumePercentage', 'Idle Volume', 0, 300, '%')}
        ${liveSlider('dieselKnockVolumePercentage', 'Diesel Knock', 0, 1000, '%')}
        ${liveSlider('turboVolumePercentage', 'Turbo Whistle', 0, 300, '%')}
        ${liveSlider('startVolumePercentage', 'Start Sound', 0, 300, '%')}
        ${liveSlider('acc', 'Acceleration (inertia)', 1, 9, '')}
        ${liveSlider('dec', 'Deceleration (inertia)', 1, 9, '')}
        ${liveToggle('autoEngineStart', 'Auto Engine Start', 'Engine starts on throttle (no switch needed)')}
      </div>
      <div>
        <div style="font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;margin-bottom:6px;">Sounds &amp; ESC</div>
        ${liveSlider('hornVolumePercentage', 'Horn', 0, 300, '%')}
        ${liveSlider('brakeVolumePercentage', 'Air Brake', 0, 300, '%')}
        ${liveSlider('reversingVolumePercentage', 'Reversing Beep', 0, 300, '%')}
        ${liveSlider('hydraulicPumpVolumePercentage', 'Hyd. Pump', 0, 300, '%')}
        ${liveSlider('hydraulicFlowVolumePercentage', 'Hyd. Flow', 0, 300, '%')}
        ${liveSlider('trackRattleVolumePercentage', 'Track Rattle', 0, 300, '%')}
        ${liveToggle('trackRattle2Enabled', 'Track Rattle 2', 'Secondary track rattle triggered by track movement')}
        ${liveSlider('bucketRattleVolumePercentage', 'Bucket Rattle', 0, 300, '%')}
        ${liveSlider('escRampTimeLow', 'ESC Ramp (Low)', 5, 200, 'ms')}
        ${liveSlider('escRampTimeHigh', 'ESC Ramp (High)', 5, 200, 'ms')}
        ${liveSlider('escBrakeSteps', 'Brake Steps', 1, 100, '')}
        ${liveSlider('escAccelerationSteps', 'Accel Steps', 1, 20, '')}
      </div>
    </div>

    <style>
      .st-playing { outline:2px solid var(--accent) !important; animation:pulse 1s infinite; }
      #p-soundtech table tr:hover td { background:rgba(255,203,5,0.05); }
      #p-soundtech input[type=range] { accent-color: var(--accent); }
    </style>
  </div>`;
}

// ── Serial Push (no-rebuild channel config) ─────
async function refreshPorts() {
  const sel = document.getElementById('serialPort');
  if (!sel) return;
  try {
    const r = await fetch('/api/serial_ports');
    const ports = await r.json();
    const prev = sel.value;
    sel.innerHTML = '<option value="">Select COM port...</option>';
    ports.forEach(p => {
      const opt = document.createElement('option');
      opt.value = p.port;
      opt.textContent = p.port + (p.desc && p.desc !== p.port ? ' — ' + p.desc : '');
      sel.appendChild(opt);
    });
    if (prev) sel.value = prev;
  } catch(e) { console.error('refreshPorts:', e); }
}

async function pushChannels() {
  const port = document.getElementById('serialPort').value;
  if (!port) { toast('Select a COM port first', false); return; }
  const statusEl = document.getElementById('pushStatus');
  const logEl = document.getElementById('pushLog');
  if (statusEl) statusEl.textContent = 'Pushing...';
  if (logEl) { logEl.style.display = 'block'; logEl.textContent = 'Connecting to ' + port + '...\n'; }

  // Gather all channel mappings (0 = unassigned/none)
  const all = [...CH_MAP_COMMON, ...(CH_MAP_MACHINE[CFG.machineType] || [])];
  const channels = {};
  for (const m of all) {
    channels[m.v] = CFG[m.v] !== undefined ? CFG[m.v] : 0;
  }

  // Gather runtime settings
  const SETTINGS_KEYS = [
    'masterVolume', 'idleVolumePercentage', 'dieselKnockVolumePercentage',
    'turboVolumePercentage', 'hornVolumePercentage', 'brakeVolumePercentage',
    'hydraulicPumpVolumePercentage', 'hydraulicFlowVolumePercentage',
    'trackRattleVolumePercentage', 'bucketRattleVolumePercentage',
    'reversingVolumePercentage', 'startVolumePercentage',
    'acc', 'dec', 'escRampTimeLow', 'escRampTimeHigh',
    'escBrakeSteps', 'escAccelerationSteps',
  ];
  const settings = {};
  for (const k of SETTINGS_KEYS) {
    if (CFG[k] !== undefined) settings[k] = CFG[k];
  }
  // Boolean setting
  const autoStart = (CFG.autoEngineStart === 'true' || CFG.autoEngineStart === true);
  settings['autoEngineStart'] = autoStart ? 1 : 0;

  // Gather channel reverse flags
  const reversed = CFG.channelReversed || {};
  // Gather channel enable flags
  const enabled = CFG.channelEnabled || {};

  try {
    const resp = await fetch('/api/push_channels', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ port: port, channels: channels, settings: settings, channelReversed: reversed, channelEnabled: enabled })
    });
    const data = await resp.json();
    if (logEl && data.log) logEl.textContent += data.log.join('\n') + '\n';
    if (data.ok) {
      if (statusEl) statusEl.innerHTML = '<span style="color:#2ecc71;">&#10003; Pushed &amp; saved!</span>';
      toast('Channels + settings pushed to ESP32!', true);
    } else {
      if (statusEl) statusEl.innerHTML = '<span style="color:var(--danger);">Failed: ' + data.error + '</span>';
      toast('Push failed: ' + data.error, false);
    }
  } catch(e) {
    if (statusEl) statusEl.innerHTML = '<span style="color:var(--danger);">Error: ' + e + '</span>';
    toast('Push error: ' + e, false);
  }
}

// ── Save ────────────────────────────────────────
let _currentVehicle = null;  // name of currently loaded vehicle profile
let _vehicleList = [];       // cached list of saved vehicle names

function _gatherCfg() {
  CFG.sounds = CFG.sounds || {};
  document.querySelectorAll('[data-sound]').forEach(sel => {
    CFG.sounds[sel.dataset.sound] = sel.value;
  });
  // Sync toggle checkboxes into CFG so stale values don't get saved
  document.querySelectorAll('.sw input[type=checkbox]').forEach(cb => {
    const m = cb.getAttribute('onchange');
    if (m) {
      const km = m.match(/CFG\['(\w+)'\]/);
      if (km) CFG[km[1]] = cb.checked ? 'true' : 'false';
    }
  });
  return CFG;
}

async function saveConfig() {
  _gatherCfg();
  // First save ever (no vehicle loaded) → prompt for a name
  if (!_currentVehicle) {
    const name = prompt('Save as new vehicle profile:', CFG.customMachineName || MACHINE_NAMES[CFG.machineType] || 'My Vehicle');
    if (!name) return;
    await _saveVehicleProfile(name);
    return;
  }
  // Already have a vehicle loaded → overwrite it + write config.h
  const resp = await fetch('/api/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(CFG)
  });
  const data = await resp.json();
  // Also update the vehicle profile JSON
  await fetch('/api/vehicle/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ name: _currentVehicle, config: CFG })
  });
  toast(data.ok ? ('Saved — ' + _currentVehicle) : ('Error: ' + data.error), data.ok);
  _refreshVehicleList();
}

async function _saveVehicleProfile(name) {
  // Write config.h
  const resp = await fetch('/api/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(CFG)
  });
  const data = await resp.json();
  if (!data.ok) { toast('Error: ' + data.error, false); return; }
  // Save vehicle profile JSON
  const resp2 = await fetch('/api/vehicle/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ name: name, config: CFG })
  });
  const data2 = await resp2.json();
  if (data2.ok) {
    _currentVehicle = data2.name;
    toast('Saved new vehicle: ' + data2.name, true);
    _refreshVehicleList();
  } else {
    toast('Error: ' + data2.error, false);
  }
}

async function saveAsNewVehicle() {
  _gatherCfg();
  const name = prompt('Save as new vehicle profile:', CFG.customMachineName || MACHINE_NAMES[CFG.machineType] || 'My Vehicle');
  if (!name) return;
  await _saveVehicleProfile(name);
}

async function loadVehicle(name) {
  if (!name) return;
  try {
    const resp = await fetch('/api/vehicle/load', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ name: name })
    });
    const data = await resp.json();
    if (data.ok) {
      soundFiles = data.config.soundFiles || soundFiles;
      delete data.config.soundFiles;
      CFG = data.config;
      _currentVehicle = name;
      renderPanels();
      updateMachineName();
      activateTab('machine');
      toast('Loaded: ' + name, true);
      _refreshVehicleList();
    } else {
      toast('Error: ' + data.error, false);
    }
  } catch(e) { toast('Load error: ' + e, false); }
}

async function deleteVehicle(name) {
  if (!name) return;
  if (!confirm('Delete vehicle profile "' + name + '"?')) return;
  const resp = await fetch('/api/vehicle/delete', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ name: name })
  });
  const data = await resp.json();
  if (data.ok) {
    if (_currentVehicle === name) _currentVehicle = null;
    toast('Deleted: ' + name, true);
    _refreshVehicleList();
  } else {
    toast('Delete error: ' + data.error, false);
  }
}

function exportVehicle() {
  _gatherCfg();
  const name = _currentVehicle || CFG.customMachineName || MACHINE_NAMES[CFG.machineType] || 'vehicle';
  const blob = new Blob([JSON.stringify(CFG, null, 2)], {type: 'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name + '.json';
  a.click();
  URL.revokeObjectURL(a.href);
  toast('Exported: ' + name + '.json', true);
}

async function importVehicle(input) {
  const file = input.files[0];
  if (!file) return;
  try {
    const text = await file.text();
    const cfg = JSON.parse(text);
    const name = file.name.replace(/\.json$/i, '');
    const resp = await fetch('/api/vehicle/save', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ name: name, config: cfg })
    });
    const data = await resp.json();
    if (data.ok) {
      toast('Imported: ' + data.name + ' — click Load to apply', true);
      _refreshVehicleList();
    } else {
      toast('Import error: ' + data.error, false);
    }
  } catch(e) { toast('Import error: ' + e, false); }
  input.value = '';
}

async function _refreshVehicleList() {
  try {
    const resp = await fetch('/api/vehicles');
    const data = await resp.json();
    if (data.ok) {
      _vehicleList = data.vehicles;
      _currentVehicle = data.current;
    }
  } catch(e) {}
  // Update the vehicle list UI if visible
  const el = document.getElementById('vehicleListBody');
  if (el) el.innerHTML = _vehicleListRows();
  const cur = document.getElementById('currentVehicleName');
  if (cur) cur.textContent = _currentVehicle || '(unsaved)';
}

function _vehicleListRows() {
  if (!_vehicleList.length) return '<tr><td colspan="2" style="padding:8px;color:var(--dim);font-style:italic;">No saved vehicles yet</td></tr>';
  return _vehicleList.map(v => {
    const isCurrent = v === _currentVehicle;
    return '<tr style="border-bottom:1px solid var(--border);">' +
      '<td style="padding:6px 8px;font-size:13px;color:var(--text);">' +
        (isCurrent ? '<span style="color:var(--accent);">&#9654; </span>' : '') + v + '</td>' +
      '<td style="padding:6px 8px;text-align:right;white-space:nowrap;">' +
        '<button class="btn btn-sm" onclick="loadVehicle(\'' + v.replace(/'/g,"\\'") + '\')" style="font-size:11px;padding:2px 8px;margin-right:4px;">Load</button>' +
        '<button class="btn btn-sm" onclick="deleteVehicle(\'' + v.replace(/'/g,"\\'") + '\')" style="font-size:11px;padding:2px 8px;color:var(--danger);">&#128465;</button>' +
      '</td></tr>';
  }).join('');
}

// ── Build ───────────────────────────────────────
let buildPoll = null;
async function startBuild(upload) {
  // Always write config.h before building (skip vehicle prompt)
  _gatherCfg();
  await fetch('/api/save', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(CFG)
  });
  if (upload) {
    const port = document.getElementById('flashPort')?.value || '';
    await fetch('/api/upload', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ port: port })
    });
  } else {
    await fetch('/api/build', {method:'POST'});
  }
  toast(upload ? 'Build & flash started...' : 'Build started...', true);
  pollBuild();
}

async function refreshFlashPorts() {
  const sel = document.getElementById('flashPort');
  if (!sel) return;
  try {
    const r = await fetch('/api/serial_ports');
    const ports = await r.json();
    const prev = sel.value;
    sel.innerHTML = '<option value="">Auto-detect port...</option>';
    ports.forEach(p => {
      const opt = document.createElement('option');
      opt.value = p.port;
      opt.textContent = p.port + (p.desc && p.desc !== p.port ? ' \u2014 ' + p.desc : '');
      sel.appendChild(opt);
    });
    if (prev) sel.value = prev;
  } catch(e) { console.error('refreshFlashPorts:', e); }
}

function pollBuild() {
  if (buildPoll) clearInterval(buildPoll);
  buildPoll = setInterval(async () => {
    const resp = await fetch('/api/build-log');
    const data = await resp.json();
    const el = document.getElementById('buildOutput');
    if (el) { el.textContent = data.log.join('\n') || 'No output yet...'; el.scrollTop = el.scrollHeight; }
    const st = document.getElementById('buildStatus');
    if (st) {
      if (data.running) {
        st.innerHTML = '<span class="status-dot busy"></span> Building...';
      } else if (data.log.length > 0) {
        const last = data.log[data.log.length-1] || '';
        const ok = last.includes('Exit code: 0') || data.log.some(l => l.includes('[SUCCESS]'));
        st.innerHTML = ok
          ? '<span class="status-dot ok"></span> Success'
          : '<span class="status-dot" style="background:var(--danger)"></span> Failed';
        if (!data.running) clearInterval(buildPoll);
      }
    }
  }, 1000);
}

// ── Toast ───────────────────────────────────────
function toast(msg, ok) {
  const div = document.createElement('div');
  div.className = 'toast ' + (ok ? 'ok' : 'err');
  div.textContent = msg;
  document.body.appendChild(div);
  setTimeout(() => { div.style.opacity = '0'; setTimeout(() => div.remove(), 300); }, 2500);
}

init();
</script>
</body>
</html>"""

# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    import argparse
    parser = argparse.ArgumentParser(description="HydraulicController Web Configurator")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        # Do NOT reuse the address: on Windows SO_REUSEADDR lets several instances bind the
        # SAME port at once, so double-clicking the app stacked servers that fought over every
        # request (page hung on "Loading configuration…"). With this off, a second launch fails
        # to bind and we just focus the instance that's already running (below).
        allow_reuse_address = False

    url = f"http://localhost:{args.port}"
    try:
        server = ThreadedHTTPServer(("0.0.0.0", args.port), Handler)
    except OSError:
        # Port busy -> an instance is already up. Open the browser to it and exit (no stacking).
        print(f"Configurator already running at {url} — opening it.")
        if not args.no_browser:
            webbrowser.open(url)
        return

    print(f"HydraulicController Configurator running at {url}")
    print("Press Ctrl+C to stop.")

    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
