#include "drive.h"

#include <math.h>

#include "acs550.h"
#include "app_config.h"
#include "esp_task_wdt.h"
#include "logger.h"
#include "net.h"
#include "settings.h"

using namespace acs550;

const DriveValueDef DRIVE_VALUES[DRIVE_VALUE_COUNT] = {
    {102, "Motor speed"}, {103, "Output freq"}, {104, "Current"},      {105, "Torque"},
    {106, "Power"},       {107, "DC bus"},      {109, "Output volts"}, {110, "Drive temp"},
};

// Block reads of DRIVE_VALUES; a block the drive refuses is read one by one.
struct ReadGroup {
    uint16_t firstPar;
    uint8_t firstIdx;
    uint8_t count;
    bool split;
};
static ReadGroup s_groups[] = {{102, 0, 6, false}, {109, 6, 2, false}};

static ModbusRtu s_mb;
static QueueHandle_t s_cmdQ = nullptr;
static SemaphoreHandle_t s_snapMutex = nullptr;
static DriveSnapshot s_shared;  // copy handed to the UI
static DriveSnapshot st;        // working state, drive task only

static uint8_t s_slave = 1;
static uint16_t s_pollMs = 250;
static uint32_t s_baud = 0;
static uint8_t s_format = 0xFF;
static uint32_t s_minPubMs = 1000;
static uint32_t s_settingsVer = 0;

static int s_failStreak = 0;
static bool s_needResync = true;
static uint32_t s_nextResyncMs = 0;
static uint32_t s_resyncFails = 0;
static char s_stateText[24] = "";
static bool s_wasFaulted = false;

// ---------------------------------------------------------------------------
// MQTT publishing: changed values go out at most every s_minPubMs, unchanged
// ones are refreshed every MQTT_HEARTBEAT_MS.
// ---------------------------------------------------------------------------
struct PubEntry {
    bool used;
    uint16_t par;
    uint32_t lastMs;
    bool hasPending;
    char last[16];
    char pending[16];
};
static PubEntry s_pub[40];

static PubEntry *pubSlot(uint16_t par)
{
    for (PubEntry &e : s_pub)
        if (e.used && e.par == par) return &e;
    PubEntry *slot = nullptr;
    for (PubEntry &e : s_pub)
        if (!e.used) {
            slot = &e;
            break;
        }
    if (!slot) {  // evict the entry published longest ago
        slot = &s_pub[0];
        for (PubEntry &e : s_pub)
            if (e.lastMs < slot->lastMs) slot = &e;
    }
    *slot = PubEntry{};
    slot->used = true;
    slot->par = par;
    return slot;
}

static void publishText(uint16_t par, const char *payload, bool now)
{
    PubEntry *e = pubSlot(par);
    if (now) {
        netPublishPar(par, payload);
        strlcpy(e->last, payload, sizeof(e->last));
        e->lastMs = millis();
        e->hasPending = false;
    } else if (strcmp(payload, e->last) == 0) {
        e->hasPending = false;
    } else {
        strlcpy(e->pending, payload, sizeof(e->pending));
        e->hasPending = true;
    }
}

static void publishRaw(uint16_t par, uint16_t raw, bool now)
{
    char buf[16];
    formatEng(par, raw, st.scalarMode, buf, sizeof(buf));
    publishText(par, buf, now);
}

static void publishSetpoint()
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", st.dir * st.setpointRpm);
    publishText(2, buf, true);
}

static void flushPublishes(bool all)
{
    const uint32_t now = millis();
    for (PubEntry &e : s_pub) {
        if (!e.used) continue;
        bool send = false;
        if (e.hasPending && (all || now - e.lastMs >= s_minPubMs)) {
            strlcpy(e.last, e.pending, sizeof(e.last));
            e.hasPending = false;
            send = true;
        } else if (e.last[0] && (all || now - e.lastMs >= MQTT_HEARTBEAT_MS)) {
            send = true;
        }
        if (!send) continue;
        netPublishPar(e.par, e.last);
        e.lastMs = now;
    }
}

