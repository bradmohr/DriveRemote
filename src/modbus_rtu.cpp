#include "modbus_rtu.h"

#include <algorithm>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "logger.h"

static constexpr size_t MAX_REGS = 60;

uint16_t modbusCrc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void appendCrc(uint8_t *frame, size_t len)
{
    uint16_t crc = modbusCrc16(frame, len);
    frame[len] = crc & 0xFF;
    frame[len + 1] = crc >> 8;
}

static bool crcOk(const uint8_t *frame, size_t len)
{
    return len >= 4 && modbusCrc16(frame, len - 2) == (frame[len - 2] | (frame[len - 1] << 8));
}

bool ModbusRtu::begin(int uartNum, int txPin, int rxPin, uint32_t baud, uint8_t format)
{
    end();
    uart_config_t cfg = {};
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = format == 2 ? UART_PARITY_EVEN : format == 3 ? UART_PARITY_ODD : UART_PARITY_DISABLE;
    cfg.stop_bits = format == 1 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    port_ = uartNum;
    // GPIO43/44 are UART0's default pins, and the ESP-IDF console runs on UART0. Detach them first,
    // otherwise UART0's TX keeps driving GPIO43 and the transceiver's replies never get through.
    gpio_reset_pin((gpio_num_t)txPin);
    gpio_reset_pin((gpio_num_t)rxPin);
    if (uart_driver_install((uart_port_t)port_, 512, 0, 0, nullptr, 0) != ESP_OK) return false;
    if (uart_param_config((uart_port_t)port_, &cfg) != ESP_OK ||
        uart_set_pin((uart_port_t)port_, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete((uart_port_t)port_);
        return false;
    }
    uart_set_rx_timeout((uart_port_t)port_, 2);  // hand bytes over after 2 idle character times

    // Every character is 11 bits on the wire (start + 8 data + parity/stop)
    charUs_ = (uint32_t)(11ULL * 1000000ULL / baud) + 1;
    // Modbus needs >= 3.5 characters of silence between frames (1.75 ms fixed above 19200)
    gapUs_ = max<uint32_t>(2000, charUs_ * 4);
    open_ = true;
    return true;
}

void ModbusRtu::end()
{
    if (open_) uart_driver_delete((uart_port_t)port_);
    open_ = false;
}

void ModbusRtu::waitInterFrameGap()
{
    int64_t wait = lastActivityUs_ + gapUs_ - esp_timer_get_time();
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000) + 1);
}

MbStatus ModbusRtu::transact(const uint8_t *req, size_t reqLen, uint8_t *resp, size_t expectLen)
{
    if (!open_) return MbStatus::NotOpen;
    const uart_port_t port = (uart_port_t)port_;
    const uint8_t fc = req[1];

    waitInterFrameGap();
    uart_flush_input(port);  // drop anything stale before the request goes out
    uart_write_bytes(port, (const char *)req, reqLen);
    stats_.tx++;
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));
    // Nothing is flushed after sending. If this task is held up (Wi-Fi, TLS), the reply may
    // already be arriving and flushing would cut off its first bytes. A transceiver that echoes
    // the request is handled below by recognising the exact copy of what was sent.

    uint8_t buf[(9 + 2 * MAX_REGS) + (5 + 2 * MAX_REGS)];  // room for an echo plus the reply
    size_t len = 0;
    // A single-register write is answered with a copy of the request, so its echo can only be
    // recognised once the transceiver has been seen echoing another request.
    bool echoPossible = fc != 0x06 || echoSeen_;
    rawCount_ = 0;
    const int64_t deadline = esp_timer_get_time() + MODBUS_RESPONSE_TIMEOUT_MS * 1000LL + (int64_t)expectLen * charUs_;
    while (true) {
        if (len == sizeof(buf)) len = 0;  // only noise fills the buffer
        // Wait for the next byte, then take whatever else has arrived
        int n = uart_read_bytes(port, buf + len, 1, pdMS_TO_TICKS(5));
        if (n > 0) {
            size_t more = 0;
            uart_get_buffered_data_len(port, &more);
            more = std::min(more, sizeof(buf) - len - 1);
            if (more) n += std::max(0, uart_read_bytes(port, buf + len + 1, more, 0));
            // Keep the first bytes for the timeout message: garbage here usually means A/B swapped
            for (int i = 0; i < n; i++, rawCount_++)
                if (rawCount_ < sizeof(rawHead_)) rawHead_[rawCount_] = buf[len + i];
            len += n;

            if (echoPossible) {
                if (memcmp(buf, req, std::min(len, reqLen)) != 0) {
                    echoPossible = false;  // not our request: the reply itself
                } else if (len >= reqLen) {
                    memmove(buf, buf + reqLen, len - reqLen);  // our own request echoed back: drop it
                    len -= reqLen;
                    echoPossible = false;
                    if (!echoSeen_) logf(LOG_INFO, "RS485 transceiver echoes requests; the echo is removed");
                    echoSeen_ = true;
                } else {
                    continue;  // could still be the echo: wait for the rest of it
                }
            }
            // Re-synchronise on line noise: a reply must start with our slave address
            size_t skip = 0;
            while (skip < len && buf[skip] != req[0]) skip++;
            if (skip) {
                memmove(buf, buf + skip, len - skip);
                len -= skip;
            }
        }
        if (len >= 5 && buf[1] == (fc | 0x80)) {
            lastActivityUs_ = esp_timer_get_time();
            if (!crcOk(buf, 5)) {
                stats_.crcErrors++;
                return MbStatus::Crc;
            }
            stats_.rx++;
            stats_.exceptions++;
            lastEx_ = buf[2];
            return MbStatus::Exception;
        }
        if (len >= expectLen) break;
        if (esp_timer_get_time() > deadline) {
            lastActivityUs_ = esp_timer_get_time();
            stats_.timeouts++;
            return MbStatus::Timeout;
        }
    }
    lastActivityUs_ = esp_timer_get_time();
    memcpy(resp, buf, expectLen);

    if (!crcOk(resp, expectLen)) {
        stats_.crcErrors++;
        return MbStatus::Crc;
    }
    if (resp[0] != req[0] || resp[1] != fc) {
        stats_.badReplies++;
        return MbStatus::BadReply;
    }
    stats_.rx++;
    return MbStatus::Ok;
}

