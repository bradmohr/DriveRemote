#pragma once
#include <Arduino.h>

// Minimal Modbus RTU master (FC03 / FC06 / FC16) on an ESP-IDF UART.
// Register numbers are 4xxxx style (40001 = wire address 0), matching the
// ACS550 manual: parameter xxyy lives in register 4xxyy.

enum class MbStatus : uint8_t { Ok, Timeout, Crc, Exception, BadReply, NotOpen };

struct MbStats {
    uint32_t tx = 0;
    uint32_t rx = 0;
    uint32_t timeouts = 0;
    uint32_t crcErrors = 0;
    uint32_t exceptions = 0;
    uint32_t badReplies = 0;
};

class ModbusRtu {
public:
    // format: 0=8N1 1=8N2 2=8E1 3=8O1 (same numbering as ACS550 parameter 5304)
    bool begin(int uartNum, int txPin, int rxPin, uint32_t baud, uint8_t format);
    void end();
    bool isOpen() const { return open_; }

    MbStatus readHolding(uint8_t slave, uint16_t reg, uint16_t count, uint16_t *out);
    MbStatus writeSingle(uint8_t slave, uint16_t reg, uint16_t value);
    MbStatus writeMultiple(uint8_t slave, uint16_t reg, const uint16_t *values, uint16_t count);

    uint8_t lastException() const { return lastEx_; }
    const MbStats &stats() const { return stats_; }

    // Human readable result, e.g. "Timeout (no reply)" or "Exception 2 (illegal address)"
    const char *statusText(MbStatus st) const;

private:
    MbStatus transact(const uint8_t *req, size_t reqLen, uint8_t *resp, size_t expectLen);
    void waitInterFrameGap();

    int port_ = -1;
    bool open_ = false;
    uint32_t charUs_ = 573;  // one 11-bit character at 19200 baud
    uint32_t gapUs_ = 2000;
    int64_t lastActivityUs_ = 0;
    uint8_t lastEx_ = 0;
    MbStats stats_;
    mutable char text_[48];
};

uint16_t modbusCrc16(const uint8_t *data, size_t len);
