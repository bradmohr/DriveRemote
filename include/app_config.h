#pragma once

// ---------------------------------------------------------------------------
// ACS550 remote panel - build-time configuration
// Target: Waveshare ESP32-S3-Touch-LCD-4 (480x480). Pin map is shared by
// board revisions V1-V4; only the IO expander differs (auto-detected).
// ---------------------------------------------------------------------------

#define APP_NAME    "ACS550 Panel"
#define APP_VERSION "1.0.0"
#define APP_HOSTNAME "acs550-panel"

// I2C bus: IO expander, GT911 touch, RTC
#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 7

// RS485 (transceiver switches direction automatically, no DE pin)
#define PIN_RS485_TX 44
#define PIN_RS485_RX 43
#define RS485_UART_NUM 1

// ST7701 3-wire SPI (init only) + RGB565 parallel bus
#define PIN_LCD_CS    42
#define PIN_LCD_SCK   2
#define PIN_LCD_MOSI  1
#define PIN_LCD_DE    40
#define PIN_LCD_VSYNC 39
#define PIN_LCD_HSYNC 38
#define PIN_LCD_PCLK  41

#define LCD_WIDTH  480
#define LCD_HEIGHT 480

// 2 = rotate 180 degrees (Waveshare's default for this panel).
// Set to 0 if the picture and touch come out upside down.
#define LCD_ROTATION 2

// Task watchdog: any watched task silent for this long reboots the panel.
#define WDT_TIMEOUT_S 20

// Modbus master timing
#define MODBUS_RESPONSE_TIMEOUT_MS 300
#define MODBUS_FAILS_BEFORE_OFFLINE 3

// MQTT: unchanged values are re-published this often (keeps dashboards fresh)
#define MQTT_HEARTBEAT_MS 60000
