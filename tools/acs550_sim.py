#!/usr/bin/env python3
"""ABB ACS550 drive simulator, for testing the Drive Remote panel without a drive.

Wire a USB-RS485 adapter to the panel's RS485 A/B terminals and run

    python tools/acs550_sim.py              (finds the adapter by itself)
    python tools/acs550_sim.py --port COM4

It answers Modbus RTU the way an ACS550 does with 9802 = 1 (STD MODBUS),
5305 = 0 (ABB DRV LIM), station 1, 19200 8N1:

    40001 control word   40002 REF1   40003 REF2   40004 status word
    4xxyy = parameter xx.yy, e.g. 40102 = 0102 SPEED, 41105 = 1105 REF1 MAX
    FC03/04 read, FC06/16 write. Registers it doesn't have answer exception 02.

The motor follows the control word through the ABB Drives state machine: START
ramps to the reference over 2202, STOP ramps down over 2203, QUICK STOP uses
2208 and COAST switches the output off. The load is a pump curve, so amps,
torque, kW and temperature rise with speed. With 3018 = 1 the drive trips with
fault 28 when the panel has been quiet for longer than 3019.

Settings (port, station, motor nameplate, REF1 MAX, ramps, comm loss, load, any
parameter) are in ACS550-Simulator.ini next to the program. It is created with the
defaults on the first run. Command-line options override it.

Keys:  f trip a fault   r keypad reset   l keypad LOC/REM   p power-cycle
       c cut/repair the cable   + / - load   v show every frame   e open the settings   q quit

Needs pyserial (PlatformIO's Python already has it). build_exe.bat makes a stand-alone
ACS550-Simulator.exe. The behaviour is simplified: it is a test aid for the panel, not
a model of the drive's own firmware.
"""

import argparse
import configparser
import math
import os
import random
import shutil
import struct
import sys
import time

try:
    import msvcrt  # key presses on Windows
except ImportError:
    msvcrt = None

# Control word bits (ABB DRV LIM)
CW_OFF1 = 0x0001            # 0 = ramp stop
CW_OFF2 = 0x0002            # 0 = coast stop
CW_OFF3 = 0x0004            # 0 = quick stop (2208)
CW_ENABLE = 0x0008          # 1 = run
CW_RAMP_OUT_ZERO = 0x0010   # 0 = force the ramp output to zero
CW_RAMP_HOLD = 0x0020       # 0 = hold the ramp
CW_RAMP_IN_ZERO = 0x0040    # 0 = force the ramp input to zero
CW_RESET = 0x0080           # 0 -> 1 resets a fault
CW_REMOTE = 0x0400          # fieldbus control

CW_NAMES = {0x0476: "STOP", 0x047F: "RUN", 0x047D: "COAST", 0x047B: "QUICK STOP", 0x04F6: "RESET"}
KEYPAD_STOP = CW_OFF2 | CW_OFF3 | CW_RAMP_OUT_ZERO | CW_RAMP_HOLD | CW_RAMP_IN_ZERO

# Status word bits
SW_RDY_ON = 0x0001
SW_RDY_RUN = 0x0002
SW_RDY_REF = 0x0004         # running
SW_TRIPPED = 0x0008
SW_OFF2_STA = 0x0010
SW_OFF3_STA = 0x0020
SW_SWC_ON_INHIB = 0x0040
SW_ALARM = 0x0080
SW_AT_SETPOINT = 0x0100
SW_REMOTE = 0x0200
SW_EXT_RUN_ENABLE = 0x1000

FAULT_NAMES = {1: "OVERCURRENT", 2: "DC OVERVOLT", 3: "DEV OVERTEMP", 16: "EARTH FAULT",
               22: "SUPPLY PHASE", 28: "SERIAL 1 ERR"}
TEST_FAULTS = [1, 2, 3, 16, 22]  # the f key steps through these

# Drive states (ABB Drives profile)
INHIBITED, READY_ON, READY_RUN, RUNNING, OFF1_ACTIVE, OFF3_ACTIVE, TRIPPED = range(7)
STATE_NAMES = ["SWITCH-ON INHIBITED", "READY TO SWITCH ON", "READY TO RUN", "RUNNING",
               "RAMP STOP", "QUICK STOP", "TRIPPED"]
MODULATING = (RUNNING, OFF1_ACTIVE, OFF3_ACTIVE)

SIM_VERSION = "1.0"
FORMATS = ["8N1", "8N2", "8E1", "8O1"]  # same order as 5304 EFB PARITY

KEY_HELP = ("Keys: f fault  r reset  l LOC/REM  p power-cycle  c cable  +/- load  v frames  e settings  h help  "
            "q quit")

