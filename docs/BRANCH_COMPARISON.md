# Branch Comparison: main vs theme-upgrade

## Overview / 概述

This document explains the differences between the `main` and `theme-upgrade` branches. The two branches have **incompatible partition layouts** and cannot be cross-upgraded via OTA.

本文档说明 `main` 和 `theme-upgrade` 分支的区别。两个分支的**分区布局不兼容**，无法通过 OTA 互相升级。

---

## Key Differences / 主要区别

### 1. Partition Layout / 分区布局

#### main branch / main 分支

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xF000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
ota_0,    app,  ota_0,   0x20000, 0x300000,
ota_1,    app,  ota_1,   0x320000,0x300000,
bootmedia,data, 0x82,    0x620000,0x9E0000,  # 9.875 MB
```

**Total flash usage**: ~16MB
- No dedicated theme partition
- Bootmedia: 9.875MB (much larger, stores boot animation)

#### theme-upgrade branch / theme-upgrade 分支

```
# Name,     Type, SubType,  Offset,    Size,     Flags
nvs,        data, nvs,      0x9000,    0x6000,
otadata,    data, ota,      0xF000,    0x2000,
phy_init,   data, phy,      0x11000,   0x1000,
ota_0,      app,  ota_0,    0x20000,   0x300000,
ota_1,      app,  ota_1,    0x320000,  0x300000,
theme_0,    data, spiffs,   0x620000,  0x400000,  # 4 MB (NEW!)
bootmedia,  data, 0x82,     0xA20000,  0x5E0000,  # 5.875 MB (reduced)
```

**Total flash usage**: ~16MB
- **NEW**: `theme_0` partition (4MB) for custom themes
- Bootmedia: 5.875MB (reduced by 4MB to make room for themes)

**Flash address changes**:
- `bootmedia.bin`: `0x620000` → `0xA20000` ⚠️

---

### 2. Theme System / 主题系统

#### main branch

- Themes are **embedded at compile time** (`themes/` directory → compiled into firmware)
- No runtime theme loading
- Changing themes requires recompiling firmware

#### theme-upgrade branch

- Themes are **loaded from Flash partition at runtime**
- Theme files stored in `theme_0` partition (4MB SPIFFS)
- Supports OTA theme updates via `/ota/theme` API
- Themes can be uploaded without reflashing firmware
- Fallback to built-in default theme if partition is corrupt/empty

主题系统：
- **main**: 编译时嵌入，换主题需要重新编译固件
- **theme-upgrade**: 运行时从 Flash 分区加载，支持 OTA 更新主题

---

### 3. OTA API Endpoints / OTA API 端点

#### Common endpoints (both branches) / 共同端点

- `POST /ota/firmware` - Upload firmware
- `POST /ota/bootmedia` - Upload boot animation
- `GET /ota/info` - Device manifest
- `GET /ota/status` - OTA status

#### theme-upgrade exclusive endpoints / theme-upgrade 专有端点

- `POST /ota/theme/prepare` - Prepare theme upload (doesn't erase Flash)
- `POST /ota/theme` - Upload theme.bin (4MB blob)
- `POST /ota/theme/erase` - Erase entire theme partition (format)

---

### 4. Device Manifest / 设备清单

#### main branch `/ota/info` response

```json
{
  "device": {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": {"w": 360, "h": 360, "bpp": 16},
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1
  },
  "firmware": {
    "project": "obd_brz_gauge",
    "version": "1",
    "build_tag": "main-62-28a880204b56",
    "git": "28a880204b56",
    "branch": "main",
    "count": 62,
    "built": "Aug 26 2026 11:43:25",
    "idf": "v5.5.3",
    "slot": "ota_0"
  }
}
```

#### theme-upgrade branch `/ota/info` response (streamlined)

```json
{
  "device": {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": {"w": 360, "h": 360, "bpp": 16},
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1
  },
  "firmware": {
    "build_tag": "theme-upgrade-123-abc123def",
    "branch": "theme-upgrade",
    "count": 123
  }
}
```

**Removed fields** (to keep response under 512 bytes):
- `project`, `version`, `git`, `built`, `idf`, `slot`

移除了 App 不使用的字段以确保响应不超过 512 字节

---

### 5. WiFi Logging / WiFi 日志

#### main branch

- WiFi driver logs at default verbosity (INFO level)
- Shows all WiFi initialization messages during OTA

#### theme-upgrade branch

- WiFi logs suppressed to WARN level during OTA
- Cleaner console output, only shows warnings/errors

```c
// theme-upgrade only:
esp_log_level_set("wifi", ESP_LOG_WARN);
esp_log_level_set("wifi_init", ESP_LOG_WARN);
```

---

## Compatibility Matrix / 兼容性矩阵

| Action / 操作 | main → main | theme → theme | main → theme | theme → main |
|--------------|-------------|---------------|--------------|--------------|
| OTA firmware update | ✅ Yes | ✅ Yes | ❌ **NO** | ❌ **NO** |
| Full flash (esptool) | ✅ Yes | ✅ Yes | ✅ Yes | ✅ Yes |

⚠️ **Important**: You **cannot** OTA upgrade between `main` and `theme-upgrade` branches due to incompatible partition layouts. You must use full flash via esptool.

⚠️ **重要**：`main` 和 `theme-upgrade` 分支之间**无法通过 OTA 互相升级**（分区布局不兼容）。必须使用 esptool 完整烧录。

---

## Migration Guide / 迁移指南

### From main to theme-upgrade / 从 main 迁移到 theme-upgrade

1. **Backup NVS settings** (optional, if you want to preserve settings)
   ```bash
   esptool.py -p PORT read_flash 0x9000 0x6000 nvs_backup.bin
   ```

2. **Full flash the theme-upgrade firmware**
   ```bash
   esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
     0x0 bootloader/bootloader.bin \
     0x8000 partition_table/partition-table.bin \
     0xf000 ota_data_initial.bin \
     0x20000 obd_brz_gauge.bin \
     0xA20000 bootmedia.bin
   ```
   Note: `bootmedia.bin` address changed from `0x620000` to `0xA20000`

3. **Restore NVS** (optional)
   ```bash
   esptool.py -p PORT write_flash 0x9000 nvs_backup.bin
   ```

### From theme-upgrade to main / 从 theme-upgrade 迁移到 main

1. **Backup NVS settings** (optional)
2. **Full flash the main firmware** (use old bootmedia address `0x620000`)
3. **Restore NVS** (optional)

Note: You will lose any custom themes stored in the `theme_0` partition.

注意：存储在 `theme_0` 分区的自定义主题会丢失。

---

## Which Branch Should I Use? / 我应该用哪个分支？

### Use `main` if / 使用 `main` 如果：

- You want a stable, well-tested firmware
- You don't need custom theme support
- You're okay with themes being compiled into firmware
- You prefer larger boot animation storage (9.875MB)

### Use `theme-upgrade` if / 使用 `theme-upgrade` 如果：

- You want runtime-loadable custom themes
- You plan to use the theme store/designer
- You want to update themes without reflashing firmware
- You can accept smaller boot animation storage (5.875MB, still plenty)
- You want cleaner OTA logs (WiFi verbosity suppressed)

---

## Developer Notes / 开发者注意事项

### Building for theme-upgrade / 为 theme-upgrade 构建

```bash
git checkout theme-upgrade
idf.py set-target esp32s3
idf.py build
```

The partition table is automatically selected from `partitions.csv` in the branch.

分区表会自动从分支中的 `partitions.csv` 选择。

### Theme Development / 主题开发

- `main` branch: Edit `themes/` directory, rebuild firmware
- `theme-upgrade` branch: Use theme designer tool, upload via `/ota/theme` API

---

## Future Plans / 未来计划

The `theme-upgrade` branch is currently experimental. Once stable, it may be merged into `main` or become the new default branch.

`theme-upgrade` 分支目前处于实验阶段。稳定后可能会合并到 `main` 或成为新的默认分支。

**Tracking issue**: [Link to GitHub issue if exists]

---

## Questions? / 有问题？

If you're unsure which branch to use, ask in the project's issue tracker or discussions.

如果不确定使用哪个分支，请在项目的 issue 或讨论区提问。
