#pragma once
#include <Arduino.h>

#include "modbus_rtu.h"

// ACS550 controller: owns the RS485 link in its own task, polls actual values,
// executes commands from the panel and MQTT, and publishes every value it
// reads to MQTT as <base>/<parameter>.

constexpr int DRIVE_VALUE_COUNT = 8;
struct DriveValueDef {
    uint16_t par;
    const char *label;
};
extern const DriveValueDef DRIVE_VALUES[DRIVE_VALUE_COUNT];

enum class CmdSource : uint8_t { Panel, Mqtt };

enum class DriveCmdType : uint8_t {
    Start,
    Stop,
    Coast,
    QuickStop,
    ResetFault,
    SetSpeed,      // f = rpm. i != 0: sign of f also sets the direction (MQTT 0002)
    SetDirection,  // i = +1 forward / -1 reverse
    RawControl,    // i = control word written as-is
    ReadParam,     // par
    WriteParam,    // par, f = value in engineering units
};

struct DriveCmd {
    DriveCmdType type;
    CmdSource src;
    uint16_t par;
    int32_t i;
    float f;
};

struct ParamResult {
    uint32_t seq;  // bumps on every completed read/write
    uint16_t par;
    bool ok;
    bool write;
    uint16_t raw;
    char text[96];
};

struct DriveSnapshot {
    bool online;       // drive answering
    bool scaled;       // 9904/1105 read since (re)connect
    bool scalarMode;   // 9904 = 3: references are in Hz
    float ref1Max;     // 1105, rpm or Hz
    float nomFreq;     // 9907 Hz
    float nomSpeed;    // 9908 rpm
    float spMaxRpm;    // largest setpoint (= 1105 expressed in rpm)
    float gaugeMaxRpm;

    bool swValid;
    uint16_t sw;
    bool valValid[DRIVE_VALUE_COUNT];
    bool valNa[DRIVE_VALUE_COUNT];
    uint16_t valRaw[DRIVE_VALUE_COUNT];
    bool faultValid;
    uint16_t lastFault;

    bool cwActive;     // panel is writing the control word
    uint16_t cw;
    int16_t refWord;
    float setpointRpm; // magnitude
    int8_t dir;        // +1 / -1

    MbStats stats;
    uint32_t cycleMs;
    ParamResult param;
};

void driveStart();
bool driveSend(const DriveCmd &cmd);  // non-blocking
void driveGetSnapshot(DriveSnapshot *out);
