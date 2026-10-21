#include "ui.h"

#include <Arduino.h>
#include <math.h>

#include "acs550.h"
#include "app_config.h"
#include "drive.h"
#include "logger.h"
#include "net.h"
#include "ui_internal.h"

using namespace acs550;

namespace ui {

// Indices into DRIVE_VALUES
enum { V_SPEED = 0, V_FREQ = 1, V_CURRENT = 2, V_TORQUE = 3, V_POWER = 4 };
static const int QUICK_VALUES[4] = {V_FREQ, V_CURRENT, V_POWER, V_TORQUE};

static lv_obj_t *s_tabview;

// Header
static lv_obj_t *s_dotDrive, *s_dotWifi, *s_dotMqtt;

// Drive tab
static lv_obj_t *s_arc, *s_speedLbl, *s_gaugeMaxLbl, *s_runPill, *s_runLbl;
static lv_obj_t *s_quickVal[4];
static lv_obj_t *s_spBtn, *s_spSlider, *s_btnFwd, *s_btnRev;
static lv_obj_t *s_btnStart, *s_btnStop, *s_btnCoast, *s_btnQuick, *s_btnReset;
static lv_obj_t *s_logStrip;

// Values tab
static lv_obj_t *s_tileVal[DRIVE_VALUE_COUNT];
static lv_obj_t *s_faultLbl, *s_swLbl;
static lv_obj_t *s_bitDot[STATUS_BIT_COUNT], *s_bitLbl[STATUS_BIT_COUNT];

// Param tab
static lv_obj_t *s_parTa, *s_valTa, *s_parInfo, *s_parKb;
static uint32_t s_paramSeq = 0;
static DriveCmd s_pendingWrite;

// Log tab
static lv_obj_t *s_logList, *s_statsLbl;
static uint32_t s_logSeq = 0;

static DriveSnapshot s_snap;
static float s_spLocal = 0;          // setpoint shown while the user is adjusting it
static uint32_t s_spEditUntil = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
lv_obj_t *tabview() { return s_tabview; }

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t bg, const lv_font_t *font)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *makePanel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 8, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

void setText(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void setButtonText(lv_obj_t *btn, const char *text)
{
    setText(lv_obj_get_child(btn, 0), text);
}

static void setEnabled(lv_obj_t *obj, bool on)
{
    if (on == !lv_obj_has_state(obj, LV_STATE_DISABLED)) return;
    if (on) lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void setDot(lv_obj_t *dot, uint32_t color)
{
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
}

static const char *NUM_KB_MAP[] = {"1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
                                   "4", "5", "6", "-", "\n",
                                   "7", "8", "9", ".", "\n",
                                   "0", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const lv_btnmatrix_ctrl_t NUM_KB_CTRL[] = {
    1, 1, 1, LV_BTNMATRIX_CTRL_CHECKED | 1,
    1, 1, 1, 1,
    1, 1, 1, 1,
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1,
};

void keyboardUseNumericMap(lv_obj_t *kb)
{
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, NUM_KB_MAP, NUM_KB_CTRL);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
}

static void sendCmd(DriveCmdType type, float f = 0, int32_t i = 0, uint16_t par = 0)
{
    DriveCmd c = {};
    c.type = type;
    c.src = CmdSource::Panel;
    c.f = f;
    c.i = i;
    c.par = par;
    driveSend(c);
}

static void formatValue(int idx, char *buf, size_t len, bool coloredUnit)
{
    if (s_snap.valNa[idx]) {
        snprintf(buf, len, "n/a");
        return;
    }
    if (!s_snap.online || !s_snap.valValid[idx]) {
        snprintf(buf, len, "--");
        return;
    }
    const uint16_t par = DRIVE_VALUES[idx].par;
    char v[16];
    formatEng(par, s_snap.valRaw[idx], s_snap.scalarMode, v, sizeof(v));
    const char *unit = scalingFor(par, s_snap.scalarMode).unit;
    if (coloredUnit) snprintf(buf, len, "%s #8b98a5 %s#", v, unit);
    else snprintf(buf, len, "%s %s", v, unit);
}

static uint32_t logColor(char level)
{
    switch (level) {
    case LOG_ERR: return 0xff8a7a;
    case LOG_OK: return 0x7dffb0;
    case LOG_TX: return 0x8fc9ff;
    case LOG_WARN: return 0xffd58a;
    default: return 0xc9d3dc;
    }
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
static lv_obj_t *statusChip(lv_obj_t *parent, const char *text)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 5, 0);

    lv_obj_t *dot = lv_obj_create(chip);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x444444), 0);
    makeLabel(chip, text, &lv_font_montserrat_12, COL_MUTED);
    return dot;
}