static void publishState(bool force)
{
    const char *text = runStateText(runState(st.online, st.swValid, st.sw), st.sw);
    if (!force && strcmp(text, s_stateText) == 0) return;
    if (strcmp(text, s_stateText) != 0) logf(LOG_INFO, "State: %s", text);
    strlcpy(s_stateText, text, sizeof(s_stateText));
    netPublish("state", text, true);
}

// ---------------------------------------------------------------------------
// Scaling: the bus reference is +-20000 = +-1105 REF1 MAX. 1105 is in rpm in
// vector mode and in Hz in scalar mode (9904 = 3), where rpm is converted with
// the motor nameplate ratio 9907 / 9908.
// ---------------------------------------------------------------------------
static float unitsPerRpm()
{
    return st.scalarMode ? st.nomFreq / st.nomSpeed : 1.0f;
}

static int16_t computeRefWord()
{
    if (!st.scaled || st.ref1Max <= 0) return 0;
    float ref = st.dir * st.setpointRpm * unitsPerRpm() / st.ref1Max * REF_FULL_SCALE;
    ref = constrain(ref, -REF_FULL_SCALE, REF_FULL_SCALE);
    return (int16_t)lroundf(ref);
}

static float refWordToRpm(int16_t ref)
{
    return ref / REF_FULL_SCALE * st.ref1Max / unitsPerRpm();
}

// ---------------------------------------------------------------------------
// Modbus helpers
// ---------------------------------------------------------------------------
static MbStatus readPar(uint16_t par, uint16_t *value)
{
    return s_mb.readHolding(s_slave, parToReg(par), 1, value);
}

static MbStatus writeCwRef()
{
    st.refWord = computeRefWord();
    const uint16_t regs[2] = {st.cw, (uint16_t)st.refWord};
    return s_mb.writeMultiple(s_slave, REG_CONTROL, regs, 2);
}

static void applySettings()
{
    Settings s = settingsGet();
    s_settingsVer = settingsVersion();
    s_pollMs = s.mbPollMs;
    s_minPubMs = constrain(s.mqttMinIntervalMs, 100, 60000);
    if (s.mbSlave != s_slave) s_needResync = true;
    s_slave = s.mbSlave;

    if (!s_mb.isOpen() || s.mbBaud != s_baud || s.mbFormat != s_format) {
        s_baud = s.mbBaud;
        s_format = s.mbFormat;
        if (s_mb.begin(RS485_UART_NUM, PIN_RS485_TX, PIN_RS485_RX, s_baud, s_format)) {
            logf(LOG_INFO, "RS485 %lu %s, station %u, poll %u ms", (unsigned long)s_baud, MB_FORMAT_NAMES[s_format],
                 s_slave, s_pollMs);
        } else {
            logf(LOG_ERR, "RS485 UART init failed");
        }
        s_needResync = true;
    }
}

