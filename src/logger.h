#pragma once
#include <Arduino.h>

// Thread-safe event log shared by the panel's Log page, the Drive page strip,
// the USB serial port and the MQTT "<base>/log" topic.

enum LogLevel : char {
    LOG_INFO = 'I',
    LOG_OK   = 'O',
    LOG_WARN = 'W',
    LOG_ERR  = 'E',
    LOG_TX   = 'T',  // command sent to the drive
};

struct LogEntry {
    uint32_t seq;
    char level;
    char time[10];   // "HH:MM:SS" (wall clock once NTP has synced, else uptime)
    char text[100];
};

void logInit();
void logf(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Fetch the oldest entry newer than *seq. Each reader keeps its own cursor.
// Returns false when there is nothing new.
bool logNext(uint32_t *seq, LogEntry *out);

// Sequence number of the newest entry (0 if empty).
uint32_t logLatestSeq();

void logTimestamp(char *buf, size_t len);
