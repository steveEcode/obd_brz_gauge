# App Integration Protocol

English | [简体中文](APP_PROTOCOL.md)

The WiFi HTTP API the device exposes in OTA mode (`/ota/*` for the App), the web USB
management protocol, plus the ESP-NOW frame format between triple-gauge units.
For the user-facing flashing and update flow see [FLASH.en.md](FLASH.en.md).

## What the device exposes in OTA mode

After entering OTA mode via **version page → OTA button**:

- A WiFi SoftAP: SSID `OBD-Gauge-OTA-XXXX` (last two MAC bytes), password
  `88888888`, IP `192.168.4.1`, port 80, channel 1, at most 1 client at a time
- A fresh random 16-hex-char token per session, used for HTTP authentication
- Opening `http://192.168.4.1/` in a browser is the device page (vehicles,
  theme sidecar, SD / `/config` files, coredump, import log), driven by the
  `/api/v1` API below. The device page only exists in OTA mode; `GET /`
  without `text/html` (phone connectivity probes) returns 204
- The server auto-stops after 10 minutes with no upload, device-page or
  `/api/v1` request

Entering OTA mode releases Bluetooth entirely (video and theme resources are
also released to the SoftAP); the device offers no BLE OTA / manifest
service. Leaving OTA mode reboots the device.

## Device manifest (`GET /ota/info`)

Used by the app for hardware compatibility checks before flashing:

- No authentication; available in OTA mode
- Payload: UTF-8 JSON (512-byte cap; only fields the app actually uses)

```json
{
  "device": {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": { "w": 360, "h": 360, "bpp": 16 },
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1
  },
  "firmware": {
    "build_tag": "main-124-abcdef123456",
    "branch": "main",
    "count": 124
  }
}
```

`build_tag` is `<branch>-<commit-count>-<short-hash>`, injected from git by
`main/CMakeLists.txt` at build time. The app compares `firmware.count`
against `latest.json` to decide whether an update exists. If any hardware
field differs from the selected release, the app must refuse to flash.

Web and App read `PUBLIC_BASE_URL/firmware/esp32/latest.json` (directory configurable through `VITE_RELEASE_BASE_URL`). Both publisher `device/files` and legacy `hardware/firmware` manifests are supported; downloads and caches verify SHA256 and byte count. Complete web flashing verifies all four files before writing; the App rereads current device hardware before uploading. The release manifest references the source commit; subsequent artifact commits do not change the device build tag.

## WiFi OTA HTTP API

Once the app has joined the device SoftAP, transfers go over HTTP (every path
answers OPTIONS preflight; CORS headers cover `/ota/*` and the vehicle-community install allowlist below). First
call the unauthenticated `GET /ota/discover` to get this session's token:

```json
{"ssid":"OBD-Gauge-OTA-ABCD","password":"88888888","ip":"192.168.4.1","token":"a1b2c3d4e5f6a7b8","port":80}
```

