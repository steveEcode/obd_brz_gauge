# OBD BRZ Gauge

> Based on the open-source project [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp) — original work by zhaizhaitao ([Bilibili demo](https://www.bilibili.com/video/BV18oHXz6EiQ/)). This repository is a derivative with additional vehicle profiles, multi-gauge support, and other changes.
>
> 本项目基于开源项目 [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp) 二次开发，原作者 zhaizhaitao（[B 站演示](https://www.bilibili.com/video/BV18oHXz6EiQ/)）。本仓库在其基础上新增了多车型适配、三连表联动等功能。

OBD BRZ Gauge is an ESP-IDF based round dashboard project for the Waveshare ESP32-S3-Touch-LCD-1.85 development board. It connects to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders a touch UI with LVGL.

这是一个基于 ESP-IDF 的圆形车载仪表项目，目标硬件为微雪 Waveshare ESP32-S3-Touch-LCD-1.85 开发板。项目通过 BLE 连接兼容 ELM327 的 OBD 适配器，读取车辆数据并使用 LVGL 显示触控界面。

效果展示/Demo Video
https://www.douyin.com/video/7614174567678984187

## Status

- Hardware platform: Waveshare ESP32-S3-Touch-LCD-1.85
- Software stack: ESP-IDF 5.5.3, LVGL 8
- Protocol path: BLE + ELM327 (standard OBD PID + CAN broadcast frame ATMA monitoring)
- Vehicle profiles: BRZ ZC/N6 CAN / ZD8 CAN, Mazda MX-5 ND, BMW G-series, Porsche 987.1/997.1/997.2, MINI JCW F56, OBD2 Generic
- Multi-gauge: one master + multiple slaves linked over ESP-NOW
- Validation status: fully verified on Subaru BRZ ZC/N6; other profiles are configured and may still need on-car verification

- 硬件平台：微雪 Waveshare ESP32-S3-Touch-LCD-1.85
- 软件栈：ESP-IDF 5.5.3、LVGL 8
- 通信链路：BLE + ELM327（标准 OBD PID + CAN 广播帧 ATMA 监听）
- 已内置车型：BRZ ZC/N6 CAN / ZD8 CAN、马自达 MX-5 ND、宝马 G 系、保时捷 987.1/997.1/997.2、MINI JCW F56、OBD2 通用
- 三连表：一主多从，通过 ESP-NOW 联动
- 当前验证状态：已在斯巴鲁 BRZ ZC/N6 上完整验证；其余车型已配置，部分仍需上车验证

## Features

- BLE scan and connection for ELM327-compatible adapters
- **CAN broadcast frame ATMA monitoring**: bypasses standard OBD PID polling for high-frequency data — RPM at 100 Hz, oil/coolant temp at 10–20 Hz, with periodic OBD fallback for remaining channels (speed, load, voltage, intake temp)
  - ZC/N6 (Gen1): 0x140 (RPM + TPS) + 0x360 (oil + coolant temp)
  - ZD8 (Gen2): 0x40 (RPM + TPS) + 0x345 (oil + coolant temp)
- **Custom boot logo & boot media playback**: configurable boot logo with multi-block animation, SPIFFS-mounted bootmedia partition for rich startup sequences
- **Unified vehicle configuration system**: compile-time `vehicle_custom_config.h` for per-vehicle thresholds, warnings, and gauge ranges
- Real-time display for RPM, speed, coolant/intake/oil temperature, oil pressure, turbo boost, throttle, engine load, battery voltage, gear (prefers CAN-decoded precise gear when available, falls back to RPM/speed estimate), and related values
- "NO SIGNAL" indicator on the gauge pages when BLE/ESP-NOW data goes stale
- Vehicle profile selector: per-car gear ratios (up to 8-speed), oil-temp strategy, turbo boost, and per-vehicle protocol lock (e.g. BMW/Porsche force ISO 15765-4 CAN)
- Manufacturer oil-temp paths beyond standard PID 01 5C: Toyota/Subaru Mode 21, Mazda Mode 22, MINI/BMW Mode 22, BMW F-series Mode 22 44 02, Porsche CAN broadcast 0x441 monitoring
- Configurable pointer (needle) gauge page with swipe-down data-source selection
- **RPM over-limit flash warning**: configurable threshold, background image flashes red when RPM exceeds the limit
- **Triple-gauge startup animation**: three boards display "RACE / AS / ONE" in sequence over ESP-NOW synchronized intro
- Robust connection handling: waits for BLE notify subscription, re-initializes on every reconnect, and self-heals (re-init + auto reconnect) when data stalls — no manual reconnect needed after ignition
- Multi-gauge (三连表) over ESP-NOW: a master reads OBD and broadcasts; slaves display the same data with no extra OBD load; role is chosen in settings; startup sweep is synchronized; slaves show the master name
- Optional custom boot logo via a compile-time switch
- LVGL touch UI exported from SquareLine-based assets
- User configuration persisted via NVS; mileage/trip statistics are runtime-only (reset each power cycle, not written to flash)

- 支持扫描并连接兼容 ELM327 的 BLE OBD 设备
- **CAN 广播帧 ATMA 监听**：绕过标准 OBD PID 轮询，高频直读 — 转速 100Hz、油温/水温 10–20Hz，其余通道（车速/负荷/电压/进气温）定期 OBD 回退查询
  - ZC/N6 (Gen1)：0x140（转速+节气门）+ 0x360（油温+水温）
  - ZD8 (Gen2)：0x40（转速+节气门）+ 0x345（油温+水温）
- **自定义开机图 & 开机动画**：可配置开机 Logo，支持多 Block 动画播放，SPIFFS 分区挂载 bootmedia 实现丰富开机流程
- **统一车辆配置系统**：编译时 `vehicle_custom_config.h` 管理各车型阈值、报警、表盘范围
- 实时显示转速、车速、水温/进气温/机油温、机油压力、涡轮压力、节气门、发动机负荷、电压、档位（优先用 CAN 直接解码的精确档位，无效时回退转速/车速估算）等数据
- 蓝牙/ESP-NOW 数据断开超时后,仪表页显示 "NO SIGNAL" 提示
- 车型选择：各车型独立的传动比（最高 8 挡）、油温策略、涡轮增压，以及按车型锁定协议（如宝马/保时捷强制 ISO 15765-4 CAN）
- 除标准 PID 01 5C 外的厂商油温读取：丰田/斯巴鲁 Mode 21、马自达 Mode 22、MINI/宝马 Mode 22、宝马 F 系 Mode 22 44 02、保时捷 CAN 广播帧 0x441 监听
- 可配置指针表盘页，下滑切换显示的数据源
- **转速超限闪烁报警**：可设阈值，超限时背景图红色闪烁
- **三连表开机动画**：三块板通过 ESP-NOW 同步依次显示 "RACE / AS / ONE"
- 稳健的连接处理：等待 BLE 通知订阅完成后再初始化，每次重连都重新初始化，数据中断时自愈（重初始化 + 自动重连）——上车通电后无需手动重连
- 三连表（ESP-NOW）：主表读 OBD 并广播，从表零额外 OBD 负载同步显示；主/从角色在设置页选择；开机扫表动画同步；从表显示主表名字
- 可选自定义开机图（编译开关）
- 使用 LVGL 和导出 UI 资源实现触控界面
- 用户配置通过 NVS 持久化；里程/行程统计只在运行时内存里累计（每次开机清零，不再写 flash）

## Quick Start

1. Install ESP-IDF 5.1 or newer.
2. Set the target to ESP32-S3.
3. Build and flash the project.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

1. 安装 ESP-IDF 5.1 或更高版本。
2. 将目标芯片设置为 ESP32-S3。
3. 编译并烧录项目。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Multi-Gauge (三连表)

An ELM327 BLE adapter accepts only one client, so multiple gauges cannot each connect to it. Instead, one board is the **master** (keeps the BLE + ELM327 link and reads OBD) and the others are **slaves**. The master broadcasts the parsed data cache over ESP-NOW; slaves render it with no extra OBD load. Flash the same firmware to every board and pick the role in Settings → swipe down → MULTI-GAUGE (reboot to apply). The master's BLE/WiFi coexist on the ESP32-S3; slaves run ESP-NOW only.

Pairing is done over real BLE, not a blind "bind whatever I last heard" button: once a board is set to MASTER, it advertises as `SkyGauge-XXYY` (a per-device suffix so multiple masters at a meet don't collide). An unbound slave boots straight into a "FIND MASTER" scan screen (the same screen used to pick an OBD adapter), lets you pick the right master from the list, and remembers it — every following boot skips the scan and goes straight to the gauge display.

一个 ELM327 蓝牙适配器只允许一个客户端连接，多块表无法各自直连。因此一块板作**主表**（保持 BLE + ELM327 连接、读取 OBD），其余作**从表**。主表通过 ESP-NOW 广播解析后的数据缓存，从表零额外 OBD 负载渲染。三块板烧同一份固件，在「设置页 → 下滑 → MULTI-GAUGE」选择角色（重启生效）。主表在 ESP32-S3 上蓝牙/WiFi 共存，从表只跑 ESP-NOW。

配对走的是真蓝牙，不是"盲收广播绑定"：主表设为 MASTER 后会广播 `SkyGauge-XXYY`（带设备后缀，赛道日多车同时用也不会互相干扰）。从表未配对时开机会直接进入 "FIND MASTER" 扫描页（和选 OBD 适配器共用同一个页面），从列表里选中要跟随的主表即可，之后每次开机都会自动跳过扫描直接显示仪表数据。

## Project Layout

- [main/app_main.c](main/app_main.c): application entry, LVGL initialization, BLE startup, task startup
- [main/app_obd_dsp](main/app_obd_dsp): runtime OBD data cache, vehicle profiles, CAN frame decoders, mileage statistics, boot media playback, and vehicle custom config
- [main/bsp_obd_dsp](main/bsp_obd_dsp): board support package, BLE client, NVS, LCD, touch, I2C (ESP-IDF 5.5 new API), IO expander, and ESP-NOW link drivers
- [main/export_path](main/export_path): UI source exported from the design tool (SquareLine)
- [bootmedia](bootmedia): boot animation media blocks (SPIFFS partition source)
- [model](model): open-source 3D printable files (housing, gauge pods, brackets)
- [tools](tools): helper scripts (image conversion, boot block builder)
- [firmware/release](firmware/release): pre-compiled firmware binaries for flashing
- [docs](docs): open-source documentation, bilingual README, structure notes

## Documentation

- 中文说明：[docs/README.zh-CN.md](docs/README.zh-CN.md)
- English documentation: [docs/README.en.md](docs/README.en.md)
- 发布结构说明：[docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)

## Changelog

### Multi-gauge: real BLE pairing replaces the MAC-bind button

The old "BIND MASTER" button worked by grabbing whichever master's ESP-NOW broadcast the slave happened to be receiving at the moment — with no way to pick a specific one when multiple masters are nearby (e.g. a track day with several cars running the same product). The master now advertises a real BLE peripheral (`SkyGauge-XXYY`); the slave discovers and binds to it through the existing BLE scan page (now doubling as a "FIND MASTER" screen when the device role is SLAVE), and reconnects automatically on every following boot.

- New `gauge_pair_ble_client.c/h` (slave-side one-shot BLE pairing client) and `ble_adv_util.c/h` (shared BLE advertisement-name parsing, deduplicated out of the OBD BLE client).
- `racechrono_ble_diy.c` gained an independent pairing GATT service alongside the existing RaceChrono service, sharing one BLE advertisement.
- `ui_ScreenPageMultiGauge.c` no longer has BIND MASTER / UNBIND buttons.
- Boot flow: an unbound slave lands on the pairing screen; a bound slave skips straight to its gauge display.

### Fixed: master watchdog reboot during OBD protocol detection

Root-caused a board reboot seen during testing: the blocking wait for an ELM327 response could sit for up to 3 seconds without feeding the task watchdog. A run of consecutive protocol auto-detect timeouts could add up past the 5 s TWDT window and reboot the board mid-poll. Fixed by resetting the watchdog inside that wait loop.

### Other fixes

- Slave-side BLE scan state could get stuck once its 15 s scan window elapsed, silently blocking retry/rescan.
- Leaving the pairing screen in slave mode stopped the wrong BLE scan API, leaving a scan running in the background.
- I2C device cache could read out of bounds once more than 8 addresses were queried (latent crash, not yet hit in practice).
- LCD init could read an uninitialized register value if the QSPI probe failed.

### Improvements

- "NO SIGNAL" indicator on the gauge pages when BLE/ESP-NOW data goes stale.
- Gear display now prefers the CAN-decoded precise gear over the RPM/speed estimate when a vehicle profile provides one.
- Mileage/trip statistics are runtime-only now (no longer written to flash every 30 s) — nothing displayed them, so it was pure flash wear.
- Removed unused `fsm.h` state-machine scaffolding and an unused OBD-data "dirty flag" tracking layer — neither was ever wired up to anything.
- De-duplicated a shared BLE-advertisement-name parser, a screen ring border, and a dark roller LVGL style across ~18 screens; throttled a full chart redraw and a few gauge pages to only refresh when actually visible or actually changed.

### 三连表：用真蓝牙配对取代绑定按钮

原来的 "BIND MASTER" 按钮是抓从表当下收到的任意一台主表 ESP-NOW 广播来绑定——附近有多台主表时（比如赛道日好几台车都在用）没法指定要跟哪一台。现在主表会真正通过蓝牙广播身份（`SkyGauge-XXYY`），从表在现成的蓝牙扫描页（从表角色下会变成 "FIND MASTER" 配对页）里发现并绑定，之后每次开机都会自动重连。

- 新增 `gauge_pair_ble_client.c/h`（从表侧一次性蓝牙配对客户端）和 `ble_adv_util.c/h`（从 OBD 蓝牙客户端里提出来的共享广播名解析工具）。
- `racechrono_ble_diy.c` 在现有 RaceChrono 服务旁边加了一个独立的配对 GATT 服务，共用同一份蓝牙广播。
- `ui_ScreenPageMultiGauge.c` 去掉了 BIND MASTER / UNBIND 按钮。
- 开机流程：从表没配对过会停在配对页；配对过则直接跳过、进入仪表显示。

### 修复：主表在 OBD 协议探测时看门狗重启

定位到了测试中出现的一次真实重启：等待 ELM327 响应的阻塞循环最多能空等 3 秒且不喂狗，协议自动探测连续超时几次累加起来就会超过 5 秒的 TWDT 窗口，导致轮询过程中重启。已经在等待循环里补上喂狗。

### 其它修复

- 从表蓝牙扫描 15 秒窗口自然到期后状态不会复位，导致重试/删除后重扫静默失效。
- 从表在配对页划走时停的是错误的蓝牙扫描 API，导致配对扫描在后台空跑。
- I2C 设备缓存计数逻辑在超过 8 个地址后会越界读（潜在崩溃，实际还没触发过）。
- LCD 初始化时如果 QSPI 探测失败，会用到未初始化的寄存器数据。

### 优化

- 蓝牙/ESP-NOW 数据断连超时后，仪表页显示 "NO SIGNAL" 提示。
- 档位显示优先使用车型支持的 CAN 精确解码档位，没有时才回退到转速/车速估算。
- 里程/行程统计不再每 30 秒写一次 flash，只在运行时内存里累计（没有界面显示过，纯粹是 flash 损耗）。
- 删除了从未被真正接入使用的 `fsm.h` 状态机脚手架和 OBD 数据的 dirty-flag 跟踪层。
- 把蓝牙广播名解析、屏幕白色圆环边框、深色滚轮样式这三处在约 18 个页面里重复的代码提取成共享实现；节流了一处全量重绘的图表和几个仪表页，只在真正可见/真正变化时才刷新。

## Notes

- This repository contains project-specific board adaptation and UI resources; if you port it to another ESP32-S3 board or another vehicle, you will likely need to adjust pin mapping, display settings, and OBD parsing behavior.
- The current parsing and verification focus on Subaru BRZ ZC/N6. Other vehicles may require protocol, PID, or adapter compatibility adjustments.

- 仓库内包含针对当前开发板的适配代码和 UI 资源；如果迁移到其他 ESP32-S3 开发板或其他车型，需要重新检查引脚定义、屏幕参数和 OBD 解析逻辑。
- 当前解析和验证重点面向 Subaru BRZ ZC/N6，其他车型可能需要额外调整协议、PID 或适配器兼容性。

## 3D Models (开源模型)

The `model/` directory contains open-source 3D printable files for mounting the gauge:

`model/` 目录包含开源的 3D 打印模型文件，用于安装仪表：

| File | Description |
|------|-------------|
| `model/esp32_1.85_weixue/housing.stl` | ESP32-S3-Touch-LCD-1.85 board housing / 开发板外壳 |
| `model/Subaru/brz_zc_n6/triple_gauge_pod.stp` | BRZ ZC/N6 triple gauge pod (3× 1.85" round) / 三连表底座 |
| `model/Subaru/brz_zc_n6/passenger_dashboard_scan.stl` | BRZ ZC/N6 passenger dashboard scan (for fitting reference) / 副驾仪表台扫描件 |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND air vent mount bracket / 出风口支架 |

## Acknowledgments

- [Hokori23](https://github.com/Hokori23) — performance optimization suggestions and contributions (NVS flush lock hold-time, page-aware refresh cadence, and OBD polling throughput).
- [timurrrr/ft86](https://github.com/timurrrr/ft86) — comprehensive FT86 CAN bus documentation (Gen1 & Gen2 CAN ID mappings, byte-level decoding formulas), which made the CAN broadcast ATMA monitoring possible.

- [Hokori23](https://github.com/Hokori23) —— 性能优化建议与贡献者（NVS 落盘锁优化、按页面自适应刷新频率、OBD 轮询效率提升）。
- [timurrrr/ft86](https://github.com/timurrrr/ft86) —— 提供了完整的 FT86 CAN 总线文档（Gen1/Gen2 CAN ID 映射及字节级解码公式），使 CAN 广播帧 ATMA 监听得以实现。
