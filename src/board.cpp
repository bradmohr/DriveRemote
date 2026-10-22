#include "board.h"

#include <Wire.h>
#include <lvgl.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// IO expander (V4: CH32V003, V1-V3: TCA9554)
// ---------------------------------------------------------------------------
static constexpr uint8_t CH32_ADDR = 0x24;
static constexpr uint8_t CH32_REG_DIR = 0x02;
static constexpr uint8_t CH32_REG_OUT = 0x03;
static constexpr uint8_t CH32_REG_PWM = 0x05;
static constexpr uint8_t CH32_TP_RST = 1 << 1;
static constexpr uint8_t CH32_LCD_RST = 1 << 3;
static constexpr uint8_t CH32_SYS_EN = 1 << 5;

static constexpr uint8_t TCA_ADDR = 0x20;
static constexpr uint8_t TCA_REG_OUT = 0x01;
static constexpr uint8_t TCA_REG_CFG = 0x03;
static constexpr uint8_t TCA_TP_RST = 1 << 0;
static constexpr uint8_t TCA_BL_EN = 1 << 1;
static constexpr uint8_t TCA_LCD_RST = 1 << 2;
static constexpr uint8_t TCA_SD_CS = 1 << 3;
static constexpr uint8_t TCA_BUZZER = 1 << 5;

static BoardRev s_rev = BoardRev::Unknown;
static uint8_t s_tcaOut = 0;

static bool i2cProbe(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        Wire.write(value);
        if (Wire.endTransmission() == 0) return true;
        delay(10);
    }
    return false;
}

// A quick reset can leave a slave holding SDA low; clock it free.
static void i2cRecoverBus()
{
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
        digitalWrite(PIN_I2C_SCL, LOW);
        delayMicroseconds(10);
        digitalWrite(PIN_I2C_SCL, HIGH);
        delayMicroseconds(10);
    }
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);
    delay(2);
}

static void expanderInit()
{
    bool ch32 = false, tca = false;
    for (int attempt = 0; attempt < 5 && !ch32 && !tca; attempt++) {
        ch32 = i2cProbe(CH32_ADDR);
        tca = !ch32 && i2cProbe(TCA_ADDR);
        if (!ch32 && !tca) delay(20);
    }
    if (ch32) {
        s_rev = BoardRev::V4;
        i2cWriteReg(CH32_ADDR, CH32_REG_DIR, 0xFF);
        i2cWriteReg(CH32_ADDR, CH32_REG_OUT, 0x00);  // hold LCD + touch in reset
        delay(120);
        i2cWriteReg(CH32_ADDR, CH32_REG_DIR, 0xFF);
        i2cWriteReg(CH32_ADDR, CH32_REG_OUT, CH32_SYS_EN | CH32_LCD_RST | CH32_TP_RST);
        delay(200);
    } else if (tca) {
        s_rev = BoardRev::V1toV3;
        const uint8_t outputs = TCA_TP_RST | TCA_BL_EN | TCA_LCD_RST | TCA_SD_CS | TCA_BUZZER;
        i2cWriteReg(TCA_ADDR, TCA_REG_CFG, (uint8_t)~outputs);  // 0 = output
        s_tcaOut = 0;                                           // resets held, backlight + buzzer off
        i2cWriteReg(TCA_ADDR, TCA_REG_OUT, s_tcaOut);
        delay(120);
        s_tcaOut = TCA_TP_RST | TCA_LCD_RST | TCA_SD_CS;  // backlight comes on after the first frame
        i2cWriteReg(TCA_ADDR, TCA_REG_OUT, s_tcaOut);
        delay(200);
    }
}

static uint8_t s_brightness = 100;
static bool s_asleep = false;     // backlight switched off by the screen saver
static bool s_wakeTouch = false;  // the touch that woke the screen, hidden from LVGL until released

static void setBacklight(uint8_t percent)
{
    if (s_rev == BoardRev::V4) {
        // The PWM drives the backlight inverted: 0 = full brightness, 255 = off
        i2cWriteReg(CH32_ADDR, CH32_REG_PWM, (uint8_t)(((100 - percent) * 255 + 50) / 100));
    } else if (s_rev == BoardRev::V1toV3) {
        s_tcaOut = percent ? (s_tcaOut | TCA_BL_EN) : (s_tcaOut & ~TCA_BL_EN);  // on/off only
        i2cWriteReg(TCA_ADDR, TCA_REG_OUT, s_tcaOut);
    }
}

void boardBacklight(uint8_t percent)
{
    s_brightness = min<uint8_t>(percent, 100);
    if (!s_asleep) setBacklight(s_brightness);
}

void boardScreenSleep()
{
    if (s_asleep) return;
    s_asleep = true;
    setBacklight(0);
}

bool boardScreenAsleep() { return s_asleep; }

BoardRev boardRev() { return s_rev; }

const char *boardRevText()
{
    switch (s_rev) {
    case BoardRev::V4: return "V4 (CH32V003)";
    case BoardRev::V1toV3: return "V1-V3 (TCA9554)";
    default: return "unknown (no IO expander)";
    }
}

