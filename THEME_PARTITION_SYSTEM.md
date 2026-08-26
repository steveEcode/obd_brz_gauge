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
│  Theme Data Partition (theme_0/theme_1, 2MB each)   │
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
4. **OTA Compatible**: Themes can be sent via BLE OTA like firmware
5. **Dual Slots**: theme_0/theme_1 for safe upgrades (rollback on corruption)
6. **Color Palette**: 8 themed colors (bg, ring, arc_track, etc.)
7. **Assets**: Optional 360x360 dial/ring images (memory-mapped from Flash)
8. **Custom Layouts**: JSON-based page element definitions

## Changes

### Partition Table (partitions.csv)
- **ota_0/ota_1**: 3MB each (unchanged)
- **theme_0/theme_1**: 2MB each (new)
- **bootmedia**: Reduced to 5.96MB (was 9.88MB)

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
esptool.py --chip esp32s3 write_flash 0x612000 firmware/my_theme.bin
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
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x612000 firmware/theme_example.bin
```

### Switch Theme Slot
- Via NVS: Modify `theme_cfg.theme_slot` (0 or 1)
- Via app: Send theme slot change command → device restarts

## Notes

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