**Common header**: `X-OTA-Token: <16-hex token returned by GET /ota/discover>` (`/ota/discover` and `/ota/info` need no token)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/ota/discover` | GET | LAN discovery; returns SoftAP info and the token |
| `/ota/info` | GET | Device manifest JSON (format above)|
| `/ota/status` | GET | `{"state":"ready\|receiving\|done\|error","received":N,"expected":M}` |
| `/ota/firmware` | POST | Chunked firmware upload |
| `/ota/bootmedia/prepare` | POST | Boot-animation preflight (headers `X-OTA-Size`, `X-OTA-Manifest-Size`), returns `{"ok":true,"storage":"sd","free":N,"psramFree":N,"largestBlock":N}`, no erase |
| `/ota/bootmedia` | POST | Chunked boot-animation upload |
| `/ota/bootmedia/upload` | POST | Legacy browser serial-upload alias; sizes reuse the latest successful prepare in this session |
| `/ota/theme/prepare` | POST | Theme preflight (header `X-OTA-Size`), returns `{"ok":true,"storage":"sd","partitionSize":N,"psramFree":N,"largestBlock":N}`, no erase |
| `/ota/theme` | POST | Chunked theme upload |
| `/ota/theme/erase` | POST | Delete the selected theme's `/OBD/THEMES/<key>/` directory and clear selection; returns `{"ok":true,"deleted":true|false}` and uses the built-in theme on next boot. Requires a mounted SD card; other themes and shared boot animations are untouched |
| `/ota/vehicles/clear` | POST | Delete all installed vehicle JSON/PID profiles and SD mirrors, returning `{"ok":true,"cleared":true,"pending":"obd2.generic"}`; requires a mounted SD card and retains the builtin generic profile, templates, themes and animations |
| `/ota/coredump` | GET | Download the stored coredump image (`application/octet-stream`, `coredump.bin`); returns 204 No Content with an empty body when there is no coredump |
| `/ota/coredump/erase` | POST | Erase the coredump partition, returns `{"ok":true}` |

**Chunked uploads** (`/ota/firmware`, `/ota/theme`, `/ota/bootmedia`): every
request carries `X-OTA-SHA256` (64-char hex of the whole payload),
`X-OTA-Size` (total payload bytes), `X-Offset` (this chunk's starting offset,
default 0), `X-Last` (`1` = final chunk); `/ota/bootmedia` also carries
`X-OTA-Manifest-Size`. Body is this chunk's data (decoded as base64 text when
`Content-Type` contains `base64`).
The legacy browser serial uploader uses `/ota/bootmedia/upload` and sends lengths only during prepare;
the device reuses the latest successful `/ota/bootmedia/prepare` sizes in this session. Chunking,
SHA verification and SD installation are otherwise identical.
`X-OTA-Manifest-Size: 0` denotes a complete OBV/OBV2 file; the compatibility path may also reuse this value from prepare.

For themes, boot animations, vehicle installation/activation and theme deletion, the App discovers a fresh Wi-Fi session for every operation and tries Wi-Fi first. Android releases the network binding and falls back to native USB on failure; iOS uses Wi-Fi only. When USB `hello` reports `ota_active: true`, send `ota.stop` to stop HTTP and clean up the incomplete upload before using USB. Clients must verify a JSON object, `ok === true` and the exact intermediate `nextOffset`; protocol errors stop immediately without network retries. Firmware automatically reboots after theme/animation installation, so the App sends no second reboot. Theme deletion and vehicle activation request reboot separately after business success; reboot failure prompts a manual restart without replaying the operation.

- `X-Offset: 0` starts a new upload; intermediate chunks get `{"ok":true,"nextOffset":N}`
- Offset, total size or SHA that don't match the upload in progress: `unexpected chunk offset`
- A network receive failure (`receive failed` / `no memory`) rolls back to the chunk's starting offset; resend the same chunk. An SD write, sync, rollback-seek or digest-processing failure ends the session; investigate the error and restart at offset 0
- Once everything is received it is verified; failure returns `size mismatch` / `sha mismatch`, requiring a retransmit from offset 0
- Firmware is verified before writing the inactive OTA slot. Themes/animation stream into
  SD temporary files during reception and install after verification. The final reply follows
  installation: `{"ok":true,"message":"<类型> received, installing"}`, rebooting after about 1 s.
- Installation failure returns `install failed`; retry from offset 0. SD transfer does not replace old files. Single-file replacement first renames the old file to `<destination>.obd-bak` and attempts to restore it if the new rename fails. If restoration also fails, the backup is retained and never overwritten; check the SD card and restore it manually. FAT metadata operations are not guaranteed power-loss-atomic.

Upload bodies:

- `/ota/firmware`: firmware binary within the 3 MB OTA slot.
- `/ota/theme`: the same `OBDT` stream as USB command `5` (client-unpacked ZIP containing `theme.bin`, `project.json` and resources), limited by SD space and format. After staged validation, a recovery journal protects replacement and the NVS selection. Upload or validation failure preserves the current theme without deleting it first. Legacy clients may still upload bare `theme.bin`; the file header selects the importer, and an invalid `OBDT` bundle never falls back to legacy import.
- `/ota/bootmedia`: `[manifest][bin]`, total length in `X-OTA-Size`, manifest ≤4 KB.
  SD `/OBD/BOOT/bootmedia.raw` pads the manifest to 4 KB with `0xFF`; SHA excludes padding.
  A manifest length of `0` denotes complete OBV/OBV2, staged as `boot.obv.part`, synced and validated before committing to `boot.obv`. Other old formats are cleaned up after commit; cleanup failures log warnings, preserve the new file and return installation success.

Upload total, manifest-size and offset headers are decimal unsigned 32-bit integers. Explicit empty values, signs, non-digit characters, overflow and oversized values return 400. An absent `X-Offset` defaults to 0; an absent `/api/v1` `X-OTA-Size` defaults to the request body length.

**Errors**: `/ota/*` returns HTTP 400 + `{"error":"<原因>"}`, including `unauthorized`,
`missing headers`, `invalid headers`, `SD card required`, `not enough SD space`, `install failed`.

**Theme library busy**: while selection runs, `/ota/theme/prepare`, `/ota/theme/erase` and
first chunks of `/ota/theme` / `/ota/bootmedia` return `theme library busy`.

**Media requires SD**: preflight and upload require mounted SD with free space ≥ physical
file size (including animation padding) + 64 KB. `storage` is always `"sd"`. Legacy fields
`partitionSize` (theme) and `free` (animation) use one preflight snapshot of SD free space minus 64 KB, clamped to zero and capped at `UINT32_MAX`. They do not reserve space or represent Flash capacity. `psramFree` / `largestBlock` are diagnostics; the entire
media package need not fit in PSRAM. Animation uploads do not change themes; other formats in the shared directory are cleaned up only after the new animation commits.

## USB Web Management, Theme and Boot-Animation

All web operations on the gauge use ESP32-S3 native USB: ROM flashing for firmware, the media transactions below for themes/boot animations, and management RPC for vehicle installation, activation, theme deletion and reboot. No device address, token, hotspot or OTA mode is needed; exit gauge OTA mode before operating. Community browsing, login, downloads and publishing remain network operations. The App WiFi API remains compatible.

Management RPC uses command `6` BEGIN_RPC: its 40-byte payload is JSON length `u32` (1–16448), reserved `u32=0`, and 32-byte JSON SHA-256; DATA, END and ABORT are reused. Requests are received in memory without holding the SD reception lock; business functions manage their own storage locks. END appends UTF-8 JSON after status `u8` and received length `u32`, with CRC covering the entire payload; ordinary media replies are five bytes, with optional error text for OBV2 validation failure. A successfully transported business failure has transport status `0` and JSON `{"ok":false,"error":"..."}`, optionally with `issues`. A `theme.erase` failure caused by boot theme preparation, display-lock or storage-lock contention includes `retryable:true`; missing SD and other deletion errors are not retryable. New firmware failures include `stage` (`prepare` or `delete`) and `code` (the `esp_err_to_name` error code); missing SD explicitly reports `SD card is not mounted`. The page starts a new RPC for explicitly retryable business failures every 500 ms with a two-minute cleanup waiting budget and a visible startup-loading message; legacy replies without `retryable` are also supported when the exact error is `theme delete failed` or `display busy`. Persistent generic legacy failures stop at the deadline and prompt a firmware upgrade for diagnostics; explicit `retryable:false` or other business errors stop immediately. Communication failures are not retried as new deletion operations. Transport retries of the same END replay cached JSON without executing the operation again. Older firmware rejects command `6` with status `1`, prompting an upgrade.

| Request JSON | Success reply | Effect |
|----------|----------|------|
| `{"op":"hello"}` | `{"ok":true,"usb_management":1,"ota_active":false}` | Confirm protocol and OTA state without changes |
| `{"op":"ota.stop"}` | `{"ok":true}` | Stop HTTP OTA and clean up the incomplete upload for subsequent USB takeover; no reboot. Concurrent start/stop returns `ok:false`, `retryable:true` without waiting on the LVGL caller |
| `{"op":"vehicle.install","profile":{...}}` | `{"ok":true,"id":"...","warnings":0,"issues":[]}` | Validate and install; no activation or reboot |
| `{"op":"vehicle.activate","id":"..."}` | `{"ok":true,"pending":"..."}` | Activate on next boot; supports `obd2.generic` |
| `{"op":"vehicle.clear","confirm":true}` | `{"ok":true,"cleared":true,"pending":"obd2.generic"}` | Explicit confirmation required. Clear installed vehicle JSON and SD `/OBD/CFG/vehicles` mirrors, including the current profile; mounted SD is required. Schedule builtin generic for the next boot and retain this boot's compiled pointer; no automatic reboot. Failure returns `ok:false` and may leave a partial deletion. CATALOG / IMPORT / EXPORT templates, themes and animations are retained |
| `{"op":"theme.erase"}` | `{"ok":true,"deleted":true}` | Release theme pages, delete current SD theme directory and selection; `false` means no package was found |
| `{"op":"reboot"}` | `{"ok":true}` | Acknowledge before delayed restart |

After “Delete current theme” is clicked, the web client separately requests reboot to restore the built-in default. For local or community installation, the page validates the new bundle, uses `theme.erase` to detect and clear the current theme (continuing directly if none exists), then uploads and enables the replacement after success is confirmed, without an intermediate reboot. Other themes and the shared boot animation remain. Web operations require Chrome/Edge Web Serial over HTTPS or localhost. The application protocol requires ESP32-S3 native USB; CH340/CP210x are for firmware flashing only.

The USB flasher keeps the existing “connect device → select animation → upload” workflow. After
selection, the page downloads `[manifest][bin]`, resets the device from ROM download mode into the
application, and sends framed data over **ESP32-S3 native USB Serial/JTAG**. Firmware reuses
`upload_sink` for SHA-256 verification and stages/installs the file on SD at
`/OBD/BOOT/bootmedia.raw`, then reboots. A mounted SD card with enough free space is required.
This serial protocol does not support external CH340 or CP210x USB-UART bridges; those remain
usable for firmware flashing but not animation upload. The WiFi `/ota/bootmedia` path remains
compatible.

Frames are little-endian: `OBDS`, version `1`, command, sequence `u16`, payload length `u32`,
payload CRC-32 `u32`, payload. Command `1` BEGIN carries total size `u32`, manifest size `u32`, and
32-byte package SHA-256; command `2` DATA starts with offset `u32` followed by up to 4096 bytes;
command `3` END verifies and installs; command `4` ABORT removes the temporary file. Replies use
the command value plus `0x80`; payload is status `u8` and received length `u32`. The client waits
for each sequence reply and retries the identical frame on timeout; firmware idempotently replays
the response for its latest frame. Status 0 means success; nonzero means rejection or verification
failure.

The web page unpacks theme ZIPs (STORE / DEFLATE; firmware does not decompress ZIP).
Command `5` BEGIN_THEME uses the same 40-byte BEGIN payload with manifest length `0` and SHA-256 over the whole theme stream;
DATA, END, and ABORT are reused. Older firmware rejects command `5` with status `1`, prompting a firmware upgrade.
Status `2` means invalid parameters, unavailable SD, or insufficient space; `6` means validation or installation failed;
`8` is a retryable frame CRC error; `9` means OTA or an SD storage job is busy. Idle uploads are cleaned up after 30 seconds.
Reply CRC covers the full five-byte payload after the received length has been filled in.

OBV2 boot animation uses command `7` BEGIN_OBV with the same 40-byte payload: total length is the complete `.obv` length (≤256 MiB),
manifest length must be `0`, and SHA-256 covers the whole file; DATA, END, and ABORT are reused. Data is staged at
`/OBD/BOOT/boot.obv.part`. END verifies SHA, calls `fflush` / `fsync`, then reopens the file to check header/index/timeline, every frame header, and the data CRC, and fully decodes the first second of frames
before committing `boot.obv`, deleting the old `bootmedia.raw` and `boot.jpg`, and rebooting.
Command `1` legacy-raw uploads likewise delete old `boot.obv` and `boot.jpg` only after the new file commits.
Both web USB upload formats keep the new file and remove the other old formats; transfer interruption, file-sync failure, or validation failure preserves the existing animation. SD sync failure returns status `6` and skips file validation. Old-file cleanup failure returns status `6` and retains the newly committed file. Status `10` indicates OBV2 validation failure,
appending UTF-8 error text after status `u8` and received length `u32`; reply CRC covers the complete payload. The client allows 120 seconds for END.
Older firmware returns status `1` for unsupported command `7`, prompting an upgrade. H.264 MP4 is converted locally in the browser to Q90 JPEG/OBV2;
ESP32 performs no transcoding. See [THEMES.en.md](THEMES.en.md) for format and playback budgets.

The USB receiver holds the storage job lock from BEGIN through verification and commit, so background
import scans cannot delete its open `.part` file. An unrecoverable DATA storage or digest error returns status `5`,
aborts the session and releases the lock; investigate the error and send a new BEGIN. Abort, receive timeout and failure close the temporary
file and release the lock. After committing a theme stream, the receiver releases storage before entering
the installer, preserving the LVGL → storage lock order. Media BEGIN commands (`1`, `5`, `7`) wait up to one second
for the storage lock. If BEGIN returns status `9`, the web uploader retries every 500 ms with a 30-second waiting budget
to tolerate boot-time theme reads and background SD jobs. A BUSY reply definitively rejects that BEGIN, so each busy retry
uses a new sequence to bypass the firmware's cached BUSY reply; transport retries after response timeouts or CRC errors
retain the original sequence. Once the budget expires, report storage busy without sending resource data; ensure OTA mode is off.

The little-endian theme stream is `OBDT`, version `u8=1`, reserved `u8=0`, file count `u16`, followed by each file's
name length `u16`, file length `u32`, ASCII filename, and contents. At most 128 files and 256 MiB are accepted.
`theme.bin` and `project.json` are required; other files must be `<key>.obv`. Duplicate names and path traversal are rejected.
The page transmits gaugeproj as `project.json`; firmware streams it into `sidecar.json`, checking the theme ID.
After writing and validating all files, firmware releases the old theme page, journals the directory backup, replaces the theme directory,
saves the NVS selection, removes the journal, acknowledges END, and restarts. Errors attempt rollback; boot recovers uncommitted journals
before loading themes. Resources stay under `/OBD/THEMES/<theme-key>/`, without writes to the media Flash partition.
At least `2 × total length + 1 MiB` free SD space is required for the received stream, extracted files, and reserve; the old theme stays until commit.

**Security**: the token is random per session; the SoftAP uses WPA2-PSK;
every `/ota/*` payload is SHA256-verified. Theme packaging is described in
[THEMES.en.md](THEMES.en.md).

## Device API (`/api/v1`)

This HTTP API remains for older web clients and compatibility. Current web management uses USB RPC; the App continues to use `/ota/*`.

The API used by the device page, sharing the SoftAP and token with `/ota/*`:
every actual API request needs `X-OTA-Token`. Only `POST /api/v1/vehicles`,
`POST /api/v1/vehicles/activate`, and `POST /api/v1/reboot` expose CORS to browsers
(including error responses). The existing global OPTIONS handler permits only
`POST, OPTIONS` and the `Content-Type, X-OTA-Token` headers, with no cross-origin
credentials. Other `/api/v1` endpoints do not expose CORS; authentication is unchanged.

The web vehicle community installs over USB and displays firmware validation results;
selection and reboot require separate confirmation. Installing never switches profiles automatically.
Legacy HTTP clients may still be blocked by mixed-content/local-network policies.
Firmware also processes files in SD `/OBD/IMPORT/` automatically on boot or card insertion.
Community profiles are not vehicle-certified. Verify compatibility, requests and decoding while parked;
a matching ID may replace an existing profile, so back it up first.

Success is
`{"ok":true,...}`; failure is always `{"ok":false,"error":"<code>","detail":"<description>"}` —
a missing or wrong token gives 401 `unauthorized`, an unknown path gives 404
`not_found`, a wrong method gives 405 `method_not_allowed`, and other causes
map to 400 `invalid_arg`, 404 `not_found`, 409 `invalid_state` /
`not_supported`, 413 `too_large`, 500 `no_memory` / `failed`.

| Endpoint | Method | Request | Response / errors |
|----------|--------|---------|--------------------|
| `/api/v1/info` | GET | – | `firmware`, `board`, `partitions`, `sd`, `config`, `vehicle`, `theme`, `coredump`, `uptime_s`, `heap`, `ssid` |
| `/api/v1/vehicles` | GET | – | `{ok,active,state,safe_mode,last_error,vehicles[{id,name,rev,builtin,active,pending,state}]}` |
| `/api/v1/vehicles` | POST | Body: vehicle profile JSON (≤16 KB) | installs without activating: `{ok,id,errors,warnings,issues}`; 400 `empty_body`, 413 `too_large`, 422 `invalid_profile` (with `issues`) |
| `/api/v1/vehicles/<id>` | GET | – | the stored profile JSON as-is; 404 `not_found` |
| `/api/v1/vehicles/activate` | POST | `{"id":"..."}` | `{"ok":true,"pending":"<id>","reboot_required":true}`; 400 `bad_id` |
| `/api/v1/vehicles/reset` | POST | – | same, `pending` is `obd2.generic`; also cancels a vehicle the first-boot migration still wanted to activate |
| `/api/v1/vehicles/rollback` | POST | – | same, `pending` is the previous vehicle |
| `/api/v1/theme/sidecar` | POST | chunked upload of a sidecar or a full `.gaugeproj.json` (≤32 MB) | `{ok,size,report}`; saving requires the SD card: 409 `sd_unavailable`, 507 `sd_full`; 422 `invalid_sidecar` |
| `/api/v1/theme/sidecar` | DELETE | `?theme=<id>` (≤64 chars, omitted = current theme) | `{"ok":true}`; 409 `sd_unavailable` without an SD card |
| `/api/v1/fs/put` | PUT / POST | `?path=`, chunked body | `{ok,path,size}`; 400 `bad_path`, 409 `sd_unavailable` / `config_unavailable`, 507 `sd_full` / `config_full` |
| `/api/v1/fs/list` | GET | `?path=` (default `/sdcard/OBD`) | `{ok,path,entries[{name,dir,size}],truncated}`, capped at 200 entries; 404 `not_found` |
| `/api/v1/logs` | GET | `?bytes=` (default 8192, max 32768) | tail of `IMPORT.LOG` (`text/plain`); 409 `sd_unavailable` without SD |
| `/api/v1/sd` | GET | – | `{ok,state,total,free,freq_khz}`, `state` is `absent\|mounted\|error\|ejected\|disabled` |
| `/api/v1/time` | POST | `{"epoch":<unix seconds>}` | `{ok,epoch}`; before 2020-09-13 or after year 2100 returns 400 `bad_request` |
| `/api/v1/reboot` | POST | – | `{"ok":true}`, reboots about 0.5 s later |

- Vehicle ids are at most 31 characters, from `A-Z a-z 0-9 . _ -` only;
  activate / reset / rollback all take effect on the next boot — the device
  does not reboot automatically
- `fs/put` / `fs/list`'s `path` must be under `/sdcard/OBD/...` or
  `/config/...` (a relative path resolves under `/sdcard/OBD`); `..`, `//`,
  `\` are rejected; a single `/config` file is ≤256 KB

`/api/v1/info` reports `partitions.resource_storage:"sd"`. Legacy writable media fields
`theme_size`, `bootmedia_size`, `bootmedia_free` are 0 because Flash media caches are retired,
not because the physical partitions have zero size. `bootmedia_video` reports existence of
selected-theme or common SD animation files, not validated playability.
Physical layout: [FLASH.en.md](FLASH.en.md).

**Chunked uploads** (`theme/sidecar`, `fs/put`): headers `X-OTA-Size` (total,
defaults to Content-Length), `X-Offset`, `X-Last`, optional `X-OTA-SHA256`
(when given it must be 64-char hex and is checked on the final chunk).

- `X-Offset: 0` starts a new upload (discarding any unfinished one); intermediate chunks get `{"ok":true,"nextOffset":N}`
- A non-contiguous offset returns 409 `bad_offset`, with `detail` giving the expected offset
- A chunk that fails to receive is rolled back to its start and can be resent as-is; a failed final-chunk verification returns 400 `sha_mismatch` / `size_mismatch`
- Only one `/api/v1` upload is in flight at a time; starting an `/ota/*` upload discards it

## Boot-animation editor rules (app side)

- 360×360 canvas, aspect locked to 1:1; a circular preview mask is
  recommended (the round screen clips the corners)
- Trim start/end on the phone before encoding and uploading
- An SD card is required, with room for the 4 KB manifest padding and 64 KB margin;
  the old media partition size is no longer a limit.
- The app may retain drafts; legacy upload replaces SD `/OBD/BOOT/bootmedia.raw`.
- VIDEO mode reads only the shared SD boot directory:
  `/OBD/BOOT/boot.obv` → `/OBD/BOOT/boot.jpg` → legacy `/OBD/BOOT/bootmedia.raw`.
  Boot animation media is independent of the selected theme. The legacy webpage / app upload
  path is unchanged and continues to write under `/OBD/BOOT/`.

## Triple-gauge ESP-NOW frames

The master and slave gauges talk over ESP-NOW broadcast on WiFi channel 1.
Every multi-byte field is little-endian, and every frame starts with magic
`0x4F42` plus a version byte. v5 is the legacy format, v6 is the current one;
both coexist, so old and new firmware can be mixed and upgraded one gauge at
a time.

| Frame | Direction | Length | Content |
|-------|-----------|--------|---------|
| v5 presence | slave → master | 4 B | magic, version `5`, position 1..3; every 500 ms |
| v6 HELLO | slave → master | 8 / 10 / 15 B (extended demand) | see table below; every 500 ms, sent together with v5 presence |
| v5 control | both ways | 6 B | magic, version `5`, cmd (`1` linked-test start / `2` sync RPM warning threshold), u16 arg |
| v5 DATA | master → slave | 47 B | legacy fixed structure, see below |
| v6 DATA | master → slave | 38 + 6n + 2 B (≤250) | see table below |
| v6 TPMS | master → slave | 142 B | type `3`; every 500 ms while subscribed |

The master broadcasts one snapshot every 100 ms (20 ms during the linked
test). It sends v6 only while at least one slave is online and every online
slave has sent a HELLO with caps bit0 set within the last 2 s; otherwise it
sends v5, logging `Broadcast format -> vN` on each switch. Old firmware
dispatches by length (4 / 6 / 47) and then requires version = 5, so it drops
v6 frames; no v6 length collides with those. Control frames are always v5, so
threshold sync and the linked test keep working across mixed firmware. Once a
slave is bound to a master MAC, it only accepts frames from that MAC.

**v6 DATA**:

| Off | Len | Field |
|-----|-----|-------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `1` (DATA) |
| 4 | 1 | flags: bit0 master `sys.obd_link == 2`, bit1 linked test in progress, bit2 master `rpm_warn_linked_en` |
| 5 | 1 | frag = (index << 4) \| count, count 1..15 |
| 6 | 1 | n_rec (≤35) |
| 7 | 1 | gear, int8: -1 = R, 0 = N, 1..8, 127 = invalid |
| 8 | 4 | seq (same across all fragments of one snapshot) |
| 12 | 4 | profile_hash |
| 16 | 4 | std_valid: bit i = num_id `0x0001+i` is valid at the master |
| 20 | 4 | veh_valid: bit i = num_id `0x0100+i` is valid at the master |
| 24 | 1 | sweep_step |
| 25 | 1 | intro_step |
| 26 | 12 | name, NUL padded |
| 38 | 6n | records `{u16 num_id, i32 value}`, internal units |
| end | 2 | CRC-16/CCITT-FALSE over all preceding bytes |

- Every fragment repeats the full header, so the slave applies each one on
  its own, with no reassembly needed
- Only valid values are sent; a slot whose valid bit is clear is invalidated on the slave
- num_id and internal units follow `contracts/signals.json`; `sys.*`
  (`0x0080`–`0x00BF`) are never transported and are ignored on receive;
  unknown ids are ignored; gear travels only in the gear byte
- `veh.*` ids follow the vehicle profile's declaration order, and are applied
  only when profile_hash matches the slave's own current vehicle;
  profile_hash = FNV-1a 32 (vehicle id string + `0x00` + `rev` as u16 LE)

**v6 HELLO**:

| Off | Len | Field |
|-----|-----|-------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `2` |
| 4 | 1 | position 1..3 |
| 5 | 1 | caps: bit0 = decodes v6 DATA; bit1 = appended local OBD demand; bit2 = extended source subscriptions and TPMS reception |
| 6 | 2 | reserved, 0 (receivers accept length ≥ 8) |
| 8 | 2 | local OBD demand mask when caps bit1 is set; bit i maps to `SIG_OBD_*` |
| 10 | 4 | `veh.*` demand mask when caps bit2 is set; bit i maps to vehicle extension slot i |
| 14 | 1 | TPMS wheel demand when caps bit2 is set; bit0..3 = FL / FR / RL / RR, upper four bits 0 |

Slaves advertise all shareable sources used by current-page bindings, value-driven videos and rules. Navigation replaces rather than accumulates page demand. The master unions slave masks within a 2-second demand window and requests required standard signals through the single serial OBD poller. `veh.*` keeps broadcasting all valid values with profile_hash checks without changing vehicle sampling policy. `sys.*` is neither subscribed nor forwarded (`sys.obd_link` still represents the master's OBD link via DATA flags). Caps bit2 requires bit1 and at least 15 B; missing extension fields are rejected. Undeclared demand in legacy HELLOs is zero. Old masters ignore appended fields; old slaves continue using 8 / 10 B HELLO but cannot receive TPMS extensions.

**v6 TPMS (type 3, 142 B)**: negotiated independently of DATA. While an extended HELLO has subscribed to wheels within the last 2 seconds, the master sends a complete snapshot of all subscribed wheels every 500 ms. Nothing is sent without subscriptions. Other old slaves forcing v5 DATA do not suppress this extension; old firmware ignores it. The master forwards local sensor data without needing to display TPMS or issuing additional ELM requests.

| Off | Len | Field |
|-----|-----|-------|
| 0 | 2 | magic `0x4F42` |
| 2 | 1 | version `6` |
| 3 | 1 | type `3` |
| 4 | 4 | seq, from the same master DATA snapshot cycle |
| 8 | 1 | wheel mask, bit0..3 = FL / FR / RL / RR; 1..15 |
| 9 | 3 | reserved, 0 |
| 12 | 128 | four 32 B wheel records, fixed FL / FR / RL / RR order |
| 140 | 2 | CRC-16/CCITT-FALSE over all preceding bytes |

Offsets within each 32 B wheel record:

| Off | Len | Field |
|-----|-----|-------|
| 0 | 1 | flags: bit0 bound, bit1 received; received requires bound |
| 1 | 3 | six-HEX sensor ID as u24 LE, including leading zeros |
| 4 | 4 | pressure_pa, u32 |
| 8 | 2 | temperature_c, i16 |
| 10 | 2 | voltage_mv, u16; receiver derives estimated battery from voltage |
| 12 | 1 | state |
| 13 | 1 | version |
| 14 | 1 | ble_version_code |
| 15 | 4 | RSSI, i32 |
| 19 | 6 | MAC, diagnostic only, not TPMS binding identity |
| 25 | 1 | addr_type |
| 26 | 4 | age_ms since the original sensor broadcast at the master, saturated u32 |
| 30 | 2 | reserved, 0 |

Unsubscribed or unbound wheel records are zero-filled. Bound but unreceived wheels carry only ID and flags; other bytes are zero. The slave maintenance worker applies data; the receive callback only queues it, and the worker rechecks the bound MAC and the queued packet's 2-second lifetime. Only increasing seq values are accepted, including u32 wrap. After 2 seconds without an applied snapshot, a restarted master's new sequence is allowed. Slaves advance age_ms from reception time; measurements become invalid at 120000 ms. Forwarding heartbeats do not reset sensor age. Two seconds without a new TPMS snapshot, rebinding or link shutdown clears the slave TPMS view without falling back to old local bindings.

**v5 DATA** (47 B, legacy units): offset 0 magic, 2 version, 3 flags (same
bit layout as v6), 4 seq u32, 8 rpm u16, 10 speed u8, 11 sweep_step, 12
intro_step, 13 coolant temp, 15 intake temp, 17 oil temp, 19 oil pressure, 21
boost, 23 **retired slot**, 25 load, 27 throttle, 29 battery voltage u32, 33
AFR, 35 name 12 B (13–33 are i16 except voltage). Offset 23 used to be brake
temperature, is now always written as -1000, is never read by the decoder,
and must **never be reused**.

A slave treats 2 s with no data from the master as a lost link, invalidating
all `obd.*` / `veh.*` values. v5 frames carry no gear or `veh.*`, so a slave
shows gear only once the master and every slave run the new firmware.
