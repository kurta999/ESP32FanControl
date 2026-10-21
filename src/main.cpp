#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstdlib>
#include "control_logic.h"
#include "web_page.h"
#include "fan_config.h"
#include "secrets.h"

static_assert(FAN_MIN_PERCENT > 0 && FAN_MAX_PERCENT <= 100, "Duty limits must be inside 1..100");
static_assert(FAN_DEFAULT_PERCENT >= FAN_MIN_PERCENT && FAN_DEFAULT_PERCENT <= FAN_MAX_PERCENT, "Invalid default");
static_assert(FAN_STARTUP_PERCENT >= FAN_MIN_PERCENT && FAN_STARTUP_PERCENT <= FAN_MAX_PERCENT, "Invalid startup");
static_assert(FAN_PWM_BITS > 0 && FAN_PWM_BITS <= 16, "Invalid PWM resolution");
static_assert(FAN_PWM_PIN >= 0 && FAN_PWM_PIN <= 33 && !(FAN_PWM_PIN >= 6 && FAN_PWM_PIN <= 11), "Choose an ESP32 output pin, excluding flash pins 6..11");

WebServer server(80);
unsigned targetPercent = FAN_DEFAULT_PERCENT;
unsigned appliedPercent = 0;
bool startupActive = true;
bool mdnsActive = false;
uint32_t startupAt = 0;
uint32_t lastWifiAttempt = 0;
wl_status_t previousWifi = WL_IDLE_STATUS;
constexpr uint8_t pwmChannel = 0;
String stationSsid;
String stationPassword;
bool wifiSaved = false;
bool networkChangePending = false;
uint32_t networkChangeAt = 0;
ControlConfig control;
bool controlSaved = false;
bool autoRunning = false;
bool temperatureValid = false;
bool temperatureSampled = false;
bool sensorFailsafe = false;
float rawTemperatureC = NAN;
float filteredTemperatureC = NAN;
uint32_t lastTemperatureAt = 0;
uint32_t lastHistoryAt = 0;
TemperatureHistory<TEMPERATURE_HISTORY_CAPACITY> temperatureHistory;

struct NetworkConfig { char ssid[33]; char password[65]; };
static_assert(sizeof(WIFI_AP_PASSWORD) >= 9 && sizeof(WIFI_AP_PASSWORD) <= 64, "AP password must be 8..63 characters");

const char wifiPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Wi-Fi setup · ESP32 Fan</title>
<style>:root{color-scheme:dark;font:17px system-ui;background:#0d1522;color:#e7edf5}body{margin:0;min-height:100vh;display:grid;place-items:center}main{width:min(440px,85vw);padding:32px;background:#172336;border:1px solid #30415c;border-radius:24px}h1{font-size:27px}label{display:block;margin:22px 0 8px}input{box-sizing:border-box;width:100%;border:1px solid #415779;border-radius:8px;padding:13px;background:#0d1522;color:#e7edf5;font:inherit}button{width:100%;margin-top:24px;padding:14px;border:0;border-radius:9px;background:#60d6bd;color:#102321;font:inherit;font-weight:600}p{font-size:14px;color:#aac0dc;line-height:1.6}a{color:#60d6bd}#message{min-height:3em}</style>
<main><a href="/">← Fan control</a><h1>Connect to Wi-Fi</h1><p>Enter your 2.4 GHz network details. Settings are saved on this ESP32 and restored after restart. The setup access point stays available.</p>
<form id="setup"><label for="ssid">Network name (SSID)</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off"><label for="password">Network password</label><input id="password" name="password" type="password" maxlength="64" autocomplete="new-password"><p>Leave the password empty only for an open network.</p><button id="save">Save and connect</button></form><p id="message" role="status" aria-live="polite"></p><p id="state">Reading connection status…</p></main>
<script>
const form=document.getElementById('setup'),button=document.getElementById('save'),msg=document.getElementById('message');let busy=false;
async function status(){if(busy)return;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const s=await r.json();document.getElementById('state').textContent=s.connected?'Connected. Fan control: http://'+s.ip+'/':'Station Wi-Fi is connecting. Setup address: http://'+s.ap_ip+'/'}catch(e){document.getElementById('state').textContent='Reconnect to the setup Wi-Fi if this page becomes unreachable.'}}
form.onsubmit=async e=>{e.preventDefault();button.disabled=true;busy=true;try{const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(form))});if(!r.ok)throw Error(await r.text());document.getElementById('password').value='';msg.textContent='Saved to ESP32 flash. Connecting to the network…'}catch(e){msg.textContent='Could not save: '+e.message}finally{button.disabled=false;busy=false}};status();setInterval(status,2500);
</script></html>)HTML";