static void buildHeader(lv_obj_t *scr)
{
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LCD_WIDTH, HEADER_H);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_pad_hor(hdr, 12, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = makeLabel(hdr, "ABB #ff6b1a ACS550#", &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_recolor(title, true);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *chips = lv_obj_create(hdr);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 14, 0);
    lv_obj_align(chips, LV_ALIGN_RIGHT_MID, 0, 0);
    s_dotDrive = statusChip(chips, "DRIVE");
    s_dotWifi = statusChip(chips, "WIFI");
    s_dotMqtt = statusChip(chips, "MQTT");
}

// ---------------------------------------------------------------------------
// Drive tab: gauge, run state, setpoint, start/stop
// ---------------------------------------------------------------------------
static void onCommandBtn(lv_event_t *e)
{
    sendCmd((DriveCmdType)(intptr_t)lv_event_get_user_data(e));
}

static float spMax()
{
    return s_snap.spMaxRpm > 0 ? s_snap.spMaxRpm : 1500;
}

static void showSetpoint(float rpm)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f rpm", rpm);
    setButtonText(s_spBtn, buf);
}

static void beginSetpointEdit(float rpm)
{
    s_spLocal = constrain(rpm, 0.0f, spMax());
    s_spEditUntil = millis() + 1500;
    showSetpoint(s_spLocal);
}

static void onSlider(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        beginSetpointEdit(lv_slider_get_value(s_spSlider));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        beginSetpointEdit(lv_slider_get_value(s_spSlider));
        sendCmd(DriveCmdType::SetSpeed, s_spLocal);
    }
}

static void onStep(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const int sign = (int)(intptr_t)lv_event_get_user_data(e);
    const float step = spMax() >= 600 ? 10 : 1;
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        float base = millis() < s_spEditUntil ? s_spLocal : s_snap.setpointRpm;
        beginSetpointEdit(roundf(base / step) * step + sign * step);
        lv_slider_set_value(s_spSlider, (int32_t)s_spLocal, LV_ANIM_OFF);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        sendCmd(DriveCmdType::SetSpeed, s_spLocal);
    }
}

static void onDirection(lv_event_t *e)
{
    sendCmd(DriveCmdType::SetDirection, 0, (int32_t)(intptr_t)lv_event_get_user_data(e));
}

// Full-screen numeric entry for an exact setpoint
static void closeOverlay(lv_event_t *e)
{
    lv_obj_del_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void onSetpointPadReady(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    const char *txt = lv_textarea_get_text(ta);
    if (txt[0]) {
        beginSetpointEdit(atof(txt));
        sendCmd(DriveCmdType::SetSpeed, s_spLocal);
    }
    lv_obj_del_async(overlay);
}

static void openSetpointPad(lv_event_t *)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = makePanel(overlay);
    lv_obj_set_size(box, 456, 400);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 12);

    char title[48];
    snprintf(title, sizeof(title), "Speed setpoint (0 - %.0f rpm)", spMax());
    makeLabel(box, title, &lv_font_montserrat_16, COL_TEXT);

    lv_obj_t *cancel = makeButton(box, LV_SYMBOL_CLOSE, COL_BTN, &lv_font_montserrat_16);
    lv_obj_set_size(cancel, 44, 36);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(cancel, closeOverlay, LV_EVENT_CLICKED, overlay);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_textarea_set_max_length(ta, 5);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    lv_obj_set_size(ta, 436, 52);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 36);
    char cur[12];
    snprintf(cur, sizeof(cur), "%.0f", s_snap.setpointRpm);
    lv_textarea_set_text(ta, cur);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    lv_obj_t *kb = lv_keyboard_create(box);
    keyboardUseNumericMap(kb);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, 436, 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, onSetpointPadReady, LV_EVENT_READY, overlay);
}

