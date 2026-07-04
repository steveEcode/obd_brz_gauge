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
- Software stack: ESP-IDF 5.1+, LVGL 8
- Protocol path: BLE + ELM327
- Vehicle profiles: Subaru BRZ ZC6/ZD8, Mazda MX-5 ND, BMW G-series / X1 F48, Porsche 987.2/997.1/997.2, MINI JCW F56
- Multi-gauge: one master + multiple slaves linked over ESP-NOW
- Validation status: fully verified on Subaru BRZ ZC6; other profiles are configured and may still need on-car verification

- 硬件平台：微雪 Waveshare ESP32-S3-Touch-LCD-1.85
- 软件栈：ESP-IDF 5.1+、LVGL 8
- 通信链路：BLE + ELM327
- 已内置车型：斯巴鲁 BRZ ZC6/ZD8、马自达 MX-5 ND、宝马 G 系 / X1 F48、保时捷 987.2/997.1/997.2、MINI JCW F56
- 三连表：一主多从，通过 ESP-NOW 联动
- 当前验证状态：已在斯巴鲁 BRZ ZC6 上完整验证；其余车型已配置，部分仍需上车验证

## Features

- BLE scan and connection for ELM327-compatible adapters
- Real-time display for RPM, speed, coolant/intake/oil temperature, oil pressure, turbo boost, throttle, engine load, battery voltage, gear, and related values
- Vehicle profile selector: per-car gear ratios (up to 7-speed), oil-temp strategy, turbo boost, and per-vehicle protocol lock (e.g. BMW/Porsche force ISO 15765-4 CAN)
- Manufacturer oil-temp paths beyond standard PID 01 5C: Toyota/Subaru Mode 21, Mazda Mode 22, MINI/BMW Mode 22, BMW F-series Mode 22 44 02, Porsche CAN broadcast 0x441 monitoring
- Configurable pointer (needle) gauge page with swipe-down data-source selection
- Robust connection handling: waits for BLE notify subscription, re-initializes on every reconnect, and self-heals (re-init + auto reconnect) when data stalls — no manual reconnect needed after ignition
- Multi-gauge (三连表) over ESP-NOW: a master reads OBD and broadcasts; slaves display the same data with no extra OBD load; role is chosen in settings; startup sweep is synchronized; slaves show the master name
- Optional custom boot logo via a compile-time switch
- LVGL touch UI exported from SquareLine-based assets
- Local persistence for user configuration and mileage statistics via NVS

- 支持扫描并连接兼容 ELM327 的 BLE OBD 设备
- 实时显示转速、车速、水温/进气温/机油温、机油压力、涡轮压力、节气门、发动机负荷、电压、档位等数据
- 车型选择：各车型独立的传动比（最高 7 挡）、油温策略、涡轮增压，以及按车型锁定协议（如宝马/保时捷强制 ISO 15765-4 CAN）
- 除标准 PID 01 5C 外的厂商油温读取：丰田/斯巴鲁 Mode 21、马自达 Mode 22、MINI/宝马 Mode 22、宝马 F 系 Mode 22 44 02、保时捷 CAN 广播帧 0x441 监听
- 可配置指针表盘页，下滑切换显示的数据源
- 稳健的连接处理：等待 BLE 通知订阅完成后再初始化，每次重连都重新初始化，数据中断时自愈（重初始化 + 自动重连）——上车通电后无需手动重连
- 三连表（ESP-NOW）：主表读 OBD 并广播，从表零额外 OBD 负载同步显示；主/从角色在设置页选择；开机扫表动画同步；从表显示主表名字
- 可选自定义开机图（编译开关）
- 使用 LVGL 和导出 UI 资源实现触控界面
- 通过 NVS 持久化保存用户配置和里程统计数据

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

一个 ELM327 蓝牙适配器只允许一个客户端连接，多块表无法各自直连。因此一块板作**主表**（保持 BLE + ELM327 连接、读取 OBD），其余作**从表**。主表通过 ESP-NOW 广播解析后的数据缓存，从表零额外 OBD 负载渲染。三块板烧同一份固件，在「设置页 → 下滑 → MULTI-GAUGE」选择角色（重启生效）。主表在 ESP32-S3 上蓝牙/WiFi 共存，从表只跑 ESP-NOW。

## Project Layout

- [main/app_main.c](main/app_main.c): application entry, LVGL initialization, BLE startup, task startup
- [main/app_obd_dsp](main/app_obd_dsp): runtime OBD data cache and mileage statistics logic
- [main/bsp_obd_dsp](main/bsp_obd_dsp): board support package, BLE client, NVS, LCD, touch, I2C, IO expander drivers
- [main/export_path](main/export_path): UI source exported from the design tool
- [docs](docs): open-source documentation, bilingual README, structure notes

## Documentation

- 中文说明：[docs/README.zh-CN.md](docs/README.zh-CN.md)
- English documentation: [docs/README.en.md](docs/README.en.md)
- 发布结构说明：[docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)

## Notes

- This repository contains project-specific board adaptation and UI resources; if you port it to another ESP32-S3 board or another vehicle, you will likely need to adjust pin mapping, display settings, and OBD parsing behavior.
- The current parsing and verification focus on Subaru BRZ ZC6. Other vehicles may require protocol, PID, or adapter compatibility adjustments.

- 仓库内包含针对当前开发板的适配代码和 UI 资源；如果迁移到其他 ESP32-S3 开发板或其他车型，需要重新检查引脚定义、屏幕参数和 OBD 解析逻辑。
- 当前解析和验证重点面向 Subaru BRZ ZC6，其他车型可能需要额外调整协议、PID 或适配器兼容性。

## Acknowledgments

- [Hokori23](https://github.com/Hokori23) — performance optimization suggestions and contributions (NVS flush lock hold-time, page-aware refresh cadence, and OBD polling throughput).

- [Hokori23](https://github.com/Hokori23) —— 性能优化建议与贡献者（NVS 落盘锁优化、按页面自适应刷新频率、OBD 轮询效率提升）。