bool validCredentials(const String &ssid, const String &password) {
    if (ssid.length() < 1 || ssid.length() > 32 || password.length() > 64) return false;
    for (size_t i = 0; i < ssid.length(); ++i) if (ssid[i] == '\0') return false;
    for (size_t i = 0; i < password.length(); ++i) if (password[i] == '\0') return false;
    if (password.length() == 64) {
        for (size_t i = 0; i < password.length(); ++i) if (!isxdigit(static_cast<unsigned char>(password[i]))) return false;
        return true;
    }
    return password.length() == 0 || password.length() >= 8;
}

bool saveNetwork(const String &ssid, const String &password) {
    NetworkConfig config = {};
    ssid.toCharArray(config.ssid, sizeof(config.ssid));
    password.toCharArray(config.password, sizeof(config.password));
    Preferences preferences;
    if (!preferences.begin("fan-control", false)) return false;
    // One NVS blob updates SSID and password together, including on power loss.
    const bool ok = preferences.putBytes("wifi-v1", &config, sizeof(config)) == sizeof(config);
    preferences.end();
    return ok;
}

void loadNetwork() {
    NetworkConfig config = {};
    Preferences preferences;
    bool loaded = false;
    if (preferences.begin("fan-control", true)) {
        if (preferences.getBytesLength("wifi-v1") == sizeof(config))
            loaded = preferences.getBytes("wifi-v1", &config, sizeof(config)) == sizeof(config);
        preferences.end();
    }
    if (loaded && memchr(config.ssid, 0, sizeof(config.ssid)) && memchr(config.password, 0, sizeof(config.password)) && validCredentials(String(config.ssid), String(config.password))) {
        stationSsid = config.ssid;
        stationPassword = config.password;
        wifiSaved = true;
        Serial.println("Wi-Fi settings loaded from flash.");
    } else {
        stationSsid = WIFI_SSID;
        stationPassword = WIFI_PASSWORD;
        if (validCredentials(stationSsid, stationPassword)) wifiSaved = saveNetwork(stationSsid, stationPassword);
        Serial.println(wifiSaved ? "Initial Wi-Fi settings saved to flash." : "Use the setup AP to save Wi-Fi settings.");
    }
}

ControlConfig defaultControlConfig() {
    ControlConfig c;
    c.manualPercent = FAN_DEFAULT_PERCENT;
    c.maxPercent = FAN_MAX_PERCENT;
    c.minPercent = constrain(30u, FAN_MIN_PERCENT, FAN_MAX_PERCENT);
    return c;
}

bool saveControl(const ControlConfig &candidate) {
    Preferences preferences;
    if (!preferences.begin("fan-control", false)) return false;
    const bool ok = preferences.putBytes("control-v2", &candidate, sizeof(candidate)) == sizeof(candidate);
    preferences.end();
    return ok;
}

void loadControl() {
    control = defaultControlConfig();
    ControlConfig candidate;
    struct LegacyControl {
        float stopC, startC, fullC, offsetC;
        uint8_t autoEnabled, minPercent, maxPercent, manualPercent;
    } legacy;
    Preferences preferences;
    if (preferences.begin("fan-control", false)) {
        bool loaded = false;
        if (preferences.isKey("control-v2") && preferences.getBytesLength("control-v2") == sizeof(candidate))
            loaded = preferences.getBytes("control-v2", &candidate, sizeof(candidate)) == sizeof(candidate);
        else if (preferences.isKey("control-v1") && preferences.getBytesLength("control-v1") == sizeof(legacy)
            && preferences.getBytes("control-v1", &legacy, sizeof(legacy)) == sizeof(legacy)) {
            candidate.stopC = legacy.stopC; candidate.startC = legacy.startC;
            candidate.fullC = legacy.fullC; candidate.offsetC = legacy.offsetC;
            candidate.autoEnabled = legacy.autoEnabled; candidate.minPercent = legacy.minPercent;
            candidate.maxPercent = legacy.maxPercent; candidate.manualPercent = legacy.manualPercent;
            loaded = true;
        }
        if (loaded && validControl(candidate, FAN_MIN_PERCENT, FAN_MAX_PERCENT)) {
            control = candidate;
            controlSaved = true;
        }
        preferences.end();
    }
}

