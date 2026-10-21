#pragma once
// Shared pieces of the LVGL user interface (ui.cpp, ui_setup.cpp).
#include <lvgl.h>

namespace ui {

// Palette taken from the web panel (Project.html)
constexpr uint32_t COL_BG = 0x0f1317;
constexpr uint32_t COL_HEADER = 0x12171c;
constexpr uint32_t COL_PANEL = 0x171d23;
constexpr uint32_t COL_PANEL2 = 0x1d252c;
constexpr uint32_t COL_LINE = 0x2a343d;
constexpr uint32_t COL_TEXT = 0xe6edf3;
constexpr uint32_t COL_MUTED = 0x8b98a5;
constexpr uint32_t COL_ACCENT = 0xff6b1a;
constexpr uint32_t COL_ACCENT2 = 0xffa45c;
constexpr uint32_t COL_OK = 0x2ecc71;
constexpr uint32_t COL_WARN = 0xf1c40f;
constexpr uint32_t COL_BAD = 0xe74c3c;
constexpr uint32_t COL_START = 0x1f9d55;
constexpr uint32_t COL_STOP = 0xc0392b;
constexpr uint32_t COL_AMBER = 0xb7791f;
constexpr uint32_t COL_BTN = 0x33404b;
constexpr uint32_t COL_LOGBG = 0x0b0f12;
constexpr uint32_t COL_OFF = 0x3a444e;

constexpr int HEADER_H = 40;
constexpr int TABBAR_H = 46;
constexpr int KEYBOARD_H = 220;

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t bg, const lv_font_t *font);
lv_obj_t *makePanel(lv_obj_t *parent);
void setText(lv_obj_t *label, const char *text);  // only redraws when the text changed
void setButtonText(lv_obj_t *btn, const char *text);

// Digits-only keypad map (LV_KEYBOARD_MODE_USER_1) with "-" "." and OK
void keyboardUseNumericMap(lv_obj_t *kb);

lv_obj_t *tabview();

// Setup page (ui_setup.cpp)
void buildSetupTab(lv_obj_t *tab, const char *bootInfo);
void refreshSetupTab();

}  // namespace ui
