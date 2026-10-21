// Setup page: Wi-Fi, MQTT, Modbus and panel settings (saved to NVS).
#include <Arduino.h>

#include <vector>

#include "app_config.h"
#include "board.h"
#include "logger.h"
#include "net.h"
#include "settings.h"
#include "ui_internal.h"

namespace ui {

static lv_obj_t *s_page;
static lv_obj_t *s_kb, *s_kbTarget;
static lv_obj_t *s_taSsid, *s_taWifiPass, *s_btnScan;
static lv_obj_t *s_swMqtt, *s_taHost, *s_taPort, *s_swTls, *s_swAuth, *s_taUser, *s_taMqttPass, *s_taBase, *s_taSuffix;
static lv_obj_t *s_swRetain, *s_swCtrl, *s_swParW, *s_taMinMs;
static lv_obj_t *s_ddBaud, *s_ddFmt, *s_taSlave, *s_taPoll;
static lv_obj_t *s_slBright, *s_taTz;
static lv_obj_t *s_saveLbl, *s_infoLbl;
static bool s_scanning = false;
static const char *s_bootInfo = "";

// ---------------------------------------------------------------------------
// On-screen keyboard (shared by all text fields on this page)
// ---------------------------------------------------------------------------
static void kbHide()
{
    if (lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) return;
    lv_keyboard_set_textarea(s_kb, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(tabview(), LCD_HEIGHT - HEADER_H);
    if (s_kbTarget) lv_obj_clear_state(s_kbTarget, LV_STATE_FOCUSED);
    s_kbTarget = nullptr;
}

static void kbShow(lv_obj_t *ta)
{
    if (lv_obj_get_user_data(ta)) keyboardUseNumericMap(s_kb);
    else lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    s_kbTarget = ta;
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(tabview(), LCD_HEIGHT - HEADER_H - KEYBOARD_H);
    lv_obj_update_layout(tabview());
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
}

static void onTextarea(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (s_kbTarget != ta || lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) kbShow(ta);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (s_kbTarget == ta) kbHide();
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        kbHide();
    }
}

// ---------------------------------------------------------------------------
// Field builders
// ---------------------------------------------------------------------------
static lv_obj_t *section(const char *title)
{
    lv_obj_t *p = makePanel(s_page);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 6, 0);
    makeLabel(p, title, &lv_font_montserrat_16, COL_ACCENT2);
    return p;
}

static lv_obj_t *hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = makeLabel(parent, text, &lv_font_montserrat_12, COL_MUTED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    return r;
}

static lv_obj_t *textArea(lv_obj_t *parent, const char *value, bool numeric, bool password, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, lv_pct(100));
    if (numeric) lv_textarea_set_accepted_chars(ta, "0123456789");
    if (password) lv_textarea_set_password_mode(ta, true);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, value);
    lv_obj_set_user_data(ta, numeric ? (void *)1 : nullptr);
    lv_obj_add_event_cb(ta, onTextarea, LV_EVENT_ALL, nullptr);
    return ta;
}

static lv_obj_t *textField(lv_obj_t *sec, const char *label, const char *value, bool numeric = false,
                           const char *placeholder = nullptr)
{
    makeLabel(sec, label, &lv_font_montserrat_12, COL_MUTED);
    return textArea(sec, value, numeric, false, placeholder);
}

