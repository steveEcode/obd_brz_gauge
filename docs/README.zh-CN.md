# OBD BRZ Gauge 中文说明

## 项目简介

OBD BRZ Gauge 是一个基于 ESP-IDF 的车载仪表显示项目，运行在微雪 Waveshare ESP32-S3-Touch-LCD-1.85 开发板上。项目通过 BLE 连接兼容 ELM327 的 OBD 设备，读取车辆运行数据，并在圆形触摸屏上进行可视化展示。

当前工程包含完整的主程序入口、BLE OBD 通信、板级驱动、LVGL 图形界面以及 NVS 配置与统计逻辑，适合作为个人开源项目继续维护和扩展。

## 当前状态

- 开发板：Waveshare ESP32-S3-Touch-LCD-1.85
- 芯片：ESP32-S3
- 框架：ESP-IDF 5.1+
- 图形库：LVGL 8
- 通信方式：BLE 连接 ELM327 OBD 适配器
- 当前仅验证车型：斯巴鲁 Subaru BRZ ZC6

说明：目前“数据可用性”只在 Subaru BRZ ZC6 上做过实际测试。其他车型、其他 OBD 适配器、以及不同年份或不同 ECU 配置的车辆，均需要重新验证。

## 功能概览

- 启动后初始化 LCD、触摸、LVGL、NVS 和 BLE 模块
- 支持 BLE 扫描与连接兼容 ELM327 的 OBD 设备
- 支持展示发动机转速、车速、水温、进气温度、机油温度、发动机负荷、节气门开度、燃油液位、电池电压等数据
- 支持用户配置持久化保存
- 支持里程、运行时间等统计信息记录
- 包含导出的 UI 资源，便于后续继续调整界面

## 目录说明

- [main/app_main.c](../main/app_main.c)：程序入口，负责初始化硬件、LVGL、UI 和 BLE 任务
- [main/app_obd_dsp](../main/app_obd_dsp)：业务层数据缓存、档位推算、里程统计等逻辑
- [main/bsp_obd_dsp](../main/bsp_obd_dsp)：板级支持包，包括 BLE 客户端、NVS、LCD、触摸、I2C、IO 扩展器等驱动
- [main/export_path](../main/export_path)：UI 设计导出代码、字体、图片和页面逻辑
- [managed_components](../managed_components)：ESP-IDF 组件管理器自动拉取的依赖
- [tools](../tools)：资源转换等辅助脚本

## 依赖环境

- ESP-IDF 5.1 或更高版本
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

## 开发与适配说明

1. 这个项目是针对微雪 ESP32-S3-Touch-LCD-1.85 做的板级适配。
2. 如果你更换了屏幕、触摸芯片、IO 扩展器或引脚定义，需要重点检查 [main/bsp_obd_dsp](../main/bsp_obd_dsp) 下的驱动实现。
3. 如果你更换了车辆或 OBD 适配器，需要重新验证 BLE 服务、特征值、命令格式和返回数据解析。
4. UI 资源来自 [main/export_path](../main/export_path)，后续可继续调整页面布局、字体和动画。

## 已知限制

- 当前只验证了 Subaru BRZ ZC6 的数据读取情况
- 不保证所有 ELM327 兼容设备都能稳定工作
- 不保证所有车型的 PID 与返回格式一致
- 若使用不同版本的 ESP-IDF，可能需要对 BSP 或组件依赖进行小幅调整

## 开源发布建议

在正式公开发布前，建议补充以下内容：

- 选择并添加开源许可证
- 补充实物照片、接线说明和界面截图
- 说明测试所使用的 OBD 设备型号
- 标明哪些 PID 是通用 OBD，哪些可能是当前车型相关逻辑

英文版说明见 [docs/README.en.md](README.en.md)。