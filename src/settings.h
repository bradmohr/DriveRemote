#pragma once
#include <Arduino.h>

// Persistent settings (stored in NVS, survive reboots and firmware updates).

struct Settings {
    // Wi-Fi
    String wifiSsid;
    String wifiPass;

    // MQTT
    bool mqttEnabled = true;
    String mqttHost = "l11f1a19.ala.asia-southeast1.emqxsl.com";  // owner's EMQX Cloud (default for now)
    uint16_t mqttPort = 8883;
    bool mqttTls = true;            // encrypted connection (EMQX Cloud: port 8883)
    bool mqttAuth = true;
    String mqttUser;
    String mqttPass;
    String mqttBase = "Drive";      // topics are <base>/<XXXX>
    String mqttCmdSuffix = "";      // "" = commands on <base>/XXXX, "/set" = <base>/XXXX/set
    bool mqttRetain = true;         // publish drive values as retained messages
    bool mqttAllowControl = true;   // accept start/stop/speed (0001/0002) over MQTT
    bool mqttAllowParamWrite = false;  // accept writes to other parameters over MQTT
    uint16_t mqttMinIntervalMs = 1000; // fastest re-publish of a changing value

    // Modbus RTU (must match ACS550 group 53)
    uint32_t mbBaud = 19200;        // 5303
    uint8_t mbFormat = 0;           // 5304: 0=8N1 1=8N2 2=8E1 3=8O1
    uint8_t mbSlave = 1;            // 5302
    uint16_t mbPollMs = 250;

    // Panel
    uint8_t brightness = 100;       // percent (dimming needs a V4 board)
    uint8_t screenOffMin = 15;      // backlight off after this long without a touch, 0 = never
    String timezone = "UTC0";       // POSIX TZ string, e.g. "EST5EDT,M3.2.0,M11.1.0"
};

void settingsInit();
Settings settingsGet();                // thread-safe copy
void settingsSave(const Settings &s);  // persist + bump version
uint32_t settingsVersion();            // changes on every save; tasks re-apply when it moves

extern const uint32_t MB_BAUD_RATES[];
extern const size_t MB_BAUD_COUNT;
extern const char *const MB_FORMAT_NAMES[4];
