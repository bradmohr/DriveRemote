#include "settings.h"

#include <Preferences.h>

#include <atomic>

const uint32_t MB_BAUD_RATES[] = {9600, 19200, 38400, 57600, 76800};
const size_t MB_BAUD_COUNT = sizeof(MB_BAUD_RATES) / sizeof(MB_BAUD_RATES[0]);
const char *const MB_FORMAT_NAMES[4] = {"8N1", "8N2", "8E1", "8O1"};

static Settings s_settings;
static SemaphoreHandle_t s_mutex = nullptr;
static std::atomic<uint32_t> s_version{1};

static const char *NVS_NAMESPACE = "acs550";

void settingsInit()
{
    s_mutex = xSemaphoreCreateMutex();
    Settings d;  // defaults
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    Settings &s = s_settings;
    s.wifiSsid = p.getString("wSsid", d.wifiSsid);
    s.wifiPass = p.getString("wPass", d.wifiPass);
    s.mqttEnabled = p.getBool("mEn", d.mqttEnabled);
    s.mqttHost = p.getString("mHost", d.mqttHost);
    s.mqttPort = p.getUShort("mPort", d.mqttPort);
    s.mqttTls = p.getBool("mTls", d.mqttTls);
    s.mqttAuth = p.getBool("mAuth", d.mqttAuth);
    s.mqttUser = p.getString("mUser", d.mqttUser);
    s.mqttPass = p.getString("mPass", d.mqttPass);
    s.mqttBase = p.getString("mBase", d.mqttBase);
    s.mqttCmdSuffix = p.getString("mSuffix", d.mqttCmdSuffix);
    s.mqttRetain = p.getBool("mRetain", d.mqttRetain);
    s.mqttAllowControl = p.getBool("mCtrl", d.mqttAllowControl);
    s.mqttAllowParamWrite = p.getBool("mParW", d.mqttAllowParamWrite);
    s.mqttMinIntervalMs = p.getUShort("mMinMs", d.mqttMinIntervalMs);
    s.mbBaud = p.getULong("bBaud", d.mbBaud);
    s.mbFormat = p.getUChar("bFmt", d.mbFormat);
    s.mbSlave = p.getUChar("bSlave", d.mbSlave);
    s.mbPollMs = p.getUShort("bPoll", d.mbPollMs);
    s.brightness = p.getUChar("pBright", d.brightness);
    s.screenOffMin = p.getUChar("pScrOff", d.screenOffMin);
    s.timezone = p.getString("pTz", d.timezone);
    p.end();

    // Guard against corrupt or out-of-range values
    if (s.mbFormat > 3) s.mbFormat = 0;
    if (s.mbSlave < 1 || s.mbSlave > 247) s.mbSlave = 1;
    if (s.mbPollMs < 100 || s.mbPollMs > 5000) s.mbPollMs = 250;
    if (s.mqttBase.isEmpty()) s.mqttBase = d.mqttBase;
    // No broker entered yet (or saved before there was a default): use the default broker
    if (s.mqttHost.isEmpty()) {
        s.mqttHost = d.mqttHost;
        s.mqttPort = d.mqttPort;
        s.mqttTls = d.mqttTls;
        s.mqttAuth = d.mqttAuth;
    }
    if (s.brightness < 10 || s.brightness > 100) s.brightness = 100;
}

Settings settingsGet()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Settings copy = s_settings;
    xSemaphoreGive(s_mutex);
    return copy;
}

void settingsSave(const Settings &s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = s;
    xSemaphoreGive(s_mutex);

    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("wSsid", s.wifiSsid);
    p.putString("wPass", s.wifiPass);
    p.putBool("mEn", s.mqttEnabled);
    p.putString("mHost", s.mqttHost);
    p.putUShort("mPort", s.mqttPort);
    p.putBool("mTls", s.mqttTls);
    p.putBool("mAuth", s.mqttAuth);
    p.putString("mUser", s.mqttUser);
    p.putString("mPass", s.mqttPass);
    p.putString("mBase", s.mqttBase);
    p.putString("mSuffix", s.mqttCmdSuffix);
    p.putBool("mRetain", s.mqttRetain);
    p.putBool("mCtrl", s.mqttAllowControl);
    p.putBool("mParW", s.mqttAllowParamWrite);
    p.putUShort("mMinMs", s.mqttMinIntervalMs);
    p.putULong("bBaud", s.mbBaud);
    p.putUChar("bFmt", s.mbFormat);
    p.putUChar("bSlave", s.mbSlave);
    p.putUShort("bPoll", s.mbPollMs);
    p.putUChar("pBright", s.brightness);
    p.putUChar("pScrOff", s.screenOffMin);
    p.putString("pTz", s.timezone);
    p.end();

    s_version++;
}

uint32_t settingsVersion()
{
    return s_version;
}
