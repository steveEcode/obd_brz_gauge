# OBD BRZ Gauge 中文说明

> 📖 完整文档索引：[项目主 README](../README.md) · English: [README.en.md](README.en.md)

## 项目简介

OBD BRZ Gauge 是一个基于 ESP-IDF 的车载圆形仪表显示项目，运行在微雪 Waveshare ESP32-S3-Touch-LCD-1.85 开发板上。项目通过 BLE 连接兼容 ELM327 的 OBD 适配器，读取车辆运行数据，并在圆形触摸屏上通过 LVGL 进行可视化展示。

## 当前状态

- 硬件平台：微雪 Waveshare ESP32-S3-Touch-LCD-1.85
- 软件栈：ESP-IDF 5.5.3、LVGL 8
- 通信链路：BLE + ELM327（标准 OBD PID；只有 ZN/C6 CAN 使用 CAN 广播帧 ATMA 监听）
- 已内置车型（12 个）：OBD2 Generic、ZN/C6 CAN、ZN/C6 PID、ZD8 OBD、ZD8、MX-5 ND、BMW F/G、BMW G OBD、JCW F56、POS 997.2、POS 997.1、GIULIA 2.0T
- 三连表：一主多从，通过 ESP-NOW 联动
- 当前验证状态：已在斯巴鲁 BRZ ZN/C6 上完整验证；其余车型已配置，部分仍需上车验证

## 功能概览

- 支持扫描并连接兼容 ELM327 的 BLE OBD 设备
- **CAN 广播帧 ATMA 监听**：只有 ZN/C6 (Gen1) 绕过标准 OBD PID 轮询——0x140（节气门）+ 0x360（油温+水温）；其余车型都保持标准 OBD 轮询
- **ELM327 单线程轮询**：不再同时调 OBD 和 CAN；只有 ZN/C6 CAN 走 ATMA，其余车型全部 OBD-only，避免适配器抢占和数据延迟
- **自定义开机图 & 开机动画**：可配置开机 Logo，支持多 Block 动画播放，SPIFFS 分区挂载 bootmedia 实现丰富开机流程
- **统一车辆配置系统**：编译时 `vehicle_custom_config.h` 管理各车型阈值、报警、表盘范围
- 实时显示转速、车速、水温/进气温/机油温、机油压力、涡轮压力、节气门、发动机负荷、电压、档位（优先用 CAN 直接解码的精确档位，无效时回退转速/车速估算）等数据
- 蓝牙/ESP-NOW 数据断开超时后，仪表页显示 "NO SIGNAL" 提示
- 车型选择：各车型独立的传动比（最高 8 挡）、油温策略、涡轮增压，以及按车型锁定协议（需要时启用）
- 除标准 PID 01 5C 外的厂商油温读取：丰田/斯巴鲁 Mode 21、马自达 Mode 22、MINI/宝马 Mode 22、宝马 F 系 Mode 22 44 02
- 可配置指针表盘页，下滑切换显示的数据源
- **转速超限闪烁报警**：可设阈值，超限时背景图红色闪烁
- **刹车温度 / 油压报警节流**：两项报警限制为 30 秒一次，减少刷屏
- **三连表开机动画**：三块板通过 ESP-NOW 同步依次显示 "RACE / AS / ONE"
- 稳健的连接处理：等待 BLE 通知订阅完成后再初始化，每次重连都重新初始化，数据中断时自愈（重初始化 + 自动重连）——上车通电后无需手动重连
- 三连表（ESP-NOW）：主表读 OBD 并广播，从表零额外 OBD 负载同步显示；主/从角色在设置页选择；开机扫表动画同步；从表显示主表名字
- 可选自定义开机图（编译开关）
- 使用 LVGL 和导出 UI 资源实现触控界面
- 用户配置通过 NVS 持久化；里程/行程统计只在运行时内存里累计（每次开机清零，不再写 flash）

## 三连表说明

一个 ELM327 蓝牙适配器只允许一个客户端连接，多块表无法各自直连。因此一块板作**主表**（保持 BLE + ELM327 连接、读取 OBD），其余作**从表**。主表通过 ESP-NOW 广播解析后的数据缓存，从表零额外 OBD 负载渲染。三块板烧同一份固件，在「设置页 → 下滑 → MULTI-GAUGE」选择角色（重启生效）。主表在 ESP32-S3 上蓝牙/WiFi 共存，从表只跑 ESP-NOW。

