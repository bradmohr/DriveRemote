#include "logger.h"

#include <stdarg.h>
#include <time.h>
#include "esp_heap_caps.h"

static constexpr uint32_t LOG_CAPACITY = 128;

static LogEntry *s_ring = nullptr;
static uint32_t s_next = 1;  // sequence number of the next entry
static SemaphoreHandle_t s_mutex = nullptr;

void logInit()
{
    if (s_ring) return;
    s_ring = (LogEntry *)heap_caps_calloc(LOG_CAPACITY, sizeof(LogEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) s_ring = (LogEntry *)calloc(LOG_CAPACITY, sizeof(LogEntry));
    s_mutex = xSemaphoreCreateMutex();
}

void logTimestamp(char *buf, size_t len)
{
    time_t now = time(nullptr);
    if (now > 1700000000) {  // NTP has set the clock
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, len, "%H:%M:%S", &tm);
        return;
    }
    uint32_t s = millis() / 1000;
    snprintf(buf, len, "%02u:%02u:%02u", (unsigned)((s / 3600) % 100), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

void logf(LogLevel level, const char *fmt, ...)
{
    LogEntry e;
    e.level = (char)level;
    logTimestamp(e.time, sizeof(e.time));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    va_end(ap);

    Serial.printf("%s %c %s\n", e.time, e.level, e.text);

    if (!s_ring || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    e.seq = s_next++;
    s_ring[e.seq % LOG_CAPACITY] = e;
    xSemaphoreGive(s_mutex);
}

bool logNext(uint32_t *seq, LogEntry *out)
{
    if (!s_ring || !s_mutex) return false;
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t oldest = s_next > LOG_CAPACITY ? s_next - LOG_CAPACITY : 1;
    uint32_t want = *seq + 1;
    if (want < oldest) want = oldest;  // reader fell behind: skip what was overwritten
    if (want < s_next) {
        *out = s_ring[want % LOG_CAPACITY];
        *seq = want;
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

uint32_t logLatestSeq()
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t latest = s_next - 1;
    xSemaphoreGive(s_mutex);
    return latest;
}
