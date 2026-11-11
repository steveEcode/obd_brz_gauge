# User Guide

English | [简体中文](USER_GUIDE.md)

For flashing and building see [FLASH.en.md](FLASH.en.md); if you cannot
connect, start with [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md).

All gauge operations in the web flasher and vehicle community use ESP32-S3 native USB: firmware flashing, theme/animation installation, deleting the current theme, vehicle installation/activation, and reboot. No device address, hotspot or OTA mode is needed. First update to firmware supporting USB management, boot normally, and select the serial port from the web action. Use Chrome/Edge over HTTPS or localhost. CH340/CP210x can flash firmware but cannot perform application media or management operations. Community browsing, login, downloads and publishing still use the network; App WiFi OTA remains compatible.

The firmware also fixes a missed LCD SPI lock wakeup in ESP-IDF 5.5.3 that could leave boot video stuck near its last frame. Existing animations and transparent-image themes need no re-export; update the firmware.

## First boot

1. On power-up the backlight stays off until the first Sky Gauge logo frame has
   reached the LCD and a 50 ms panel-scan settling delay has elapsed; the
   configured boot animation then plays (RACE by default, can be disabled)
2. Install vehicle profiles and themes through the web UI or App; activate a
   profile on the gauge's VEHICLE page and select a theme under Settings → THEME
   - **BOOT PAGE** — pick the screen the gauge lands on after boot
3. **Swipe up from the version page → ELM327 PAIRING → BLE SCAN** and select your
   ELM327 BLE adapter