static void buildDriveTab(lv_obj_t *tab)
{
    // Gauge (actual speed 0102)
    s_arc = lv_arc_create(tab);
    lv_obj_set_size(s_arc, 156, 156);
    lv_obj_set_pos(s_arc, 8, 4);
    lv_arc_set_bg_angles(s_arc, 135, 45);
    lv_arc_set_range(s_arc, 0, 1500);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x26303a), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    s_speedLbl = makeLabel(s_arc, "----", &lv_font_montserrat_36, COL_TEXT);
    lv_obj_align(s_speedLbl, LV_ALIGN_CENTER, 0, -8);
    lv_obj_t *unit = makeLabel(s_arc, "rpm  0102", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 22);
    s_gaugeMaxLbl = makeLabel(s_arc, "max 1500", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_align(s_gaugeMaxLbl, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Run state pill
    s_runPill = lv_obj_create(tab);
    lv_obj_remove_style_all(s_runPill);
    lv_obj_set_pos(s_runPill, 172, 6);
    lv_obj_set_size(s_runPill, 300, 34);
    lv_obj_set_style_radius(s_runPill, 17, 0);
    lv_obj_set_style_bg_opa(s_runPill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_runPill, lv_color_hex(0x26303a), 0);
    s_runLbl = makeLabel(s_runPill, "NO COMMS", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_center(s_runLbl);

    // Key actual values
    lv_obj_t *vals = makePanel(tab);
    lv_obj_set_pos(vals, 172, 46);
    lv_obj_set_size(vals, 300, 114);
    for (int i = 0; i < 4; i++) {
        const DriveValueDef &d = DRIVE_VALUES[QUICK_VALUES[i]];
        lv_obj_t *name = makeLabel(vals, d.label, &lv_font_montserrat_14, COL_MUTED);
        lv_obj_set_pos(name, 2, 4 + i * 25);
        s_quickVal[i] = makeLabel(vals, "--", &lv_font_montserrat_20, COL_TEXT);
        lv_label_set_recolor(s_quickVal[i], true);
        lv_obj_align(s_quickVal[i], LV_ALIGN_TOP_RIGHT, -2, 1 + i * 25);
    }

    // Setpoint
    lv_obj_t *spTitle = makeLabel(tab, "SETPOINT", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(spTitle, 8, 179);
    s_spBtn = makeButton(tab, "0 rpm", COL_PANEL2, &lv_font_montserrat_24);
    lv_obj_set_style_border_width(s_spBtn, 1, 0);
    lv_obj_set_style_border_color(s_spBtn, lv_color_hex(COL_LINE), 0);
    lv_obj_set_pos(s_spBtn, 84, 166);
    lv_obj_set_size(s_spBtn, 176, 40);
    lv_obj_add_event_cb(s_spBtn, openSetpointPad, LV_EVENT_CLICKED, nullptr);

    s_btnFwd = makeButton(tab, "FWD", COL_PANEL2, &lv_font_montserrat_16);
    lv_obj_set_pos(s_btnFwd, 268, 166);
    lv_obj_set_size(s_btnFwd, 100, 40);
    lv_obj_add_event_cb(s_btnFwd, onDirection, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    s_btnRev = makeButton(tab, "REV", COL_PANEL2, &lv_font_montserrat_16);
    lv_obj_set_pos(s_btnRev, 372, 166);
    lv_obj_set_size(s_btnRev, 100, 40);
    lv_obj_add_event_cb(s_btnRev, onDirection, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

    lv_obj_t *minus = makeButton(tab, LV_SYMBOL_MINUS, COL_BTN, &lv_font_montserrat_20);
    lv_obj_set_pos(minus, 8, 212);
    lv_obj_set_size(minus, 52, 36);
    lv_obj_add_event_cb(minus, onStep, LV_EVENT_ALL, (void *)(intptr_t)-1);

    s_spSlider = lv_slider_create(tab);
    lv_obj_set_pos(s_spSlider, 80, 224);
    lv_obj_set_size(s_spSlider, 320, 12);
    lv_slider_set_range(s_spSlider, 0, 1500);
    lv_obj_set_style_bg_color(s_spSlider, lv_color_hex(0x26303a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_spSlider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_spSlider, lv_color_hex(COL_ACCENT2), LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_spSlider, 14);
    lv_obj_add_event_cb(s_spSlider, onSlider, LV_EVENT_ALL, nullptr);

    lv_obj_t *plus = makeButton(tab, LV_SYMBOL_PLUS, COL_BTN, &lv_font_montserrat_20);
    lv_obj_set_pos(plus, 420, 212);
    lv_obj_set_size(plus, 52, 36);
    lv_obj_add_event_cb(plus, onStep, LV_EVENT_ALL, (void *)(intptr_t)1);

    // Start / stop
    s_btnStart = makeButton(tab, LV_SYMBOL_PLAY "  START", COL_START, &lv_font_montserrat_24);
    lv_obj_set_pos(s_btnStart, 8, 254);
    lv_obj_set_size(s_btnStart, 228, 56);
    lv_obj_add_event_cb(s_btnStart, onCommandBtn, LV_EVENT_CLICKED, (void *)(intptr_t)DriveCmdType::Start);

    s_btnStop = makeButton(tab, LV_SYMBOL_STOP "  STOP", COL_STOP, &lv_font_montserrat_24);
    lv_obj_set_pos(s_btnStop, 244, 254);
    lv_obj_set_size(s_btnStop, 228, 56);
    lv_obj_add_event_cb(s_btnStop, onCommandBtn, LV_EVENT_CLICKED, (void *)(intptr_t)DriveCmdType::Stop);

    s_btnCoast = makeButton(tab, "Coast stop", COL_BTN, &lv_font_montserrat_14);
    lv_obj_set_pos(s_btnCoast, 8, 316);
    lv_obj_set_size(s_btnCoast, 150, 36);
    lv_obj_add_event_cb(s_btnCoast, onCommandBtn, LV_EVENT_CLICKED, (void *)(intptr_t)DriveCmdType::Coast);

    s_btnQuick = makeButton(tab, "Quick stop", COL_BTN, &lv_font_montserrat_14);
    lv_obj_set_pos(s_btnQuick, 164, 316);
    lv_obj_set_size(s_btnQuick, 150, 36);
    lv_obj_add_event_cb(s_btnQuick, onCommandBtn, LV_EVENT_CLICKED, (void *)(intptr_t)DriveCmdType::QuickStop);

    s_btnReset = makeButton(tab, LV_SYMBOL_REFRESH " Reset fault", COL_AMBER, &lv_font_montserrat_14);
    lv_obj_set_pos(s_btnReset, 320, 316);
    lv_obj_set_size(s_btnReset, 152, 36);
    lv_obj_add_event_cb(s_btnReset, onCommandBtn, LV_EVENT_CLICKED, (void *)(intptr_t)DriveCmdType::ResetFault);

    // Latest log line (tap for the full log)
    lv_obj_t *strip = lv_obj_create(tab);
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, 8, 358);
    lv_obj_set_size(strip, 464, 32);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(COL_LOGBG), 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_set_style_pad_hor(strip, 8, 0);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(strip, [](lv_event_t *) { lv_tabview_set_act(s_tabview, 3, LV_ANIM_OFF); }, LV_EVENT_CLICKED,
                        nullptr);
    s_logStrip = makeLabel(strip, "", &lv_font_montserrat_12, COL_MUTED);
    lv_label_set_long_mode(s_logStrip, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_logStrip, 448);
    lv_obj_align(s_logStrip, LV_ALIGN_LEFT_MID, 0, 0);
}

static void refreshDriveTab()
{
    const DriveSnapshot &s = s_snap;
    char buf[48];

    // Gauge
    const int gaugeMax = max(1, (int)lroundf(s.gaugeMaxRpm));
    if (lv_arc_get_max_value(s_arc) != gaugeMax) {
        lv_arc_set_range(s_arc, 0, gaugeMax);
        snprintf(buf, sizeof(buf), "max %d", gaugeMax);
        setText(s_gaugeMaxLbl, buf);
    }
    if (s.online && s.valValid[V_SPEED]) {
        const int rpm = (int16_t)s.valRaw[V_SPEED];
        snprintf(buf, sizeof(buf), "%d", rpm);
        setText(s_speedLbl, buf);
        lv_arc_set_value(s_arc, min(abs(rpm), gaugeMax));
    } else {
        setText(s_speedLbl, "----");
        lv_arc_set_value(s_arc, 0);
    }

    // Run state
    const RunState rs = runState(s.online, s.swValid, s.sw);
    setText(s_runLbl, runStateText(rs, s.sw));
    uint32_t pillBg = 0x26303a, pillFg = COL_TEXT;
    if (rs == RunState::Running) pillBg = 0x174d2e, pillFg = 0x7dffb0;
    else if (rs == RunState::Fault || rs == RunState::Offline) pillBg = 0x5a1a14, pillFg = 0xffb3a8;
    else if (rs == RunState::Local || rs == RunState::Inhibited) pillBg = 0x3a2a10, pillFg = 0xffd58a;
    lv_obj_set_style_bg_color(s_runPill, lv_color_hex(pillBg), 0);
    lv_obj_set_style_text_color(s_runLbl, lv_color_hex(pillFg), 0);

    for (int i = 0; i < 4; i++) {
        formatValue(QUICK_VALUES[i], buf, sizeof(buf), true);
        setText(s_quickVal[i], buf);
    }

    // Setpoint (local value wins while the user is adjusting it)
    const int sliderMax = (int)lroundf(spMax());
    if (lv_slider_get_max_value(s_spSlider) != sliderMax) lv_slider_set_range(s_spSlider, 0, sliderMax);
    const bool editing = millis() < s_spEditUntil || lv_obj_has_state(s_spSlider, LV_STATE_PRESSED);
    if (!editing) {
        s_spLocal = s.setpointRpm;
        showSetpoint(s_spLocal);
        if (lv_slider_get_value(s_spSlider) != (int32_t)lroundf(s_spLocal))
            lv_slider_set_value(s_spSlider, (int32_t)lroundf(s_spLocal), LV_ANIM_OFF);
    }
    const bool rev = s.dir < 0;
    lv_obj_set_style_bg_color(s_btnFwd, lv_color_hex(rev ? COL_PANEL2 : COL_ACCENT), 0);
    lv_obj_set_style_bg_color(s_btnRev, lv_color_hex(rev ? COL_ACCENT : COL_PANEL2), 0);

    const bool ready = s.online && s.scaled;
    setEnabled(s_btnStart, ready);
    setEnabled(s_btnCoast, s.online);
    setEnabled(s_btnQuick, s.online);
    setEnabled(s_btnReset, s.online);
}

// ---------------------------------------------------------------------------
// Values tab
// ---------------------------------------------------------------------------
static void buildValuesTab(lv_obj_t *tab)
{
    for (int i = 0; i < DRIVE_VALUE_COUNT; i++) {
        lv_obj_t *tile = makePanel(tab);
        lv_obj_set_pos(tile, 8 + (i % 2) * 236, 4 + (i / 2) * 50);
        lv_obj_set_size(tile, 228, 46);
        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_set_style_pad_hor(tile, 8, 0);
        char name[32];
        snprintf(name, sizeof(name), "%s  %04u", DRIVE_VALUES[i].label, DRIVE_VALUES[i].par);
        lv_obj_t *n = makeLabel(tile, name, &lv_font_montserrat_12, COL_MUTED);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        s_tileVal[i] = makeLabel(tile, "--", &lv_font_montserrat_20, COL_TEXT);
        lv_label_set_recolor(s_tileVal[i], true);
        lv_obj_align(s_tileVal[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    s_faultLbl = makeLabel(tab, "Last fault (0401): --", &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_recolor(s_faultLbl, true);
    lv_obj_set_pos(s_faultLbl, 10, 208);

    s_swLbl = makeLabel(tab, "STATUS WORD 0004", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(s_swLbl, 10, 232);

    for (int i = 0; i < STATUS_BIT_COUNT; i++) {
        const int x = 10 + (i / 7) * 236, y = 252 + (i % 7) * 20;
        s_bitDot[i] = lv_obj_create(tab);
        lv_obj_remove_style_all(s_bitDot[i]);
        lv_obj_set_size(s_bitDot[i], 9, 9);
        lv_obj_set_pos(s_bitDot[i], x, y + 4);
        lv_obj_set_style_radius(s_bitDot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_bitDot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_bitDot[i], lv_color_hex(COL_OFF), 0);
        char txt[40];
        snprintf(txt, sizeof(txt), "%d  %s", i, STATUS_BITS[i].name);
        s_bitLbl[i] = makeLabel(tab, txt, &lv_font_montserrat_12, COL_MUTED);
        lv_obj_set_pos(s_bitLbl[i], x + 16, y);
    }
}

static void refreshValuesTab()
{
    const DriveSnapshot &s = s_snap;
    char buf[96];
    for (int i = 0; i < DRIVE_VALUE_COUNT; i++) {
        formatValue(i, buf, sizeof(buf), true);
        setText(s_tileVal[i], buf);
    }
    if (s.faultValid) snprintf(buf, sizeof(buf), "Last fault (0401): #ffa45c %u - %s#", s.lastFault, faultName(s.lastFault));
    else snprintf(buf, sizeof(buf), "Last fault (0401): --");
    setText(s_faultLbl, buf);

    if (s.swValid)
        snprintf(buf, sizeof(buf), "STATUS WORD 0004:  0x%04X     CW 0x%04X   REF %d", s.sw, s.cw, s.refWord);
    else snprintf(buf, sizeof(buf), "STATUS WORD 0004:  --");
    setText(s_swLbl, buf);

    for (int i = 0; i < STATUS_BIT_COUNT; i++) {
        const bool on = s.online && s.swValid && (s.sw >> i & 1);
        uint32_t dot = COL_OFF;
        if (on) dot = STATUS_BITS[i].kind == 'b' ? COL_BAD : STATUS_BITS[i].kind == 'w' ? COL_WARN : COL_OK;
        lv_obj_set_style_bg_color(s_bitDot[i], lv_color_hex(dot), 0);
        lv_obj_set_style_text_color(s_bitLbl[i], lv_color_hex(on ? COL_TEXT : COL_MUTED), 0);
    }
}

// ---------------------------------------------------------------------------
// Param tab: read / write any parameter
// ---------------------------------------------------------------------------
static bool parseParNumber(uint16_t *par)
{
    const char *t = lv_textarea_get_text(s_parTa);
    const long p = atol(t);
    if (!t[0] || p <= 0 || p > 9999) {
        lv_obj_set_style_text_color(s_parInfo, lv_color_hex(COL_BAD), 0);
        lv_label_set_text(s_parInfo, "Enter a parameter number, e.g. 1105 or 2202");
        return false;
    }
    *par = (uint16_t)p;
    return true;
}

static void onParRead(lv_event_t *)
{
    uint16_t par;
    if (!parseParNumber(&par)) return;
    lv_obj_set_style_text_color(s_parInfo, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text_fmt(s_parInfo, "Reading %04u ...", par);
    sendCmd(DriveCmdType::ReadParam, 0, 0, par);
}

static void onWriteConfirm(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn = lv_msgbox_get_active_btn_text(mbox);
    if (btn && !strcmp(btn, "Write")) {
        lv_label_set_text_fmt(s_parInfo, "Writing %04u ...", s_pendingWrite.par);
        driveSend(s_pendingWrite);
    }
    lv_msgbox_close_async(mbox);
}

static void onParWrite(lv_event_t *)
{
    uint16_t par;
    if (!parseParNumber(&par)) return;
    const char *v = lv_textarea_get_text(s_valTa);
    char *end;
    const float value = strtof(v, &end);
    if (!v[0] || *end) {
        lv_obj_set_style_text_color(s_parInfo, lv_color_hex(COL_BAD), 0);
        lv_label_set_text(s_parInfo, "Enter the value to write");
        return;
    }
    s_pendingWrite = DriveCmd{};
    s_pendingWrite.type = DriveCmdType::WriteParam;
    s_pendingWrite.src = CmdSource::Panel;
    s_pendingWrite.par = par;
    s_pendingWrite.f = value;

    static const char *btns[] = {"Write", "Cancel", ""};
    char text[128];
    snprintf(text, sizeof(text), "Write %s %s to parameter %04u %s?\n\nThe value is changed in the drive immediately.", v,
             scalingFor(par, s_snap.scalarMode).unit, par, parName(par));
    lv_obj_t *mbox = lv_msgbox_create(nullptr, "Confirm write", text, btns, false);
    lv_obj_set_width(mbox, 400);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, onWriteConfirm, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void onParTaFocus(lv_event_t *e)
{
    lv_keyboard_set_textarea(s_parKb, lv_event_get_target(e));
}

static void onParKbReady(lv_event_t *)
{
    if (lv_keyboard_get_textarea(s_parKb) == s_parTa) onParRead(nullptr);
    else onParWrite(nullptr);
}

static void buildParamTab(lv_obj_t *tab)
{
    lv_obj_t *l1 = makeLabel(tab, "Parameter", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(l1, 10, 2);
    lv_obj_t *l2 = makeLabel(tab, "Value (scaled units)", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_set_pos(l2, 138, 2);

    s_parTa = lv_textarea_create(tab);
    lv_textarea_set_one_line(s_parTa, true);
    lv_textarea_set_accepted_chars(s_parTa, "0123456789");
    lv_textarea_set_max_length(s_parTa, 4);
    lv_textarea_set_placeholder_text(s_parTa, "1105");
    lv_obj_set_pos(s_parTa, 8, 18);
    lv_obj_set_width(s_parTa, 120);
    lv_obj_add_event_cb(s_parTa, onParTaFocus, LV_EVENT_FOCUSED, nullptr);

    s_valTa = lv_textarea_create(tab);
    lv_textarea_set_one_line(s_valTa, true);
    lv_textarea_set_accepted_chars(s_valTa, "0123456789.-");
    lv_textarea_set_max_length(s_valTa, 10);
    lv_obj_set_pos(s_valTa, 136, 18);
    lv_obj_set_width(s_valTa, 150);
    lv_obj_add_event_cb(s_valTa, onParTaFocus, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t *rd = makeButton(tab, "Read", COL_BTN, &lv_font_montserrat_16);
    lv_obj_set_pos(rd, 294, 18);
    lv_obj_set_size(rd, 86, 42);
    lv_obj_add_event_cb(rd, onParRead, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *wr = makeButton(tab, "Write", COL_AMBER, &lv_font_montserrat_16);
    lv_obj_set_pos(wr, 386, 18);
    lv_obj_set_size(wr, 86, 42);
    lv_obj_add_event_cb(wr, onParWrite, LV_EVENT_CLICKED, nullptr);

    s_parInfo = makeLabel(tab,
                          "Register = 40000 + parameter. Values use the drive's scaling (e.g. 2202 in s, "
                          "1105 in rpm or Hz). Results are also published to MQTT.",
                          &lv_font_montserrat_14, COL_MUTED);
    lv_label_set_long_mode(s_parInfo, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_parInfo, 10, 68);
    lv_obj_set_width(s_parInfo, 460);

    s_parKb = lv_keyboard_create(tab);
    keyboardUseNumericMap(s_parKb);
    lv_obj_set_pos(s_parKb, 8, 136);
    lv_obj_set_size(s_parKb, 464, 254);
    lv_keyboard_set_textarea(s_parKb, s_parTa);
    lv_obj_add_event_cb(s_parKb, onParKbReady, LV_EVENT_READY, nullptr);
}

static void refreshParamTab()
{
    const ParamResult &p = s_snap.param;
    if (p.seq == s_paramSeq) return;
    s_paramSeq = p.seq;
    lv_obj_set_style_text_color(s_parInfo, lv_color_hex(p.ok ? COL_OK : COL_BAD), 0);
    lv_label_set_text(s_parInfo, p.text);
    if (p.ok && !p.write && atol(lv_textarea_get_text(s_parTa)) == p.par) {
        char v[16];
        formatEng(p.par, p.raw, s_snap.scalarMode, v, sizeof(v));
        lv_textarea_set_text(s_valTa, v);
    }
}

// ---------------------------------------------------------------------------
// Log tab
// ---------------------------------------------------------------------------
static void buildLogTab(lv_obj_t *tab)
{
    s_statsLbl = makeLabel(tab, "", &lv_font_montserrat_12, COL_MUTED);
    lv_label_set_recolor(s_statsLbl, true);
    lv_obj_set_pos(s_statsLbl, 10, 8);

    lv_obj_t *clr = makeButton(tab, "Clear", COL_BTN, &lv_font_montserrat_12);
    lv_obj_set_pos(clr, 400, 2);
    lv_obj_set_size(clr, 72, 26);
    lv_obj_add_event_cb(clr, [](lv_event_t *) { lv_obj_clean(s_logList); }, LV_EVENT_CLICKED, nullptr);

    s_logList = lv_obj_create(tab);
    lv_obj_set_pos(s_logList, 8, 32);
    lv_obj_set_size(s_logList, 464, 358);
    lv_obj_set_style_bg_color(s_logList, lv_color_hex(COL_LOGBG), 0);
    lv_obj_set_style_border_color(s_logList, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_radius(s_logList, 6, 0);
    lv_obj_set_style_pad_all(s_logList, 6, 0);
    lv_obj_set_style_pad_row(s_logList, 2, 0);
    lv_obj_set_flex_flow(s_logList, LV_FLEX_FLOW_COLUMN);
}

static void refreshLog()
{
    LogEntry e;
    bool any = false;
    const bool atBottom = lv_obj_get_scroll_bottom(s_logList) < 30;
    for (int n = 0; n < 30 && logNext(&s_logSeq, &e); n++) {
        any = true;
        char line[128];
        snprintf(line, sizeof(line), "%s  %s", e.time, e.text);
        lv_obj_t *l = makeLabel(s_logList, line, &lv_font_montserrat_12, logColor(e.level));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        setText(s_logStrip, line);
        lv_obj_set_style_text_color(s_logStrip, lv_color_hex(logColor(e.level)), 0);
    }
    if (!any) return;
    while (lv_obj_get_child_cnt(s_logList) > 120) lv_obj_del(lv_obj_get_child(s_logList, 0));
    if (atBottom) lv_obj_scroll_to_y(s_logList, LV_COORD_MAX, LV_ANIM_OFF);
}

static void refreshStats()
{
    const MbStats &m = s_snap.stats;
    char buf[128];
    snprintf(buf, sizeof(buf), "TX #e6edf3 %lu#  RX #e6edf3 %lu#  Timeouts #e6edf3 %lu#  CRC #e6edf3 %lu#  Exc #e6edf3 %lu#  Cycle #e6edf3 %lu ms#",
             (unsigned long)m.tx, (unsigned long)m.rx, (unsigned long)m.timeouts, (unsigned long)m.crcErrors,
             (unsigned long)m.exceptions, (unsigned long)s_snap.cycleMs);
    setText(s_statsLbl, buf);
}

// ---------------------------------------------------------------------------
// Refresh loop
// ---------------------------------------------------------------------------
static void refresh(lv_timer_t *)
{
    driveGetSnapshot(&s_snap);
    NetStatus n;
    netGetStatus(&n);

    setDot(s_dotDrive, s_snap.online ? COL_OK : COL_BAD);
    setDot(s_dotWifi, n.wifiConnected ? COL_OK : n.wifiConfigured ? COL_WARN : 0x444444);
    setDot(s_dotMqtt, n.mqttConnected ? COL_OK : n.mqttConfigured ? COL_WARN : 0x444444);

    refreshLog();
    switch (lv_tabview_get_tab_act(s_tabview)) {
    case 0: refreshDriveTab(); break;
    case 1: refreshValuesTab(); break;
    case 2: refreshParamTab(); break;
    case 3: refreshStats(); break;
    case 4: refreshSetupTab(); break;
    }
}

static void styleTabButtons(lv_obj_t *btns)
{
    constexpr lv_style_selector_t ITEMS = LV_PART_ITEMS;
    constexpr lv_style_selector_t CHECKED = (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED;
    lv_obj_set_style_bg_color(btns, lv_color_hex(COL_HEADER), 0);
    lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(btns, 1, 0);
    lv_obj_set_style_border_color(btns, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_text_font(btns, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(btns, lv_color_hex(COL_MUTED), ITEMS);
    lv_obj_set_style_text_color(btns, lv_color_hex(COL_ACCENT), CHECKED);
    lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, CHECKED);
    lv_obj_set_style_bg_color(btns, lv_color_hex(COL_PANEL), CHECKED);
    lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_TOP, CHECKED);
    lv_obj_set_style_border_width(btns, 3, CHECKED);
    lv_obj_set_style_border_color(btns, lv_color_hex(COL_ACCENT), CHECKED);
}

static lv_obj_t *addTab(const char *name, bool scrollable)
{
    lv_obj_t *tab = lv_tabview_add_tab(s_tabview, name);
    lv_obj_set_style_pad_all(tab, 0, 0);
    if (!scrollable) lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    return tab;
}

}  // namespace ui

using namespace ui;

void uiInit(const char *bootInfo)
{
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(COL_ACCENT), lv_color_hex(0x3fa9f5), true,
                                              &lv_font_montserrat_16);
    lv_disp_set_theme(disp, theme);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    buildHeader(scr);

    s_tabview = lv_tabview_create(scr, LV_DIR_BOTTOM, TABBAR_H);
    lv_obj_set_pos(s_tabview, 0, HEADER_H);
    lv_obj_set_size(s_tabview, LCD_WIDTH, LCD_HEIGHT - HEADER_H);
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(COL_BG), 0);
    styleTabButtons(lv_tabview_get_tab_btns(s_tabview));
    lv_obj_clear_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);  // no swiping between tabs

    buildDriveTab(addTab(LV_SYMBOL_HOME " Drive", false));
    buildValuesTab(addTab(LV_SYMBOL_LIST " Values", false));
    buildParamTab(addTab(LV_SYMBOL_EDIT " Param", false));
    buildLogTab(addTab(LV_SYMBOL_FILE " Log", false));
    buildSetupTab(addTab(LV_SYMBOL_SETTINGS " Setup", true), bootInfo);

    s_logSeq = 0;
    lv_timer_create(refresh, 200, nullptr);
    refresh(nullptr);
}