CONFIG_NAME = "ACS550-Simulator.ini"
DEFAULT_CONFIG = """\
; ACS550 drive simulator settings. Edit, save, then restart the simulator.
; Lines starting with ; are comments. Delete this file to get the defaults back.

[Connection]
; COM port of the USB-RS485 adapter, e.g. COM4, or auto (asks if there are several)
port = auto
; Must match the panel's Setup page (drive parameters 5302 / 5303 / 5304)
station = 1
baud = 19200
; 8N1, 8N2, 8E1 or 8O1
format = 8N1

[Drive]
; scalar (9904 = 3, speed reference in Hz) or vector (9904 = 1, speed reference in rpm)
control_mode = scalar
; Motor nameplate, 9905-9909
motor_power_kw = 37.0
motor_voltage = 415
motor_current_a = 66.0
motor_frequency_hz = 50.0
motor_speed_rpm = 1475
; 1105 REF1 MAX: the Hz value is used in scalar mode, the rpm value in vector mode
ref1_max_hz = 50.0
ref1_max_rpm = 1500
; 2002 MAXIMUM SPEED and 2008 MAXIMUM FREQ. Ramp times run from zero to these.
max_speed_rpm = 1500
max_frequency_hz = 50.0
; Ramp times in seconds: 2202 ACCELER TIME 1, 2203 DECELER TIME 1, 2208 EMERG DEC TIME
accel_time_s = 5.0
decel_time_s = 5.0
quick_stop_time_s = 1.0
; 1003 DIRECTION: 1 forward only, 2 reverse only, 3 either (needed for REV)
direction = 3
; 3018 COMM FAULT FUNC: 0 none, 1 fault, 2 constant speed 7, 3 last speed
comm_fault_function = 1
; 3019 COMM FAULT TIME, seconds
comm_fault_time_s = 3.0

[Simulation]
; Pump load at full speed, percent (the + and - keys change it while running)
load_percent = 100
; Mains supply voltage; the DC bus sits at about 1.35 times this
supply_voltage = 415

[Parameters]
; Any other parameter by number, as the raw value the drive sends (see the ACS550 manual), e.g.
; 1601 = 7
; 1604 = 8
"""


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def u16(v):
    return int(round(v)) & 0xFFFF


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(body):
    crc = crc16(body)
    return bytes(body) + bytes((crc & 0xFF, crc >> 8))


def panel_state(sw):
    """The text the panel shows for a status word (same logic as acs550.cpp)."""
    if sw & SW_TRIPPED:
        return "FAULT"
    if sw & SW_RDY_REF:
        return "RUNNING - AT SPEED" if sw & SW_AT_SETPOINT else "RUNNING"
    if not sw & SW_REMOTE:
        return "LOCAL / NOT REMOTE"
    if sw & SW_SWC_ON_INHIB:
        return "SWITCH-ON INHIBITED"
    return "READY" if sw & SW_RDY_ON else "STOPPED"