// Read 9904 / 1105 / 9907 / 9908 so rpm setpoints can be scaled.
static bool readScaling()
{
    uint16_t mode, r1max, nfreq = 0, nspeed = 0;
    MbStatus r = readPar(9904, &mode);
    if (r == MbStatus::Ok) r = readPar(1105, &r1max);
    if (r != MbStatus::Ok) {
        // While the drive is off-line the poll already reports the error; don't repeat it every retry
        if (st.online || s_resyncFails % 20 == 0) logf(LOG_ERR, "Could not read 9904/1105: %s", s_mb.statusText(r));
        s_resyncFails++;
        return false;
    }
    s_resyncFails = 0;
    const bool haveNominal = readPar(9907, &nfreq) == MbStatus::Ok && readPar(9908, &nspeed) == MbStatus::Ok;

    st.scalarMode = mode == 3;
    st.ref1Max = st.scalarMode ? (int16_t)r1max / 10.0f : (float)(int16_t)r1max;
    st.nomFreq = nfreq / 10.0f;
    st.nomSpeed = nspeed;
    if (st.ref1Max <= 0) {
        logf(LOG_ERR, "1105 REF1 MAX is zero - set it on the drive");
        return false;
    }
    if (st.scalarMode && (!haveNominal || st.nomFreq <= 0 || st.nomSpeed <= 0)) {
        logf(LOG_ERR, "Scalar mode: need 9907/9908 to convert rpm to Hz");
        return false;
    }
    st.spMaxRpm = st.ref1Max / unitsPerRpm();
    st.gaugeMaxRpm = max(st.spMaxRpm, st.nomSpeed);
    if (st.setpointRpm > st.spMaxRpm) st.setpointRpm = st.spMaxRpm;
    st.scaled = true;

    publishRaw(9904, mode, true);
    publishRaw(1105, r1max, true);
    if (haveNominal) {
        publishRaw(9907, nfreq, true);
        publishRaw(9908, nspeed, true);
    }
    logf(LOG_OK, "Drive answered: 9904=%u (%s), 1105 REF1 MAX %.1f %s, setpoint 0-%.0f rpm", mode,
         st.scalarMode ? "SCALAR" : "VECTOR", st.ref1Max, st.scalarMode ? "Hz" : "rpm", st.spMaxRpm);
    return true;
}

