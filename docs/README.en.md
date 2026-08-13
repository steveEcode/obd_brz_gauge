# OBD BRZ Gauge

> 📖 Full documentation index: [project README](../README.md) · 中文说明: [README.zh-CN.md](README.zh-CN.md)

## Overview

OBD BRZ Gauge is an ESP-IDF based round dashboard project for the Waveshare ESP32-S3-Touch-LCD-1.85 development board. It connects to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders the information on a round touch display using LVGL.

## Status

- Hardware platform: Waveshare ESP32-S3-Touch-LCD-1.85
- Software stack: ESP-IDF 5.5.3, LVGL 8
- Protocol path: BLE + ELM327 (standard OBD PID + CAN broadcast frame ATMA monitoring)
- Vehicle profiles (12): OBD2 Generic, ZN/C6 CAN, ZN/C6 PID, ZD8 CAN, ZD8, MX-5 ND, BMW F/G, BMW G CAN, JCW F56, POS 997.2, POS 997.1, GIULIA 2.0T
- Multi-gauge: one master + multiple slaves linked over ESP-NOW
- Validation status: fully verified on Subaru BRZ ZN/C6; other profiles are configured and may still need on-car verification

## Features

- BLE scan and connection for ELM327-compatible adapters
- **CAN broadcast frame ATMA monitoring**: bypasses standard OBD PID polling for high-frequency data — RPM at 100 Hz, oil/coolant temp at 10–20 Hz, with periodic OBD fallback for remaining channels (speed, load, voltage, intake temp)
  - ZN/C6 (Gen1): 0x140 (RPM + TPS) + 0x360 (oil + coolant temp)
  - ZD8 (Gen2): 0x40 (RPM + TPS) + 0x345 (oil + coolant temp)
- **Custom boot logo & boot media playback**: configurable boot logo with multi-block animation, SPIFFS-mounted bootmedia partition for rich startup sequences
- **Unified vehicle configuration system**: compile-time `vehicle_custom_config.h` for per-vehicle thresholds, warnings, and gauge ranges
- Real-time display for RPM, speed, coolant/intake/oil temperature, oil pressure, turbo boost, throttle, engine load, battery voltage, gear (prefers CAN-decoded precise gear when available, falls back to RPM/speed estimate), and related values
- "NO SIGNAL" indicator on the gauge pages when BLE/ESP-NOW data goes stale
- Vehicle profile selector: per-car gear ratios (up to 8-speed), oil-temp strategy, turbo boost, and per-vehicle protocol lock (e.g. BMW/Porsche force ISO 15765-4 CAN)
- Manufacturer oil-temp paths beyond standard PID 01 5C: Toyota/Subaru Mode 21, Mazda Mode 22, MINI/BMW Mode 22, BMW F-series Mode 22 44 02, Porsche CAN broadcast 0x441 monitoring
- Configurable pointer (needle) gauge page with swipe-down data-source selection
- **RPM over-limit flash warning**: configurable threshold, background image flashes red when RPM exceeds the limit
- **Triple-gauge startup animation**: three boards display "RACE / AS / ONE" in sequence over ESP-NOW synchronized intro
- Robust connection handling: waits for BLE notify subscription, re-initializes on every reconnect, and self-heals (re-init + auto reconnect) when data stalls — no manual reconnect needed after ignition
- Multi-gauge over ESP-NOW: a master reads OBD and broadcasts; slaves display the same data with no extra OBD load; role is chosen in settings; startup sweep is synchronized; slaves show the master name
- Optional custom boot logo via a compile-time switch
- LVGL touch UI exported from SquareLine-based assets
- User configuration persisted via NVS; mileage/trip statistics are runtime-only (reset each power cycle, not written to flash)

## Multi-Gauge

An ELM327 BLE adapter accepts only one client, so multiple gauges cannot each connect to it. Instead, one board is the **master** (keeps the BLE + ELM327 link and reads OBD) and the others are **slaves**. The master broadcasts the parsed data cache over ESP-NOW; slaves render it with no extra OBD load. Flash the same firmware to every board and pick the role in Settings → swipe down → MULTI-GAUGE (reboot to apply). The master's BLE/WiFi coexist on the ESP32-S3; slaves run ESP-NOW only.

