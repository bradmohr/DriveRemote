#pragma once
#include <Arduino.h>

// Waveshare ESP32-S3-Touch-LCD-4 hardware: IO expander, ST7701 RGB display,
// GT911 touch and the LVGL display/input drivers.

enum class BoardRev : uint8_t {
    Unknown,
    V1toV3,  // TCA9554 IO expander at 0x20
    V4,      // CH32V003 IO expander at 0x24 (PWM backlight)
};

// Bring up I2C, expander, display, touch and LVGL. Call once from the task
// that will run lv_timer_handler() (the Arduino loop task).
bool boardInit();

void boardBacklight(uint8_t percent);
BoardRev boardRev();
const char *boardRevText();
bool boardTouchFound();
