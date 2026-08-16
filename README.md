# OBD BRZ Gauge

A round ESP-IDF car gauge for the Waveshare ESP32-S3-Touch-LCD-1.85. It connects
to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders a
touch UI with LVGL.

基于 ESP-IDF 的圆形车载仪表，硬件为微雪 Waveshare ESP32-S3-Touch-LCD-1.85。
通过 BLE 连接兼容 ELM327 的 OBD 适配器读取车辆数据，用 LVGL 渲染触控界面。

**[效果演示 / Demo video](https://www.douyin.com/video/7614174567678984187)**

> Based on [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp)
> by zhaizhaitao ([Bilibili demo](https://www.bilibili.com/video/BV18oHXz6EiQ/)).
> This is a derivative with additional vehicle profiles, multi-gauge support and
> a theming system.
>
> 本项目基于 [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp)
> 二次开发，原作者 zhaizhaitao（[B 站演示](https://www.bilibili.com/video/BV18oHXz6EiQ/)）。
> 本仓库新增了多车型适配、三连表联动和主题系统。

---

## 📖 Documentation / 文档索引

**Start here / 从这里开始**

| Document | What it covers / 内容 |
|----------|----------------------|
| [docs/README.zh-CN.md](docs/README.zh-CN.md) | **完整中文说明** — 功能、依赖、编译烧录、适配要点 |
| [docs/README.en.md](docs/README.en.md) | **Full English guide** — features, requirements, build and flash |
| [firmware/README.md](firmware/README.md) | Pre-built binaries and flash addresses / 预编译固件与烧录地址 |
| [CHANGELOG.md](CHANGELOG.md) | Changelog / 更新日志（中英双语） |

**Vehicles & OBD / 车辆适配与 OBD**

| Document | What it covers / 内容 |
|----------|----------------------|
| [docs/VEHICLE_CONFIG.md](docs/VEHICLE_CONFIG.md) | **Adding a vehicle profile** / 新增车型看这篇（中英对照） |
| [docs/OBD_TROUBLESHOOTING.md](docs/OBD_TROUBLESHOOTING.md) | No data / won't connect — diagnosis / 连不上或没数据时的排查 |
| [docs/AUTO_PROTOCOL_DETECTION.md](docs/AUTO_PROTOCOL_DETECTION.md) | Protocol auto-detection: usage, FAQ, debugging / 自动协议检测：用法、常见问题、调试 |
| [docs/BRZ_ZD8_PROTOCOL_GUIDE.md](docs/BRZ_ZD8_PROTOCOL_GUIDE.md) | BRZ ZD8 (Gen2) protocol diagnosis / ZD8 协议诊断 |

**UI themes / 界面主题**

| Document | What it covers / 内容 |
|----------|----------------------|
| [themes/README.md](themes/README.md) | **Build a theme** — no C code needed / 做一套主题（中英双语） |
| [docs/THEMING.md](docs/THEMING.md) | Theme framework internals / 主题框架内部实现 |

Repository layout is in [this README](#repository-layout--目录结构) below.
目录结构见本文下方的[目录结构](#repository-layout--目录结构)一节。

---

## Status / 当前状态

| | |
|---|---|
| Hardware / 硬件 | Waveshare ESP32-S3-Touch-LCD-1.85 (360×360 round, 16 MB flash, 8 MB PSRAM) |
| Stack / 软件栈 | ESP-IDF 5.5.3, LVGL 8 |
| Link / 通信链路 | BLE + ELM327 — standard OBD PID; only ZN/C6 CAN keeps ATMA monitoring |
| Multi-gauge / 三连表 | One master + multiple slaves over ESP-NOW / 一主多从，ESP-NOW 联动 |
| Verified on / 已验证 | Subaru BRZ ZN/C6 (fully) — ZN/C6 CAN is the only CAN-backed profile; other profiles are OBD-only / 其余车型已配置，部分仍需上车验证 |

**Vehicle profiles / 内置车型** (12) — full list in
[vehicle_profiles.c](main/app_obd_dsp/vehicle_profiles.c), selectable in Settings:

`OBD2 Generic` · `ZN/C6 CAN` · `ZN/C6 PID` · `ZD8 OBD` · `ZD8` · `MX-5 ND` ·
`BMW F/G` · `BMW G OBD` · `JCW F56` · `POS 997.2` · `POS 997.1` · `GIULIA 2.0T`

## Highlights / 主要特性

- **CAN broadcast monitoring** — only `ZN/C6 CAN` bypasses PID polling for
  high-rate channels; the rest stay on OBD-only polling. / **CAN 广播帧监听** —— 仅 `ZN/C6 CAN` 使用高速监听，其余车型保持 OBD 轮询。
- **Single-thread ELM327 loop** — no mixed OBD/CAN parallel path; only `ZN/C6 CAN`
  uses ATMA, so the adapter does not get contended by dual polling. /
  **ELM327 单线程轮询** —— 不再混跑 OBD/CAN；只有 `ZN/C6 CAN` 走 ATMA，避免适配器抢占和数据延迟。
- **Brake-temp / oil-pressure alarm throttle** — these two alarms are rate-limited
  to once every 30 seconds. / **刹车温度 / 油压报警节流** —— 两项报警限制为 30 秒一次，减少刷屏。
- **Multi-gauge over ESP-NOW** — one board reads OBD and broadcasts; the others
  display with zero extra OBD load, paired over real BLE. /
  **三连表** —— 主表读 OBD 广播，从表零额外负载显示，走真蓝牙配对。
- **Data-driven themes** — a theme is a folder with a manifest plus optional
  artwork; no C code. / **配置驱动主题** —— 一个文件夹 + 一份清单，不用写 C。
- **RPM warning, incl. linked mode** — three gauges light up in sequence as revs
  climb. / **转速报警（含联动模式）** —— 三块表随转速依次亮起。
- Manufacturer oil-temp paths beyond PID 01 5C (Mode 21/22, Mazda, MINI/BMW),
  per-vehicle protocol lock, gear from CAN when available.
- Self-healing BLE: re-initializes on reconnect and recovers when data stalls —
  no manual reconnect after ignition. / 数据中断自愈，上车通电无需手动重连。

Full feature lists: [中文](docs/README.zh-CN.md#功能概览) ·
[English](docs/README.en.md#features)

## Quick Start / 快速开始

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Requires ESP-IDF 5.1+. Flashing pre-built binaries instead:
[firmware/README.md](firmware/README.md).
需要 ESP-IDF 5.1 以上。想直接烧预编译固件见 [firmware/README.md](firmware/README.md)。

> ⚠️ The bootmedia flash address changed from `0x620000` to **`0x420000`**.
> Update older scripts. / 烧录地址已变更，旧脚本需同步修改。

## Repository Layout / 目录结构

| Path | Contents |
|------|----------|
| [main/app_main.c](main/app_main.c) | Entry point: hardware, LVGL, BLE and task startup |
| [main/app_obd_dsp](main/app_obd_dsp) | OBD data cache, vehicle profiles, CAN decoders, boot media |
| [main/bsp_obd_dsp](main/bsp_obd_dsp) | Board support: BLE, NVS, LCD, touch, I2C, IO expander, ESP-NOW |
| [main/export_path](main/export_path) | LVGL UI (SquareLine export) + theme framework |
| [themes](themes) | Theme manifests and artwork / 主题清单与素材 |
| [bootmedia](bootmedia) | Boot animation blocks (SPIFFS partition source) |
| [tools](tools) | Helper scripts: theme codegen, image conversion, boot blocks |
| [firmware/release](firmware/release) | Pre-compiled binaries |
| [model](model) | 3D printable housings, gauge pods and brackets |
| [docs](docs) | Documentation (see index above) |

## 3D Models / 开源模型

| File | Description |
|------|-------------|
| `model/esp32_1.85_weixue/housing.stl` | Board housing / 开发板外壳 |
| `model/Subaru/brz_zc_n6/triple_gauge_pod.stp` | BRZ ZN/C6 triple gauge pod / 三连表底座 |
| `model/Subaru/brz_zc_n6/passenger_dashboard_scan.stl` | BRZ ZN/C6 passenger dash scan (fitting reference) / 副驾仪表台扫描件 |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND air vent bracket / 出风口支架 |

## Porting Notes / 适配提醒

This repository contains board-specific adaptation and UI resources. Porting to
another ESP32-S3 board means revisiting pin mapping and display settings
([main/bsp_obd_dsp](main/bsp_obd_dsp)); porting to another car means
re-verifying BLE services, PIDs and response parsing
([docs/VEHICLE_CONFIG.md](docs/VEHICLE_CONFIG.md)).

仓库内是针对当前开发板的适配代码和 UI 资源。换开发板需要重新检查引脚定义和屏幕参数；
换车型或适配器需要重新验证 BLE 服务、PID 和返回数据解析。

## Acknowledgments / 致谢

- [Hokori23](https://github.com/Hokori23) — performance optimization suggestions
  and contributions (NVS flush lock hold-time, page-aware refresh cadence, OBD
  polling throughput). / 性能优化建议与贡献。
- [timurrrr/ft86](https://github.com/timurrrr/ft86) — comprehensive FT86 CAN bus
  documentation (Gen1/Gen2 CAN ID mappings and byte-level decoding formulas),
  which made the CAN broadcast monitoring possible. /
  提供了完整的 FT86 CAN 总线文档，使 CAN 广播帧监听得以实现。