class Drive:
    def __init__(self, slave=1, baud=19200, fmt=0, vector=False, log=print, params=None, load=1.0, supply=415.0):
        self.log = log
        # Parameters as raw register values. Actual signals (01xx) are computed on read.
        self.par = {
            401: 0, 412: 0, 413: 0,                         # fault history
            1001: 10, 1003: 3, 1102: 0, 1103: 8, 1104: 0,   # start/stop and REF1 from COMM
            1105: 1500 if vector else 500,                  # REF1 MAX: rpm, or 0.1 Hz in scalar mode
            1601: 0, 1602: 1, 1604: 8, 1607: 0,
            2001: 0, 2002: 1500, 2003: 990, 2007: 0, 2008: 500,
            2202: 50, 2203: 50, 2205: 600, 2206: 600, 2208: 10,  # ramp times in 0.1 s
            3018: 1, 3019: 30,                              # comm loss: fault after 3.0 s
            5302: slave, 5303: baud // 100, 5304: fmt, 5305: 0,
            9802: 1,
            9904: 1 if vector else 3,
            9905: 415, 9906: 660, 9907: 500, 9908: 1475, 9909: 370,  # 37 kW 415 V 50 Hz 4-pole
        }
        self.par.update(params or {})
        self.supply = supply
        self.cw = 0
        self.ref1 = 0
        self.ref2 = 0
        self.state = INHIBITED
        self.local = False
        self.load = load
        self.freq = 0.0         # output frequency, Hz, negative = reverse
        self.prev_freq = 0.0
        self.target = 0.0
        self.speed = 0.0        # rpm
        self.torque = 0.0       # %
        self.amps = 0.0
        self.kw = 0.0
        self.volts = 0.0
        self.dc = 1.35 * supply
        self.temp = 30.0
        self.run_hours = 1234.0
        self.kwh = 12345.0
        self.last_rx = None     # comm-loss supervision starts with the first message
        self.comm_alarm = False
        self.cable_cut = False
        self.boot_until = 0.0
        self.last_tick = None
        self.ignored_cw = None
        self.fault_index = 0

    # --- scaling ------------------------------------------------------------
    @property
    def scalar(self):
        return self.par[9904] == 3

    def nominal(self):
        return max(self.par[9907] / 10.0, 1.0), max(self.par[9908], 1)

    def fmax(self):
        fn, nn = self.nominal()
        return max(self.par[2008] / 10.0 if self.scalar else self.par[2002] * fn / nn, 1.0)

    def reference_hz(self):
        if self.par[1103] != 8:  # REF1 doesn't come from COMM (e.g. AI1 at 0 %)
            return 0.0
        fn, nn = self.nominal()
        frac = s16(self.ref1) / 20000.0
        hz = frac * s16(self.par[1105]) / 10.0 if self.scalar else frac * s16(self.par[1105]) * fn / nn
        hz = max(-self.fmax(), min(self.fmax(), hz))
        if self.par[1003] == 1:  # DIRECTION = FORWARD
            hz = max(hz, 0.0)
        elif self.par[1003] == 2:  # REVERSE
            hz = -abs(hz)
        return hz

    def describe_ref(self):
        fn, nn = self.nominal()
        frac = self.ref1 / 20000.0
        if self.scalar:
            hz = frac * s16(self.par[1105]) / 10.0
            return "%.1f Hz (%.0f rpm)" % (hz, hz * nn / fn)
        return "%.0f rpm" % (frac * s16(self.par[1105]))

    # --- control ------------------------------------------------------------
    def effective_cw(self):
        if self.local or self.par[1001] != 10:  # keypad or another start source has control
            return KEYPAD_STOP
        return self.cw

    def run_enabled(self):
        return self.par[1601] in (0, 7)  # NOT SEL or COMM; a digital input is never closed here

    def tick(self, now):
        dt = 0.0 if self.last_tick is None else min(now - self.last_tick, 0.5)
        self.last_tick = now
        self.supervise_comms(now)
        cw = self.effective_cw()
        self.step_state(cw)
        self.step_ramp(cw, dt)
        self.step_motor(dt)

    def supervise_comms(self, now):
        quiet = self.last_rx is not None and now - self.last_rx > self.par[3019] / 10.0
        self.comm_alarm = quiet and self.par[3018] not in (0, 1)
        if quiet and self.par[3018] == 1 and self.state != TRIPPED:
            self.trip(28, "no messages for %.1f s (3019)" % (self.par[3019] / 10.0))

    def step_state(self, cw):
        s = self.state
        if s == TRIPPED:
            return
        off1, off2, off3 = not cw & CW_OFF1, not cw & CW_OFF2, not cw & CW_OFF3
        run = bool(cw & CW_ENABLE) and self.run_enabled()
        moving = abs(self.freq) > 0.05
        if s == OFF3_ACTIVE:  # a quick stop always finishes
            new = OFF3_ACTIVE if moving else INHIBITED
        elif off2:
            new = INHIBITED
        elif off3:
            new = OFF3_ACTIVE if s in (RUNNING, OFF1_ACTIVE) and moving else INHIBITED
        elif s == INHIBITED:
            new = READY_ON if off1 else INHIBITED
        elif s == READY_ON:
            new = READY_ON if off1 else READY_RUN
        elif s == READY_RUN:
            new = READY_ON if off1 else RUNNING if run else READY_RUN
        elif s == RUNNING:
            new = OFF1_ACTIVE if off1 else RUNNING if run else READY_RUN
        else:  # OFF1_ACTIVE: ramping down after STOP
            new = RUNNING if not off1 and run else OFF1_ACTIVE if moving else READY_ON
        if new != s:
            self.state = new
            self.log("Drive: " + STATE_NAMES[new])

    def step_ramp(self, cw, dt):
        if self.state not in MODULATING:
            self.freq = self.target = 0.0  # output off: the drive shows zero while the motor coasts
            return
        target = 0.0
        if self.state == RUNNING:
            target = self.reference_hz() if cw & CW_RAMP_IN_ZERO else 0.0
            if not cw & CW_RAMP_OUT_ZERO:
                self.freq, self.target = 0.0, target
                return
            if not cw & CW_RAMP_HOLD:
                self.target = target
                return
        self.target = target
        if self.state == OFF3_ACTIVE:
            ramp = self.par[2208]
        elif abs(target) > abs(self.freq) and target * self.freq >= 0:
            ramp = self.par[2202]
        else:
            ramp = self.par[2203]
        step = self.fmax() / max(ramp / 10.0, 0.1) * dt
        if abs(target - self.freq) <= step:
            self.freq = target
        else:
            self.freq += step if target > self.freq else -step

    def step_motor(self, dt):
        fn, nn = self.nominal()
        rated_amps = max(self.par[9906] / 10.0, 0.1)
        f = self.freq
        x = abs(f) / fn
        sign = -1.0 if f < 0 else 1.0
        accel = (abs(f) - abs(self.prev_freq)) / dt if dt > 0 else 0.0  # Hz/s, + = speeding up
        self.prev_freq = f
        if self.state in MODULATING:
            torque = self.load * 80.0 * x * x                    # pump: torque rises with speed squared
            torque += max(-20.0, min(20.0, accel * 1.5))          # plus acceleration torque
            self.torque = sign * torque
            magnetising = 0.35 * min(1.0, 0.4 + x)
            self.amps = rated_amps * math.hypot(magnetising, 0.9 * torque / 100.0)
            if self.scalar:  # open loop: the motor slips more under load
                pp = max(1, round(fn * 60.0 / nn))
                slip = (fn * 60.0 / pp - nn) * torque / 100.0
                self.speed = sign * max(0.0, abs(f) * 60.0 / pp - slip)
            else:
                self.speed = f * nn / fn
            self.kw = self.par[9909] / 10.0 * self.torque / 100.0 * self.speed / nn
            self.volts = self.par[9905] * max(0.04, min(1.0, x))
        else:
            self.torque = self.amps = self.kw = self.volts = self.speed = 0.0
        # DC bus is about 1.35 x the supply: sags under load, rises when braking
        dc_target = 1.35 * self.supply - 0.05 * self.amps + max(0.0, -self.kw) * 4.0
        self.dc += (dc_target - self.dc) * min(1.0, dt * 2.0)
        temp_target = 30.0 + 35.0 * (self.amps / rated_amps) ** 2
        self.temp += (temp_target - self.temp) * min(1.0, dt / 90.0)
        if self.state in MODULATING:
            self.run_hours += dt / 3600.0
            self.kwh += max(0.0, self.kw) * dt / 3600.0

    def status_word(self):
        s = self.state
        cw = self.effective_cw()
        sw = 0
        if s in (READY_ON, READY_RUN, RUNNING):
            sw |= SW_RDY_ON
        if s in (READY_RUN, RUNNING):
            sw |= SW_RDY_RUN
        if s == RUNNING:
            sw |= SW_RDY_REF
        if s == TRIPPED:
            sw |= SW_TRIPPED
        if cw & CW_OFF2:
            sw |= SW_OFF2_STA
        if cw & CW_OFF3 and s != OFF3_ACTIVE:
            sw |= SW_OFF3_STA
        if s == INHIBITED:
            sw |= SW_SWC_ON_INHIB
        if self.comm_alarm or not self.run_enabled():
            sw |= SW_ALARM
        if s == RUNNING and abs(self.freq - self.target) < 0.1:
            sw |= SW_AT_SETPOINT
        if not self.local:
            sw |= SW_REMOTE
        if self.run_enabled():
            sw |= SW_EXT_RUN_ENABLE
        return sw

    # --- registers ----------------------------------------------------------
    def read(self, reg):
        """reg is the register number without the leading 4 (40001 -> 1). None = not available."""
        if reg == 1:
            return self.cw
        if reg == 2:
            return self.ref1 & 0xFFFF
        if reg == 3:
            return self.ref2 & 0xFFFF
        if reg == 4:
            return self.status_word()
        if reg == 5:
            return u16(self.speed)       # ACT1
        if reg == 6:
            return u16(self.amps * 10)   # ACT2
        if 101 <= reg <= 199:
            return self.actual(reg)
        return self.par.get(reg)

    def actual(self, par):
        on = self.state in MODULATING

        def jitter(amount):
            return random.uniform(-amount, amount) if on else 0.0

        if par in (101, 102):
            return u16(self.speed + jitter(1.0))
        if par == 103:
            return u16(self.freq * 10)
        if par == 104:
            return u16(max(0.0, self.amps + jitter(0.3)) * 10)
        if par == 105:
            return u16((self.torque + jitter(0.6)) * 10)
        if par == 106:
            return u16((self.kw + jitter(0.15)) * 10)
        if par == 107:
            return u16(self.dc + random.uniform(-1.5, 1.5))
        if par == 109:
            return u16(self.volts)
        if par == 110:
            return u16(self.temp * 10)
        if par == 111:  # EXTERNAL REF 1: rpm, or 0.1 Hz in scalar mode
            return u16(s16(self.ref1) / 20000.0 * s16(self.par[1105]))
        if par == 112:
            return u16(s16(self.ref2) / 10.0)
        if par == 113:  # CTRL LOCATION: 0 LOCAL, 1 EXT1
            return 0 if self.local else 1
        if par == 114:
            return u16(self.run_hours)
        if par == 115:
            return u16(self.kwh)
        if par in (120, 121, 126):
            return 0
        if par in (124, 125):  # AO 4-20 mA following the output frequency
            return u16((4.0 + 16.0 * abs(self.freq) / self.fmax()) * 10)
        return None

    def write(self, reg, value):
        """Returns 0, or a Modbus exception code."""
        if reg == 1:
            self.set_cw(value)
            return 0
        if reg == 2:
            if s16(value) != self.ref1:
                self.ref1 = s16(value)
                self.log("REF1 %d = %s" % (self.ref1, self.describe_ref()))
            return 0
        if reg == 3:
            self.ref2 = s16(value)
            return 0
        if reg not in self.par or reg // 100 in (1, 3, 4):
            return 2  # not a writable parameter
        if reg == 9904 and value not in (1, 2, 3):
            return 3
        if reg == 1607:
            if value == 1:
                self.log("1607 PARAM SAVE: parameters saved")
            return 0
        if value != self.par[reg]:
            self.log("Parameter %04d = %d (was %d)" % (reg, value, self.par[reg]))
            self.par[reg] = value
        return 0

    def set_cw(self, value):
        if value and not value & CW_REMOTE:  # DRV LIM: without bit 10 the drive keeps its last command
            if value != self.ignored_cw:
                self.log("CW 0x%04X ignored: bit 10 (0x0400) not set" % value)
            self.ignored_cw = value
            return
        self.ignored_cw = None
        old, self.cw = self.cw, value
        if value != old:
            self.log("CW 0x%04X %s" % (value, CW_NAMES.get(value, "")))
        if value & CW_RESET and not old & CW_RESET:
            if self.local:
                self.log("Reset ignored: drive is in LOCAL")
            elif self.par[1604] != 8:
                self.log("Reset ignored: 1604 FAULT RESET SEL is not 8 (COMM)")
            else:
                self.reset("bus")

    # --- events -------------------------------------------------------------
    def trip(self, code, why=""):
        if self.state == TRIPPED:
            return
        self.par[413], self.par[412], self.par[401] = self.par[412], self.par[401], code
        self.state = TRIPPED
        self.freq = 0.0
        self.log("FAULT %d %s%s" % (code, FAULT_NAMES.get(code, ""), " - " + why if why else ""))

    def reset(self, source):
        if self.state == TRIPPED:
            self.state = INHIBITED
            self.log("Fault reset (%s)" % source)

    def trip_test_fault(self):
        code = TEST_FAULTS[self.fault_index % len(TEST_FAULTS)]
        self.fault_index += 1
        self.trip(code, "from the keyboard")

    def toggle_local(self):
        self.local = not self.local
        self.log("Keypad: LOCAL - bus commands ignored, motor stops" if self.local
                 else "Keypad: REMOTE - following the bus control word again")

    def power_cycle(self, now):
        self.cw = self.ref1 = self.ref2 = 0
        self.state = INHIBITED
        self.freq = 0.0
        self.last_rx = None
        self.boot_until = now + 2.0
        self.log("Power cycle: control word and reference cleared, back in 2 s")

    def toggle_cable(self):
        self.cable_cut = not self.cable_cut
        self.log("Cable CUT: the drive hears nothing" if self.cable_cut else "Cable repaired")

    def change_load(self, delta):
        self.load = max(0.0, min(1.5, round(self.load + delta, 1)))
        self.log("Load %.0f %%" % (self.load * 100))

    def heard(self, now):
        self.last_rx = now

    def deaf(self, now):
        return self.cable_cut or now < self.boot_until

    def summary(self, now):
        sw = self.status_word()
        flags = [name for on, name in ((self.local, "LOCAL"), (self.cable_cut, "CABLE CUT"),
                                       (now < self.boot_until, "BOOTING"), (self.comm_alarm, "COMM ALARM")) if on]
        return "%-19s| CW %04X REF %6d | %5.0f rpm %5.1f Hz %5.1f A %6.1f %% %5.1f kW | %3.0f V %4.1f C | load %3.0f%% %s" % (
            panel_state(sw), self.cw, self.ref1, self.speed, self.freq, self.amps, self.torque, self.kw,
            self.dc, self.temp, self.load * 100, " ".join(flags))


