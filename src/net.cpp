#include "net.h"

#include <WiFi.h>

#include <algorithm>

#include "acs550.h"
#include "app_config.h"
#include "drive.h"
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "logger.h"
#include "mqtt_client.h"
#include "settings.h"

struct PubItem {
    char sub[8];
    char payload[32];
    bool retain;
};

static QueueHandle_t s_pubQ = nullptr;
static esp_mqtt_client_handle_t s_client = nullptr;
static volatile bool s_mqttConnected = false;
static volatile bool s_republish = false;

// Applied configuration (owned by the net task; the MQTT handler only reads it
// while the client exists, and the client is destroyed before it changes).
static Settings s_cfg;
static uint32_t s_cfgVer = 0;
static String s_base, s_suffix, s_statusTopic, s_logTopic, s_clientId;
static volatile bool s_allowControl = true;
static volatile bool s_allowParamWrite = false;
static volatile bool s_retain = true;

static bool s_wifiUp = false;
static uint32_t s_lastWifiAttempt = 0;
static uint32_t s_logSeq = 0;
static uint32_t s_lastMqttErrorLog = 0;

static portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;
static NetStatus s_status = {};

// ---------------------------------------------------------------------------
// Echo suppression. With an empty command suffix the panel subscribes to the
// same topics it publishes, so each of its own messages comes back once.
// ---------------------------------------------------------------------------
struct Echo {
    uint16_t par;
    uint32_t ms;
    char payload[16];
};
static Echo s_echo[32];
static uint8_t s_echoNext = 0;
static portMUX_TYPE s_echoMux = portMUX_INITIALIZER_UNLOCKED;

static void echoRecord(uint16_t par, const char *payload)
{
    taskENTER_CRITICAL(&s_echoMux);
    Echo &e = s_echo[s_echoNext];
    s_echoNext = (s_echoNext + 1) % (sizeof(s_echo) / sizeof(s_echo[0]));
    e.par = par;
    e.ms = millis();
    strlcpy(e.payload, payload, sizeof(e.payload));
    taskEXIT_CRITICAL(&s_echoMux);
}