// After boot or a comms drop, decide whether the panel keeps writing the
// control word. Never resend an old RUN to a drive that was restarted.
static void adoptDriveCommand()
{
    uint16_t regs[2];
    const MbStatus r = s_mb.readHolding(s_slave, REG_CONTROL, 2, regs);
    if (st.cwActive) {
        if (r == MbStatus::Ok && regs[0] == st.cw) {
            logf(LOG_OK, "Drive still holds CW 0x%04X - control resumed", st.cw);
        } else {
            st.cwActive = false;
            logf(LOG_WARN, "Drive restarted or changed - monitoring only, press START to run");
        }
        return;
    }
    if (r != MbStatus::Ok) return;  // registers not readable: monitor only

    const int16_t ref = (int16_t)regs[1];
    st.dir = ref < 0 ? -1 : 1;
    st.setpointRpm = min(fabsf(refWordToRpm(ref)), st.spMaxRpm);
    if (regs[0] & CW_REMOTE_BIT) {
        st.cw = regs[0];
        st.cwActive = true;
        logf(LOG_OK, "Adopted drive command: CW 0x%04X, setpoint %.0f rpm %s", st.cw, st.setpointRpm,
             st.dir < 0 ? "REV" : "FWD");
    } else {
        logf(LOG_INFO, "Setpoint in drive: %.0f rpm %s", st.setpointRpm, st.dir < 0 ? "REV" : "FWD");
    }
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------
static bool pollOnce(MbStatus *err)
{
    MbStatus r;
    if (st.cwActive && st.scaled) {
        if ((r = writeCwRef()) != MbStatus::Ok) return *err = r, false;
    }

    uint16_t sw;
    if ((r = s_mb.readHolding(s_slave, REG_STATUS, 1, &sw)) != MbStatus::Ok) return *err = r, false;
    st.sw = sw;
    st.swValid = true;

    for (ReadGroup &g : s_groups) {
        if (!g.split) {
            uint16_t vals[8];
            r = s_mb.readHolding(s_slave, parToReg(g.firstPar), g.count, vals);
            if (r == MbStatus::Ok) {
                for (uint8_t i = 0; i < g.count; i++) {
                    st.valRaw[g.firstIdx + i] = vals[i];
                    st.valValid[g.firstIdx + i] = true;
                }
                continue;
            }
            if (r != MbStatus::Exception) return *err = r, false;
            g.split = true;
            logf(LOG_WARN, "Block read %04u+%u refused, reading one by one", g.firstPar, g.count);
        }
        for (uint8_t i = 0; i < g.count; i++) {
            const uint8_t idx = g.firstIdx + i;
            if (st.valNa[idx]) continue;
            uint16_t v;
            r = readPar(DRIVE_VALUES[idx].par, &v);
            if (r == MbStatus::Ok) {
                st.valRaw[idx] = v;
                st.valValid[idx] = true;
            } else if (r == MbStatus::Exception) {
                st.valNa[idx] = true;
                logf(LOG_WARN, "%04u not available on this drive", DRIVE_VALUES[idx].par);
            } else {
                return *err = r, false;
            }
        }
    }

    uint16_t fault;
    r = s_mb.readHolding(s_slave, REG_LAST_FAULT, 1, &fault);
    if (r == MbStatus::Ok) {
        st.lastFault = fault;
        st.faultValid = true;
    } else if (r != MbStatus::Exception) {
        return *err = r, false;
    }
    return true;
}

static void pollCycle()
{
    const uint32_t now = millis();
    if (s_needResync && (int32_t)(now - s_nextResyncMs) >= 0) {
        if (readScaling()) {
            adoptDriveCommand();
            s_needResync = false;
            publishSetpoint();
            if (st.cwActive) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%u", st.cw);
                publishText(1, buf, true);
            }
        } else {
            s_nextResyncMs = now + 3000;
        }
    }

    MbStatus err = MbStatus::Ok;
    if (pollOnce(&err)) {
        s_failStreak = 0;
        if (!st.online) {
            st.online = true;
            logf(LOG_OK, "Drive comms OK (station %u)", s_slave);
        }
        publishRaw(4, st.sw, false);
        for (int i = 0; i < DRIVE_VALUE_COUNT; i++)
            if (st.valValid[i] && !st.valNa[i]) publishRaw(DRIVE_VALUES[i].par, st.valRaw[i], false);
        if (st.faultValid) publishRaw(401, st.lastFault, false);

        const bool faulted = st.sw & SW_FAULT;
        if (faulted && !s_wasFaulted)
            logf(LOG_ERR, "Drive FAULT: %u %s", st.lastFault, faultName(st.faultValid ? st.lastFault : 0xFFFF));
        s_wasFaulted = faulted;
    } else {
        s_failStreak++;
        if (s_failStreak == 1 || s_failStreak % 20 == 0) logf(LOG_ERR, "Poll: %s", s_mb.statusText(err));
        if (s_failStreak >= MODBUS_FAILS_BEFORE_OFFLINE && st.online) {
            st.online = false;
            st.scaled = false;  // re-read scaling and re-check the command word on reconnect
            s_needResync = true;
            logf(LOG_ERR, "No response from drive");
        }
    }
    publishState(false);

    if (netTakeRepublishRequest()) {
        flushPublishes(true);
        publishState(true);
    } else {
        flushPublishes(false);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static bool requireOnline(const char *what)
{
    if (st.online && st.scaled) return true;
    logf(LOG_ERR, "%s refused: drive not connected", what);
    return false;
}

static bool sendCw(uint16_t cw, const char *label)
{
    st.cw = cw;
    st.cwActive = true;
    const MbStatus r = writeCwRef();
    if (r != MbStatus::Ok) {
        logf(LOG_ERR, "%s failed: %s", label, s_mb.statusText(r));
        return false;
    }
    logf(LOG_TX, "%s: CW 0x%04X REF %d", label, cw, st.refWord);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", cw);
    publishText(1, buf, true);
    return true;
}

static void applyReference(const char *why)
{
    if (st.scaled) {
        MbStatus r;
        if (st.cwActive) {
            r = writeCwRef();
        } else {  // not driving the control word: update REF1 only (never starts the motor)
            st.refWord = computeRefWord();
            r = s_mb.writeSingle(s_slave, REG_REF1, (uint16_t)st.refWord);
        }
        if (r != MbStatus::Ok) logf(LOG_ERR, "Reference write failed: %s", s_mb.statusText(r));
    }
    logf(LOG_TX, "Setpoint %.0f rpm %s (%s)", st.setpointRpm, st.dir < 0 ? "REV" : "FWD", why);
    publishSetpoint();
}

static void setParamResult(uint16_t par, bool ok, bool write, uint16_t raw, const char *fmt, ...)
{
    ParamResult &p = st.param;
    p.seq++;
    p.par = par;
    p.ok = ok;
    p.write = write;
    p.raw = raw;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p.text, sizeof(p.text), fmt, ap);
    va_end(ap);
}

static void describeParam(uint16_t par, uint16_t raw, char *buf, size_t len)
{
    char val[16];
    formatEng(par, raw, st.scalarMode, val, sizeof(val));
    const Scaling sc = scalingFor(par, st.scalarMode);
    snprintf(buf, len, "%04u %s = %s %s (raw %u, 0x%04X)", par, parName(par), val, sc.unit, raw, raw);
}

static void handleCommand(const DriveCmd &c)
{
    const char *src = c.src == CmdSource::Mqtt ? "MQTT" : "panel";
    switch (c.type) {
    case DriveCmdType::Start: {
        if (!requireOnline("Start")) return;
        if (st.swValid && (st.sw & SW_FAULT)) {
            logf(LOG_ERR, "Start refused: drive is faulted - reset first");
            return;
        }
        if (!sendCw(CW_STOP, "Ready")) return;
        for (int i = 0; i < 10; i++) {  // wait for "ready to switch on"
            vTaskDelay(pdMS_TO_TICKS(60));
            uint16_t sw;
            if (s_mb.readHolding(s_slave, REG_STATUS, 1, &sw) == MbStatus::Ok && (sw & SW_READY_ON)) break;
        }
        sendCw(CW_RUN, c.src == CmdSource::Mqtt ? "Start (MQTT)" : "Start");
        break;
    }
    case DriveCmdType::Stop:
        sendCw(CW_STOP, c.src == CmdSource::Mqtt ? "Stop (MQTT)" : "Stop");
        break;
    case DriveCmdType::Coast:
        sendCw(CW_COAST, "Coast stop");
        break;
    case DriveCmdType::QuickStop:
        sendCw(CW_QUICK, "Quick stop");
        break;
    case DriveCmdType::ResetFault:
        if (!sendCw(CW_STOP, "Reset prep")) return;
        vTaskDelay(pdMS_TO_TICKS(150));
        sendCw(CW_RESET, "Reset pulse");
        vTaskDelay(pdMS_TO_TICKS(250));
        sendCw(CW_STOP, "Reset done");
        break;
    case DriveCmdType::RawControl:
        if (!requireOnline("Control word")) return;
        sendCw((uint16_t)c.i, "Control word");
        break;
    case DriveCmdType::SetSpeed: {
        float rpm = c.f;
        if (c.i) {
            st.dir = rpm < 0 ? -1 : 1;
            rpm = fabsf(rpm);
        }
        if (st.spMaxRpm > 0 && rpm > st.spMaxRpm) {
            logf(LOG_WARN, "Setpoint %.0f limited to %.0f rpm (1105)", rpm, st.spMaxRpm);
            rpm = st.spMaxRpm;
        }
        st.setpointRpm = max(rpm, 0.0f);
        applyReference(src);
        break;
    }
    case DriveCmdType::SetDirection:
        st.dir = c.i < 0 ? -1 : 1;
        applyReference(src);
        break;
    case DriveCmdType::ReadParam: {
        uint16_t v;
        const MbStatus r = readPar(c.par, &v);
        if (r != MbStatus::Ok) {
            setParamResult(c.par, false, false, 0, "Read %04u failed: %s", c.par, s_mb.statusText(r));
            logf(LOG_ERR, "%s", st.param.text);
            return;
        }
        char text[96];
        describeParam(c.par, v, text, sizeof(text));
        setParamResult(c.par, true, false, v, "%s", text);
        logf(LOG_INFO, "Read %s", text);
        publishRaw(c.par, v, true);
        break;
    }
    case DriveCmdType::WriteParam: {
        uint16_t raw;
        if (!engToRaw(c.par, c.f, st.scalarMode, &raw)) {
            setParamResult(c.par, false, true, 0, "%g is out of range for %04u", c.f, c.par);
            logf(LOG_ERR, "%s", st.param.text);
            return;
        }
        MbStatus r = s_mb.writeSingle(s_slave, parToReg(c.par), raw);
        uint16_t back = raw;
        if (r == MbStatus::Ok) r = readPar(c.par, &back);
        if (r != MbStatus::Ok) {
            setParamResult(c.par, false, true, raw, "Write %04u failed: %s", c.par, s_mb.statusText(r));
            logf(LOG_ERR, "%s", st.param.text);
            return;
        }
        char text[96];
        describeParam(c.par, back, text, sizeof(text));
        setParamResult(c.par, true, true, back, "Wrote %s", text);
        logf(LOG_TX, "Wrote %s (%s)", text, src);
        publishRaw(c.par, back, true);
        if (c.par == 1105 || c.par == 9904 || c.par == 9907 || c.par == 9908) s_needResync = true;
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void commitSnapshot()
{
    st.stats = s_mb.stats();
    xSemaphoreTake(s_snapMutex, portMAX_DELAY);
    s_shared = st;
    xSemaphoreGive(s_snapMutex);
}

static void driveTask(void *)
{
    esp_task_wdt_add(nullptr);
    applySettings();
    uint32_t nextPoll = millis();
    for (;;) {
        esp_task_wdt_reset();
        if (settingsVersion() != s_settingsVer) applySettings();

        DriveCmd cmd;
        while (xQueueReceive(s_cmdQ, &cmd, 0) == pdTRUE) {
            handleCommand(cmd);
            commitSnapshot();
            esp_task_wdt_reset();
        }

        if ((int32_t)(millis() - nextPoll) >= 0) {
            const uint32_t t0 = millis();
            nextPoll = t0 + s_pollMs;
            pollCycle();
            st.cycleMs = millis() - t0;
            commitSnapshot();
        }

        // Sleep until the next poll, waking early if a command arrives
        const int32_t wait = (int32_t)(nextPoll - millis());
        if (wait > 0) xQueuePeek(s_cmdQ, &cmd, pdMS_TO_TICKS(wait));
    }
}

void driveStart()
{
    st = DriveSnapshot{};
    st.dir = 1;
    st.gaugeMaxRpm = 1500;
    s_shared = st;
    s_cmdQ = xQueueCreate(16, sizeof(DriveCmd));
    s_snapMutex = xSemaphoreCreateMutex();
    // Priority 10: above the MQTT task (5, which runs the TLS handshake) and the net task (2), below
    // Wi-Fi and lwIP. At equal priority a TLS handshake could pause this task between sending a
    // request and clearing the echo, and the start of the drive's reply was cleared with it.
    // The task mostly waits on the UART, so it doesn't hold anything else up.
    xTaskCreatePinnedToCore(driveTask, "drive", 8192, nullptr, 10, nullptr, 0);
}

bool driveSend(const DriveCmd &cmd)
{
    if (!s_cmdQ || xQueueSend(s_cmdQ, &cmd, 0) != pdTRUE) {
        logf(LOG_ERR, "Drive command queue full");
        return false;
    }
    return true;
}

void driveGetSnapshot(DriveSnapshot *out)
{
    if (!s_snapMutex) {
        *out = DriveSnapshot{};
        return;
    }
    xSemaphoreTake(s_snapMutex, portMAX_DELAY);
    *out = s_shared;
    xSemaphoreGive(s_snapMutex);
}
