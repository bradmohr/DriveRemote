#pragma once
#include <Arduino.h>

#include <vector>

// Wi-Fi + MQTT bridge.
//
// Outgoing: every value read from the drive is published to <base>/XXXX
// (XXXX = parameter / register number, payload already scaled), plus
// <base>/state, <base>/status (online/offline) and <base>/log.
//
// Incoming: <base>/XXXX<suffix> (suffix "" by default, or e.g. "/set"):
//   0001  START | STOP | COAST | QSTOP | RESET | ON | OFF | raw control word
//   0002  speed setpoint in rpm (negative = reverse)
//   XXXX  "?" reads the parameter; a number writes it (engineering units)

void netStart();

// Non-blocking, callable from any task. Topic = <base>/<sub>
void netPublish(const char *sub, const char *payload, bool retain);
void netPublishPar(uint16_t par, const char *payload);

// True once after each MQTT (re)connect so the drive task can resend all values.
bool netTakeRepublishRequest();

struct NetStatus {
    bool wifiConfigured;
    bool wifiConnected;
    bool mqttConfigured;
    bool mqttConnected;
    int rssi;
    char ip[16];
    char mac[18];
};
void netGetStatus(NetStatus *out);

// Wi-Fi scan for the setup page (UI task)
void netScanStart();
int netScanResults(std::vector<String> *ssids);  // -1 while scanning, else number found
