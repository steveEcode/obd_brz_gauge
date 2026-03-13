# OBD BRZ Gauge

OBD BRZ Gauge is an ESP-IDF based round dashboard project for the Waveshare ESP32-S3-Touch-LCD-1.85 development board. It connects to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders a touch UI with LVGL.

这是一个基于 ESP-IDF 的圆形车载仪表项目，目标硬件为微雪 Waveshare ESP32-S3-Touch-LCD-1.85 开发板。项目通过 BLE 连接兼容 ELM327 的 OBD 适配器，读取车辆数据并使用 LVGL 显示触控界面。

## Status

- Hardware platform: Waveshare ESP32-S3-Touch-LCD-1.85
- Software stack: ESP-IDF 5.1+, LVGL 8
- Protocol path: BLE + ELM327
- Validation status: data is currently tested only on Subaru BRZ ZC6

- 硬件平台：微雪 Waveshare ESP32-S3-Touch-LCD-1.85
- 软件栈：ESP-IDF 5.1+、LVGL 8
- 通信链路：BLE + ELM327
- 当前验证状态：数据目前仅在斯巴鲁 Subaru BRZ ZC6 上完成测试

## Features

- BLE scan and connection for ELM327-compatible adapters
- Real-time display for RPM, speed, coolant temperature, intake temperature, oil temperature, throttle position, fuel level, battery voltage, and related values
- LVGL touch UI exported from SquareLine-based assets
- Local persistence for user configuration and mileage statistics via NVS

- 支持扫描并连接兼容 ELM327 的 BLE OBD 设备
- 实时显示转速、车速、水温、进气温度、机油温度、节气门开度、油量、电压等数据
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
