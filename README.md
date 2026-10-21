# ESP32 standalone Wi-Fi fan controller

Standalone firmware for an original ESP32 development board. The ESP32 drives a two-wire fan through a BD139 transistor and serves its own browser control page, with station Wi-Fi, a setup access point, and persistent network settings.

## PWM pin and wiring

**Set `FAN_PWM_PIN` in `include/fan_config.h`. The default is GPIO23**, usually marked `23`, `IO23`, or `D23` on an ESP32 development board. This is a GPIO number, not the 23rd physical connector pin. Rebuild and flash after changing it. Use an exposed output pin appropriate for your board; avoid flash GPIO6..11, input-only GPIO34..39, UART GPIO1/3, and boot strapping GPIO0/2/5/12/15. The supplied build targets the original ESP32, not C3/S2/S3 variants.

```text
ESP32 GPIO23 ---- 470 ohms ---- BD139 base (B)
                                  |
                               10 kohms
                                  |
ESP32 GND -------------------- BD139 emitter (E) ---- 12 V negative
fan negative ----------------- BD139 collector (C)
fan positive --------------------------------------- +12 V
```

Connect GPIO23 through the base resistor to the BD139. USB powers the ESP32; the fan uses its external supply. Join ESP32 GND and fan supply negative. Never connect 12 V to an ESP32 pin. Verify the transistor manufacturer's B/C/E pinout. Fit the flyback diode across the fan: cathode to +12 V, anode to collector/fan negative. The base-to-emitter resistor holds the transistor off during reset.

The existing 470 ohm base resistor provides roughly 5 mA from a 3.3 V GPIO. Check BD139 saturation and heating against the fan's running and starting currents; a suitable 3.3 V logic-level MOSFET may be needed for higher current.

## Behavior

- Hardware PWM at 100 Hz for the existing two-wire fan power-switching circuit.
- On restart: 80% for one second, then 50%. Web/serial commands during startup change the target applied afterward.
- Set duty from 20% through 80%. Adjust the duty limits and startup behavior in `include/fan_config.h`.
- Settings remain in RAM; reboot restores the default.
- Wi-Fi connection/reconnection runs without waiting in the application loop; PWM continues independently of the network.
- No RPM feedback is connected. Duty is not a measured speed; verify the fan starts and stays turning at the chosen minimum.

## Wi-Fi and web control

`include/secrets.h` contains your initial local station Wi-Fi credentials and is ignored by Git. On the first boot, these settings are saved to ESP32 NVS flash. Later boots load the saved settings. A fresh checkout requires copying `include/secrets.example.h` to `include/secrets.h` and supplying initial credentials. Once settings have been saved, change them through the setup page; editing compiled defaults does not override saved NVS settings. Credentials are stored on the device and compiled into the initial firmware.

Use a 2.4 GHz Wi-Fi network. After boot, read the DHCP address from serial at 115200 baud and open `http://ADDRESS/`, or try `http://esp32-fan.local/` on devices with mDNS support. The page shows a speed slider, applied duty, GPIO, and connection state. It is served from ESP32 flash; the PC can be disconnected after flashing when the ESP32 has another USB power supply.

The HTTP interface has no login and is intended for your trusted local network. Serial control remains available and station connection retries occur every 30 seconds.

### Setup access point and saved Wi-Fi settings

The ESP32 also runs a password-protected AP, including when station Wi-Fi is disconnected:

- Wi-Fi name: **ESP32-Fan-Setup**
- AP password: **FanSetup32!**
- Setup page: **http://192.168.4.1/wifi**
- Fan control over the AP: **http://192.168.4.1/**

Connect a phone or computer to this AP, stay connected if it reports no internet, and open the setup address. Enter the station SSID and password, then select **Save and connect**. The form clears the password after saving and never reads the stored password back. SSID and password are written together as one NVS record; the device then reconnects while keeping the setup AP available. Saved settings survive reboot and normal firmware uploads. Invalid input does not overwrite existing settings.

SSID must be 1..32 bytes. WPA passwords must be 8..63 bytes, or a 64-digit hexadecimal key; an empty password selects an open network. This original ESP32 supports 2.4 GHz Wi-Fi. AP and station share one radio/channel, so an AP client may briefly reconnect when the station changes channel. Change the AP name/password in `include/fan_config.h` and reflash if desired.

API:

- `GET /api/status`: JSON target/applied duty, configured limits, PWM pin/frequency, startup state, Wi-Fi state, station/AP IPs, and saved-settings state. Passwords are never returned.
- `POST /api/wifi`, form body with `ssid` and `password`: validate and save station settings in flash, then reconnect. Returns HTTP 400 for invalid input or HTTP 503 if flash storage fails.
- `POST /api/fan`, form body `percent=50`: apply an integer within the configured limits and return status. Invalid/missing/duplicate arguments return HTTP 400 without changing the setting.
- Serial: send a duty such as `50` followed by Enter, or `?` for status and the web address. Invalid commands leave the target unchanged.

## Build and flash

Uses PlatformIO Core with pinned `espressif32@6.12.0` and its Arduino-ESP32 2.0.17 framework. This version uses the channel-based LEDC API. All libraries are included in the ESP32 framework.

```powershell
Copy-Item include\secrets.example.h include\secrets.h
# Edit include/secrets.h with your initial Wi-Fi credentials.
.\build.ps1
.\flash.ps1 -Port COM4 -SkipBuild
.\.venv\Scripts\pio.exe device monitor --port COM4 --baud 115200
```

Scripts use `.venv/Scripts/pio.exe`, falling back to `pio` in PATH. Install PlatformIO Core if needed: `python -m pip install platformio`. The first build downloads its pinned ESP32 tools. Firmware outputs are in `.pio/build/esp32dev/`. Uploads use 460800 baud. Close any serial monitor before uploading. If another port is assigned after reconnecting, pass that port to `flash.ps1`.

## Validation

Verified on an ESP32-D0WDQ6 with 4 MB flash:

- Build and USB upload at 460800 baud passed, including flash hash verification.
- Fan and Wi-Fi setup pages responded over the local network.
- Web and serial controls accepted 20% and 80%, rejected invalid commands, and restored 50%.
- The Wi-Fi setup endpoint saved credentials to NVS; a reboot loaded them and rejoined the network.
- The WPA2 setup AP was visible and accepted a PC connection.

Electrical PWM waveform, actual fan RPM, and transistor temperature have not been measured. No temperature-sensor support is included.

## References

- [Espressif Arduino Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html)
- [Pinned PlatformIO ESP32 platform](https://github.com/platformio/platform-espressif32/tree/v6.12.0)
- [Arduino-ESP32 2.0.17 LEDC header](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-ledc.h)