def frame_length(buf):
    """Length of the request at the start of buf: None = need more bytes, 0 = unknown function."""
    if len(buf) < 2:
        return None
    fc = buf[1]
    if fc in (1, 2, 3, 4, 5, 6):
        return 8
    if fc in (15, 16):
        return None if len(buf) < 7 else 9 + buf[6]
    return 0


def describe(frame):
    fc = frame[1]
    reg = struct.unpack(">H", frame[2:4])[0] + 40001
    if fc in (3, 4):
        return "read %d x%d" % (reg, struct.unpack(">H", frame[4:6])[0])
    if fc == 6:
        return "write %d = %d" % (reg, struct.unpack(">H", frame[4:6])[0])
    if fc == 16:
        count = struct.unpack(">H", frame[4:6])[0]
        values = struct.unpack(">%dH" % count, frame[7:7 + 2 * count])
        return "write %d = %s" % (reg, ", ".join("0x%04X" % v if i == 0 and reg == 40001 else str(v)
                                                for i, v in enumerate(values)))
    return "function %d" % fc


class RtuServer:
    """Splits the byte stream into frames, answers them, and removes the adapter's echo if it has one."""

    def __init__(self, drive, slave_id, log, verbose=False):
        self.drive = drive
        self.slave_id = slave_id
        self.log = log
        self.verbose = verbose
        self.buf = bytearray()
        self.last_byte = 0.0
        self.last_reply = None
        self.echo = False
        self.bytes = self.frames = self.exceptions = self.junk = 0

    def receive(self, data, now):
        """Feed received bytes; returns the replies to send."""
        replies = []
        if data:
            if self.drive.deaf(now):
                return replies
            self.bytes += len(data)
            self.buf += data
            self.last_byte = now
        elif self.buf and now - self.last_byte > 0.05:  # a frame that never completed
            self.junk += len(self.buf)
            self.buf.clear()
        while self.buf:
            if self.last_reply is not None:
                r = self.last_reply
                if len(self.buf) < len(r) and r.startswith(bytes(self.buf)):
                    break  # may be our own reply coming back
                # Only a read reply can't be mistaken for a request, so that is what turns echo removal on
                if self.buf.startswith(r) and (self.echo or r[1] in (3, 4) or r[1] & 0x80):
                    del self.buf[:len(r)]
                    self.last_reply = None
                    if not self.echo:
                        self.echo = True
                        self.log("The adapter echoes what it sends: removing the echo")
                    continue
                self.last_reply = None
            n = frame_length(self.buf)
            if n == 0:
                del self.buf[0]
                self.junk += 1
                continue
            if n is None or len(self.buf) < n:
                break
            frame = bytes(self.buf[:n])
            if crc16(frame) != 0:  # noise or a broken frame: resynchronise
                del self.buf[0]
                self.junk += 1
                continue
            del self.buf[:n]
            reply = self.handle(frame, now)
            if self.verbose and frame[0] in (self.slave_id, 0):
                self.log("%-32s %s  ->  %s" % (describe(frame), frame.hex(" "), reply.hex(" ") if reply else "-"))
            if reply:
                replies.append(reply)
                self.last_reply = reply
        return replies

    def handle(self, frame, now):
        slave, fc = frame[0], frame[1]
        if slave not in (self.slave_id, 0):
            return None
        self.frames += 1
        self.drive.heard(now)

        def exception(code):
            self.exceptions += 1
            return None if slave == 0 else with_crc(bytes((slave, fc | 0x80, code)))

        if fc in (3, 4):
            addr, count = struct.unpack(">HH", frame[2:6])
            if not 1 <= count <= 125:
                return exception(3)
            values = []
            for a in range(addr, addr + count):
                v = self.drive.read(a + 1)
                if v is None:
                    return exception(2)
                values.append(v)
            body = bytes((slave, fc, 2 * count)) + struct.pack(">%dH" % count, *values)
        elif fc == 6:
            addr, value = struct.unpack(">HH", frame[2:6])
            code = self.drive.write(addr + 1, value)
            if code:
                return exception(code)
            body = frame[:6]
        elif fc == 16:
            addr, count = struct.unpack(">HH", frame[2:6])
            if not 1 <= count <= 123 or frame[6] != 2 * count:
                return exception(3)
            for i, value in enumerate(struct.unpack(">%dH" % count, frame[7:7 + 2 * count])):
                code = self.drive.write(addr + 1 + i, value)
                if code:
                    return exception(code)
            body = frame[:6]
        else:
            return exception(1)
        return None if slave == 0 else with_crc(body)


