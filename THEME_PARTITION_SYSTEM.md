# Theme Partition System

This branch implements a complete theme partition system that separates UI presentation from core firmware logic.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Core Firmware (ota_0/ota_1, 3MB each)              │
├─────────────────────────────────────────────────────┤
│  • OBD data acquisition (ELM327 BLE)                │
│  • BLE/WiFi services                                 │
│  • NVS configuration                                 │
│  • Vehicle profiles                                  │
│  • System pages (settings/OTA/bluetooth/wifi)       │
│  • Theme engine (loads themes from partition)       │
└─────────────────────────────────────────────────────┘
           ↓ reads
┌─────────────────────────────────────────────────────┐
│  Theme Data Partition (theme_0, 2MB)                │
├─────────────────────────────────────────────────────┤
│  • theme_manifest.json (metadata, colors, pages)    │
│  • assets/ (dial.png, ring.png - 360x360 images)    │
│  • layout.json (custom page element definitions)    │
└─────────────────────────────────────────────────────┘
```

## Key Features

1. **System Pages Preserved**: Settings, OTA, Bluetooth pairing remain in core firmware
2. **Boot Sequence Protected**: Sky Gauge logo, intro animation, boot video NEVER themed
3. **Theme Pages Replaceable**: Gauge displays loaded from theme partition
4. **Theme *binaries* OTA-able once installed**: after a device has this partition table, a
   theme package (the 2MB blob written to theme_0) can be sent over BLE, same as firmware
5. **Single Slot, Safe Fallback**: only `theme_0` exists — a missing/corrupt/unparseable theme
   falls back to the built-in default theme rather than bricking the UI (see
   `theme_load_default()`), so a second slot for A/B rollback isn't needed
6. **Color Palette**: 8 themed colors (bg, ring, arc_track, etc.)
7. **Assets**: Optional 360x360 dial/ring images (memory-mapped from Flash)
8. **Custom Layouts**: JSON-based page element definitions

## ⚠️ IMPORTANT: This branch's partition table change is NOT OTA-deployable

Adding `theme_0` and resizing `bootmedia` changes `partitions.csv`, which changes the
**partition table itself** — the layout ESP-IDF flashes to `0x8000` and every `esp_partition_*`
call trusts at runtime. Existing devices in the field are running the *old* partition table
(no theme_0, bootmedia at the old offset/size).

The existing BLE OTA path (`ota_update_ble.c`) only ever writes into an already-existing
`ota_0`/`ota_1` app slot — it has no mechanism to rewrite the partition table on a live device,
and doing that safely (relocating/resizing a data partition without erasing what's on it) is not
something `esp_ota_ops` supports at all.

**Consequence**: shipping this to existing users requires a one-time **USB reflash** (`idf.py
flash` / esptool, full image). Only *after* a device is on this new partition table can theme
*binaries* be pushed via BLE OTA going forward. Plan the rollout accordingly — e.g. bundle it
with a service visit, or clearly communicate to users that this specific update needs a cable.

## Changes

### Partition Table (partitions.csv)
The original `partitions.csv` left the `Offset` column blank and let `gen_esp32part.py`
auto-place and auto-align every partition (verified against `git show main:partitions.csv`).
This branch's table now specifies explicit offsets so theme_0 can be inserted at a known
location; those offsets were computed to match what the auto-placement would have produced, and
are re-verified with `gen_esp32part.py --verify` at build time. (An intermediate draft of this
table had a 64KB-alignment bug in ota_0's offset — caught and fixed before merging; the original
table on `main` never had this bug.)

- **ota_0**: 0x020000, 3MB (unchanged from auto-placement)
- **ota_1**: 0x320000, 3MB (unchanged from auto-placement)
- **theme_0**: 0x620000, 2MB (new — this is where `bootmedia` used to start)
- **bootmedia**: 0x820000, 7.875MB (was 9.875MB — actual usage today is ~312KB, so this has ample room)

### New Files
- `main/theme_engine/theme_interface.h` - Public API
- `main/theme_engine/theme_loader.c` - Theme engine implementation
- `tools/theme_packer/pack_theme.py` - Theme binary packer
- `themes/example_boost_oil/` - Example theme

### Modified Files
- `main/bsp_obd_dsp/nvs_storage.h` - Added `theme_slot` field to `theme_cfg_t`

## Building a Theme

### 1. Create Theme Directory

```bash
mkdir -p themes/my_theme/assets
```

### 2. Create theme_manifest.json

```json
{
  "schema_version": "1.0",
  "theme": {
    "id": "my_theme",
    "name": "MY THEME",
    "version": "1.0.0",
    "author": "your-name"
  },
  "colors": {
    "bg": "0x000000",
    "ring": "0xFF0000",
    ...
  },
  "pages": {
    "comment": "Boot pages (logo/intro/boot_video) are protected - never themed",
    "system_pages": ["logo", "intro", "boot_video", "settings", "ota", "bluetooth_pair"],
    "theme_pages": [...]
  }
}
```

**Important**: `logo`, `intro`, and `boot_video` must **always** be in `system_pages`.
These ensure Sky Gauge branding and consistent boot experience.

### 3. (Optional) Add Assets

- `assets/dial.png` - 360x360 RGB background
- `assets/ring.png` - 360x360 RGBA ring overlay

### 4. Pack Theme

```bash
python3 tools/theme_packer/pack_theme.py themes/my_theme firmware/my_theme.bin
```

### 5. Flash or OTA

**Direct flash (USB):**
```bash
esptool.py --chip esp32s3 write_flash 0x620000 firmware/my_theme.bin
```

**BLE OTA:**
Use companion app to send `my_theme.bin` to device.

## Next Steps

### Phase 1 (Current - MVP)
- [x] Partition table redesign
- [x] Theme engine core API
- [x] Theme loader (manifest parser, asset mmap)
- [x] Theme packer tool
- [x] Example theme

### Phase 2 (In Progress)
- [ ] Integrate with existing UI system
- [ ] System page registration (settings/OTA/bluetooth)
- [ ] Custom page builder (parse layout.json)
- [ ] Data binding layer (obd_snapshot → UI elements)
- [ ] OTA theme update (add OTA_KIND_THEME)

### Phase 3 (Future)
- [ ] Android app theme selector UI
- [ ] Theme preview rendering
- [ ] Community theme repository
- [ ] Advanced layout elements (charts, animations)

## Testing

### Build Firmware
```bash
idf.py build
```

### Flash Complete System
```bash
idf.py flash
```

### Flash Example Theme
```bash
python3 tools/theme_packer/pack_theme.py themes/example_boost_oil firmware/theme_example.bin
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x620000 firmware/theme_example.bin
```

## Notes

- Only one theme partition (`theme_0`) exists; `theme_cfg.theme_slot` in NVS is unused,
  kept only so the struct layout doesn't shift
- Theme partition uses SPIFFS subtype (generic data partition)
- Assets are memory-mapped (zero-copy) from Flash to PSRAM
- Theme corruption falls back to default theme (8 colors from existing system)
- All system functionality (OBD/BLE/settings) remains intact
- Theme switching requires restart (same as existing theme system)

## Compatibility

- **Firmware**: ESP-IDF v5.5.3
- **Hardware**: ESP32-S3, 16MB Flash, 8MB PSRAM
- **Display**: 360x360 round LCD (ST77916)
- **Existing features**: Fully preserved (vehicle profiles, multi-gauge, etc.)
