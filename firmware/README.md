# Firmware Package

This directory contains a prebuilt firmware package copied from the existing successful build artifacts of the original project snapshot.

本目录包含一套预编译固件，来源于旧工程中已经生成的可用构建产物。

## Files

- `release/bootloader/bootloader.bin`
- `release/partition_table/partition-table.bin`
- `release/obd_brz_gauge.bin`

## Flash Offsets

- `0x0` -> `bootloader/bootloader.bin`
- `0x8000` -> `partition_table/partition-table.bin`
- `0x10000` -> `obd_brz_gauge.bin`

## Example Flash Command

```bash
esptool.py --chip esp32s3 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 obd_brz_gauge.bin
```

## Note

The firmware file was renamed to match the new project name, but it was copied from the previously built binary of the migrated source tree.

应用固件文件名已经按新项目名重命名，但当前文件来源仍然是迁移前工程中已有的编译产物。