static bool echoConsume(uint16_t par, const char *payload)
{
    bool found = false;
    const uint32_t now = millis();
    taskENTER_CRITICAL(&s_echoMux);
    for (Echo &e : s_echo) {
        if (e.ms && e.par == par && now - e.ms < 5000 && strcmp(e.payload, payload) == 0) {
            e.ms = 0;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_echoMux);
    return found;
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------
void netPublish(const char *sub, const char *payload, bool retain)
{
    if (!s_pubQ) return;
    PubItem it;
    strlcpy(it.sub, sub, sizeof(it.sub));
    strlcpy(it.payload, payload, sizeof(it.payload));
    it.retain = retain;
    xQueueSend(s_pubQ, &it, 0);  // drop when full; everything is resent on reconnect
}

void netPublishPar(uint16_t par, const char *payload)
{
    char sub[8];
    snprintf(sub, sizeof(sub), "%04u", par);
    netPublish(sub, payload, s_retain);
}

bool netTakeRepublishRequest()
{
    if (!s_republish) return false;
    s_republish = false;
    return true;
}

static void publishQueued()
{
    PubItem it;
    while (xQueueReceive(s_pubQ, &it, 0) == pdTRUE) {
        if (!s_client || !s_mqttConnected) continue;
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/%s", s_base.c_str(), it.sub);
        if (s_suffix.isEmpty() && isdigit((unsigned char)it.sub[0])) echoRecord(atoi(it.sub), it.payload);
        esp_mqtt_client_enqueue(s_client, topic, it.payload, 0, 0, it.retain, true);
    }
}

static void forwardLog()
{
    if (!s_client || !s_mqttConnected) {
        s_logSeq = logLatestSeq();  // only stream lines logged while connected
        return;
    }
    LogEntry e;
    for (int i = 0; i < 10 && logNext(&s_logSeq, &e); i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s %c %s", e.time, e.level, e.text);
        esp_mqtt_client_enqueue(s_client, s_logTopic.c_str(), msg, 0, 0, 0, true);
    }
}

// ---------------------------------------------------------------------------
// Incoming commands
// ---------------------------------------------------------------------------
static bool parseNumber(const char *s, float *out)
{
    char *end;
    const float v = strtof(s, &end);
    if (end == s) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = v;
    return true;
}

static void dispatch(uint16_t par, const char *payload, const char *topic)
{
    DriveCmd c = {};
    c.src = CmdSource::Mqtt;
    c.par = par;
    float v;

    if (par == 1 || par == 2) {
        if (!s_allowControl) {
            logf(LOG_WARN, "MQTT %s ignored: control over MQTT is off (Setup)", topic);
            return;
        }
        if (par == 2) {
            if (!parseNumber(payload, &v)) {
                logf(LOG_WARN, "MQTT %s: '%s' is not a speed in rpm", topic, payload);
                return;
            }
            c.type = DriveCmdType::SetSpeed;
            c.f = v;
            c.i = 1;  // sign selects direction
            driveSend(c);
            return;
        }
        char word[16];
        strlcpy(word, payload, sizeof(word));
        for (char *p = word; *p; p++) *p = toupper((unsigned char)*p);
        if (!strcmp(word, "START") || !strcmp(word, "RUN") || !strcmp(word, "ON")) c.type = DriveCmdType::Start;
        else if (!strcmp(word, "STOP") || !strcmp(word, "OFF")) c.type = DriveCmdType::Stop;
        else if (!strcmp(word, "COAST")) c.type = DriveCmdType::Coast;
        else if (!strcmp(word, "QSTOP") || !strcmp(word, "QUICK") || !strcmp(word, "QUICKSTOP")) c.type = DriveCmdType::QuickStop;
        else if (!strcmp(word, "RESET")) c.type = DriveCmdType::ResetFault;
        else if (parseNumber(payload, &v) && v >= 0 && v <= 65535 && v == (int32_t)v) {
            c.type = DriveCmdType::RawControl;
            c.i = (int32_t)v;
        } else {
            logf(LOG_WARN, "MQTT %s: unknown command '%s'", topic, payload);
            return;
        }
        driveSend(c);
        return;
    }

    if (payload[0] == '\0' || !strcmp(payload, "?")) {
        c.type = DriveCmdType::ReadParam;
        driveSend(c);
        return;
    }
    if (acs550::isReadOnly(par)) {
        logf(LOG_WARN, "MQTT %s ignored: %04u is read-only", topic, par);
        return;
    }
    if (!s_allowParamWrite) {
        logf(LOG_WARN, "MQTT %s ignored: parameter writes over MQTT are off (Setup)", topic);
        return;
    }
    if (!parseNumber(payload, &v)) {
        logf(LOG_WARN, "MQTT %s: '%s' is not a number", topic, payload);
        return;
    }
    c.type = DriveCmdType::WriteParam;
    c.f = v;
    driveSend(c);
}

static void handleIncoming(esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) return;  // fragmented: not a command
    char topic[96], payload[32];
    if (ev->topic_len <= 0 || ev->topic_len >= (int)sizeof(topic) || ev->data_len >= (int)sizeof(payload)) return;
    memcpy(topic, ev->topic, ev->topic_len);
    topic[ev->topic_len] = '\0';
    memcpy(payload, ev->data, ev->data_len);
    payload[ev->data_len] = '\0';

    // trim whitespace
    char *p = payload;
    while (isspace((unsigned char)*p)) p++;
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) p[--n] = '\0';

    // <base>/XXXX<suffix>
    const size_t bl = s_base.length(), sl = s_suffix.length();
    if (strncmp(topic, s_base.c_str(), bl) != 0 || topic[bl] != '/') return;
    const char *rest = topic + bl + 1;
    if (strlen(rest) != 4 + sl || (sl && strcmp(rest + 4, s_suffix.c_str()) != 0)) return;
    for (int i = 0; i < 4; i++)
        if (!isdigit((unsigned char)rest[i])) return;
    const uint16_t par = (rest[0] - '0') * 1000 + (rest[1] - '0') * 100 + (rest[2] - '0') * 10 + (rest[3] - '0');

    if (ev->retain) {
        // Never act on retained messages (a stale START must not restart the motor)
        if (sl) logf(LOG_WARN, "MQTT %s: retained message ignored - publish commands without retain", topic);
        return;
    }
    if (!sl && echoConsume(par, p)) return;
    dispatch(par, p, topic);
}