static void onShowPassword(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    lv_textarea_set_password_mode((lv_obj_t *)lv_event_get_user_data(e), !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

static lv_obj_t *passwordField(lv_obj_t *sec, const char *label, const char *value)
{
    makeLabel(sec, label, &lv_font_montserrat_12, COL_MUTED);
    lv_obj_t *r = row(sec);
    lv_obj_t *ta = textArea(r, value, false, true, nullptr);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_t *show = lv_checkbox_create(r);
    lv_checkbox_set_text(show, "Show");
    lv_obj_set_style_text_font(show, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(show, onShowPassword, LV_EVENT_VALUE_CHANGED, ta);
    return ta;
}

static lv_obj_t *switchField(lv_obj_t *sec, const char *label, bool on)
{
    lv_obj_t *r = row(sec);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = makeLabel(r, label, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *sw = lv_switch_create(r);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t *dropdownField(lv_obj_t *sec, const char *label, const char *options, uint16_t selected)
{
    makeLabel(sec, label, &lv_font_montserrat_12, COL_MUTED);
    lv_obj_t *dd = lv_dropdown_create(sec);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_width(dd, lv_pct(100));
    return dd;
}

static bool isOn(lv_obj_t *sw) { return lv_obj_has_state(sw, LV_STATE_CHECKED); }

static String textOf(lv_obj_t *ta)
{
    String s = lv_textarea_get_text(ta);
    s.trim();
    return s;
}

// Accept a pasted URL such as mqtts://host:8883/mqtt as well as a bare host
static String hostOnly(String s)
{
    s.trim();
    const int scheme = s.indexOf("://");
    if (scheme >= 0) s = s.substring(scheme + 3);
    const int slash = s.indexOf('/');
    if (slash >= 0) s = s.substring(0, slash);
    const int colon = s.indexOf(':');
    if (colon >= 0) s = s.substring(0, colon);
    return s;
}

// ---------------------------------------------------------------------------
// Wi-Fi scan
// ---------------------------------------------------------------------------
static void onScanPick(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *list = lv_obj_get_parent(btn);
    lv_textarea_set_text(s_taSsid, lv_list_get_btn_text(list, btn));
    lv_obj_del_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void showScanResults(const std::vector<String> &ssids)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = makePanel(overlay);
    lv_obj_set_size(box, 420, 400);
    lv_obj_center(box);
    makeLabel(box, ssids.empty() ? "No networks found" : "Select network", &lv_font_montserrat_16, COL_TEXT);

    lv_obj_t *close = makeButton(box, LV_SYMBOL_CLOSE, COL_BTN, &lv_font_montserrat_16);
    lv_obj_set_size(close, 44, 36);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -6);
    lv_obj_add_event_cb(close, [](lv_event_t *e) { lv_obj_del_async((lv_obj_t *)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, overlay);

    lv_obj_t *list = lv_list_create(box);
    lv_obj_set_size(list, 400, 330);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    for (const String &ssid : ssids) {
        lv_obj_t *b = lv_list_add_btn(list, LV_SYMBOL_WIFI, ssid.c_str());
        lv_obj_add_event_cb(b, onScanPick, LV_EVENT_CLICKED, overlay);
    }
}

// Follow the usual port when TLS is switched, unless a custom port was entered
static void onTlsToggle(lv_event_t *)
{
    const char *port = lv_textarea_get_text(s_taPort);
    if (isOn(s_swTls) && !strcmp(port, "1883")) lv_textarea_set_text(s_taPort, "8883");
    else if (!isOn(s_swTls) && !strcmp(port, "8883")) lv_textarea_set_text(s_taPort, "1883");
}

static void onScan(lv_event_t *)
{
    if (s_scanning) return;
    kbHide();
    netScanStart();
    s_scanning = true;
    setButtonText(s_btnScan, "...");
}

// ---------------------------------------------------------------------------
// Save / reboot
// ---------------------------------------------------------------------------
static void showSaveResult(bool ok, const char *msg)
{
    lv_obj_set_style_text_color(s_saveLbl, lv_color_hex(ok ? COL_OK : COL_BAD), 0);
    lv_label_set_text(s_saveLbl, msg);
}

static void onSave(lv_event_t *)
{
    kbHide();
    Settings s = settingsGet();
    s.wifiSsid = lv_textarea_get_text(s_taSsid);
    s.wifiPass = lv_textarea_get_text(s_taWifiPass);

    s.mqttEnabled = isOn(s_swMqtt);
    s.mqttHost = hostOnly(lv_textarea_get_text(s_taHost));
    lv_textarea_set_text(s_taHost, s.mqttHost.c_str());
    const long port = atol(lv_textarea_get_text(s_taPort));
    s.mqttTls = isOn(s_swTls);
    s.mqttAuth = isOn(s_swAuth);
    s.mqttUser = textOf(s_taUser);
    s.mqttPass = lv_textarea_get_text(s_taMqttPass);
    s.mqttBase = textOf(s_taBase);
    while (s.mqttBase.endsWith("/")) s.mqttBase.remove(s.mqttBase.length() - 1);
    s.mqttCmdSuffix = textOf(s_taSuffix);
    s.mqttRetain = isOn(s_swRetain);
    s.mqttAllowControl = isOn(s_swCtrl);
    s.mqttAllowParamWrite = isOn(s_swParW);
    const long minMs = atol(lv_textarea_get_text(s_taMinMs));

    s.mbBaud = MB_BAUD_RATES[lv_dropdown_get_selected(s_ddBaud)];
    s.mbFormat = lv_dropdown_get_selected(s_ddFmt);
    const long slave = atol(lv_textarea_get_text(s_taSlave));
    const long poll = atol(lv_textarea_get_text(s_taPoll));

    s.brightness = lv_slider_get_value(s_slBright);
    s.timezone = textOf(s_taTz);
    if (s.timezone.isEmpty()) s.timezone = "UTC0";

    const char *err = nullptr;
    if (port < 1 || port > 65535) err = "MQTT port must be 1-65535";
    else if (s.mqttBase.isEmpty() || s.mqttBase.indexOf('+') >= 0 || s.mqttBase.indexOf('#') >= 0)
        err = "Base topic must be set and cannot contain + or #";
    else if (s.mqttCmdSuffix.indexOf('+') >= 0 || s.mqttCmdSuffix.indexOf('#') >= 0)
        err = "Command suffix cannot contain + or #";
    else if (minMs < 100 || minMs > 60000) err = "Publish interval must be 100-60000 ms";
    else if (slave < 1 || slave > 247) err = "Station ID must be 1-247";
    else if (poll < 100 || poll > 5000) err = "Poll time must be 100-5000 ms";
    if (err) {
        showSaveResult(false, err);
        return;
    }
    s.mqttPort = port;
    s.mqttMinIntervalMs = minMs;
    s.mbSlave = slave;
    s.mbPollMs = poll;

    settingsSave(s);
    logf(LOG_OK, "Settings saved");
    showSaveResult(true, "Saved and applied");
}

static void onRebootConfirm(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn = lv_msgbox_get_active_btn_text(mbox);
    if (btn && !strcmp(btn, "Reboot")) {
        logf(LOG_WARN, "Rebooting (requested from panel)");
        delay(200);
        ESP.restart();
    }
    lv_msgbox_close_async(mbox);
}

static void onReboot(lv_event_t *)
{
    static const char *btns[] = {"Reboot", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(nullptr, "Reboot panel",
                                      "Restart the panel now? The drive keeps its last command and the panel "
                                      "resumes control after it reconnects.",
                                      btns, false);
    lv_obj_set_width(mbox, 400);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, onRebootConfirm, LV_EVENT_VALUE_CHANGED, nullptr);
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void buildSetupTab(lv_obj_t *tab, const char *bootInfo)
{
    s_bootInfo = bootInfo;
    s_page = tab;
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_set_style_pad_row(tab, 8, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    const Settings st = settingsGet();
    char num[12];

    // Wi-Fi
    lv_obj_t *sec = section(LV_SYMBOL_WIFI "  Wi-Fi");
    makeLabel(sec, "Network (SSID)", &lv_font_montserrat_12, COL_MUTED);
    lv_obj_t *r = row(sec);
    s_taSsid = textArea(r, st.wifiSsid.c_str(), false, false, "network name");
    lv_obj_set_flex_grow(s_taSsid, 1);
    s_btnScan = makeButton(r, "Scan", COL_BTN, &lv_font_montserrat_14);
    lv_obj_set_size(s_btnScan, 80, 40);
    lv_obj_add_event_cb(s_btnScan, onScan, LV_EVENT_CLICKED, nullptr);
    s_taWifiPass = passwordField(sec, "Password", st.wifiPass.c_str());

    // MQTT
    sec = section(LV_SYMBOL_SHUFFLE "  MQTT");
    s_swMqtt = switchField(sec, "MQTT enabled", st.mqttEnabled);
    s_taHost = textField(sec, "Broker host or IP", st.mqttHost.c_str(), false, "192.168.1.10");
    snprintf(num, sizeof(num), "%u", st.mqttPort);
    s_taPort = textField(sec, "Port", num, true);
    s_swTls = switchField(sec, "Use TLS encryption (EMQX Cloud: on, port 8883)", st.mqttTls);
    lv_obj_add_event_cb(s_swTls, onTlsToggle, LV_EVENT_VALUE_CHANGED, nullptr);
    s_swAuth = switchField(sec, "Broker needs username / password", st.mqttAuth);
    s_taUser = textField(sec, "Username", st.mqttUser.c_str());
    s_taMqttPass = passwordField(sec, "Password", st.mqttPass.c_str());
    s_taBase = textField(sec, "Base topic", st.mqttBase.c_str(), false, "Drive");
    s_taSuffix = textField(sec, "Command topic suffix", st.mqttCmdSuffix.c_str(), false, "empty, or /set");
    hint(sec,
         "Values are published to <base>/XXXX (e.g. Drive/0102 = speed in rpm). Commands go to "
         "<base>/XXXX<suffix>: 0001 START/STOP/COAST/QSTOP/RESET, 0002 speed in rpm (negative = "
         "reverse), any other XXXX: ? to read or a value to write. Commands sent with retain are ignored.");
    s_swRetain = switchField(sec, "Publish values as retained", st.mqttRetain);
    s_swCtrl = switchField(sec, "Allow start / stop / speed over MQTT", st.mqttAllowControl);
    s_swParW = switchField(sec, "Allow parameter writes over MQTT", st.mqttAllowParamWrite);
    snprintf(num, sizeof(num), "%u", st.mqttMinIntervalMs);
    s_taMinMs = textField(sec, "Fastest re-publish of a changing value (ms)", num, true);

    // Modbus
    sec = section(LV_SYMBOL_LOOP "  Modbus RTU (RS485)");
    uint16_t baudIdx = 1;
    String baudOpts;
    for (size_t i = 0; i < MB_BAUD_COUNT; i++) {
        if (i) baudOpts += "\n";
        baudOpts += String(MB_BAUD_RATES[i]);
        if (MB_BAUD_RATES[i] == st.mbBaud) baudIdx = i;
    }
    s_ddBaud = dropdownField(sec, "Baud rate (5303)", baudOpts.c_str(), baudIdx);
    s_ddFmt = dropdownField(sec, "Format (5304)", "8N1\n8N2\n8E1\n8O1", st.mbFormat);
    snprintf(num, sizeof(num), "%u", st.mbSlave);
    s_taSlave = textField(sec, "Station ID (5302)", num, true);
    snprintf(num, sizeof(num), "%u", st.mbPollMs);
    s_taPoll = textField(sec, "Poll interval (ms)", num, true);
    hint(sec, "Must match the drive's group 53 settings. Set 3018 COMM FAULT FUNC = 1 FAULT so the drive "
              "stops if this panel goes quiet.");

    // Panel
    sec = section(LV_SYMBOL_IMAGE "  Panel");
    makeLabel(sec, boardRev() == BoardRev::V4 ? "Brightness" : "Brightness (on/off only on this board revision)",
              &lv_font_montserrat_12, COL_MUTED);
    s_slBright = lv_slider_create(sec);
    lv_obj_set_width(s_slBright, lv_pct(95));
    lv_slider_set_range(s_slBright, 10, 100);
    lv_slider_set_value(s_slBright, st.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(
        s_slBright, [](lv_event_t *e) { boardBacklight(lv_slider_get_value(lv_event_get_target(e))); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    s_taTz = textField(sec, "Time zone (POSIX TZ)", st.timezone.c_str(), false, "UTC0");
    hint(sec, "Examples: EST5EDT,M3.2.0,M11.1.0   CST6CDT,M3.2.0,M11.1.0   AEST-10AEDT,M10.1.0,M4.1.0/3   GMT0BST,M3.5.0/1,M10.5.0");

    // Actions
    r = row(s_page);
    lv_obj_t *save = makeButton(r, LV_SYMBOL_SAVE "  Save & apply", COL_ACCENT, &lv_font_montserrat_16);
    lv_obj_set_size(save, 260, 48);
    lv_obj_add_event_cb(save, onSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reboot = makeButton(r, LV_SYMBOL_POWER "  Reboot", COL_BTN, &lv_font_montserrat_16);
    lv_obj_set_size(reboot, 180, 48);
    lv_obj_add_event_cb(reboot, onReboot, LV_EVENT_CLICKED, nullptr);
    s_saveLbl = makeLabel(s_page, "", &lv_font_montserrat_14, COL_OK);

    // Status
    sec = section(LV_SYMBOL_LIST "  Status");
    s_infoLbl = hint(sec, "");
    lv_obj_set_style_text_font(s_infoLbl, &lv_font_montserrat_14, 0);

    // Keyboard lives on the top layer so it can overlap the tab bar
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, LCD_WIDTH, KEYBOARD_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        s_kb, [](lv_event_t *e) {
            const lv_event_code_t c = lv_event_get_code(e);
            if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) kbHide();
        },
        LV_EVENT_ALL, nullptr);
}

void refreshSetupTab()
{
    if (s_scanning) {
        std::vector<String> ssids;
        if (netScanResults(&ssids) >= 0) {
            s_scanning = false;
            setButtonText(s_btnScan, "Scan");
            showScanResults(ssids);
        }
    }

    NetStatus n;
    netGetStatus(&n);
    char buf[320];
    snprintf(buf, sizeof(buf),
             "Wi-Fi: %s   IP %s   %d dBm\nMQTT: %s\nMAC %s\n%s %s, board %s\n%s",
             n.wifiConnected ? "connected" : n.wifiConfigured ? "connecting..." : "not set up", n.ip, n.rssi,
             n.mqttConnected ? "connected" : n.mqttConfigured ? "connecting..." : "off / not set up", n.mac, APP_NAME,
             APP_VERSION, boardRevText(), s_bootInfo);
    setText(s_infoLbl, buf);
}

}  // namespace ui
