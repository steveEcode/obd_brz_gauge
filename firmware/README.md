# Firmware Package

This directory contains pre-built firmware binaries ready to flash onto an ESP32-S3 board.

本目录包含可直接烧录到 ESP32-S3 开发板的预编译固件。

⚠️ **Branch-specific flash addresses**: The `theme-upgrade` branch has different partition layout. See [Flash Offsets](#flash-offsets) below.

⚠️ **分支特定烧录地址**：`theme-upgrade` 分支的分区布局不同。参见下方[烧录地址](#flash-offsets)。

## Files

| File | Description |
|------|-------------|
| `release/bootloader/bootloader.bin` | Bootloader |
| `release/partition_table/partition-table.bin` | Partition table |
| `release/ota_data_initial.bin` | OTA data initial partition |
| `release/obd_brz_gauge.bin` | Application firmware |
| `release/bootmedia.bin` | Boot animation media (SPIFFS partition) |
| `release/latest.json` | Release manifest (build tag, file hashes) — the app fetches this to detect a new firmware |
| `release/flash_address_map.txt` | Flash address reference |

## Flash Offsets

### main branch / main 分支

| Offset | File |
|--------|------|
| `0x0` | `release/bootloader/bootloader.bin` |
| `0x8000` | `release/partition_table/partition-table.bin` |
| `0xf000` | `release/ota_data_initial.bin` |
| `0x20000` | `release/obd_brz_gauge.bin` |
| `0x620000` | `release/bootmedia.bin` |

### theme-upgrade branch / theme-upgrade 分支

| Offset | File | Notes |
|--------|------|-------|
| `0x0` | `release/bootloader/bootloader.bin` | |
| `0x8000` | `release/partition_table/partition-table.bin` | |
| `0xf000` | `release/ota_data_initial.bin` | |
| `0x20000` | `release/obd_brz_gauge.bin` | |
| `0x620000` | `release/theme_0.bin` (optional) | 4MB theme partition (NEW) |
| `0xA20000` | `release/bootmedia.bin` | ⚠️ **Address changed** from 0x620000 |

**Key difference**: `bootmedia.bin` moved from `0x620000` to `0xA20000` to make room for the 4MB theme partition.

**主要区别**：`bootmedia.bin` 从 `0x620000` 移动到 `0xA20000`，为 4MB 主题分区腾出空间。

## Example Flash Command

### main branch / main 分支

Flash all partitions at once:

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 release/bootloader/bootloader.bin \
  0x8000 release/partition_table/partition-table.bin \
  0xf000 release/ota_data_initial.bin \
  0x20000 release/obd_brz_gauge.bin \
  0x620000 release/bootmedia.bin
```

### theme-upgrade branch / theme-upgrade 分支

Flash all partitions (note different bootmedia address):

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 release/bootloader/bootloader.bin \
  0x8000 release/partition_table/partition-table.bin \
  0xf000 release/ota_data_initial.bin \
  0x20000 release/obd_brz_gauge.bin \
  0xA20000 release/bootmedia.bin
```

Optional: Flash a theme to the theme partition (theme-upgrade only):

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x620000 release/theme_0.bin
```

## Notes

- All binaries are built from the current source tree.
- The `bootmedia.bin` partition contains boot animation assets and is optional if you do not need the animated startup sequence.
- `release/latest.json` is the release manifest the companion app fetches to detect and verify a new firmware; its `firmware.count`/`build_tag` fields are compared against the device's own manifest (see [docs/APP_INTEGRATION.md](../docs/APP_INTEGRATION.md)). Regenerate it whenever the release binaries change.
- See the flash address map in `release/flash_address_map.txt` for full layout details.

- 所有二进制文件均从当前源码树构建。
- `bootmedia.bin` 分区包含开机动画资源，如不需要可跳过烧录。
- 完整分区布局参见 `release/flash_address_map.txt`。
