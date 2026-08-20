# App Integration / App 集成说明

This document defines the device-manifest contract for the companion app and the minimum rules for firmware / boot-animation updates.

本文定义了配套 App 的设备清单协议，以及固件 / 开机动画更新的最小约束。

## Goals / 目标

- One app for iOS and Android.
- Validate hardware before any flash/write action.
- Keep boot animation as a single active slot on the device.
- Trim/crop video on the phone before encoding and upload.

- 一套 App 覆盖 iOS 和 Android。
- 写入前先验证硬件匹配。
- 设备端只保留一个当前生效的开机动画。
- 视频在手机端先裁切、选时长，再编码并上传。

## Device identity manifest / 设备身份清单

The firmware exposes a read-only BLE GATT service for compatibility checks:

- Service UUID: `0x1FFA`
- Characteristic UUID: `0x0001`
- Payload: UTF-8 JSON string

固件通过只读 BLE GATT 服务暴露兼容性检查信息：

- Service UUID：`0x1FFA`
- Characteristic UUID：`0x0001`
- 载荷：UTF-8 JSON 字符串

### Expected fields / 约定字段

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
    "project": "obd_brz_gauge",
    "version": "1",
    "build_tag": "main-1234-abcd1234",
    "git": "abcd1234",
    "branch": "main",
    "count": 1234,
    "built": "Aug 16 2026 12:34:56",
    "idf": "v5.5.3",
    "slot": "ota_0"
  }
}
```

The app should reject updates when any required hardware field differs from the manifest baked into the selected release.

App 必须在硬件字段与所选 release 的清单不一致时拒绝刷写。

## Firmware update flow / 固件更新流程

1. App fetches `latest.json` from the NAS static release directory.
2. App reads the device manifest over BLE and compares it to the release manifest.
3. If compatible, App flashes the firmware image to the non-running OTA slot.
4. Device reboots into the new slot and self-checks.
5. On success, firmware marks the slot valid; on failure, bootloader rollback restores the previous slot.

1. App 从 NAS 的静态发布目录读取 `latest.json`。
2. App 通过 BLE 读取设备清单，并与 release 清单比对。
3. 兼容后，把固件刷到当前未运行的 OTA 槽位。
4. 设备重启到新槽并执行自检。
5. 自检成功后标记新槽有效；失败则由 bootloader 自动回滚。

## Boot animation flow / 开机动画流程

The device keeps only one active boot animation image. The app may still store multiple drafts locally, but only the selected one is encoded and uploaded to the device.

设备端只保留一个当前生效的开机动画镜像。App 可以本地保存多个草稿，但最终只会把选中的那一个编码并上传到设备。

### Editor rules / 编辑规则

- Canvas size: `360x360`
- Aspect ratio: locked to `1:1`
- Video trim: choose start and end timestamps before encoding
- Safe area: circular preview overlay recommended, because corners are clipped on the round screen
- Size budget: the final encoded package must fit the `bootmedia` partition
- Update transaction: the device stages `boot_block.txt.new` and `boot_block.bin.new`, then commits them atomically into the single active slot

- 画布尺寸：`360x360`
- 比例：锁定 `1:1`
- 裁切：先选起止时间，再编码
- 安全区：建议显示圆形预览遮罩，因为圆屏四角会被裁掉
- 大小限制：最终编码包必须放得进 `bootmedia` 分区
- 更新事务：设备先写入 `boot_block.txt.new` 和 `boot_block.bin.new`，再原子切换到唯一生效槽位

## BLE OTA service / BLE OTA 服务

The companion app flashes firmware and boot-animation assets over a dedicated BLE GATT service:

- Service UUID: `0x1FFB`
- Control characteristic UUID: `0x0001`
- Data characteristic UUID: `0x0002`
- Status characteristic UUID: `0x0003`

控制包格式：

- Magic: `OTA1`
- Command: `begin` / `end` / `cancel`
- Target: `firmware` or `bootmedia`
- Size: little-endian `u32`
- SHA256: raw 32-byte digest
- Filename: length-prefixed UTF-8 string

The firmware marks the new app valid only after the startup delay succeeds, so bootloader rollback still protects against crashes during early boot.

固件会在启动成功并经过延迟自检后再标记新 App 有效，因此如果新版本在早期启动阶段崩溃，bootloader 仍可自动回滚。

## WiFi OTA service / WiFi OTA 服务（推荐）

For faster transfers, the device supports BLE handshake + WiFi data transfer:

### Flow / 流程

1. **BLE 握手**：APP 通过 BLE control characteristic 发送 command `4`（启动 WiFi OTA）
2. **设备启动 WiFi SoftAP**：
   - SSID: `OBD-Gauge-OTA-XXXX`（XXXX 为 MAC 地址后两字节）
   - Password: `obd2024`
   - IP: `192.168.4.1`
   - Port: `80`
3. **BLE 通知 APP**：设备通过 status characteristic 返回 JSON：
   ```json
   {"ssid":"OBD-Gauge-OTA-ABCD","password":"obd2024","ip":"192.168.4.1","token":"a1b2c3d4e5f6a7b8","port":80}
   ```
4. **APP 连接 WiFi**：连接到设备 SoftAP
5. **HTTP 上传**：
   - `POST /ota/firmware` - 上传固件
   - `POST /ota/bootmedia` - 上传开机动画
6. **BLE 状态同步**：设备通过 BLE 通知上传进度
7. **完成后重启**：设备自动关闭 WiFi 并重启

### HTTP API

**Headers（所有请求必需）：**
- `X-OTA-Token`: 从 BLE 握手获取的 token（16 字符 hex）

**POST /ota/firmware**
- `X-OTA-SHA256`: 固件 SHA256（64 字符 hex）
- `X-OTA-Size`: 固件大小（字节）
- Body: 固件二进制数据

**POST /ota/bootmedia**
- `X-OTA-SHA256`: 整体 SHA256（64 字符 hex）
- `X-OTA-Size`: 总大小（manifest + bin）
- `X-OTA-Manifest-Size`: manifest 大小
- Body: `[manifest][bin]` 拼接数据

**GET /ota/status**
- 返回: `{"state":"ready|receiving|done|error","received":12345,"expected":67890}`

### State values / 状态值

BLE status characteristic 会同步 WiFi OTA 状态：
- `wifi-starting`: WiFi 服务启动中
- `wifi-ready`: WiFi 服务就绪，等待 APP 连接
- `wifi-receiving`: 正在接收数据
- `wifi-done`: 完成，即将重启
- `wifi-error`: 错误

### Security / 安全性

- Token 每次 OTA 随机生成，防止未授权访问
- WiFi 使用 WPA2-PSK 加密（密码 `obd2024`）
- SHA256 校验确保数据完整性

## NAS release layout / NAS 发布目录

A minimal static layout is enough:

```text
/releases/
  latest.json
  firmware/
    obd_brz_gauge.bin
    partition-table.bin
    bootloader.bin
    ota_data_initial.bin
  bootmedia/
    bootmedia.bin
```

`latest.json` can be generated by a Git hook or cron job when `main` changes.

最小静态目录即可：

```text
/releases/
  latest.json
  firmware/
    obd_brz_gauge.bin
    partition-table.bin
    bootloader.bin
    ota_data_initial.bin
  bootmedia/
    bootmedia.bin
```

`latest.json` 可以由 Git hook 或 cron 在 `main` 分支变化时生成。
