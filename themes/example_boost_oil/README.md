# Example Boost + Oil Pressure Theme

This is an example theme for the OBD gauge theme partition system.

## Contents

- `theme_manifest.json` - Theme metadata, colors, and page configuration
- `layout.json` - Custom page layout (boost arc + oil pressure bar)
- `assets/` - Theme assets (dial.png and ring.png should be placed here)

## Assets (Optional)

To add custom background images:

1. Create `assets/dial.png` - 360x360 RGB background image
2. Create `assets/ring.png` - 360x360 RGBA ring overlay (transparent center)

If assets are not provided, the theme will use colored backgrounds.

## Building

```bash
python3 tools/theme_packer/pack_theme.py themes/example_boost_oil firmware/theme_boost_oil.bin
```

This will generate a 2MB binary that can be:
- Flashed directly to theme_0 partition (offset 0x612000)
- Or sent via BLE OTA to the device

## Installing

### Via esptool (USB):
```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x612000 firmware/theme_boost_oil.bin
```

### Via BLE OTA:
Use the companion Android app to send the theme binary to the device.

## Theme Features

- Custom boost gauge with arc (range: -0.70 to 2.50 bar)
- Oil pressure bar with 10 segments (0-100 PSI)
- Orange/yellow color scheme
- Preserves all system pages (settings/OTA/bluetooth)