配对走的是真蓝牙，不是"盲收广播绑定"：主表设为 MASTER 后会广播 `SkyGauge-XXYY`（带设备后缀，赛道日多车同时用也不会互相干扰）。从表未配对时开机会直接进入 "FIND MASTER" 扫描页（和选 OBD 适配器共用同一个页面），从列表里选中要跟随的主表即可，之后每次开机都会自动跳过扫描直接显示仪表数据。

## 目录说明

- [main/app_main.c](../main/app_main.c)：程序入口，负责初始化硬件、LVGL、UI 和 BLE 任务
- [main/app_obd_dsp](../main/app_obd_dsp)：业务层 OBD 数据缓存、车辆配置、CAN 解码器、里程统计、开机动画播放
- [main/bsp_obd_dsp](../main/bsp_obd_dsp)：板级支持包，包括 BLE 客户端、NVS、LCD、触摸、I2C（ESP-IDF 5.5 新 API）、IO 扩展器、ESP-NOW 链路等驱动
- [main/export_path](../main/export_path)：UI 设计导出代码、字体、图片和页面逻辑（SquareLine 导出）
- [bootmedia](../bootmedia)：开机动画媒体块（SPIFFS 分区源文件）
- [model](../model)：开源 3D 打印模型（外壳、表座、支架）
- [tools](../tools)：辅助脚本（图片转换、开机 Block 构建等）
- [firmware/release](../firmware/release)：预编译固件二进制文件
- [managed_components](../managed_components)：ESP-IDF 组件管理器自动拉取的依赖

## 依赖环境

- ESP-IDF 5.5.3 或更高版本
- Python 环境与 ESP-IDF 工具链
- USB 数据线
- 兼容 ELM327 的 BLE OBD 适配器

组件依赖由 [main/idf_component.yml](../main/idf_component.yml) 管理，当前可见依赖包括：

- lvgl/lvgl
- espressif/esp_lcd_touch
- espressif/button
- espressif/knob

## 编译与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

如果是首次构建，ESP-IDF 可能会自动下载并安装缺失的组件到 managed_components 目录。

## 预编译固件

`firmware/release/` 目录下提供可直接烧录的预编译二进制文件：

| 文件 | 说明 |
|------|------|
| `bootloader/bootloader.bin` | 引导加载程序 |
| `partition_table/partition-table.bin` | 分区表 |
| `ota_data_initial.bin` | OTA 数据初始分区 |
| `obd_brz_gauge.bin` | 应用程序固件 |
| `bootmedia.bin` | 开机动画媒体分区 |
| `flash_address_map.txt` | 烧录地址映射表 |

使用 esptool 一键烧录：

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 firmware/release/bootloader/bootloader.bin \
  0x8000 firmware/release/partition_table/partition-table.bin \
  0xf000 firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin \
  0x420000 firmware/release/bootmedia.bin
```

## 开发与适配说明

1. 这个项目是针对微雪 ESP32-S3-Touch-LCD-1.85 做的板级适配。
2. 如果你更换了屏幕、触摸芯片、IO 扩展器或引脚定义，需要重点检查 [main/bsp_obd_dsp](../main/bsp_obd_dsp) 下的驱动实现。
3. 如果你更换了车辆或 OBD 适配器，需要重新验证 BLE 服务、特征值、命令格式和返回数据解析。
4. UI 资源来自 [main/export_path](../main/export_path)，后续可继续调整页面布局、字体和动画。

## 更新日志

见 [CHANGELOG.md](../CHANGELOG.md) —— 中英双语的单一来源。
以前这份日志在三个 README 里各存一份，容易改漏。

## 已知限制

- 当前重点验证了 Subaru BRZ ZC6 的数据读取情况
- 不保证所有 ELM327 兼容设备都能稳定工作
- 不保证所有车型的 PID 与返回格式一致
- 若使用不同版本的 ESP-IDF，可能需要对 BSP 或组件依赖进行小幅调整

## 3D 模型（开源）

`model/` 目录包含开源的 3D 打印模型文件：

| 文件 | 说明 |
|------|------|
| `model/esp32_1.85_weixue/housing.stl` | ESP32-S3-Touch-LCD-1.85 开发板外壳 |
| `model/Subaru/brz_zc6/triple_gauge_pod.stp` | BRZ ZC6 三连表底座（3× 1.85" 圆形） |
| `model/Subaru/brz_zc6/passenger_dashboard_scan.stl` | BRZ ZC6 副驾仪表台扫描件（ fitting 参考） |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND 出风口安装支架 |

英文说明见 [README.en.md](README.en.md)，项目根目录 README 见 [../README.md](../README.md)。