static void mqttEvent(void *, esp_event_base_t, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_mqttConnected = true;
        String filter = s_base + "/+" + s_suffix;
        esp_mqtt_client_subscribe_single(ev->client, filter.c_str(), 1);
        esp_mqtt_client_enqueue(ev->client, s_statusTopic.c_str(), "online", 0, 1, 1, true);
        s_republish = true;
        logf(LOG_OK, "MQTT connected, commands on %s", filter.c_str());
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_mqttConnected) logf(LOG_WARN, "MQTT disconnected");
        s_mqttConnected = false;
        break;
    case MQTT_EVENT_ERROR: {
        if (!s_wifiUp || millis() - s_lastMqttErrorLog < 30000) break;
        s_lastMqttErrorLog = millis();
        const esp_mqtt_error_codes_t *e = ev->error_handle;
        if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const char *why = e->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME   ? "bad username/password"
                              : e->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ? "not authorised"
                              : e->connect_return_code == MQTT_CONNECTION_REFUSE_ID_REJECTED    ? "client id rejected"
                                                                                                : "refused";
            logf(LOG_ERR, "MQTT broker refused connection: %s", why);
        } else if (e->esp_tls_last_esp_err) {
            logf(LOG_ERR, "MQTT TLS failed to %s:%u (%s, cert flags 0x%X)", s_cfg.mqttHost.c_str(), s_cfg.mqttPort,
                 esp_err_to_name(e->esp_tls_last_esp_err), e->esp_tls_cert_verify_flags);
        } else {
            logf(LOG_ERR, "MQTT: cannot reach %s:%u (errno %d)", s_cfg.mqttHost.c_str(), s_cfg.mqttPort,
                 e->esp_transport_sock_errno);
        }
        break;
    }
    case MQTT_EVENT_DATA:
        handleIncoming(ev);
        break;
    default:
        break;
    }
}

static void mqttStop()
{
    if (s_client) {
        esp_task_wdt_reset();
        esp_mqtt_client_destroy(s_client);  // stops the client task first
        esp_task_wdt_reset();
        s_client = nullptr;
    }
    s_mqttConnected = false;
}

