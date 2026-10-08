# SodaStream Smart CO2 Monitor & Shot Counter

An intelligent CO2 monitor and shot counter for motorized SodaStream fizzy water makers, powered by the **Seeed Studio XIAO ESP32-S3** and the **Seeed Studio Round Display for XIAO** (1.28" 240×240 IPS touchscreen).

---

## Features

- **Dynamic Circular Gauge (Screen 1)**: Real-time visual CO2 capacity gauge tracking 410.0 g (410,000 mg) capacity with 0.1 g resolution, percentage, and low-CO2 warning color shifts (turns red below 20%).
- **In-Cycle Indicator (Screen 1)**: Visual indicator button that lights up bright green during active injection cycles and dims when idle.
- **Cycle Timing & Noise Rejection (Screen 2)**: Captures exact injection durations in milliseconds with 50 ms debounce rejection for inductive solenoid spikes.
- **Canister Shot Counter (Screens 3 & 6)**: Synchronized shot count across screens to monitor usage per cylinder.
- **On-Device Calibration (Screens 4 & 5)**: Interactive sliders to calibrate milliseconds per shot (2000–2100 ms) and milligrams per shot (1270–1670 mg).
- **Hardware & Web Reset (Screen 6)**: Long-press button or web button to reset cylinder capacity to 410 g when swapping tanks.
- **Embedded Web Configuration Portal**:
  - **Fallback Access Point**: Broadcasts `SodaStream-Setup` at `192.168.4.1` with captive portal for initial Wi-Fi setup.
  - **Station Mode & mDNS**: Connects to your home Wi-Fi and provides a web dashboard at `http://sodastream.local`.
  - Web-based calibration for Wi-Fi credentials, NTP server, POSIX timezone, and tank parameters.
- **NTP Time Synchronization**: Automatically synchronizes the on-board I2C RTC (BM8563) via NTP on boot and every 24 hours (default: `MST7` / America/Phoenix).
- **Sleep & Backlight Protection**: 5-minute inactivity timeout turns off LCD backlight (`D6`) and loads a blank screen to prevent image retention, waking instantly on touch or injection trigger.
- **Non-Volatile Storage (NVS)**: All calibrations, bottle counts, shot counts, and remaining CO2 persist safely across power cycles.

---

## Hardware Pinout & Wiring

| Pin | Function | Notes |
|---|---|---|
| **D0** | SodaStream Valve Signal | Active HIGH (3.3V logic) during CO2 injection |
| **D6** | LCD Backlight Control | HIGH = Backlight ON, LOW = Backlight OFF (Sleep) |
| **I2C (D4/D5)** | BM8563 RTC & Touch | Shared I2C bus for capacitive touch and RTC |
| **SPI** | GC9A01 Display Driver | High-speed SPI interface |

---

## Directory Structure

```
├── README.md               # Project documentation
├── .gitignore              # Git ignore rules for build artifacts
├── ui/
│   ├── ui.ino              # Main Arduino sketch & application logic
│   ├── driver.h            # Touch & display driver configuration
│   └── lv_conf.h           # LVGL 8.3 configuration
├── libraries/              # Self-contained libraries and drivers
│   ├── ui/                 # SquareLine Studio exported UI code
│   ├── lvgl/               # LVGL 8.3.11 graphics library
│   ├── Seeed_GFX/          # Optimized GC9A01 display driver
│   ├── Seeed_Arduino_Round_display/ # Board support package
│   └── I2C_BM8563_RTC/     # Real-Time Clock driver
└── squareline/             # SquareLine Studio v1.6.1 visual project files
    ├── SquareLine_Project.spj
    ├── components/
    └── assets/
```

---

## Compilation & Flashing

### Arduino IDE
1. Install the **esp32** board package by Espressif in the Boards Manager.
2. Select Board: **XIAO_ESP32S3**.
3. In `Preferences`, ensure the project's `libraries/` directory is accessible in your sketchbook library path.
4. Open `ui/ui.ino` and click **Upload**.

### Command Line (`arduino-cli`)
```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 --libraries ./libraries ./ui
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3 ./ui
```