// ---------------------------------------------------------------------------
// ST7701 init over bit-banged 3-wire SPI (9-bit words: D/C bit + 8 data bits).
// Sequence is Waveshare's "type1" init for this 4" 480x480 panel.
// ---------------------------------------------------------------------------
struct LcdInitCmd {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[16];
    uint8_t delayMs;
};

static const LcdInitCmd ST7701_INIT[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}, 0},
    {0xC0, 2, {0x3B, 0x00}, 0},
    {0xC1, 2, {0x0D, 0x02}, 0},
    {0xC2, 2, {0x31, 0x05}, 0},
    {0xCD, 1, {0x08}, 0},
    {0xB0, 16, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 0},
    {0xB1, 16, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}, 0},
    {0xB0, 1, {0x60}, 0},
    {0xB1, 1, {0x32}, 0},
    {0xB2, 1, {0x07}, 0},
    {0xB3, 1, {0x80}, 0},
    {0xB5, 1, {0x49}, 0},
    {0xB7, 1, {0x85}, 0},
    {0xB8, 1, {0x21}, 0},
    {0xC1, 1, {0x78}, 0},
    {0xC2, 1, {0x78}, 0},
    {0xE0, 3, {0x00, 0x1B, 0x02}, 0},
    {0xE1, 11, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 0},
    {0xE2, 12, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 0},
    {0xE3, 4, {0x00, 0x00, 0x11, 0x11}, 0},
    {0xE4, 2, {0x44, 0x44}, 0},
    {0xE5, 16, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 0},
    {0xE6, 4, {0x00, 0x00, 0x11, 0x11}, 0},
    {0xE7, 2, {0x44, 0x44}, 0},
    {0xE8, 16, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 0},
    {0xEB, 7, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 0},
    {0xEC, 2, {0x3C, 0x00}, 0},
    {0xED, 16, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}, 0},
    {0xE5, 1, {0xE4}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}, 0},
    {0x21, 0, {}, 0},       // display inversion on (IPS)
    {0x3A, 1, {0x60}, 0},   // pixel format
    {0x11, 0, {}, 120},     // sleep out
    {0x29, 0, {}, 0},       // display on
};

static void lcdSpiWrite9(bool isData, uint8_t value)
{
    uint16_t word = (isData ? 0x100 : 0) | value;
    for (int bit = 8; bit >= 0; bit--) {
        digitalWrite(PIN_LCD_MOSI, (word >> bit) & 1);
        delayMicroseconds(1);
        digitalWrite(PIN_LCD_SCK, HIGH);
        delayMicroseconds(1);
        digitalWrite(PIN_LCD_SCK, LOW);
    }
}

static void lcdSendCommand(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    digitalWrite(PIN_LCD_CS, LOW);
    lcdSpiWrite9(false, cmd);
    for (uint8_t i = 0; i < len; i++) lcdSpiWrite9(true, data[i]);
    digitalWrite(PIN_LCD_CS, HIGH);
}

static void st7701Init()
{
    pinMode(PIN_LCD_CS, OUTPUT);
    pinMode(PIN_LCD_SCK, OUTPUT);
    pinMode(PIN_LCD_MOSI, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);
    digitalWrite(PIN_LCD_SCK, LOW);

    lcdSendCommand(0x01, nullptr, 0);  // software reset
    delay(120);
    for (const LcdInitCmd &c : ST7701_INIT) {
        lcdSendCommand(c.cmd, c.data, c.len);
        if (c.delayMs) delay(c.delayMs);
    }
}

// ---------------------------------------------------------------------------
// RGB panel (esp_lcd) - frame buffer in PSRAM, bounce buffers in SRAM so
// Wi-Fi traffic does not starve the display DMA.
// ---------------------------------------------------------------------------
static esp_lcd_panel_handle_t s_panel = nullptr;

static bool rgbPanelInit()
{
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = 12 * 1000 * 1000;
    cfg.timings.h_res = LCD_WIDTH;
    cfg.timings.v_res = LCD_HEIGHT;
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch = 50;
    cfg.timings.hsync_front_porch = 10;
    cfg.timings.vsync_pulse_width = 8;
    cfg.timings.vsync_back_porch = 20;
    cfg.timings.vsync_front_porch = 10;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = LCD_WIDTH * 20;
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = PIN_LCD_HSYNC;
    cfg.vsync_gpio_num = PIN_LCD_VSYNC;
    cfg.de_gpio_num = PIN_LCD_DE;
    cfg.pclk_gpio_num = PIN_LCD_PCLK;
    cfg.disp_gpio_num = -1;
    // RGB565 bit order: B0-B4, G0-G5, R0-R4
    const int data[16] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
    cfg.flags.fb_in_psram = 1;

    if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) return false;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    return true;
}

// ---------------------------------------------------------------------------
// GT911 touch (polled; INT/RST are not needed)
// ---------------------------------------------------------------------------
static uint8_t s_touchAddr = 0;

