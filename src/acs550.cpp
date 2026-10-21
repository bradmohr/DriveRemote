#include "acs550.h"

#include <math.h>

namespace acs550 {

const StatusBit STATUS_BITS[STATUS_BIT_COUNT] = {
    {"Ready to switch on", ' '}, {"Ready to run", ' '},     {"Running (ref enabled)", ' '},
    {"FAULT", 'b'},              {"OFF2 inactive", ' '},    {"OFF3 inactive", ' '},
    {"Switch-on inhibited", 'w'}, {"ALARM", 'w'},           {"At setpoint", ' '},
    {"Remote (fieldbus)", ' '},  {"Above limit", ' '},      {"EXT2 selected", ' '},
    {"Ext run enable", ' '},
};

RunState runState(bool online, bool haveStatus, uint16_t sw)
{
    if (!online || !haveStatus) return RunState::Offline;
    if (sw & SW_FAULT) return RunState::Fault;
    if (sw & SW_RUNNING) return RunState::Running;
    if (!(sw & SW_REMOTE)) return RunState::Local;
    if (sw & SW_INHIBITED) return RunState::Inhibited;
    return (sw & SW_READY_ON) ? RunState::Ready : RunState::Stopped;
}

const char *runStateText(RunState rs, uint16_t sw)
{
    switch (rs) {
    case RunState::Offline: return "NO COMMS";
    case RunState::Fault: return "FAULT";
    case RunState::Running: return (sw & SW_AT_SETPOINT) ? "RUNNING - AT SPEED" : "RUNNING";
    case RunState::Local: return "LOCAL / NOT REMOTE";
    case RunState::Inhibited: return "SWITCH-ON INHIBITED";
    case RunState::Ready: return "READY";
    case RunState::Stopped: return "STOPPED";
    }
    return "?";
}

const char *faultName(uint16_t code)
{
    switch (code) {
    case 0: return "None";
    case 1: return "OVERCURRENT";
    case 2: return "DC OVERVOLT";
    case 3: return "DEV OVERTEMP";
    case 4: return "SHORT CIRC";
    case 6: return "DC UNDERVOLT";
    case 7: return "AI1 LOSS";
    case 8: return "AI2 LOSS";
    case 9: return "MOT OVERTEMP";
    case 10: return "PANEL LOSS";
    case 11: return "ID RUN FAIL";
    case 12: return "MOTOR STALL";
    case 14: return "EXT FAULT 1";
    case 15: return "EXT FAULT 2";
    case 16: return "EARTH FAULT";
    case 18: return "THERM FAIL";
    case 19: return "OPEX LINK";
    case 20: return "OPEX PWR";
    case 21: return "CURR MEAS";
    case 22: return "SUPPLY PHASE";
    case 24: return "OVERSPEED";
    case 26: return "DRIVE ID";
    case 27: return "CONFIG FILE";
    case 28: return "SERIAL 1 ERR (comm loss)";
    case 29: return "EFB CON FILE";
    case 34: return "MOTOR PHASE";
    case 35: return "OUTP WIRING";
    }
    return "see manual";
}

// ---------------------------------------------------------------------------
// Parameter table. Resolutions from the ACS550-U1 user's manual (fw 3.1x).
// Parameters not listed are passed through as raw integers.
// ---------------------------------------------------------------------------
enum : uint8_t { F_SIGNED = 1, F_SPEEDREF = 2 };

struct ParInfo {
    uint16_t par;
    const char *name;
    uint16_t div;
    const char *unit;
    uint8_t flags;
};

static const ParInfo PARAMS[] = {
    {1, "CONTROL WORD", 1, "", 0},
    {4, "STATUS WORD", 1, "", 0},
    {101, "SPEED & DIR", 1, "rpm", F_SIGNED},
    {102, "SPEED", 1, "rpm", F_SIGNED},
    {103, "OUTPUT FREQ", 10, "Hz", F_SIGNED},
    {104, "CURRENT", 10, "A", 0},
    {105, "TORQUE", 10, "%", F_SIGNED},
    {106, "POWER", 10, "kW", F_SIGNED},
    {107, "DC BUS VOLTAGE", 1, "V", 0},
    {109, "OUTPUT VOLTAGE", 1, "V", 0},
    {110, "DRIVE TEMP", 10, "C", F_SIGNED},
    {111, "EXTERNAL REF 1", 1, "rpm", F_SPEEDREF},
    {112, "EXTERNAL REF 2", 10, "%", F_SIGNED},
    {113, "CTRL LOCATION", 1, "", 0},
    {114, "RUN TIME (R)", 1, "h", 0},
    {115, "KWH COUNTER (R)", 1, "kWh", 0},
    {120, "AI 1", 10, "%", 0},
    {121, "AI 2", 10, "%", 0},
    {124, "AO 1", 10, "mA", 0},
    {125, "AO 2", 10, "mA", 0},
    {126, "PID 1 OUTPUT", 10, "%", F_SIGNED},
    {401, "LAST FAULT", 1, "", 0},
    {412, "PREVIOUS FAULT 1", 1, "", 0},
    {413, "PREVIOUS FAULT 2", 1, "", 0},
    {1104, "REF1 MIN", 1, "rpm", F_SPEEDREF},
    {1105, "REF1 MAX", 1, "rpm", F_SPEEDREF},
    {2001, "MINIMUM SPEED", 1, "rpm", F_SIGNED},
    {2002, "MAXIMUM SPEED", 1, "rpm", 0},
    {2003, "MAX CURRENT", 10, "A", 0},
    {2007, "MINIMUM FREQ", 10, "Hz", F_SIGNED},
    {2008, "MAXIMUM FREQ", 10, "Hz", 0},
    {2202, "ACCELER TIME 1", 10, "s", 0},
    {2203, "DECELER TIME 1", 10, "s", 0},
    {2205, "ACCELER TIME 2", 10, "s", 0},
    {2206, "DECELER TIME 2", 10, "s", 0},
    {2208, "EMERG DEC TIME", 10, "s", 0},
    {3019, "COMM FAULT TIME", 10, "s", 0},
    {9904, "MOTOR CTRL MODE", 1, "", 0},
    {9905, "MOTOR NOM VOLT", 1, "V", 0},
    {9906, "MOTOR NOM CURR", 10, "A", 0},
    {9907, "MOTOR NOM FREQ", 10, "Hz", 0},
    {9908, "MOTOR NOM SPEED", 1, "rpm", 0},
    {9909, "MOTOR NOM POWER", 10, "", 0},
};

static const ParInfo *find(uint16_t par)
{
    for (const ParInfo &p : PARAMS)
        if (p.par == par) return &p;
    return nullptr;
}

Scaling scalingFor(uint16_t par, bool scalarMode)
{
    const ParInfo *p = find(par);
    if (!p) return {1, 0, "", false};
    if (p->flags & F_SPEEDREF) {
        return scalarMode ? Scaling{10, 1, "Hz", true} : Scaling{1, 0, "rpm", true};
    }
    return {p->div, (uint8_t)(p->div >= 100 ? 2 : p->div >= 10 ? 1 : 0), p->unit, (p->flags & F_SIGNED) != 0};
}

const char *parName(uint16_t par)
{
    const ParInfo *p = find(par);
    return p ? p->name : "";
}

bool isReadOnly(uint16_t par)
{
    const uint16_t group = par / 100;
    if (par == 4) return true;
    if (group == 1) return !(par >= 135 && par <= 138);  // 0135-0138 COMM VALUE 1-4 are writable
    return group == 3 || group == 4;
}

float rawToEng(uint16_t par, uint16_t raw, bool scalarMode)
{
    Scaling s = scalingFor(par, scalarMode);
    float v = s.isSigned ? (float)(int16_t)raw : (float)raw;
    return v / s.div;
}

bool engToRaw(uint16_t par, float eng, bool scalarMode, uint16_t *raw)
{
    Scaling s = scalingFor(par, scalarMode);
    long v = lroundf(eng * s.div);
    if (v < -32768 || v > 65535) return false;
    if (v < 0 && !s.isSigned) return false;
    *raw = (uint16_t)(v & 0xFFFF);
    return true;
}

void formatEng(uint16_t par, uint16_t raw, bool scalarMode, char *buf, size_t len)
{
    Scaling s = scalingFor(par, scalarMode);
    if (s.div == 1) {
        snprintf(buf, len, "%ld", s.isSigned ? (long)(int16_t)raw : (long)raw);
    } else {
        snprintf(buf, len, "%.*f", s.dp, rawToEng(par, raw, scalarMode));
    }
}

}  // namespace acs550
