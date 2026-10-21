#pragma once
#include <Arduino.h>

// ABB ACS550 (firmware 3.1x) fieldbus definitions: ABB DRV LIM profile
// (5305 = 0), register map, parameter scaling and fault names.

namespace acs550 {

// Holding registers (4xxyy = parameter xx.yy, 40001-40004 = fieldbus words)
constexpr uint16_t REG_CONTROL = 40001;
constexpr uint16_t REG_REF1 = 40002;
constexpr uint16_t REG_STATUS = 40004;
constexpr uint16_t REG_LAST_FAULT = 40401;

inline uint16_t parToReg(uint16_t par) { return 40000 + par; }

// Control words (ABB DRV LIM). Bit 10 (0x0400) = fieldbus control enabled.
constexpr uint16_t CW_STOP = 0x0476;   // ready / ramp stop
constexpr uint16_t CW_RUN = 0x047F;
constexpr uint16_t CW_COAST = 0x047D;  // OFF2
constexpr uint16_t CW_QUICK = 0x047B;  // OFF3, uses 2208 EMERG DEC TIME
constexpr uint16_t CW_RESET = 0x04F6;  // bit 7 rising edge
constexpr uint16_t CW_REMOTE_BIT = 0x0400;

// Reference 1 on the bus: +-20000 = +-1105 REF1 MAX
constexpr float REF_FULL_SCALE = 20000.0f;

// Status word bits
constexpr uint16_t SW_READY_ON = 1 << 0;
constexpr uint16_t SW_RUNNING = 1 << 2;
constexpr uint16_t SW_FAULT = 1 << 3;
constexpr uint16_t SW_INHIBITED = 1 << 6;
constexpr uint16_t SW_AT_SETPOINT = 1 << 8;
constexpr uint16_t SW_REMOTE = 1 << 9;

constexpr int STATUS_BIT_COUNT = 13;
struct StatusBit {
    const char *name;
    char kind;  // ' ' normal, 'b' bad, 'w' warning
};
extern const StatusBit STATUS_BITS[STATUS_BIT_COUNT];

enum class RunState : uint8_t { Offline, Fault, Running, Local, Inhibited, Ready, Stopped };
RunState runState(bool online, bool haveStatus, uint16_t sw);
const char *runStateText(RunState rs, uint16_t sw);

const char *faultName(uint16_t code);

// Parameter scaling ------------------------------------------------------
struct Scaling {
    uint16_t div;      // engineering value = raw / div
    uint8_t dp;        // decimals to show
    const char *unit;
    bool isSigned;
};

// scalarMode: 9904 MOTOR CTRL MODE == 3 (SCALAR:FREQ). Speed-reference
// parameters (1104/1105/...) are then in 0.1 Hz instead of 1 rpm.
Scaling scalingFor(uint16_t par, bool scalarMode);
const char *parName(uint16_t par);  // "" when not in the table
bool isReadOnly(uint16_t par);      // actual signals / fault history

float rawToEng(uint16_t par, uint16_t raw, bool scalarMode);
bool engToRaw(uint16_t par, float eng, bool scalarMode, uint16_t *raw);
void formatEng(uint16_t par, uint16_t raw, bool scalarMode, char *buf, size_t len);

}  // namespace acs550