static bool gtRead(uint16_t reg, uint8_t *buf, uint8_t len)
{
    Wire.beginTransmission(s_touchAddr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(s_touchAddr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool gtWrite8(uint16_t reg, uint8_t value)
{
    Wire.beginTransmission(s_touchAddr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xFF);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool touchProbe()
{
    for (uint8_t addr : {0x5D, 0x14}) {
        if (i2cProbe(addr)) {
            s_touchAddr = addr;
            return true;
        }
    }
    return false;
}

// Pulse the GT911 reset line through the IO expander. INT is held low, so it comes back at 0x5D.
static void touchReset()
{
    if (s_rev == BoardRev::V4) {
        i2cWriteReg(CH32_ADDR, CH32_REG_OUT, CH32_SYS_EN | CH32_LCD_RST);
        delay(10);
        i2cWriteReg(CH32_ADDR, CH32_REG_OUT, CH32_SYS_EN | CH32_LCD_RST | CH32_TP_RST);
    } else if (s_rev == BoardRev::V1toV3) {
        i2cWriteReg(TCA_ADDR, TCA_REG_OUT, s_tcaOut & ~TCA_TP_RST);
        delay(10);
        i2cWriteReg(TCA_ADDR, TCA_REG_OUT, s_tcaOut);
    }
    delay(100);
}

static void touchInit()
{
    for (int attempt = 0; attempt < 6 && !touchProbe(); attempt++) {
        if (attempt == 2) touchReset();
        delay(50);
    }
    if (!s_touchAddr) {
        char list[64] = "";
        size_t n = 0;
        for (uint8_t a = 0x08; a < 0x78 && n < sizeof(list) - 6; a++)
            if (i2cProbe(a)) n += snprintf(list + n, sizeof(list) - n, " 0x%02X", a);
        logf(LOG_ERR, "Touch controller (GT911) not found, still trying. I2C devices:%s", n ? list : " none");
        return;
    }
    char id[5] = {0};
    gtRead(0x8140, (uint8_t *)id, 4);
    logf(LOG_INFO, "Touch GT%s at 0x%02X", id, s_touchAddr);
}

// Keep looking for a touch controller that didn't answer at boot, so touch comes back without a reboot
static void touchRetry()
{
    static uint32_t lastTry = 0;
    static uint8_t tries = 0;
    if (millis() - lastTry < 3000) return;
    lastTry = millis();
    if (++tries % 5 == 0) touchReset();
    if (touchProbe()) logf(LOG_OK, "Touch controller found at 0x%02X", s_touchAddr);
}

bool boardTouchFound() { return s_touchAddr != 0; }

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data)
{
    static lv_coord_t lastX = 0, lastY = 0;
    static bool pressed = false;

    if (!s_touchAddr) touchRetry();
    uint8_t status;
    if (s_touchAddr && gtRead(0x814E, &status, 1) && (status & 0x80)) {
        uint8_t points = status & 0x0F;
        uint8_t p[4];
        if (points > 0 && points <= 5 && gtRead(0x8150, p, 4)) {
            lastX = constrain((lv_coord_t)(p[0] | (p[1] << 8)), 0, LCD_WIDTH - 1);
            lastY = constrain((lv_coord_t)(p[2] | (p[3] << 8)), 0, LCD_HEIGHT - 1);
            pressed = true;
        } else {
            pressed = false;
        }
        gtWrite8(0x814E, 0);  // release the buffer for the next sample
    }
    // A touch on the dark screen only wakes it. LVGL sees "released" until that finger
    // lifts, so the press can't operate whatever is underneath.
    if (pressed && s_asleep) {
        s_asleep = false;
        s_wakeTouch = true;
        setBacklight(s_brightness);
        lv_disp_trig_activity(nullptr);
    }
    if (s_wakeTouch && !pressed) s_wakeTouch = false;
    // Raw panel coordinates; LVGL applies the display rotation itself
    data->point.x = lastX;
    data->point.y = lastY;
    data->state = pressed && !s_wakeTouch ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// ---------------------------------------------------------------------------
// LVGL glue
// ---------------------------------------------------------------------------
static constexpr int DRAW_BUF_LINES = 40;

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    lv_disp_flush_ready(drv);
}

bool boardInit()
{
    i2cRecoverBus();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    expanderInit();
    logf(LOG_INFO, "Board revision: %s", boardRevText());

    st7701Init();
    if (!rgbPanelInit()) {
        logf(LOG_ERR, "RGB panel init failed");
        return false;
    }
    touchInit();

    lv_init();
    static lv_disp_draw_buf_t drawBuf;
    const size_t pixels = LCD_WIDTH * DRAW_BUF_LINES;
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&drawBuf, buf, nullptr, pixels);

    static lv_disp_drv_t dispDrv;
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = LCD_WIDTH;
    dispDrv.ver_res = LCD_HEIGHT;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
#if LCD_ROTATION == 2
    dispDrv.sw_rotate = 1;
    dispDrv.rotated = LV_DISP_ROT_180;
#endif
    lv_disp_drv_register(&dispDrv);

    static lv_indev_drv_t indevDrv;
    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchReadCb;
    lv_indev_drv_register(&indevDrv);
    return true;
}
