# OBD BRZ Gauge

English | [简体中文](README.md)

An open-source round car gauge built on ESP-IDF, running on the Waveshare
ESP32-S3-Touch-LCD-1.85. It connects to an ELM327-compatible BLE OBD adapter,
reads vehicle data, and renders a touch UI with LVGL.

**[Demo video](https://www.douyin.com/video/7614174567678984187)**

> A derivative of [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp)
> ([Bilibili demo](https://www.bilibili.com/video/BV18oHXz6EiQ/)), adding
> multi-vehicle profiles, an ESP-NOW linked triple-gauge cluster and a theme system.

---

## Documentation

| Document | Contents |
|----------|----------|
| [docs/USER_GUIDE.en.md](docs/USER_GUIDE.en.md) | **User guide** — page navigation, settings, triple-gauge pairing, boot animation, app updates |
| [docs/FLASH.en.md](docs/FLASH.en.md) | **Flash & update** — partition table, USB flashing, App OTA with rollback |
| [docs/APP_PROTOCOL.en.md](docs/APP_PROTOCOL.en.md) | **App integration protocol** — OTA-mode WiFi HTTP API |
| [docs/VEHICLES.en.md](docs/VEHICLES.en.md) | **Vehicle profiles** — JSON vehicle profiles (17, only 1 built into the firmware), install/activate, adding a new vehicle |
| [docs/PID_GUIDE.en.md](docs/PID_GUIDE.en.md) | **PID / signal definition guide** — turning a discovered PID/CAN bit into an RPN formula, with real examples |
| [docs/THEMES.en.md](docs/THEMES.en.md) | **Theming** — both theme systems, artwork specs, theme store |
| [docs/TROUBLESHOOTING.en.md](docs/TROUBLESHOOTING.en.md) | **Troubleshooting** — no connection / no data, protocol auto-detection |
| [docs/DEVELOPMENT.en.md](docs/DEVELOPMENT.en.md) | **Development** — architecture, build config, tool scripts |
| [CHANGELOG.en.md](CHANGELOG.en.md) | Changelog |

## Hardware & stack

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-1.85 (360×360 round ST77916 LCD, 16 MB flash, 8 MB PSRAM)|
| Stack | ESP-IDF 5.5.3, LVGL 8 |
| Link | BLE to an ELM327-compatible OBD adapter (standard PIDs + manufacturer Mode 21/22)|
| Triple gauge | One master reads OBD; slaves mirror it over ESP-NOW with zero extra load |
| Sensors | ADS1115 oil-pressure ADC (V1 board; cars with an OBD oil-pressure DID read it over OBD instead)|

## Vehicle profiles (18 profiles, 1 built in)

The firmware only has `OBD2 Generic` (standard OBD2) built in. The other 17
vehicles are JSON profiles: install one from an SD card (`/OBD/IMPORT`), the
companion app (via the `/api/v1/vehicles` API), or the on-device **Vehicle
page**, then activate it and reboot. Full catalog table, install steps and behavior changes are in
[docs/VEHICLES.en.md](docs/VEHICLES.en.md):

`OBD2 Generic` · `ZN/C6 CAN` · `ZN/C6 PID` · `ZD8 OBD` · `ZD8` · `MX-5 ND` ·
`BMW F/G` · `Supra A90` · `BMW G OBD` · `BMW E` · `JCW F56` · `MINI R55` ·
`POS 997.2` · `POS 997.1` · `POS 987.1` · `GIULIA 2.0T` · `jeep` · `Honda Integra`

Only the Subaru BRZ ZN/C6 has been fully verified on a real car; the other
profiles are configured but some still need road testing.

For editable configurations, see [vehicles/import/](vehicles/import/README.en.md):
**19 independent import templates** (`custom.*` copies of the 18 catalog profiles
plus BMW G direct gear), with explicit request headers, bit decoding and polling
intervals. Copy a selected JSON to the SD card's `OBD/IMPORT/`, install, activate
and reboot.

New vehicle profiles can declare only their special signals; 9 omitted standard
signals default to OBD2. Use `disabled: true` for one signal or root
`defaults: "none"` to opt out. Minimal and BMW G sparse gear examples are in
[vehicles/examples/](vehicles/examples/README.en.md).

## Highlights

- **JSON vehicle profile system** — 18 profiles; only `OBD2 Generic` is
  built in, the rest install + activate + reboot via SD/app/Vehicle
  page; per-vehicle gear ratios (up to 8 speeds), oil-temp strategies,
  protocol locks and addressing; gear display prefers direct CAN/DID reads,
  falling back to ratio estimation
- **CAN broadcast monitoring** — only `ZN/C6 CAN` uses the ATMA fast path
  (0x140 throttle, 0x360 oil/coolant temp); every other profile stays on the
  single-threaded OBD poll loop to avoid adapter contention
- **Manufacturer oil-temp paths** — Toyota/Subaru Mode 21, Mazda / MINI / BMW
  Mode 22, FCA UDS extended addressing, with automatic fallback when the
  primary strategy fails
- **Triple-gauge cluster over ESP-NOW** — the master broadcasts, slaves render
  with zero extra OBD load; real BLE pairing (master advertises
  `SkyGauge-XXYY`), survives power cycles
- **RPM warning (incl. linked mode)** — full-screen red flash past the
  threshold; in cluster mode the gauges light up in position order
- **Two theme systems** — compiled-in TOML themes (colors/artwork) and a
  SD runtime themes (custom page layouts), see
  [docs/THEMES.en.md](docs/THEMES.en.md)
- **App OTA** — the companion app pushes over a WiFi hotspot,
  SHA256-verified, with a 15-second boot self-check and automatic rollback
- **Boot animation** — OFF / RACE / VIDEO modes; VIDEO plays a custom
  animation uploaded from the phone app (360×360 circular safe area)
- **SD card** — supports FAT32 and exFAT (exFAT is a separate filesystem; the
  firmware does not auto-format cards)
- **RaceChrono BLE output** — can act as a RaceChrono DIY device
- **Self-healing link** — re-initializes and reconnects automatically when
  data stalls; Auto retains its selection and re-probes on decode failures,
  supporting 11/29-bit CAN, J1850 and K-line responses.
  Default `0100` probe failures retry the configured RPM request `010C` after safe synchronization.

## Quick start

### Option 1: flash the pre-built firmware

You need: the board, a USB data cable, and
[esptool.py](https://github.com/espressif/esptool) (`pip install esptool`).

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     firmware/release/bootloader/bootloader.bin \
  0x8000  firmware/release/partition_table/partition-table.bin \
  0xf000  firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin
```

- Replace `PORT` with your serial port (Windows `COM3`, Linux `/dev/ttyUSB0`,
  macOS `/dev/cu.usbserial-*`)
- This command does not touch NVS user settings or Bluetooth bindings; see
  [docs/FLASH.en.md](docs/FLASH.en.md) for what the one-time first-boot
  migration does
- Custom images, theme videos and independent boot animation media live on SD. Packages are at
  `/OBD/THEMES/<key>/theme.bin`; the web uploader stores boot media in shared `/OBD/BOOT/`.
  Legacy `bootmedia.bin` can be copied as `/OBD/BOOT/bootmedia.raw`. Without a card, the built-in
  UI is used
- Full partition layout in [docs/FLASH.en.md](docs/FLASH.en.md); flash
  addresses are governed by [partitions.csv](partitions.csv)

### Option 2: build from source

Requires the [ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/) environment:

```bash
git clone https://github.com/steveEcode/obd_brz_gauge.git
cd obd_brz_gauge
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

The first build downloads component dependencies into `managed_components/`.
Before building for a different board, pick the hardware version under
`idf.py menuconfig → OBD DSP Configuration` (see
[docs/DEVELOPMENT.en.md](docs/DEVELOPMENT.en.md)).

## Repository layout

```
main/
├── app_main.c          # Entry point: hardware, LVGL, BLE and task startup
├── app_obd_dsp/        # Application: OBD cache, SD import/activation, OTA, boot media, device identity
├── bsp_obd_dsp/        # Board support: ELM327 BLE client, ESP-NOW, LCD/touch/IO expander, NVS
├── export_path/        # LVGL UI (SquareLine export): 20+ screens, fonts, images
├── theme_engine/       # Runtime theme loader (SD theme.bin)
└── vehicle/            # Vehicle profile manager (vp_manager: JSON validate/compile/lifecycle)
themes/                 # Compiled-theme sources (TOML manifests + artwork)
bootmedia/              # Boot animation sources
vehicles/catalog/       # Vehicle profile catalog (JSON, not compiled in; see docs/VEHICLES.en.md)
vehicles/import/        # Editable SD import pack (18 custom.* JSON templates)
tools/                  # Helper scripts (theme codegen, packing, PID hunting, release)
firmware/release/       # Pre-built firmware
android_app/            # Companion phone app (APK)
model/                  # 3D-printable models (housing, pods, brackets)
docs/                   # Documentation (see index above)
partitions.csv          # Flash partition table (16MB)
```

## 3D-printable models

| File | Description |
|------|-------------|
| `model/esp32_1.85_weixue/housing.stl` | Board housing |
| `model/Subaru/brz_zc6/triple_gauge_pod.stp` | BRZ ZN/C6 triple-gauge pod |
| `model/Subaru/brz_zc6/passenger_dashboard_scan.stl` | BRZ ZN/C6 passenger-dash scan (fitting reference)|
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND air-vent bracket |

## Known limitations

- Data reading is fully verified only on the Subaru BRZ ZN/C6; other ELM327
  clones and vehicle PID formats are not guaranteed
- Oil-pressure alarms are throttled to one alert per 30 s
- Trip / mileage statistics are runtime-only and reset on power cycle
- Theme switching needs a reboot (screens are created once at boot)

## Acknowledgments

- [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp) — upstream project
- [Hokori23](https://github.com/Hokori23) — performance contributions (NVS flush lock, page-aware refresh cadence, OBD polling throughput)
- [timurrrr/ft86](https://github.com/timurrrr/ft86) — comprehensive FT86 CAN bus documentation that made CAN broadcast monitoring possible

## License

Licensed under **GPLv3**, see [LICENSE](LICENSE). Free to use, modify and
distribute; distributed modifications must remain under GPLv3.

```
Copyright (C) 2024-2026  steveEcode and contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
```

Generic reads boost pressure from `0170` first and falls back to MAP estimation via `010B`; coolant/oil display limits extend to 255°C. RPM, speed, boost and throttle receive priority polling when demanded by the page. Reimport older installed profiles to apply explicit range fixes; see [vehicle documentation](docs/VEHICLES.en.md).