static void mqttStart()
{
    if (!s_cfg.mqttEnabled) {
        logf(LOG_INFO, "MQTT is switched off");
        return;
    }
    if (s_cfg.mqttHost.isEmpty()) {
        logf(LOG_WARN, "MQTT broker not set - open Setup");
        return;
    }
    s_base = s_cfg.mqttBase;
    s_suffix = s_cfg.mqttCmdSuffix;
    s_suffix.trim();
    if (s_suffix.length() && s_suffix[0] != '/') s_suffix = "/" + s_suffix;
    s_statusTopic = s_base + "/status";
    s_logTopic = s_base + "/log";

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname = s_cfg.mqttHost.c_str();
    cfg.broker.address.port = s_cfg.mqttPort;
    if (s_cfg.mqttTls) {
        // Server certificate checked against the built-in public CA bundle (EMQX Cloud, Let's Encrypt, ...)
        cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    }
    cfg.credentials.client_id = s_clientId.c_str();
    if (s_cfg.mqttAuth) {
        cfg.credentials.username = s_cfg.mqttUser.c_str();
        cfg.credentials.authentication.password = s_cfg.mqttPass.c_str();
    }
    cfg.session.last_will.topic = s_statusTopic.c_str();
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.session.keepalive = 30;
    cfg.network.reconnect_timeout_ms = 5000;
    cfg.network.timeout_ms = s_cfg.mqttTls ? 10000 : 5000;  // TLS handshake over the internet takes longer
    cfg.buffer.size = 1024;
    cfg.outbox.limit = 16 * 1024;
    cfg.task.stack_size = s_cfg.mqttTls ? 8192 : 6144;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        logf(LOG_ERR, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqttEvent, nullptr);
    esp_mqtt_client_start(s_client);
    logf(LOG_INFO, "MQTT broker %s:%u%s%s, topics %s/XXXX", s_cfg.mqttHost.c_str(), s_cfg.mqttPort,
         s_cfg.mqttTls ? " TLS" : "", s_cfg.mqttAuth ? " login" : "", s_base.c_str());
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void wifiConnect()
{
    s_lastWifiAttempt = millis();
    WiFi.disconnect();
    if (s_cfg.wifiSsid.isEmpty()) {
        logf(LOG_WARN, "Wi-Fi not set up - open Setup");
        return;
    }
    WiFi.begin(s_cfg.wifiSsid.c_str(), s_cfg.wifiPass.c_str());
    logf(LOG_INFO, "Wi-Fi: connecting to %s", s_cfg.wifiSsid.c_str());
}

static void applySettings(bool first)
{
    Settings n = settingsGet();
    s_cfgVer = settingsVersion();
    const Settings &o = s_cfg;
    const bool wifiChanged = first || n.wifiSsid != o.wifiSsid || n.wifiPass != o.wifiPass;
    const bool mqttChanged = first || n.mqttEnabled != o.mqttEnabled || n.mqttHost != o.mqttHost ||
                             n.mqttPort != o.mqttPort || n.mqttTls != o.mqttTls || n.mqttAuth != o.mqttAuth ||
                             n.mqttUser != o.mqttUser ||
                             n.mqttPass != o.mqttPass || n.mqttBase != o.mqttBase ||
                             n.mqttCmdSuffix != o.mqttCmdSuffix;
    const bool tzChanged = !first && n.timezone != o.timezone;

    if (mqttChanged) mqttStop();
    s_cfg = n;
    s_allowControl = n.mqttAllowControl;
    s_allowParamWrite = n.mqttAllowParamWrite;
    s_retain = n.mqttRetain;

    if (tzChanged && s_wifiUp) configTzTime(s_cfg.timezone.c_str(), "pool.ntp.org", "time.nist.gov");
    if (wifiChanged) wifiConnect();
    if (mqttChanged) mqttStart();
}

static void updateStatus()
{
    NetStatus n = {};
    n.wifiConfigured = !s_cfg.wifiSsid.isEmpty();
    n.wifiConnected = s_wifiUp;
    n.mqttConfigured = s_cfg.mqttEnabled && !s_cfg.mqttHost.isEmpty();
    n.mqttConnected = s_mqttConnected;
    n.rssi = s_wifiUp ? WiFi.RSSI() : 0;
    strlcpy(n.ip, s_wifiUp ? WiFi.localIP().toString().c_str() : "-", sizeof(n.ip));
    strlcpy(n.mac, WiFi.macAddress().c_str(), sizeof(n.mac));
    taskENTER_CRITICAL(&s_statusMux);
    s_status = n;
    taskEXIT_CRITICAL(&s_statusMux);
}

static void netTask(void *)
{
    esp_task_wdt_add(nullptr);
    applySettings(true);
    uint32_t lastStatus = 0;
    for (;;) {
        esp_task_wdt_reset();
        if (settingsVersion() != s_cfgVer) applySettings(false);

        const bool up = WiFi.status() == WL_CONNECTED;
        if (up != s_wifiUp) {
            s_wifiUp = up;
            if (up) {
                logf(LOG_OK, "Wi-Fi connected: %s (%d dBm)", WiFi.localIP().toString().c_str(), WiFi.RSSI());
                configTzTime(s_cfg.timezone.c_str(), "pool.ntp.org", "time.nist.gov");
            } else {
                logf(LOG_WARN, "Wi-Fi disconnected");
                s_lastWifiAttempt = millis();
            }
        }
        // Belt and braces on top of the driver's auto-reconnect (e.g. after an auth failure)
        if (!up && !s_cfg.wifiSsid.isEmpty() && millis() - s_lastWifiAttempt > 30000 &&
            WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
            wifiConnect();
        }

        publishQueued();
        forwardLog();
        if (millis() - lastStatus > 1000) {
            lastStatus = millis();
            updateStatus();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void netStart()
{
    s_pubQ = xQueueCreate(64, sizeof(PubItem));
    WiFi.setHostname(APP_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char id[24];
    snprintf(id, sizeof(id), "acs550-%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_clientId = id;
    xTaskCreatePinnedToCore(netTask, "net", 6144, nullptr, 2, nullptr, 0);
}

void netGetStatus(NetStatus *out)
{
    taskENTER_CRITICAL(&s_statusMux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_statusMux);
}

void netScanStart()
{
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
}

int netScanResults(std::vector<String> *ssids)
{
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    ssids->clear();
    if (n <= 0) return 0;

    std::vector<std::pair<int, String>> found;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        auto it = std::find_if(found.begin(), found.end(), [&](const std::pair<int, String> &f) { return f.second == ssid; });
        if (it == found.end()) found.push_back({WiFi.RSSI(i), ssid});
        else it->first = max(it->first, (int)WiFi.RSSI(i));
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (auto &f : found) ssids->push_back(f.second);
    WiFi.scanDelete();
    return (int)ssids->size();
}