4. Once connected the gauge initializes automatically (protocol auto-detection
   is described in [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md#protocol-auto-detection))
   and data starts flowing

On every later power-up the gauge reconnects to the last adapter
automatically (remembers BLE name + MAC) and self-heals when data stalls —
normally **you just power on and drive**, no manual steps.

The web vehicle community supports downloading, favoriting and editing profiles. Web device operations use ESP32-S3 native USB: install a profile, review warnings, then separately activate it for next boot and request a reboot. Install themes over web USB or App WiFi OTA, then select a registered theme under Settings → THEME. Verify vehicle compatibility and perform these operations while parked. Firmware manages the SD card in the background; users do not need to remove it or organize its files.

**Delete current theme** on the web flasher removes the current theme package and its dedicated resources under SD `/OBD/THEMES/<key>/`, then restores the built-in default after restart. The SD card must be mounted. Other themes and the shared boot animation are not deleted. This operation cannot be undone from the gauge.

## Page navigation

The main UI is a **page carousel**; swipe left/right to move through the ring
(swipe left = next page):

Images, videos and other content areas on a runtime theme page pass gestures to
the page itself, so swiping anywhere in the theme content moves to the next page
or the version page. The theme's default page reads and converts its images in
a background task while the Logo / boot animation remains visible. The boot
video replaces the Logo only after its first frame finishes transferring to the LCD;
UI updates cannot clear the startup image during playback. If the boot video ends first, the gauge returns to the Logo while the theme page finishes preparing. LVGL
creates the theme widgets after video playback and preparation finish, then
switches pages; a video decode failure or interruption immediately restores
the Logo, and a failed theme preparation falls back to the built-in page. Theme images use bulk SD reads to avoid lengthy byte-by-byte loading of transparent images; existing theme packages need no re-export.

```
        ┌──────────────────────────────────────┐
        │  GEAR → RPM → SPEED → TEMP → INFO →  │
        │  NEEDLE → CHART → TPMS → version → GEAR│
        └──────────────────────────────────────┘
```

| Page | Shows | Swipe-down sub-page |
|------|-------|---------------------|
| GEAR | Gear (large) + RPM | Runtime-theme gauge page (only when a partition theme with pages is loaded)|
| RPM | RPM arc + red over-rev flash | RPM warning threshold / flash toggle |
| SPEED | Speed + trip stats (runtime-only) | — |
| TEMP | Coolant / oil / intake temps (remappable rows) | 3-row data-item mapping |
| INFO | Multi-cell info panel (remappable cells) | 5-cell data-item mapping |
| NEEDLE | Needle meter (swipe down to change source) | Needle source picker |
| CHART | Chart page | Down: chart source config / **up: alarm thresholds** |
| TPMS | Four-wheel pressure / temperature / battery | Tap a wheel for full diagnostics |
| Version page | Firmware build tag, BLE status and device name | see hidden entries below |

For multi-page runtime themes, left/right navigation prepares target images in the background while the current page remains visible and refreshing, then switches when ready. Repeated navigation during preparation is not queued; leaving cancels the pending transition, and failure keeps the current page. Font and widget creation can still briefly stall; SD speed and available memory also affect the result.

**The version page is the hub**:

- **Swipe up** → BLUETOOTH PAIRING, then **ELM327 PAIRING** or **TPMS PAIRING**; on slaves the first option is **GAUGE MASTER PAIRING**, opening FIND MASTER
- **Swipe down** → Settings
- **OTA button** → OTA update mode (app pairing, see [FLASH.en.md](FLASH.en.md#app-ota-upgrade-dual-slot-rollback))
- Swipe left/right → back to the gauge ring

**Hidden entries**:

- **Double-tap the logo page** → OBD protocol roller (**long-press 2 s** saves
  and reboots; protocol 0 = auto-detect, and detection retains that selection;
  select 0 once again if older firmware saved a nonzero protocol)
- **10 rapid taps on the version page** → showroom demo mode (sweep animation)

When data times out or BLE drops, gauge pages show a **NO SIGNAL** banner.

The Bluetooth-connect self-test sweep follows elapsed time, targeting a 16 ms refresh interval rather than advancing once per callback. Busy rendering skips obsolete animation positions instead of stretching the sweep; the master has a nominal 1.92-second duration and restores configured brightness plus live data on the ending tick. Slaves fill animation positions between sync packets, but flash and termination still follow received master slots. This does not increase actual OBD sampling rate or guarantee 60 FPS on the panel.

RPM, speed, boost and throttle receive priority polling when demanded by the current page. Built-in numeric, speed and needle pages display these inputs and load from the latest sample without an extra display ramp. Slower inputs such as temperature retain smoothing. Actual update speed remains limited by the adapter, ECU and profile minimum sampling periods.

**Deleting a saved device on the BLE SCAN page**: tapping the ✕ next to the
saved device in the list immediately disconnects the current ELM327 link and
clears the saved name / MAC — **it does not auto-reconnect**; you must scan
and select again. On a slave (SLAVE), the same action on the **FIND MASTER**
page unbinds the master and starts a fresh scan.

## Bluetooth tire sensor pairing

Swipe up from the version page, select **TPMS PAIRING**, then assign a sensor to each wheel individually:

| Wheel label | Vehicle wheel |
|-------------|---------------|
| FRONT LEFT | Front left |
| FRONT RIGHT | Front right |
| REAR LEFT | Rear left |
| REAR RIGHT | Rear right |

1. Tap a wheel, then **PAIR SENSOR**; wake that wheel's sensor and wait for scan results.
2. The list shows only six-digit IDs passing the TireKing FBB0 prefix and CRC checks, such as `18010F`. Ordinary Bluetooth devices, advertised names, MAC addresses and RSSI are not shown. IDs come from the sensor ID field, not a renamed Bluetooth device or a MAC-derived guess; leading zeros are retained. Each scan lasts up to 20 seconds and lists up to 20 distinct valid IDs.
3. Check the sensor label, then **tap its ID to save it to the selected wheel**, without a separate confirmation. IDs persist in NVS across reboots; one ID cannot be assigned to another wheel simultaneously. The wheel list shows saved IDs, and details say `ID SAVED - waiting for broadcasts`: registration alone does not mean online reception.
4. Use **RE-PAIR** to select another sensor or **DELETE PAIRING** to remove it. Restarting a scan, backing out or a save failure preserves the original binding; deletion affects only the selected wheel.

Use **BACK** or swipe left/right to return one level; **SCAN AGAIN** restarts discovery. Entering TPMS pairing yields ELM327 automatic reconnect scanning, which resumes on exit; an established OBD connection and serial polling continue running.

Legacy MAC-only records remain compatible but cannot reliably be converted to sensor IDs: the wheel shows **RE-PAIR**, with **LEGACY RECORD** in details. Rescan and save, or delete; IDs are never guessed. With ID-based binding, a changed MAC does not change the identity of a sensor broadcasting the same ID.

Only the TireKing FBB0 broadcast main path is implemented: scans directly decode pressure, temperature, voltage and state without a GATT connection or security bonding. Saved IDs now support background reception and gauge displays, but not tire-pressure alarms. Other TPMS protocols are not generically supported. The protocol comes from APK static analysis and synthetic packet tests; matching the physical label to broadcasts, sleep/wake behavior and broadcast periods still require this device to be tested. CRC checks integrity, not authenticity.

### Four-wheel TPMS overview

The default carousel adds a four-wheel overview between **CHART → TPMS → version page**. `FL` / `FR` / `RL` / `RR` mean front left, front right, rear left and rear right. The overview matches INFO / TEMP with the themed background, thin gray dividers, blue wheel labels, large white pressure values and gray secondary information. `TIRE PRESSURE` uses a smaller gray title with clearance from the circular bezel, and the `Tap tire for all data` hint is removed. The overview omits six-digit IDs, showing only wheel position, pressure (**bar, two decimal places**, such as `2.10` or `2.25`), temperature (`C`) and estimated battery (`%`). Unbound wheels, missing samples, or samples at least 120 seconds old show `--` rather than presenting cached measurements as live.

Tap a wheel for its ID, reception status, raw pressure, temperature, voltage, state code, versions, RSSI, address type, MAC and last-reception age; expired data is marked `STALE` / `cached`. Swipe horizontally or tap **BACK** to return to the overview; horizontal swipes on the overview continue the carousel. Select **BOOT PAGE → TPMS** in Settings to save the TPMS overview as the default built-in boot page. Custom themes still take priority with their own default page, and the existing startup pairing flow for unpaired devices is unchanged.

After any wheel ID is saved, background scanning starts when Bluetooth is available and ELM327 connection work does not need priority. Manual pairing and ELM327 reconnection take priority, without interrupting established OBD polling. The 120-second threshold is a display-validity limit, not the sensor's actual reporting period. Sleeping sensors may need waking before updates arrive. Background and pairing scans share one advertisement parser without making GATT connections.

Battery is a TireKing voltage-bin estimate, not a directly broadcast remaining capacity: convert voltage to float32, promote to double, then compare strictly against `>2.8` / `>2.6` / `>2.4` / `>2.2`, yielding `100%` / `75%` / `50%` / `25%`, otherwise `0%`; float32 boundaries affect decimal thresholds. Full voltage remains visible in details. Runtime custom themes can use `tpms.*` sources; see [THEMES.en.md](THEMES.en.md). The frontend editor is unchanged.

### Scan diagnostics

Record serial output with `idf.py -p <port> monitor`, open a wheel's **PAIR SENSOR** or **RE-PAIR** scan, and search for `tpms_ble`. Logs include scan requests, completion/failure/cancellation and valid sensors' IDs, MACs, address types (`type`), RSSI, `pressure_pa`, `temp_c`, `voltage_mv`, state and versions, plus complete raw advertisement `ADV` and scan response `RSP`. Payloads use contiguous uppercase hexadecimal; empty payloads use `-`. This example is synthetic, not a hardware capture:

```text
tpms_ble: sensor=123456 device=AA:BB:CC:DD:EE:FF type=0 rssi=-55 pressure_pa=251520 temp_c=25 voltage_mv=3000 state=0 version=1 ble_ver=10 ADV[26]=0201060303B0FB12FF0000B250500001F000630A000000563412 RSP[0]=-
```

Each valid ID is logged on first sight, once more for its first nonempty scan response, then at most once every 5 seconds; rescanning resets the throttle. These are diagnostic samples, not complete captures. Ordinary devices and invalid packets do not enter the candidate table or emit device logs. Only 20 valid IDs can fill the table; this produces one warning per scan, while existing IDs can still refresh. Invalid ADV / RSP lengths are ignored; independent records are never concatenated to force parsing.

If the target ID is absent, record a complete scan cycle and retry after waking the sensor. Absence does not prove device failure: its protocol may be unsupported or it may not yet be broadcasting. An actual `18010F` must come from `raw[23:26] = 0F 01 18` with a valid CRC; coincidental matching bytes are not identification. Logs contain device addresses, so consider privacy before sharing.

## Settings page

| Setting | Description |
|---------|-------------|
| BOOT PAGE | Landing page after boot (TEMP / INFO / CHART / NEEDLE / GEAR / RPM / SPEED / TPMS)|
| VEHICLE → OPEN | Open the vehicle page to review installed profiles and activate one; reboot as prompted for the change to take effect |
| THEME | Select a registered theme; the device restarts to apply its appearance |
| Brightness | 10–100% |
| RACECHRONO | ON = full BLE services (RaceChrono + pairing); OFF = minimal mode (a master keeps only the pairing service and advertises it; other roles run no BLE service and do not advertise) to save memory |
| Swipe down → MULTI-GAUGE | Triple-gauge role and boot-animation settings (see below)|

Select an installed profile on the vehicle page; install or import vehicle profiles through the web UI or App. THEME selects a theme registered in the firmware. The gauge has no SD-card management entry; firmware uses the card in the background for vehicle profiles, themes and media resources.
**Swipe down for MULTI-GAUGE and swipe left / right to return still work**.

Everything is persisted in NVS. Trip / mileage statistics accumulate per
power cycle only and reset on reboot.

## Vehicle, theme and SD storage

Install or import vehicle profiles through the web UI or App, then select and activate an installed profile from the gauge's VEHICLE page. Vehicle changes generally take effect after the device reboots; the web UI or App reports installation, activation and reboot status. If vehicle verification fails or the device enters safe mode, follow the recovery status shown in the web UI / App to restore Generic OBD2, or contact the maintainer. Select a theme from the Settings THEME roller; install theme packages through the web UI or App.

Firmware uses the SD card in the background for vehicle profiles, themes and media resources such as images, fonts and video, and handles imports and migration automatically. The gauge does not provide SD browsing, import or eject controls. Do not remove the card while the device is running or edit its files manually. When SD resources are unavailable, firmware follows its fallback behavior and uses the built-in UI and available profile.

Click **Export theme ZIP** in the web theme editor to download the theme and its resources in one ZIP. Insert an SD card,
connect the device on the web flasher page, and choose **Upload local files** in the **Theme** section to install, select, and restart automatically;
**From community** installs a community theme. Legacy BIN files are supported, optionally selected with companion JSON and `.obv` files;
a BIN alone cannot restore missing rules or videos. First update to firmware supporting this feature.
Theme upload uses ESP32-S3 native USB; users do not manage SD files on the gauge. After a complete firmware flash, reconnect before installing themes or animations.

## Triple-gauge cluster (ESP-NOW)

An ELM327 adapter accepts a single BLE client, so multiple gauges work as
**one master plus slaves**:

- **Master (MASTER)**: keeps the BLE + ELM327 connection and reads OBD,
  broadcasting parsed data over ESP-NOW
- **Slave (SLAVE)**: receives the broadcast and renders, zero extra OBD load
- **Standalone (default)**: ESP-NOW disabled

All gauges run the same firmware; the role is picked under
**Settings → swipe down → MULTI-GAUGE** (takes effect after reboot).

**Pairing** (real BLE pairing, not blind broadcast binding):

1. Set the master to MASTER; it advertises `SkyGauge-XXYY` (device suffix, so
   multiple cars at a track day don't interfere)
2. Set slaves to SLAVE; an unpaired slave boots straight into the
   **FIND MASTER** scan page
3. Pick the master to follow from the list — done (the master's MAC is
   remembered and auto-joined on every later boot)

The cluster also supports a **linked RPM warning**: past the threshold the
three gauges light up in position order and then all flash (toggleable as
Linked mode on the RPM warning page), plus the synchronized "RACE / AS / ONE"
boot animation.

The master merges `obd.*`, `veh.*` and `tpms.*` subscriptions from each online slave's current page. When a slave switches to boost, oil temperature or another signal page, the master adds the required OBD signals on the next poll round; vehicle extensions continue broadcasting all valid values. Slaves never open their own OBD connection or create extra requests. `sys.*` (role, page, uptime, etc.) retains each gauge's local meaning rather than copying master system state.

**Pair TPMS on the master only.** An updated slave automatically subscribes to all four wheels on the built-in TPMS page. Custom-theme labels, pointers, videos or rules referencing any `tpms.<wheel>.*` field subscribe to that wheel, without requiring the master to display TPMS. Every 500 ms the master forwards complete snapshots of subscribed wheels; pressure, temperature, battery and diagnostics share that snapshot. Slaves do not need duplicate bindings or background scans of their old local bindings. Those local bindings are preserved, but slave displays use only master data, never stale local values.

Navigation replaces subscriptions; when a slave leaves or stops reporting, the master withdraws its subscriptions after the 2-second demand window. After 2 seconds without a TPMS extension frame, a slave clears its master TPMS view and shows `--`. The independent 120-second sensor expiry still applies; repeated forwarding does not reset sample age. Rebinding the master or stopping ESP-NOW also clears old TPMS data. TPMS forwarding requires updated firmware on both the master and receiving slave. Other old slaves in the group do not block TPMS reception on updated slaves, but an old master cannot forward TPMS.

TEST on the RPM warning page uses the master-generated ramp; pressing TEST again on either
master or slave restarts it. A request shortens the master's idle broadcast wait, but send batches
still wait at least 20 ms between them; the master entering OTA prevents new tests.

**Mixed-firmware compatibility**: the master only broadcasts v6 data frames
when every online slave supports v6; if even one slave is still on old
firmware it falls back to v5, so the three gauges can be upgraded one at a
time instead of all at once. Gear data only shows once every gauge is on new
firmware; vehicle-specific fields (`veh.*`) only show on a slave when it has
the same vehicle (matching id and revision) active as the master. The linked
RPM warning's control packets always stay v5 and are unaffected.

## Boot animation

Three modes, picked on the **Settings → swipe down to MULTI-GAUGE → INTRO**
roller (stored as NVS `intro_enable`):

| Mode | Effect |
|------|--------|
| OFF | Skip the animation, go straight to the gauges |
| RACE | Built-in "RACE AS ONE" animation (cluster-synchronized)|
| VIDEO | Plays shared SD boot media, independently of the selected theme |

VIDEO mode reads the shared SD directory:

1. `/OBD/BOOT/boot.obv`, then `/OBD/BOOT/boot.jpg`, then legacy `bootmedia.raw`.

Create `boot.obv` with `python3 tools/make_video.py boot INPUT -o boot.obv`.
The theme editor's Media panel manages images and videos used by pages and event rules; boot animation
is not part of a theme. The existing web uploader still writes to the shared `/OBD/BOOT/` directory.
After connecting in the web flasher, choose **From community** or **Upload local files** in the **Boot animation** section.
Local input supports common video formats (including MP4, MOV, WebM, MKV, AVI, WMV and GIF), OBV2 `.obv`, an encoded ZIP, a manifest and media BIN selected together, or an old raw image with a 4 KiB manifest header.
H.264 MP4 is decoded locally with WebCodecs; other videos load an on-demand ~31 MB single-thread FFmpeg WebAssembly core and decode locally to lossless PNG before the same Q90 baseline JPEG 4:2:0 encoding in a 360×360 canvas (aspect-preserving, no crop). Source timestamps and variable-frame intervals are retained, then the OBV2 is uploaded over USB to SD. Only inputs decodable by this FFmpeg WebAssembly build are supported; encrypted or corrupt files and browser memory exhaustion produce an error. Each frame is limited to 128 KiB, the source rate to 60 fps, and the total animation to 256 MiB. The firmware prebuffers about one second and presents frames by timestamp. Resolution is limited to the display's native 360×360. File, buffer, or frame-budget overflow is reported without silently reducing quality or frame rate. For local USB import, the source stays local; `.obv` files can also be uploaded directly. Frames larger than 48 KiB require firmware with the 128 KiB frame limit; existing smaller-frame animations remain compatible.
The boot-animation community **Upload** action uses the same 360×360 / Q90 encoding, retains source frame rate by default and supports crop, zoom, trim and an explicit frame rate. Community publishing uploads the source video as its preview.
If playback temporarily falls behind, firmware skips expired intermediate frames to catch up on the timeline, retains the first and last frames and no longer ends early after consecutive late frames.
After validation, the page restarts the device into application mode
and uploads to SD over ESP32-S3 native USB Serial/JTAG. A successful upload becomes the active animation
and deletes other old-format shared boot animations. Transfer interruption, SD write-sync failure, or validation failure preserves the current animation; old-file cleanup failure is reported as an error.
External CH340 / CP210x serial adapters can flash firmware but do not support this animation upload.
An inserted, mounted SD card with enough free space is required. The phone app can still upload over
WiFi OTA. With no card or no usable files, the custom animation is skipped. Legacy raw animation duration is derived from its actual frame count and FPS, ignoring potentially stale manifest duration metadata.
When there is no theme page and no saved OBD adapter or master, the scan page is created during boot animation and scanning starts only after the page is shown.
Use a 360×360, 1:1 video canvas and allow for
circular corner clipping. Old Flash animation is never played directly. Uploads use a verified
SD temporary file before installation; FAT file replacement has a short window and is not
power-loss-atomic.
If replacement fails, the gauge attempts to restore the old file. If restoration also fails, the old file remains in the same directory with an `.obd-bak` suffix; check the SD card and restore it manually. A later replacement will not overwrite an existing backup.

## Updating from the phone app

The companion app (`android_app/app-debug.apk`) supports the transfers below. Before Wi-Fi operations,
enter OTA mode (**version page → OTA button**) and join the gauge hotspot:

- **Firmware**: the phone joins the gauge's WiFi hotspot (`OBD-Gauge-OTA-XXXX`,
  password `88888888`) for fast transfer, written to the inactive OTA slot
- **Boot animation**: trim the video in the app, encode, upload
- **Theme package**: pushes a runtime theme to the SD library and saves its selection
  (see [THEMES.en.md](THEMES.en.md))

Theme and boot-animation installation, vehicle installation/activation and theme deletion try Wi-Fi first. Android automatically falls back to native USB on Wi-Fi failure; iOS supports Wi-Fi only. Android checks USB management capability first; new firmware can stop OTA reception and hand over automatically, while older firmware requires exiting OTA mode. If OTA is starting or stopping, the operation asks you to retry later; wait for it to finish before retrying or swiping to exit. Failed theme/animation installation preserves current files; firmware automatically reboots on success. Vehicle installation and activation remain separate actions. If deletion or activation succeeds but reboot is unconfirmed, the App prompts a manual restart without replaying the operation.

Wi-Fi OTA closes automatically after 10 minutes without activity. Device discovery, device-info requests and authenticated requests refresh the timer. Each re-entry uses a new timeout session. OTA reports a startup failure if its timeout-monitor task cannot be created.

The device exposes hardware and build info via `GET /ota/info`; the app
validates compatibility before flashing firmware and refuses
mismatched hardware. Protocol details in [APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).

**Firmware verification and rollback**: after a new firmware boots, a
background task waits 15 seconds and only then calls
`esp_ota_mark_app_valid_cancel_rollback()` to mark the current slot valid. If
the new firmware reboots or crashes within those 15 seconds (never marking
itself valid), the bootloader automatically boots the previous slot on the
next startup — an automatic rollback with no manual steps. Leaving OTA mode
(swiping away) also triggers a reboot by itself.

## Optional external sensors

| Sensor | Wiring | Notes |
|--------|--------|-------|
| ADS1115 oil pressure | I2C, V1 Waveshare board only | Not started when the active vehicle profile reads oil pressure over OBD — the OBD value wins; oil-pressure alarms throttled to one per 30 s |

## Clear installed vehicle profiles

On the web USB flasher, choose “Clear installed vehicle profiles” in the vehicle section, review the scope and check the confirmation box. This clears all installed profiles and SD mirrors, then restarts with builtin `OBD2 Generic`. Insert the SD card and upgrade to firmware supporting `vehicle.clear`. A failed clear is not reported as successful; a failed restart can be retried separately. Themes, animations and vehicle templates in CATALOG / IMPORT / EXPORT are retained.

## Video decode ahead and double buffering

Playback retains sequential SD read-ahead, a PSRAM compressed-frame ring and a persistent JPEG decoder. Fullscreen mode decodes the next frame's first MCU strip or whole image ahead of time and starts panel transfer at its source deadline. Two internal DMA strips alternate by parity; submission waits for the previous transfer and never overwrites an in-flight buffer. Composite themes retain front/back frame buffers and LVGL handoff. OBV2 also decodes ahead, retaining compressed frames and shifting the timeline on pause; expired frames follow existing skip rules. Decode timing excludes deadline sleep. Actual FPS still requires device measurement; host tests do not establish a fixed hardware frame rate.

On entering OTA, the gauge stops multi-gauge communication and finishes current work. It may briefly show `WiFi: Starting...` before the hotspot is ready.
