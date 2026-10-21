#pragma once

// ESP32 GPIO number (the header marked IO23 / 23), not a physical pin number.
// Connect this pin THROUGH the existing 470 ohm resistor to the BD139 base.
#define FAN_PWM_PIN            23
#define FAN_PWM_HZ             100u
#define FAN_PWM_BITS           10u
#define FAN_MIN_PERCENT        20u
#define FAN_MAX_PERCENT        80u
#define FAN_DEFAULT_PERCENT    50u
#define FAN_STARTUP_PERCENT    80u
#define FAN_STARTUP_MS         1000u
#define SERIAL_BAUD            115200u
#define WIFI_HOSTNAME          "esp32-fan"
#define WIFI_RETRY_MS          30000u

// Setup access point remains available while station Wi-Fi is connecting.
#define WIFI_AP_SSID           "ESP32-Fan-Setup"
#define WIFI_AP_PASSWORD       "FanSetup32!"