float currentTemperature() {
    return temperatureValid ? filteredTemperatureC + control.offsetC : NAN;
}

void applyDuty(unsigned percent) {
    if (appliedPercent == percent) return;
    const uint32_t duty = (((1UL << FAN_PWM_BITS) - 1) * percent + 50) / 100;
    ledcWrite(pwmChannel, duty);
    appliedPercent = percent;
}

void updateControl() {
    const bool fresh = temperatureValid && temperatureSampled && millis() - lastTemperatureAt <= TEMPERATURE_STALE_MS;
    const auto decision = decideFan(control, currentTemperature(), fresh, autoRunning, FAN_MAX_PERCENT, FAN_MIN_PERCENT);
    targetPercent = decision.percent;
    autoRunning = decision.running;
    sensorFailsafe = decision.failsafe;
    if (targetPercent == 0) {
        startupActive = false;
        applyDuty(0);
    } else if (!startupActive) {
        if (appliedPercent == 0) {
            startupActive = true;
            startupAt = millis();
            applyDuty(FAN_STARTUP_PERCENT);
        } else applyDuty(targetPercent);
    }
}

bool changeControl(const ControlConfig &candidate) {
    if (!validControl(candidate, FAN_MIN_PERCENT, FAN_MAX_PERCENT) || !saveControl(candidate)) return false;
    if (candidate.autoEnabled != control.autoEnabled || candidate.curveMode != control.curveMode) autoRunning = false;
    control = candidate;
    controlSaved = true;
    updateControl();
    return true;
}

bool setTarget(unsigned value) {
    ControlConfig candidate = control;
    candidate.autoEnabled = 0;
    candidate.manualPercent = value;
    return changeControl(candidate);
}

uint32_t uptimeSeconds() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

void serviceTemperature() {
    if (temperatureSampled && millis() - lastTemperatureAt < TEMPERATURE_SAMPLE_MS) return;
    const float reading = temperatureRead();
    const bool valid = std::isfinite(reading) && reading >= -40 && reading <= 125;
    if (valid) {
        if (!temperatureValid) filteredTemperatureC = reading;
        else filteredTemperatureC += 0.25f * (reading - filteredTemperatureC);
        rawTemperatureC = reading;
    } else {
        rawTemperatureC = NAN;
        filteredTemperatureC = NAN;
    }
    temperatureValid = valid;
    temperatureSampled = true;
    lastTemperatureAt = millis();
    updateControl();
    if (temperatureHistory.size() == 0 || millis() - lastHistoryAt >= TEMPERATURE_HISTORY_MS) {
        temperatureHistory.add(uptimeSeconds(), currentTemperature(), appliedPercent);
        lastHistoryAt = millis();
    }
}

bool parseNumber(const String &text, float &value) {
    if (text.isEmpty() || text.length() > 16) return false;
    for (size_t i = 0; i < text.length(); ++i)
        if (isspace(static_cast<unsigned char>(text[i])) || text[i] == '\0') return false;
    char *end = nullptr;
    value = strtof(text.c_str(), &end);
    return end != text.c_str() && end == text.c_str() + text.length() && std::isfinite(value);
}

bool parseByte(const String &text, uint8_t &value) {
    if (text.isEmpty() || text.length() > 3) return false;
    unsigned number = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        number = number * 10 + text[i] - '0';
    }
    if (number > 255) return false;
    value = number;
    return true;
}

