#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include "fan_config.h"
#include "secrets.h"

static_assert(FAN_MIN_PERCENT > 0 && FAN_MAX_PERCENT < 100, "Duty limits must be inside 0..100");
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
struct NetworkConfig { char ssid[33]; char password[65]; };
static_assert(sizeof(WIFI_AP_PASSWORD) >= 9 && sizeof(WIFI_AP_PASSWORD) <= 64, "AP password must be 8..63 characters");

const char page[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Fan Control</title>
<style>
:root{color-scheme:dark;font:17px system-ui;background:#0d1522;color:#e7edf5}body{margin:0;min-height:100vh;display:grid;place-items:center}main{width:min(440px,85vw);padding:32px;background:#172336;border:1px solid #30415c;border-radius:24px}h1{font-size:27px;margin:6px 0 24px}.eyebrow{color:#60d6bd;font-size:12px;letter-spacing:.16em}output{font-size:64px;font-weight:700;display:block;margin:20px 0}input{width:100%;accent-color:#60d6bd}button{border:0;border-radius:10px;background:#60d6bd;color:#102321;padding:13px 20px;font:inherit;font-weight:600;cursor:pointer;width:100%;margin-top:20px}dl{display:grid;grid-template-columns:1fr 1fr;font-size:14px;gap:10px;margin-top:28px}dd{text-align:right;margin:0;color:#aac0dc}p{font-size:14px;color:#aac0dc}#message{min-height:3em}button:disabled{opacity:.5}
</style><main><div class="eyebrow">LOCAL FAN CONTROLLER</div><h1>ESP32 Fan Control</h1><p><a style="color:#60d6bd" href="/wifi">Wi-Fi settings</a></p>
<form id="fan"><label for="percent">Requested PWM duty</label><output id="value" for="percent">50%</output><input id="percent" name="percent" type="range" min="20" max="80" step="1" value="50"><button id="apply">Apply speed</button></form>
<p id="message" role="status" aria-live="polite">Connecting to controller…</p><dl><dt>Applied duty</dt><dd id="applied">—</dd><dt>PWM output</dt><dd id="pin">—</dd><dt>Wi-Fi</dt><dd id="wifi">—</dd></dl><p>Duty is the electrical PWM setting. Fan RPM is not measured.</p></main>
<script>
const slider=document.getElementById('percent'), value=document.getElementById('value'), message=document.getElementById('message'), button=document.getElementById('apply');let dirty=false,busy=false,initialized=false;
slider.oninput=()=>{dirty=true;value.textContent=slider.value+'%'};
async function refresh(){if(busy)return;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error('Status unavailable');const s=await r.json();slider.min=s.min_percent;slider.max=s.max_percent;if(!dirty){slider.value=s.target_percent;value.textContent=s.target_percent+'%'}document.getElementById('applied').textContent=s.applied_percent+'%'+(s.startup?' (starting)':'');document.getElementById('pin').textContent='GPIO '+s.pwm_pin+' · '+s.pwm_hz+' Hz';document.getElementById('wifi').textContent=s.connected?'Connected':'Reconnecting';if(!initialized){message.textContent='Controller ready.';initialized=true}}catch(e){message.textContent='Controller unreachable. Retrying…'}}
document.getElementById('fan').onsubmit=async e=>{e.preventDefault();busy=true;button.disabled=true;try{const r=await fetch('/api/fan',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({percent:slider.value})});if(!r.ok)throw Error(await r.text());const s=await r.json();dirty=false;message.textContent='Speed set to '+s.target_percent+'%.'}catch(e){message.textContent='Could not apply speed: '+e.message}finally{busy=false;button.disabled=false;await refresh()}};refresh();setInterval(refresh,2500);
</script></html>)HTML";

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

void applyDuty(unsigned percent) {
    const uint32_t duty = (((1UL << FAN_PWM_BITS) - 1) * percent + 50) / 100;
    ledcWrite(pwmChannel, duty);
    appliedPercent = percent;
}

bool parsePercent(const String &text, unsigned &result) {
    if (text.length() == 0 || text.length() > 3) return false;
    unsigned value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    if (value < FAN_MIN_PERCENT || value > FAN_MAX_PERCENT) return false;
    result = value;
    return true;
}

void setTarget(unsigned value) {
    targetPercent = value;
    if (!startupActive) applyDuty(value);
}

void sendStatus() {
    String json;
    json.reserve(300);
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
    json += ",\"wifi_saved\":" + String(wifiSaved ? "true" : "false") + "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

void printStatus() {
    Serial.printf("Target %u%%, applied %u%%, GPIO %d, %u Hz%s\n", targetPercent, appliedPercent, FAN_PWM_PIN, FAN_PWM_HZ, startupActive ? " (startup)" : "");
    if (WiFi.status() == WL_CONNECTED) Serial.printf("Web: http://%s/ (http://%s.local/)\n", WiFi.localIP().toString().c_str(), WIFI_HOSTNAME);
    else Serial.printf("Wi-Fi connecting (status %d); PWM continues independently.\n", static_cast<int>(WiFi.status()));
    Serial.printf("Setup AP: %s, http://%s/wifi\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    Serial.printf("Wi-Fi settings saved: %s\n", wifiSaved ? "yes" : "no");
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
                else if (!overflow && parsePercent(String(line), value)) { setTarget(value); printStatus(); }
                else Serial.printf("Invalid command. Enter %u..%u or ?.\n", FAN_MIN_PERCENT, FAN_MAX_PERCENT);
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
    applyDuty(FAN_STARTUP_PERCENT);
    startupAt = millis();
    Serial.println("ESP32 standalone fan controller");
    printStatus();
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
    server.on("/api/fan", HTTP_POST, [] {
        unsigned value;
        if (server.args() != 1 || !server.hasArg("percent") || !parsePercent(server.arg("percent"), value)) {
            server.send(400, "text/plain", "Supply one integer percent within the configured duty limits.");
            return;
        }
        setTarget(value);
        sendStatus();
    });
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop() {
    if (startupActive && millis() - startupAt >= FAN_STARTUP_MS) {
        startupActive = false;
        applyDuty(targetPercent);
    }
    serviceSerial();
    serviceWifi();
    server.handleClient();
    delay(1);
}