class Console:
    """Event lines scroll; the status line at the bottom is redrawn in place."""

    def __init__(self):
        self.tty = sys.stdout.isatty()
        self.width = max(40, shutil.get_terminal_size((120, 25)).columns - 1)
        self.status = ""
        self.next_plain = 0.0

    def event(self, text):
        if self.tty:
            sys.stdout.write("\r" + " " * self.width + "\r")
        sys.stdout.write("%s  %s\n" % (time.strftime("%H:%M:%S"), text))
        if self.tty:
            sys.stdout.write(self.status[:self.width])
        sys.stdout.flush()

    def show(self, status, now):
        self.status = status
        if self.tty:
            sys.stdout.write("\r" + status[:self.width].ljust(self.width))
            sys.stdout.flush()
        elif now >= self.next_plain:  # output redirected: a plain line every 5 s
            self.next_plain = now + 5.0
            self.event(status)


def read_keys():
    keys = []
    if msvcrt and interactive():
        while msvcrt.kbhit():
            keys.append(msvcrt.getwch())
    return keys


def handle_key(key, drive, server, log, now, settings_path=None):
    """Returns True to quit."""
    key = key.lower()
    if key == "q":
        return True
    if key == "f":
        drive.trip_test_fault()
    elif key == "r":
        drive.reset("keypad")
    elif key == "l":
        drive.toggle_local()
    elif key == "p":
        drive.power_cycle(now)
    elif key == "c":
        drive.toggle_cable()
    elif key in ("+", "="):
        drive.change_load(+0.1)
    elif key in ("-", "_"):
        drive.change_load(-0.1)
    elif key == "v":
        server.verbose = not server.verbose
        log("Showing every frame" if server.verbose else "Frames hidden")
    elif key == "e" and settings_path:
        try:
            os.startfile(settings_path)
            log("Opened %s: save it, then restart the simulator" % settings_path)
        except (OSError, AttributeError) as e:
            log("Could not open %s: %s" % (settings_path, e))
    elif key in ("h", "?"):
        log(KEY_HELP)
    return False


