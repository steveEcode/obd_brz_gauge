# OBD BRZ Gauge

## Overview

OBD BRZ Gauge is an ESP-IDF based round dashboard project for the Waveshare ESP32-S3-Touch-LCD-1.85 development board. It connects to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders the information on a round touch display using LVGL.

## Status

- Hardware platform: Waveshare ESP32-S3-Touch-LCD-1.85
- Software stack: ESP-IDF 5.5.3, LVGL 8
- Protocol path: BLE + ELM327 (standard OBD PID + CAN broadcast frame ATMA monitoring)
- Vehicle profiles: BRZ ZC6 CAN / ZD8 CAN, Toyota GT86 ZN6, Mazda MX-5 ND, BMW G-series, Porsche 987.1/997.1/997.2, MINI JCW F56, OBD2 Generic
- Multi-gauge: one master + multiple slaves linked over ESP-NOW
- Validation status: fully verified on Subaru BRZ ZC6; other profiles are configured and may still need on-car verification

## Features

- BLE scan and connection for ELM327-compatible adapters
- **CAN broadcast frame ATMA monitoring**: bypasses standard OBD PID polling for high-frequency data — RPM at 100 Hz, oil/coolant temp at 10–20 Hz, with periodic OBD fallback for remaining channels (speed, load, voltage, intake temp)
  - ZC6 (Gen1): 0x140 (RPM + TPS) + 0x360 (oil + coolant temp)
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
### Themes are now data, not code

Adding a UI theme no longer touches a single C file. A theme is a folder under `themes/` holding a `theme.toml` manifest — eight decorative colors plus optional artwork — and one appended line in `themes/registry.txt`. `tools/gen_themes.py` runs at CMake configure time: it validates every manifest, converts PNG artwork into LVGL image arrays, and emits `ui_theme_generated.c`.

- **Artwork.** `ring` (360x360, alpha) replaces the drawn bezel, `needle` replaces the drawn meter needle (LVGL rotates it around a pivot declared in the manifest; art must point right), and `dial` (360x360) becomes the page background. Anything omitted falls back to the drawn shape and its color role, so a colour-only theme is still a single file.
- **Slot stability.** `themes/registry.txt` pins slot -> id and is append-only. NVS stores the slot number, so reordering it would silently re-skin every existing device on the next OTA — invisible in local testing. The generator hard-fails on reordering, gaps, a non-`default` slot 0, or a registry line whose folder is missing.
- **Build-time validation** covers artwork dimensions, needle pivot bounds, missing files, duplicate roller names, unknown keys, and a total artwork budget (1536 KB, since every registered theme's art is linked in unconditionally). Every failure points at a file and line.
- Artwork conversion needs Pillow only when a PNG's SHA-256 changes; the generated C is checked in, so an ordinary build has no third-party Python dependency. `--check` verifies freshness for CI.
- **Fixed:** the Settings theme roller built its option list in a fixed 96-byte buffer and silently truncated once enough themes were registered. It now uses an exactly sized buffer, so truncation is structurally impossible.
- **Fixed:** leaving the RPM warning restored a hardcoded black background, which would permanently blank a themed dial face. All three restore paths now reapply the theme background.

Authoring guide: [themes/README.md](themes/README.md) (bilingual). Framework internals: [docs/THEMING.md](docs/THEMING.md).

### Multi-gauge linked RPM warning

Three gauges can now light up in sequence as revs climb, instead of all strobing at once. The 1000 rpm below the warning threshold is split into thirds; each gauge ramps black to red across its own third according to its configured position, and at the threshold all three strobe together. Every unit derives its own segment from the same ESP-NOW-synced RPM, so no extra inter-gauge messaging is needed and they stay in sync naturally.

- New LINKED toggle on the RPM warning page, mutually exclusive with the existing single-gauge flash.
- Changing the threshold on one gauge broadcasts it to the others (new ESP-NOW control packet).
- The test button drives a synthetic RPM ramp across all gauges (5 s rise, 0.8 s hold, 2.5 s fall).
- NVS config version 1 -> 2: adds `rpm_warn_linked_en`, and migrates the default theme index 1 -> 0 so existing devices keep their current look.
- RPM strobe redraw interval was 1 ms; it is now 25 ms.

### Partition layout and boot animation

- App partition shrunk 6 MB -> 4 MB; the bootmedia SPIFFS partition grew 9.8 MB -> 11.9 MB.
- **Flashing change: bootmedia moves from `0x620000` to `0x420000`.** Update your flash command and scripts.
- Boot animation re-encoded on a 300x300 grid (was 240x240) with a new `delta_varint_rgb565_black_v2` stream format.


### Multi-gauge: real BLE pairing replaces the MAC-bind button

The old "BIND MASTER" button worked by grabbing whichever master's ESP-NOW broadcast the slave happened to be receiving at the moment — with no way to pick a specific one when multiple masters are nearby (e.g. a track day with several cars running the same product). The master now advertises a real BLE peripheral (`SkyGauge-XXYY`); the slave discovers and binds to it through the existing BLE scan page (now doubling as a "FIND MASTER" screen when the device role is SLAVE), and reconnects automatically on every following boot.

- New `gauge_pair_ble_client.c/h` (slave-side one-shot BLE pairing client) and `ble_adv_util.c/h` (shared BLE advertisement-name parsing, deduplicated out of the OBD BLE client).
- `racechrono_ble_diy.c` gained an independent pairing GATT service alongside the existing RaceChrono service, sharing one BLE advertisement.
- `ui_ScreenPageMultiGauge.c` no longer has BIND MASTER / UNBIND buttons.
- Boot flow: an unbound slave lands on the pairing screen; a bound slave skips straight to its gauge display.

### Fixed: master watchdog reboot during OBD protocol detection

Root-caused a board reboot seen during testing: the blocking wait for an ELM327 response could sit for up to 3 seconds without feeding the task watchdog. A run of consecutive protocol auto-detect timeouts could add up past the 5 s TWDT window and reboot the board mid-poll. Fixed by resetting the watchdog inside that wait loop.

### Other fixes

- Slave-side BLE scan state could get stuck once its 15 s scan window elapsed, silently blocking retry/rescan.
- Leaving the pairing screen in slave mode stopped the wrong BLE scan API, leaving a scan running in the background.
- I2C device cache could read out of bounds once more than 8 addresses were queried (latent crash, not yet hit in practice).
- LCD init could read an uninitialized register value if the QSPI probe failed.

### Improvements

- "NO SIGNAL" indicator on the gauge pages when BLE/ESP-NOW data goes stale.
- Gear display now prefers the CAN-decoded precise gear over the RPM/speed estimate when a vehicle profile provides one.
- Mileage/trip statistics are runtime-only now (no longer written to flash every 30 s) — nothing displayed them, so it was pure flash wear.
- Removed unused `fsm.h` state-machine scaffolding and an unused OBD-data "dirty flag" tracking layer — neither was ever wired up to anything.
- De-duplicated a shared BLE-advertisement-name parser, a screen ring border, and a dark roller LVGL style across ~18 screens; throttled a full chart redraw and a few gauge pages to only refresh when actually visible or actually changed.

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

Chinese documentation is available at [README.zh-CN.md](README.zh-CN.md), and the project root README is at [../../README.md](../../README.md).
