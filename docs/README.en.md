# OBD BRZ Gauge

## Overview

OBD BRZ Gauge is an ESP-IDF based dashboard display project for the Waveshare ESP32-S3-Touch-LCD-1.85 development board. It connects to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders the information on a round touch display using LVGL.

The repository already includes the main application entry, BLE OBD communication, board support code, LVGL UI resources, and NVS-based configuration and statistics logic, so it is in a reasonable state for public open-source release.

## Current Status

- Board: Waveshare ESP32-S3-Touch-LCD-1.85
- MCU: ESP32-S3
- Framework: ESP-IDF 5.1+
- Graphics: LVGL 8
- Transport: BLE to an ELM327-compatible OBD adapter
- Vehicle currently verified: Subaru BRZ ZC6

Important note: vehicle data has only been tested on Subaru BRZ ZC6 so far. Other vehicles, model years, ECUs, and BLE OBD adapters still need validation.

## Features

- Initializes LCD, touch, LVGL, NVS, and BLE during startup
- Scans for and connects to ELM327-compatible BLE OBD devices
- Displays RPM, speed, coolant temperature, intake temperature, oil temperature, engine load, throttle position, fuel level, battery voltage, and related values
- Persists user configuration locally
- Records mileage and runtime statistics
- Includes exported UI assets for further customization

## Repository Layout

- [main/app_main.c](../main/app_main.c): application entry, hardware initialization, LVGL startup, UI bootstrap, BLE startup
- [main/app_obd_dsp](../main/app_obd_dsp): application-level data cache, gear calculation, mileage statistics
- [main/bsp_obd_dsp](../main/bsp_obd_dsp): board support package, BLE client, NVS, LCD, touch, I2C, and IO expander drivers
- [main/export_path](../main/export_path): exported UI code, fonts, images, and screen logic
- [managed_components](../managed_components): dependencies pulled by the ESP-IDF component manager
- [tools](../tools): helper scripts such as asset conversion utilities

## Requirements

- ESP-IDF 5.1 or newer
- Python environment and ESP-IDF toolchain
- USB cable for flashing
- An ELM327-compatible BLE OBD adapter

Dependencies are declared in [main/idf_component.yml](../main/idf_component.yml), including:

- lvgl/lvgl
- espressif/esp_lcd_touch
- espressif/button
- espressif/knob

## Build and Flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

During the first build, ESP-IDF may download missing components into the managed_components directory.

## Porting Notes

1. The current board support code is tailored for the Waveshare ESP32-S3-Touch-LCD-1.85.
2. If you change the display, touch controller, IO expander, or pin mapping, review the drivers under [main/bsp_obd_dsp](../main/bsp_obd_dsp).
3. If you use another vehicle or another OBD adapter, verify BLE services, characteristics, command formatting, and response parsing again.
4. UI assets are located under [main/export_path](../main/export_path) and can be edited further.

## Known Limitations

- Vehicle data is currently verified only on Subaru BRZ ZC6
- Not all ELM327-compatible adapters are guaranteed to behave the same way
- Not all vehicles return identical PID data or response formats
- Different ESP-IDF versions may require minor driver or dependency adjustments

## Suggested Release Checklist

Before publishing the repository, it is worth adding:

- An explicit open-source license
- Hardware photos, wiring notes, and UI screenshots
- The exact OBD adapter model used for testing
- A note describing which values are based on generic OBD PIDs and which are vehicle-specific assumptions

Chinese documentation is available at [docs/README.zh-CN.md](README.zh-CN.md).