def interactive():
    """True when someone can type into this window."""
    if sys.stdin is None or not sys.stdin.isatty():
        return False
    if sys.platform == "win32":  # isatty() is also true for NUL; only a real console has a console mode
        import ctypes
        mode = ctypes.c_uint32()
        handle = msvcrt.get_osfhandle(sys.stdin.fileno())
        return bool(ctypes.windll.kernel32.GetConsoleMode(handle, ctypes.byref(mode)))
    return True


def fail(message):
    """Exit with a message; keep a double-clicked window open long enough to read it."""
    print(message)
    if msvcrt and interactive():
        print("Press any key to close.")
        msvcrt.getwch()
    sys.exit(1)


def choose_port():
    """The RS485 adapter: the only candidate, or ask which one."""
    from serial.tools import list_ports
    while True:
        # Skip the panel's own USB port (Espressif VID) in case it is plugged into this PC too
        ports = sorted((p for p in list_ports.comports() if p.vid != 0x303A), key=lambda p: p.device)
        if len(ports) == 1:
            return ports[0].device
        if not interactive():
            names = ", ".join("%s (%s)" % (p.device, p.description) for p in ports) or "none"
            fail("Choose the RS485 adapter with --port. Serial ports found: " + names)
        if not ports:
            answer = input("No serial ports found. Plug in the USB-RS485 adapter and press Enter (q to quit): ")
        else:
            print("Serial ports:")
            for i, p in enumerate(ports, 1):
                print("  %d  %-7s %s" % (i, p.device, p.description))
            answer = input("Which one is the RS485 adapter? Number, Enter to look again, q to quit: ")
        answer = answer.strip().lower()
        if answer == "q":
            sys.exit(0)
        if answer.isdigit() and 1 <= int(answer) <= len(ports):
            return ports[int(answer) - 1].device