Pairing is done over real BLE, not a blind "bind whatever I last heard" button: once a board is set to MASTER, it advertises as `SkyGauge-XXYY` (a per-device suffix so multiple masters at a meet don't collide). An unbound slave boots straight into a "FIND MASTER" scan screen (the same screen used to pick an OBD adapter), lets you pick the right master from the list, and remembers it — every following boot skips the scan and goes straight to the gauge display.

## Repository Layout

- [main/app_main.c](../main/app_main.c): application entry, LVGL initialization, BLE startup, task startup
- [main/app_obd_dsp](../main/app_obd_dsp): runtime OBD data cache, vehicle profiles, CAN frame decoders, mileage statistics, boot media playback, and vehicle custom config
- [main/bsp_obd_dsp](../main/bsp_obd_dsp): board support package, BLE client, NVS, LCD, touch, I2C (ESP-IDF 5.5 new API), IO expander, and ESP-NOW link drivers
- [main/export_path](../main/export_path): UI source exported from the design tool (SquareLine)
- [bootmedia](../bootmedia): boot animation media blocks (SPIFFS partition source)
- [model](../model): open-source 3D printable files (housing, gauge pods, brackets)
- [tools](../tools): helper scripts (image conversion, boot block builder)
- [firmware/release](../firmware/release): pre-compiled firmware binaries for flashing
- [managed_components](../managed_components): dependencies pulled by the ESP-IDF component manager

## Requirements

- ESP-IDF 5.5.3 or newer
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

## Pre-compiled Firmware

Ready-to-flash binaries are provided under `firmware/release/`:

| File | Description |
|------|-------------|
| `bootloader/bootloader.bin` | Bootloader |
| `partition_table/partition-table.bin` | Partition table |
| `ota_data_initial.bin` | OTA data initial partition |
| `obd_brz_gauge.bin` | Application firmware |
| `bootmedia.bin` | Boot animation media partition |
| `flash_address_map.txt` | Flash address map |

Flash all at once with esptool:

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 firmware/release/bootloader/bootloader.bin \
  0x8000 firmware/release/partition_table/partition-table.bin \
  0xf000 firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin \
  0x420000 firmware/release/bootmedia.bin
```

## Porting Notes

1. The current board support code is tailored for the Waveshare ESP32-S3-Touch-LCD-1.85.
2. If you change the display, touch controller, IO expander, or pin mapping, review the drivers under [main/bsp_obd_dsp](../main/bsp_obd_dsp).
3. If you use another vehicle or another OBD adapter, verify BLE services, characteristics, command formatting, and response parsing again.
4. UI assets are located under [main/export_path](../main/export_path) and can be edited further.

## Changelog

See [CHANGELOG.md](../CHANGELOG.md) — it is the single source of truth for
both languages. It used to be duplicated across three README files.

## Known Limitations

- Vehicle data is currently verified only on Subaru BRZ ZC6
- Not all ELM327-compatible adapters are guaranteed to behave the same way
- Not all vehicles return identical PID data or response formats
- Different ESP-IDF versions may require minor driver or dependency adjustments

## 3D Models (Open Source)

The `model/` directory contains open-source 3D printable files:

| File | Description |
|------|-------------|
| `model/esp32_1.85_weixue/housing.stl` | ESP32-S3-Touch-LCD-1.85 board housing |
| `model/Subaru/brz_zc6/triple_gauge_pod.stp` | BRZ ZC6 triple gauge pod (3× 1.85" round) |
| `model/Subaru/brz_zc6/passenger_dashboard_scan.stl` | BRZ ZC6 passenger dashboard scan (for fitting reference) |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND air vent mount bracket |

Chinese documentation is available at [README.zh-CN.md](README.zh-CN.md), and the project root README is at [../README.md](../README.md).
