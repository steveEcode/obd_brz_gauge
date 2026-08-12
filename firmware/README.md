# Firmware Package

This directory contains pre-built firmware binaries ready to flash onto an ESP32-S3 board.

本目录包含可直接烧录到 ESP32-S3 开发板的预编译固件。

## Files

| File | Description |
|------|-------------|
| `release/bootloader/bootloader.bin` | Bootloader |
| `release/partition_table/partition-table.bin` | Partition table |
| `release/ota_data_initial.bin` | OTA data initial partition |
| `release/obd_brz_gauge.bin` | Application firmware |
| `release/bootmedia.bin` | Boot animation media (SPIFFS partition) |
| `release/flash_address_map.txt` | Flash address reference |

## Flash Offsets

| Offset | File |
|--------|------|
| `0x0` | `release/bootloader/bootloader.bin` |
| `0x8000` | `release/partition_table/partition-table.bin` |
| `0xf000` | `release/ota_data_initial.bin` |
| `0x20000` | `release/obd_brz_gauge.bin` |
| `0x420000` | `release/bootmedia.bin` |

## Example Flash Command

Flash all partitions at once:

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 release/bootloader/bootloader.bin \
  0x8000 release/partition_table/partition-table.bin \
  0xf000 release/ota_data_initial.bin \
  0x20000 release/obd_brz_gauge.bin \
  0x420000 release/bootmedia.bin
```

## Notes

- All binaries are built from the current source tree.
- The `bootmedia.bin` partition contains boot animation assets and is optional if you do not need the animated startup sequence.
- See the flash address map in `release/flash_address_map.txt` for full layout details.

- 所有二进制文件均从当前源码树构建。
- `bootmedia.bin` 分区包含开机动画资源，如不需要可跳过烧录。
- 完整分区布局参见 `release/flash_address_map.txt`。