def program_dir():
    """Folder of the .exe when built with PyInstaller, otherwise of this script."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def load_config(path):
    """The settings file over the defaults. Writes a default file when there is none."""
    created = False
    if not os.path.exists(path):
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(DEFAULT_CONFIG)
            created = True
        except OSError as e:
            print("Could not create %s (%s), using the defaults" % (path, e))
    cfg = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
    cfg.read_string(DEFAULT_CONFIG)
    try:
        cfg.read(path, encoding="utf-8")
    except configparser.Error as e:
        fail("Settings file %s: %s" % (path, e))
    return cfg, created


def settings_from_config(cfg, path):
    """Returns the connection settings, parameter values (raw, as the drive sends them) and simulation settings."""
    def text(section, key):
        return cfg.get(section, key).strip()

    def number(section, key, scale=1):
        value = text(section, key)
        try:
            return int(round(float(value) * scale))
        except ValueError:
            fail("Settings file %s: [%s] %s = %r is not a number" % (path, section, key, value))

    def choice(section, key, options):
        value = text(section, key)
        for option in options:
            if value.lower() == option.lower():
                return option
        fail("Settings file %s: [%s] %s must be %s, not %r" % (path, section, key, " or ".join(options), value))

    conn = {
        "port": text("Connection", "port"),
        "station": number("Connection", "station"),
        "baud": number("Connection", "baud"),
        "format": choice("Connection", "format", FORMATS),
    }
    vector = choice("Drive", "control_mode", ["scalar", "vector"]) == "vector"
    par = {
        9904: 1 if vector else 3,
        9905: number("Drive", "motor_voltage"),
        9906: number("Drive", "motor_current_a", 10),
        9907: number("Drive", "motor_frequency_hz", 10),
        9908: number("Drive", "motor_speed_rpm"),
        9909: number("Drive", "motor_power_kw", 10),
        1105: number("Drive", "ref1_max_rpm") if vector else number("Drive", "ref1_max_hz", 10),
        2002: number("Drive", "max_speed_rpm"),
        2008: number("Drive", "max_frequency_hz", 10),
        2202: number("Drive", "accel_time_s", 10),
        2203: number("Drive", "decel_time_s", 10),
        2208: number("Drive", "quick_stop_time_s", 10),
        1003: number("Drive", "direction"),
        3018: number("Drive", "comm_fault_function"),
        3019: number("Drive", "comm_fault_time_s", 10),
    }
    for key, value in cfg.items("Parameters"):
        try:
            par[int(key)] = (int(value, 16) if value.lower().startswith("0x") else int(value)) & 0xFFFF
        except ValueError:
            fail("Settings file %s: [Parameters] %s = %s: use a parameter number = whole number" % (path, key, value))
    for ok, problem in ((1 <= conn["station"] <= 247, "[Connection] station must be 1-247"),
                        (conn["baud"] > 0, "[Connection] baud must be above zero"),
                        (par[9907] > 0 and par[9908] > 0, "[Drive] motor frequency and speed must be above zero"),
                        (par[1105] > 0, "[Drive] ref1_max must be above zero"),
                        (par[1003] in (1, 2, 3), "[Drive] direction must be 1, 2 or 3"),
                        (par[3018] in (0, 1, 2, 3), "[Drive] comm_fault_function must be 0-3")):
        if not ok:
            fail("Settings file %s: %s" % (path, problem))
    sim = {"load": number("Simulation", "load_percent") / 100.0,
           "supply": float(number("Simulation", "supply_voltage"))}
    return conn, par, sim


def main():
    ap = argparse.ArgumentParser(
        description="ABB ACS550 simulator (Modbus RTU slave) for testing the Drive Remote panel. Settings come "
                    "from %s next to the program; these options override it." % CONFIG_NAME)
    ap.add_argument("--config", help="use this settings file instead")
    ap.add_argument("--port", help="RS485 adapter, e.g. COM4, or auto")
    ap.add_argument("--slave", type=int, help="station ID, 5302")
    ap.add_argument("--baud", type=int, help="5303")
    ap.add_argument("--format", choices=FORMATS, help="5304")
    ap.add_argument("--mode", choices=["scalar", "vector"], help="9904 MOTOR CTRL MODE")
    ap.add_argument("--verbose", action="store_true", help="show every frame (also the v key)")
    ap.add_argument("--seconds", type=float, help="stop after this many seconds (for scripted tests)")
    args = ap.parse_args()

    import serial  # pyserial

    print("ACS550 drive simulator %s - Drive Remote bench test tool" % SIM_VERSION)
    path = os.path.abspath(args.config or os.path.join(program_dir(), CONFIG_NAME))
    cfg, created = load_config(path)
    print("Settings: %s%s" % (path, " (new file with the default values)" if created else ""))
    for section, key, value in (("Connection", "port", args.port), ("Connection", "station", args.slave),
                                ("Connection", "baud", args.baud), ("Connection", "format", args.format),
                                ("Drive", "control_mode", args.mode)):
        if value is not None:
            cfg.set(section, key, str(value))
    conn, par, sim = settings_from_config(cfg, path)

    port = choose_port() if conn["port"].lower() == "auto" else conn["port"]
    fmt = conn["format"]
    parity = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}[fmt[1]]
    try:
        ser = serial.Serial(port, conn["baud"], bytesize=8, parity=parity, stopbits=int(fmt[2]), timeout=0)
    except serial.SerialException as e:
        from serial.tools import list_ports
        if port.upper() not in (p.device.upper() for p in list_ports.comports()):
            fail("%s isn't on this PC. Plug in the USB-RS485 adapter, or set port = auto in %s." % (port, path))
        fail("Cannot open %s: %s\nClose ModSim32 or any serial monitor that is using it." % (port, e))
    if sys.platform == "win32":
        import ctypes
        ctypes.windll.kernel32.SetConsoleTitleW("ACS550 simulator - %s" % port)

    console = Console()
    drive = Drive(conn["station"], conn["baud"], FORMATS.index(fmt), log=console.event, params=par,
                  load=sim["load"], supply=sim["supply"])
    server = RtuServer(drive, conn["station"], console.event, args.verbose)
    fn, nn = drive.nominal()
    print("Listening on %s: station %d, %d %s, %s" % (
        port, conn["station"], conn["baud"], fmt, "SCALAR (9904 = 3)" if drive.scalar else "VECTOR (9904 = 1)"))
    print("Motor %.1f kW %d V %.1f A %.1f Hz %d rpm, REF1 MAX %s, ramps %.1f / %.1f s" % (
        drive.par[9909] / 10.0, drive.par[9905], drive.par[9906] / 10.0, fn, nn,
        "%.1f Hz" % (drive.par[1105] / 10.0) if drive.scalar else "%d rpm" % drive.par[1105],
        drive.par[2202] / 10.0, drive.par[2203] / 10.0))
    print(KEY_HELP)

    start = time.monotonic()
    end = start + args.seconds if args.seconds else None
    next_status = 0.0
    hints = set()
    try:
        while end is None or time.monotonic() < end:
            now = time.monotonic()
            for reply in server.receive(ser.read(512), now):
                ser.write(reply)
            drive.tick(now)
            if any(handle_key(k, drive, server, console.event, now, path) for k in read_keys()):
                break
            if now >= next_status:
                next_status = now + 0.25
                console.show("%s| rx %d exc %d junk %d" % (drive.summary(now), server.frames, server.exceptions,
                                                          server.junk), now)
                # Wiring hints, once each
                if server.bytes == 0 and now - start > 5 and "quiet" not in hints:
                    hints.add("quiet")
                    console.event("Nothing received yet: check the A/B wires, the adapter's COM port, and that "
                                  "the panel is powered")
                if server.frames == 0 and server.junk > 40 and "junk" not in hints:
                    hints.add("junk")
                    console.event("Bytes arrive but no valid frames: swap A and B, or check the baud rate and "
                                  "format on the panel's Setup page")
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print()


if __name__ == "__main__":
    main()
