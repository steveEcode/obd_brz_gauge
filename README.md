# OBD BRZ Gauge

[English](README.en.md) | 简体中文

基于 ESP-IDF 的圆形车载仪表。硬件为微雪 Waveshare ESP32-S3-Touch-LCD-1.85，
通过 BLE 连接兼容 ELM327 的 OBD 适配器读取车辆数据，用 LVGL 渲染触控界面。

**[效果演示 / Demo video](https://www.douyin.com/video/7614174567678984187)**

> 基于 [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp)
> 二次开发（[B 站演示](https://www.bilibili.com/video/BV18oHXz6EiQ/)）。
> 本仓库新增了多车型适配、三连表联动和主题系统。

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/USER_GUIDE.md](docs/USER_GUIDE.md) | **使用指南** —— 页面导航、设置、三连表配对、开机动画、App 升级 |
| [docs/FLASH.md](docs/FLASH.md) | **烧录与升级** —— 分区表、USB 烧录、App OTA 与回滚 |
| [docs/APP_PROTOCOL.md](docs/APP_PROTOCOL.md) | **App 对接协议** —— OTA 模式的 WiFi HTTP API |
| [docs/VEHICLES.md](docs/VEHICLES.md) | **车型适配** —— JSON 车型档案（18 个，固件仅内置 1 个）、安装/激活、新增车型教程 |
| [docs/PID_GUIDE.md](docs/PID_GUIDE.md) | **PID / 信号定义指南** —— 怎么把挖到的 PID/CAN 位写成 RPN 公式，含真实例子 |
| [docs/THEMES.md](docs/THEMES.md) | **主题制作** —— 两套主题系统、素材规格、主题商店 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | **故障排查** —— 连不上 / 没数据、协议自动检测 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | **开发指南** —— 代码架构、编译配置、工具脚本 |
| [CHANGELOG.md](CHANGELOG.md) | 更新日志 |

## 硬件与软件栈

| | |
|---|---|
| 硬件 | Waveshare ESP32-S3-Touch-LCD-1.85（360×360 圆屏 ST77916，16 MB Flash，8 MB PSRAM）|
| 软件栈 | ESP-IDF 5.5.3，LVGL 8 |
| 通信 | BLE 连接 ELM327 兼容 OBD 适配器（标准 PID + 各厂商私有 Mode 21/22）|
| 三连表 | 一块主表读 OBD，其余从表通过 ESP-NOW 零负载同步显示 |
| 传感器 | ADS1115 油压 ADC（V1 板，部分车型可用 OBD 直读油压替代）|

## 车型适配（18 个档案，固件仅内置 1 个）

固件只内置 `OBD2 Generic`（标准 OBD2）。其余 17 个车型是 JSON 档案，
从 SD 卡（`/OBD/IMPORT`）、配套 App（经 `/api/v1/vehicles` 接口）或车机
**Vehicle 页**安装后，激活并
重启生效。完整的目录表格、安装步骤、行为变化见
[docs/VEHICLES.md](docs/VEHICLES.md)：

`OBD2 Generic` · `ZN/C6 CAN` · `ZN/C6 PID` · `ZD8 OBD` · `ZD8` · `MX-5 ND` ·
`BMW F/G` · `Supra A90` · `BMW G OBD` · `BMW E` · `JCW F56` · `MINI R55` ·
`POS 997.2` · `POS 997.1` · `POS 987.1` · `GIULIA 2.0T` · `jeep` · `Honda Integra`

当前仅在 Subaru BRZ ZN/C6 上完整验证；其余车型已配置，部分仍需上车验证。

可编辑配置另见 [vehicles/import/](vehicles/import/README.md)：**19 份独立导入模板**
（现有 18 份的 `custom.*` 副本 + BMW G 直接挡位），明确请求头、位解码和轮询间隔。
将选中的 JSON 放进 SD 卡 `OBD/IMPORT/`，安装后激活并重启。

新增车型可以只写特有信号，未声明的 10 个标准信号默认走 OBD2；支持单项
`disabled: true` 和根字段 `defaults: "none"`。最小配置与 BMW G 精简挡位示例见
[vehicles/examples/](vehicles/examples/README.md)。

## 核心特性

- **JSON 车型档案系统**：18 个档案，固件仅内置 `OBD2 Generic`，其余经 SD/App/Vehicle 页安装+激活+重启启用；各车型独立传动比（最高 8 挡）、油温读取策略、协议锁定、功能寻址；挡位优先用 CAN/DID 直读，无效时按传动比估算
- **CAN 广播帧监听**：仅 `ZN/C6 CAN` 走 ATMA 高速通道（0x140 节气门、0x360 油温水温）；其余车型保持单线程 OBD 轮询，避免适配器抢占
- **厂商油温路径**：丰田/斯巴鲁 Mode 21、马自达 / MINI / BMW Mode 22、FCA UDS 扩展寻址等，主策略失败自动回退
- **三连表（ESP-NOW）**：主表广播、从表零额外 OBD 负载，走真蓝牙配对（主表广播 `SkyGauge-XXYY`），断电记忆
- **转速报警（含联动模式）**：超阈值全屏红色闪烁；三连表模式按表位依次亮起
- **两套主题系统**：编译期 TOML 主题（改颜色/素材）与 SD 运行时主题（自定义表盘页面布局），见 [docs/THEMES.md](docs/THEMES.md)
- **App OTA**：配套手机 App 走 WiFi 热点传输，SHA256 校验 + 15 秒开机自检失败自动回滚
- **开机动画**：OFF / RACE / VIDEO 三模式，VIDEO 模式可从手机 App 上传自制动画（360×360 圆形安全区）
- **SD 卡**：支持 FAT32 与 exFAT（exFAT 是独立文件系统；固件不会自动格式化存储卡）
- **RaceChrono BLE 输出**：可作为 RaceChrono DIY 设备向手机 App 提供数据
- **自愈连接**：数据中断自动重初始化重连；自动协议保留自动设置，无有效数据时重新探测，兼容 11/29 位 CAN、J1850 和 K 线响应；默认 `0100` 探测失败时，在安全同步后用已配置的转速请求 `010C` 再检查一次

## 快速开始

### 方式一：烧录预编译固件

需要：开发板、USB 数据线、[esptool.py](https://github.com/espressif/esptool)（`pip install esptool`）。

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     firmware/release/bootloader/bootloader.bin \
  0x8000  firmware/release/partition_table/partition-table.bin \
  0xf000  firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin
```

- `PORT` 换成你的串口（Windows `COM3`，Linux `/dev/ttyUSB0`，macOS `/dev/cu.usbserial-*`）
- 这条命令不会动 NVS 用户设置和蓝牙绑定；首次开机的一次性迁移细节见 [docs/FLASH.md](docs/FLASH.md)
- 自定义图片、字体、主题视频和独立开机动画均存 SD；主题包放 `/OBD/THEMES/<key>/theme.bin`，开机动画由网页上传到公共 `/OBD/BOOT/`，旧 `bootmedia.bin` 可复制为 `/OBD/BOOT/bootmedia.raw`，无卡使用内置界面
- 完整分区布局见 [docs/FLASH.md](docs/FLASH.md)，烧录地址以 [partitions.csv](partitions.csv) 为准

### 方式二：源码编译

需要 [ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/) 环境：

```bash
git clone https://github.com/steveEcode/obd_brz_gauge.git
cd obd_brz_gauge
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

首次构建会自动下载组件依赖到 `managed_components/`；换开发板编译前先在
`idf.py menuconfig → OBD DSP Configuration` 里选硬件版本（见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)）。

## 仓库目录

```
main/
├── app_main.c          # 入口：硬件、LVGL、BLE 与任务启动
├── app_obd_dsp/        # 应用层：OBD 缓存、SD 导入/激活、OTA、开机动画、设备身份
├── bsp_obd_dsp/        # 板级：ELM327 BLE 客户端、ESP-NOW、LCD/触摸/IO 扩展、NVS
├── export_path/        # LVGL UI（SquareLine 导出）：20+ 页面、字体、图片
├── theme_engine/       # 运行时主题加载器（SD theme.bin）
└── vehicle/            # 车型档案管理（vp_manager，JSON 校验/编译/生命周期）
themes/                 # 编译期主题源文件（TOML 清单 + 素材）
bootmedia/              # 开机动画源文件
vehicles/catalog/       # 车型档案目录（JSON，不编译进固件，见 docs/VEHICLES.md）
vehicles/import/        # 可编辑 SD 导入包（18 份 custom.* JSON 模板）
tools/                  # 工具脚本（主题生成、打包、PID 挖掘、发布）
firmware/release/       # 预编译固件
android_app/            # 配套手机 App（APK）
model/                  # 3D 打印模型（外壳、表座、支架）
docs/                   # 文档（见上方索引）
partitions.csv          # Flash 分区表（16MB）
```

## 3D 打印模型

| 文件 | 说明 |
|------|------|
| `model/esp32_1.85_weixue/housing.stl` | 开发板外壳 |
| `model/Subaru/brz_zc6/triple_gauge_pod.stp` | BRZ ZN/C6 三连表底座 |
| `model/Subaru/brz_zc6/passenger_dashboard_scan.stl` | BRZ ZN/C6 副驾仪表台扫描件（拟合参考）|
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND 出风口支架 |

## 已知限制

- 仅 Subaru BRZ ZN/C6 完整验证过数据读取；不保证所有 ELM327 兼容设备稳定工作，不保证各车型 PID 返回格式一致
- 油压报警限制为 30 秒提示一次
- 里程 / 行程统计只在运行时内存累计，每次开机清零
- 主题切换需重启生效（所有页面仅在开机时创建一次）

## 致谢

- [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp) —— 上游项目
- [Hokori23](https://github.com/Hokori23) —— 性能优化建议与贡献（NVS 刷盘锁、分页刷新节奏、OBD 轮询吞吐）
- [timurrrr/ft86](https://github.com/timurrrr/ft86) —— 完整的 FT86 CAN 总线文档，使 CAN 广播监听得以实现

## 许可证

本项目采用 **GPLv3** 开源协议，见 [LICENSE](LICENSE)。可以自由使用、修改和分发；
分发修改版本时必须同样以 GPLv3 开源。

```
Copyright (C) 2024-2026  steveEcode and contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
```

通用配置优先用 `0170` 读取增压压力，并以 `010B` MAP 估算回退；水温／油温显示上限为 255°C，转速、车速、涡轮压力和节气门按页面需求优先采集。旧的已安装配置需重新导入才能应用显式范围修复，详见 [车型文档](docs/VEHICLES.md)。