MbStatus ModbusRtu::readHolding(uint8_t slave, uint16_t reg, uint16_t count, uint16_t *out)
{
    if (count == 0 || count > MAX_REGS || reg < 40001) return MbStatus::BadReply;
    const uint16_t addr = reg - 40001;
    uint8_t req[8] = {slave, 0x03, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(count >> 8), (uint8_t)count};
    appendCrc(req, 6);

    uint8_t resp[5 + 2 * MAX_REGS];
    const size_t expect = 5 + 2 * count;
    MbStatus st = transact(req, sizeof(req), resp, expect);
    if (st != MbStatus::Ok) return st;
    if (resp[2] != 2 * count) {
        stats_.badReplies++;
        return MbStatus::BadReply;
    }
    for (uint16_t i = 0; i < count; i++) out[i] = (resp[3 + 2 * i] << 8) | resp[4 + 2 * i];
    return MbStatus::Ok;
}

MbStatus ModbusRtu::writeSingle(uint8_t slave, uint16_t reg, uint16_t value)
{
    if (reg < 40001) return MbStatus::BadReply;
    const uint16_t addr = reg - 40001;
    uint8_t req[8] = {slave, 0x06, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(value >> 8), (uint8_t)value};
    appendCrc(req, 6);
    uint8_t resp[8];
    return transact(req, sizeof(req), resp, sizeof(resp));
}

MbStatus ModbusRtu::writeMultiple(uint8_t slave, uint16_t reg, const uint16_t *values, uint16_t count)
{
    if (count == 0 || count > MAX_REGS || reg < 40001) return MbStatus::BadReply;
    const uint16_t addr = reg - 40001;
    uint8_t req[9 + 2 * MAX_REGS];
    size_t n = 0;
    req[n++] = slave;
    req[n++] = 0x10;
    req[n++] = addr >> 8;
    req[n++] = addr & 0xFF;
    req[n++] = count >> 8;
    req[n++] = count & 0xFF;
    req[n++] = count * 2;
    for (uint16_t i = 0; i < count; i++) {
        req[n++] = values[i] >> 8;
        req[n++] = values[i] & 0xFF;
    }
    appendCrc(req, n);
    uint8_t resp[8];
    return transact(req, n + 2, resp, sizeof(resp));
}

const char *ModbusRtu::statusText(MbStatus st) const
{
    switch (st) {
    case MbStatus::Ok: return "OK";
    case MbStatus::Timeout: {
        if (rawCount_ == 0) return "Timeout (no reply)";
        int n = snprintf(text_, sizeof(text_), "Timeout: %lu bytes, no valid reply (", (unsigned long)rawCount_);
        for (uint32_t i = 0; i < rawCount_ && i < sizeof(rawHead_); i++)
            n += snprintf(text_ + n, sizeof(text_) - n, i ? " %02x" : "%02x", rawHead_[i]);
        snprintf(text_ + n, sizeof(text_) - n, "%s)", rawCount_ > sizeof(rawHead_) ? " ..." : "");
        return text_;
    }
    case MbStatus::Crc: return "CRC error";
    case MbStatus::BadReply: return "Unexpected reply";
    case MbStatus::NotOpen: return "RS485 port not open";
    case MbStatus::Exception: {
        const char *why = "";
        switch (lastEx_) {
        case 1: why = " (illegal function)"; break;
        case 2: why = " (illegal address)"; break;
        case 3: why = " (illegal value)"; break;
        case 4: why = " (device failure)"; break;
        case 6: why = " (busy)"; break;
        }
        snprintf(text_, sizeof(text_), "Modbus exception %u%s", lastEx_, why);
        return text_;
    }
    }
    return "?";
}
