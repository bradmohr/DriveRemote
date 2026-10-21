// ACS550 remote panel - Waveshare ESP32-S3-Touch-LCD-4
//
// Tasks:
//   loop (core 1)  LVGL user interface + touch
//   drive (core 0) Modbus RTU master for the ACS550, publishes values
//   net (core 0)   Wi-Fi, MQTT client, log forwarding
// All three are watched by the task watchdog; if any of them stops for
// WDT_TIMEOUT_S the panel reboots and resumes control of the drive.

#include <Arduino.h>
#include <lvgl.h>

#include "app_config.h"
#include "board.h"
#include "drive.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "logger.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

// Survive software/watchdog resets (not power cycles)
RTC_NOINIT_ATTR static uint32_t s_rtcMagic;
RTC_NOINIT_ATTR static uint32_t s_watchdogResets;
static constexpr uint32_t RTC_MAGIC = 0xAC550001;

static char s_bootInfo[96];

static const char *resetReasonText(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog (hang)";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brown-out (supply dip)";
    case ESP_RST_USB: return "USB";
    default: return "other";
    }
}

static void watchdogInit()
{
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;  // also catch a CPU that never idles
    cfg.trigger_panic = true;                            // panic -> reboot
    if (esp_task_wdt_reconfigure(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_init(&cfg);
    esp_task_wdt_add(nullptr);  // this (LVGL) task
}

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);  // never block when no USB host is attached
    logInit();

    const esp_reset_reason_t why = esp_reset_reason();
    if (s_rtcMagic != RTC_MAGIC || why == ESP_RST_POWERON) {
        s_rtcMagic = RTC_MAGIC;
        s_watchdogResets = 0;
    }
    const bool crashed =
        why == ESP_RST_TASK_WDT || why == ESP_RST_INT_WDT || why == ESP_RST_WDT || why == ESP_RST_PANIC;
    if (crashed) s_watchdogResets++;
    if (crashed)
        snprintf(s_bootInfo, sizeof(s_bootInfo), "Last reset: %s (%lu since power on)", resetReasonText(why),
                 (unsigned long)s_watchdogResets);
    else snprintf(s_bootInfo, sizeof(s_bootInfo), "Last reset: %s", resetReasonText(why));
    logf(crashed ? LOG_WARN : LOG_INFO, "%s %s starting. %s", APP_NAME, APP_VERSION, s_bootInfo);

    watchdogInit();
    settingsInit();

    // Talk to the drive first: after a watchdog reboot the drive's comm-loss
    // timer (3019) is already running.
    driveStart();

    if (!boardInit()) logf(LOG_ERR, "Display init failed");
    uiInit(s_bootInfo);
    lv_timer_handler();  // draw the first frame before the backlight comes on
    boardBacklight(settingsGet().brightness);

    netStart();
    logf(LOG_INFO, "Free RAM: internal %u KB, PSRAM %u KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

void loop()
{
    esp_task_wdt_reset();
    lv_timer_handler();
    delay(5);
}