void sendHistory() {
    unsigned minutes = 5;
    if (server.args() != 0) {
        const String text = server.arg("minutes");
        if (server.args() != 1 || !server.hasArg("minutes") || (text != "5" && text != "10" && text != "20" && text != "30")) {
            server.send(400, "text/plain", "Choose minutes=5, 10, 20, or 30.");
            return;
        }
        minutes = text.toInt();
    }
    const uint32_t now = uptimeSeconds();
    String json;
    if (!json.reserve(160 + temperatureHistory.size() * 32)) {
        server.send(503, "text/plain", "Not enough memory to return history.");
        return;
    }
    json = "{\"uptime_s\":" + String(now) + ",\"minutes\":" + String(minutes)
        + ",\"sample_period_s\":" + String(TEMPERATURE_HISTORY_MS / 1000) + ",\"samples\":[";
    bool first = true;
    for (size_t i = 0; i < temperatureHistory.size(); ++i) {
        const auto &sample = temperatureHistory.at(i);
        const uint32_t age = now - sample.seconds;
        if (age > minutes * 60) continue;
        if (!first) json += ',';
        first = false;
        json += '[' + String(age) + ',';
        json += std::isfinite(sample.temperatureC) ? String(sample.temperatureC, 2) : String("null");
        json += ',' + String(sample.percent) + ']';
    }
    json += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

bool parsePercent(const String &text, unsigned &result) {
    if (text.length() == 0 || text.length() > 3) return false;
    unsigned value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    if ((value != 0 && value < FAN_MIN_PERCENT) || value > FAN_MAX_PERCENT) return false;
    result = value;
    return true;
}

void sendStatus() {
    String json;
    json.reserve(1500);
    json = "{\"target_percent\":" + String(targetPercent);
    json += ",\"applied_percent\":" + String(appliedPercent);
    json += ",\"min_percent\":" + String(FAN_MIN_PERCENT);
    json += ",\"max_percent\":" + String(FAN_MAX_PERCENT);
    json += ",\"pwm_pin\":" + String(FAN_PWM_PIN);
    json += ",\"pwm_hz\":" + String(FAN_PWM_HZ);
    json += ",\"startup\":" + String(startupActive ? "true" : "false");
    json += ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
    json += ",\"wifi_saved\":" + String(wifiSaved ? "true" : "false");
    json += ",\"temperature_c\":" + (std::isfinite(currentTemperature()) ? String(currentTemperature(), 2) : String("null"));
    json += ",\"raw_temperature_c\":" + (std::isfinite(rawTemperatureC) ? String(rawTemperatureC, 2) : String("null"));
    json += ",\"temperature_valid\":" + String(temperatureValid ? "true" : "false");
    json += ",\"temperature_age_ms\":" + (temperatureSampled ? String(millis() - lastTemperatureAt) : String("null"));
    json += ",\"uptime_s\":" + String(uptimeSeconds());
    json += ",\"auto_running\":" + String(autoRunning ? "true" : "false");
    json += ",\"failsafe\":" + String(sensorFailsafe ? "true" : "false");
    json += ",\"control\":{\"auto_enabled\":" + String(control.autoEnabled ? "true" : "false");
    json += ",\"stop_c\":" + String(control.stopC, 2);
    json += ",\"start_c\":" + String(control.startC, 2);
    json += ",\"full_c\":" + String(control.fullC, 2);
    json += ",\"offset_c\":" + String(control.offsetC, 2);
    json += ",\"min_percent\":" + String(control.minPercent);
    json += ",\"max_percent\":" + String(control.maxPercent);
    json += ",\"manual_percent\":" + String(control.manualPercent);
    json += ",\"strategy\":\"" + String(control.curveMode ? "curve" : "threshold") + "\"";
    json += ",\"curve_hysteresis_c\":" + String(control.curveHysteresisC, 2);
    json += ",\"curve_max_points\":" + String(MAX_CURVE_POINTS);
    json += ",\"curve_points\":[";
    for (size_t i = 0; i < control.curveCount; ++i) {
        if (i) json += ',';
        json += "{\"temperature_c\":" + String(control.curvePoints[i].temperatureC, 2)
            + ",\"percent\":" + String(control.curvePoints[i].percent) + "}";
    }
    json += ']';
    json += ",\"saved\":" + String(controlSaved ? "true" : "false") + "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

bool parseCurve(const String &text, ControlConfig &candidate) {
    if (text.isEmpty() || text.length() > 300) return false;
    size_t count = 0;
    int start = 0;
    while (start < static_cast<int>(text.length())) {
        if (count >= MAX_CURVE_POINTS) return false;
        int end = text.indexOf(',', start);
        if (end < 0) end = text.length();
        const String point = text.substring(start, end);
        const int colon = point.indexOf(':');
        if (colon < 1 || !parseNumber(point.substring(0, colon), candidate.curvePoints[count].temperatureC)
            || !parseByte(point.substring(colon + 1), candidate.curvePoints[count].percent)) return false;
        ++count;
        start = end + 1;
        if (start == static_cast<int>(text.length())) return false;
    }
    candidate.curveCount = count;
    return true;
}

void handleControl() {
    if (server.args() < 1 || server.args() > 10) {
        server.send(400, "text/plain", "Supply at least one control setting.");
        return;
    }
    ControlConfig candidate = control;
    bool valid = true;
    for (int i = 0; i < server.args(); ++i) {
        const String name = server.argName(i);
        for (int j = 0; j < i; ++j) if (name == server.argName(j)) valid = false;
        const String value = server.arg(i);
        if (name == "auto_enabled") {
            if (value != "0" && value != "1") valid = false;
            else candidate.autoEnabled = value == "1";
        } else if (name == "strategy") {
            if (value != "threshold" && value != "curve") valid = false;
            else candidate.curveMode = value == "curve";
        } else if (name == "curve") valid = parseCurve(value, candidate) && valid;
        else if (name == "curve_hysteresis_c") valid = parseNumber(value, candidate.curveHysteresisC) && valid;
        else if (name == "stop_c") valid = parseNumber(value, candidate.stopC) && valid;
        else if (name == "start_c") valid = parseNumber(value, candidate.startC) && valid;
        else if (name == "full_c") valid = parseNumber(value, candidate.fullC) && valid;
        else if (name == "offset_c") valid = parseNumber(value, candidate.offsetC) && valid;
        else if (name == "min_percent") valid = parseByte(value, candidate.minPercent) && valid;
        else if (name == "max_percent") valid = parseByte(value, candidate.maxPercent) && valid;
        else valid = false;
    }
    if (!valid || !validControl(candidate, FAN_MIN_PERCENT, FAN_MAX_PERCENT)) {
        server.send(400, "text/plain", "Require -20 <= stop < start < full <= 120 C (at least 0.5 C apart), valid duty limits, and offset -20..20 C. Curve: 2..10 points, increasing temperatures at least 0.5 C apart, nondecreasing duties of 0 or 20..100%, with a nonzero final point; curve hysteresis 0..10 C. Unknown or duplicate fields are rejected.");
        return;
    }
    if (!changeControl(candidate)) {
        server.send(503, "text/plain", "Could not save control settings to flash. Existing settings unchanged.");
        return;
    }
    sendStatus();
}

void printStatus() {
    Serial.printf("Target %u%%, applied %u%%, GPIO %d, %u Hz%s\n", targetPercent, appliedPercent, FAN_PWM_PIN, FAN_PWM_HZ, startupActive ? " (startup)" : "");
    if (WiFi.status() == WL_CONNECTED) Serial.printf("Web: http://%s/ (http://%s.local/)\n", WiFi.localIP().toString().c_str(), WIFI_HOSTNAME);
    else Serial.printf("Wi-Fi connecting (status %d); PWM continues independently.\n", static_cast<int>(WiFi.status()));
    Serial.printf("Setup AP: %s, http://%s/wifi\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    Serial.printf("Wi-Fi settings saved: %s\n", wifiSaved ? "yes" : "no");
    Serial.printf("Chip temperature: %.2f C, automatic mode %s%s\n", currentTemperature(), control.autoEnabled ? "ON" : "OFF", sensorFailsafe ? " (sensor failsafe)" : "");
}

void serviceSerial() {
    static char line[16];
    static size_t length = 0;
    static bool overflow = false;
    // Bound work so an incoming serial stream cannot starve Wi-Fi/startup handling.
    for (unsigned budget = 0; budget < 64 && Serial.available(); ++budget) {
        const char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (length || overflow) {
                line[length] = '\0';
                unsigned value;
                if (!overflow && String(line) == "?") printStatus();
                else if (!overflow && parsePercent(String(line), value)) { if (!setTarget(value)) Serial.println("Could not save manual setting."); printStatus(); }
                else Serial.printf("Invalid command. Enter 0 (off), %u..%u, or ?.\n", FAN_MIN_PERCENT, FAN_MAX_PERCENT);
            }
            length = 0; overflow = false;
        } else if (length < sizeof(line) - 1) line[length++] = c;
        else overflow = true;
    }
}

void serviceWifi() {
    if (networkChangePending && millis() - networkChangeAt >= 500) {
        networkChangePending = false;
        WiFi.disconnect(false, false);
        WiFi.begin(stationSsid.c_str(), stationPassword.c_str());
        lastWifiAttempt = millis();
        previousWifi = WL_IDLE_STATUS;
    }
    const auto state = WiFi.status();
    if (state == WL_CONNECTED && previousWifi != WL_CONNECTED) {
        Serial.printf("Wi-Fi connected. Web: http://%s/\n", WiFi.localIP().toString().c_str());
        if (mdnsActive) MDNS.end();
        mdnsActive = MDNS.begin(WIFI_HOSTNAME);
        if (mdnsActive) { MDNS.addService("http", "tcp", 80); Serial.printf("Also: http://%s.local/\n", WIFI_HOSTNAME); }
    }
    if (state != WL_CONNECTED && millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
        lastWifiAttempt = millis();
        Serial.println("Retrying Wi-Fi...");
        WiFi.reconnect();
    }
    previousWifi = state;
}

void setup() {
    pinMode(FAN_PWM_PIN, OUTPUT);
    digitalWrite(FAN_PWM_PIN, LOW);
    Serial.begin(SERIAL_BAUD);
    const double actualHz = ledcSetup(pwmChannel, FAN_PWM_HZ, FAN_PWM_BITS);
    if (actualHz == 0 || !GPIO_IS_VALID_OUTPUT_GPIO(FAN_PWM_PIN)) {
        Serial.println("PWM setup failed; output held low.");
        while (true) delay(1000);
    }
    ledcAttachPin(FAN_PWM_PIN, pwmChannel);
    Serial.println("ESP32 standalone fan controller");
    loadControl();
    if (!control.autoEnabled && control.manualPercent == 0) {
        startupActive = false;
        applyDuty(0);
    } else {
        applyDuty(FAN_STARTUP_PERCENT);
        startupAt = millis();
    }
    WiFi.persistent(false);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.mode(WIFI_AP_STA);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            Serial.printf("Wi-Fi disconnected, reason %u. Setup AP remains available.\n", info.wifi_sta_disconnected.reason);
    });
    loadNetwork();
    if (WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD)) {
        Serial.printf("Setup AP: %s, http://%s/wifi\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    } else Serial.println("Setup AP failed to start.");
    WiFi.setAutoReconnect(true);
    WiFi.begin(stationSsid.c_str(), stationPassword.c_str());
    lastWifiAttempt = millis();
    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", page); });
    server.on("/api/status", HTTP_GET, sendStatus);
    server.on("/api/history", HTTP_GET, sendHistory);
    server.on("/api/control", HTTP_POST, handleControl);
    server.on("/wifi", HTTP_GET, [] { server.send_P(200, "text/html", wifiPage); });
    server.on("/api/wifi", HTTP_POST, [] {
        if (server.args() != 2 || !server.hasArg("ssid") || !server.hasArg("password") || !validCredentials(server.arg("ssid"), server.arg("password"))) {
            server.send(400, "text/plain", "Supply an SSID of 1..32 bytes and a password of 8..63 bytes (or 64 hex digits). Empty password selects an open network.");
            return;
        }
        if (!saveNetwork(server.arg("ssid"), server.arg("password"))) {
            server.send(503, "text/plain", "Flash storage failed. Existing settings are unchanged.");
            return;
        }
        stationSsid = server.arg("ssid");
        stationPassword = server.arg("password");
        wifiSaved = true;
        networkChangeAt = millis();
        networkChangePending = true;
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", "{\"saved\":true,\"connecting\":true}");
    });
    server.on("/api/fan/off", HTTP_POST, [] {
        if (server.args() != 0) {
            server.send(400, "text/plain", "The fan-off action takes no arguments.");
            return;
        }
        if (!setTarget(0)) {
            server.send(503, "text/plain", "Could not save fan-off state to flash.");
            return;
        }
        sendStatus();
    });
    server.on("/api/fan", HTTP_POST, [] {
        unsigned value;
        if (server.args() != 1 || !server.hasArg("percent") || !parsePercent(server.arg("percent"), value)) {
            server.send(400, "text/plain", "Supply zero (off), or one integer percent within the configured running duty limits.");
            return;
        }
        if (control.autoEnabled) {
            server.send(409, "text/plain", "Switch automatic mode OFF before setting manual duty.");
            return;
        }
        if (!setTarget(value)) {
            server.send(503, "text/plain", "Could not save manual setting to flash.");
            return;
        }
        sendStatus();
    });
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
    serviceTemperature();
    printStatus();
}

void loop() {
    if (startupActive && millis() - startupAt >= FAN_STARTUP_MS) {
        startupActive = false;
        applyDuty(targetPercent);
    }
    serviceTemperature();
    updateControl();
    serviceSerial();
    serviceWifi();
    server.handleClient();
    delay(1);
}
