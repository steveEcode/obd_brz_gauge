# 开发指南

[English](DEVELOPMENT.en.md) | 简体中文

改车型看 [VEHICLES.md](VEHICLES.md)，PID / 信号定义怎么写看 [PID_GUIDE.md](PID_GUIDE.md)，
改主题看 [THEMES.md](THEMES.md)，烧录升级看 [FLASH.md](FLASH.md)，
App 对接协议看 [APP_PROTOCOL.md](APP_PROTOCOL.md)，
性能审计、基准和实机统计看 [PERFORMANCE.md](PERFORMANCE.md)。

## 代码架构

```
main/
├── app_main.c            # 入口：启动顺序见下方「启动顺序」
│
├── app_obd_dsp/          # 应用层（与板子无关的业务）
│   ├── signal_registry / signal_store  # 运行时信号表（PSRAM；contracts/signals.json 生成，见下）
│   ├── obd_data_cache    # 旧接口信号门面，内部转发到 signal_store（新代码直接用 signal_store）
│   ├── storage_service   # SD 1Hz 轮询调度：导入扫描 / 配置镜像 / 主题写入任务
│   ├── sd_import / inbox_import  # /OBD/IMPORT 落地文件处理（*.vehicle.json / *.gaugeproj.json
│   │                     #   / *.bin / ACTIVATE.TXT / RESET.TXT），结果写 IMPORT.LOG
│   ├── first_boot_migration  # 分区 v1→v2 一次性迁移（旧 NVS 车型号→catalog id、旧主题/
│   │                     #   开机动画搬到 SD、config 分区格式化+SD 镜像恢复）
│   ├── theme_library     # SD 主题库枚举（/OBD/THEMES/<key>/theme.bin）与后台选择状态机
│   ├── theme_mount / boot_media_mount  # 旧 theme_0 / bootmedia 迁移访问
│   ├── coredump_store    # 崩溃转储读取（GET /ota/coredump；无转储返回 204）
│   ├── ota_wifi_server   # WiFi SoftAP HTTP 服务：/ota/* 端点（App 协议不变）
│   ├── api_v1_handlers / device_web_page.html  # 新 /api/v1/* + 设备自带网页（仅 OTA 模式）
│   ├── device_identity   # 设备清单 JSON（GET /ota/info，硬件 + build_tag）
│   └── flash_writer / upload_sink / storage_util  # 旧分区擦除及文件写入工具
│
├── bsp_obd_dsp/          # 板级支持包（换开发板主要改这里）
│   ├── elm327_ble_client # ELM327 BLE 客户端 + 单线程轮询状态机 + 协议自动检测
│   ├── tpms_ble_client   # TireKing FBB0 扫描解码与四轮 ID 绑定（解析器 tpms_fbb0）
│   ├── espnow_link / espnow_v6  # 三连表 ESP-NOW 链路（v6 TLV，兼容过渡期接受 v5）
│   ├── ble_hub           # GAP/GATTC/GATTS 事件按 app_id 统一分发、扫描与连接仲裁
│   ├── racechrono_ble_diy # RaceChrono DIY BLE GATT 服务 + 配对服务
│   ├── gauge_pair_ble_client # 从表 FIND MASTER 扫描（与 OBD 扫描共用页面）
│   ├── nvs_app / nvs_storage  # 应用 NVS（nvs_app 分区）与用户配置 NVS（新字段只能追加到末尾）
│   ├── config_part       # config 分区（LittleFS）挂载辅助
│   ├── sd_card           # SDMMC 1-bit 挂载（V1；无卡槽的板选 `OBD_SD_SLOT_NONE`，全部 SD 功能报 no card）
│   ├── ads1115_oil_pressure # ADS1115 I2C 油压 ADC（仅 V1 板；车型 profile 有 OBD 油压时不用它）
│   ├── lcd_driver/       # ST77916 QSPI LCD（V2/V3 板 init 序列不同）
│   ├── touch_driver/     # CST816 触摸（I2C）
│   ├── exio/             # TCA9554 IO 扩展（仅 V1 板）
│   └── i2c_driver/       # I2C 主驱动
│
├── vehicle/              # 车型运行时核心（纯 C，无 ESP-IDF 依赖，host 可编译）
│   ├── vp_engine         # 请求调度 + 应答解码引擎，驱动 ELM327 单线程状态机
│   ├── vp_manager        # 生命周期 INSTALLED→PENDING→ACTIVE；验车窗口、回滚、安全模式
│   ├── vp_compile / vp_validate  # JSON → 编译态 profile；服务号/AT 白名单校验
│   ├── vp_rpn / vp_rpn_json  # 安全 RPN 执行器（固定操作码、≤32 条指令、栈深 ≤8）
│   └── vp_builtin_generic  # 唯一内置车型 `obd2.generic`（由 tools/vehicles/gen_builtin.py 生成）
│
├── media/                # 开机 / 主题视频
│   ├── boot_video / video_player / video_view / video_elem
│   ├── obv1_parser.c, obv1_format.h  # OBV1 MJPEG 容器（tools/make_video.py 生成）
│   ├── delta_varint      # 旧 SD bootmedia 格式解码（向后兼容）
│   └── media_jpeg        # esp_new_jpeg 封装
│
├── theme_engine/         # 主题引擎（编辑器 theme.bin → 页面/颜色/素材/数据绑定）
│   ├── theme_loader / theme_page / theme_layout / theme_overlay
│   ├── theme_sidecar* / theme_pixconv / theme_jsplit / theme_img_cache
│   └── rules/            # 规则引擎（tr_parse/tr_eval/tr_format；RPM 自定义闪烁等现在是规则）
│
└── export_path/          # LVGL UI（SquareLine 导出 + 手写扩展）
    ├── ui.c              # 页面创建顺序、手势导航（轮播环）、联动报警逻辑
    ├── screens/          # 23 个页面：Logo/Gear/ThemeGauge/Rpm/Speed/ODBProtocal/
    │                     #   EasterEgg(版本页)/BLEScan/OTAMode/Temp/TempCustom/
    │                     #   OilPressure(曲线页)/ChartConfig/ChartAlarm/Info/InfoCustom/
    │                     #   Settings/RpmWarn/Needle/NeedleConfig/MultiGauge/
    │                     #   Intro/Vehicle
    ├── ui_tpms.c         # 手写四轮胎压总览和完整诊断详情
    ├── ui_bluetooth.c    # 手写蓝牙配对菜单与四轮胎压绑定页
    ├── ui_ext.c          # 手写扩展（扫表动画、展厅模式、开机动画调度、主题选择状态轮询）——
    │                     #   SquareLine 重导出时不会覆盖
    ├── ui_sweep.c        # 自检扫表单调时钟（16 ms 显示 / 40 ms 同步槽；无 LVGL 依赖）
    ├── ui_theme_navigation.c # 主题切页后台图片准备 / LVGL 发布与取消（单个在途请求）
    ├── ui_disp_item.c    # 显示项枚举（TEMP/INFO 页可映射的数据源；只能追加，见 AGENTS.md）
    └── ui_theme*         # 编译期主题运行时（活动主题、NVS、访问器；生成文件见 THEMES.md）
```

仓库顶层还有：`vehicles/catalog/`（18 个 `*.vehicle.json` 车型，固件只内置
`obd2.generic`，其余按需从 SD / App / 车型页安装）、`contracts/`（信号表、
车型 JSON schema、规则引擎黄金向量）、`test/host/`（9 套纯 C 单元测试）、
`tools/vehicles/`（旧车型表→JSON 转换器与 golden fixture 生成器）、
`bootmedia/slot_a/`（legacy 开机动画源，生成 SD 用的 `build/bootmedia.bin`）。

当前 I2C 定义分为两套：主总线在 V1 使用 SCL GPIO10 / SDA GPIO11，V2/V3 使用 SCL GPIO8 / SDA GPIO7；CST816 触摸另建总线，使用 SCL GPIO3 / SDA GPIO1（所有硬件版本）。具体以 `I2C_Driver.h` 和 `CST816.h` 为准；更换硬件前应核对实际接线。

主总线寄存器读写封装缓存最多 8 个设备并保护并发注册；注册失败或缓存已满时返回错误，不中止固件或反复泄漏设备句柄。写入使用 I2C 驱动的多缓冲事务，寄存器地址与数据在同一事务发送，避免按输入长度占用任务栈。

### 胎压配对扫描生命周期

`ui_bluetooth.c` 提供版本页上滑后的双入口及四轮配对界面；扫描行只有六位 ID，点击即保存到当前轮位。`tpms_ble_client` 通过 `BLE_HUB_APP_TPMS` 注册 GAP / shutdown 回调，以广播 ID 去重（MAC 仅诊断），稳定追加候选，不按 RSSI 重排。锁保护的快照传给 LVGL 定时器，BLE 回调不操作 UI。进入页面 `elm327_ble_yield_scanner(true)` 让出重连扫描，离开时恢复；已连接的 ELM327 继续单线程轮询。停止后延迟启动下一轮扫描，避开控制器异步停止。

`tpms_fbb0.c` 是无平台依赖解析器，依据 TireKing Android `com.zl.dev.tire.king` 1.1.8 / 3538 的静态分析：独立完整 raw 记录至少 26 字节，前缀 `02 01 06 03 03 B0 FB`；CRC16-XMODEM 覆盖 `raw[9:16] + raw[17:18] + raw[19:26]`，对比 `(raw[16] << 8) | raw[18]`。ID 为 `raw[25:22:-1]` 的六位大写 HEX；压力用整数 Pa 保存 `(raw[12] + (raw[17] == 1 ? 256 : 0)) * 3144`，温度 `(unsigned)raw[13] - 55`，电压 mV 为 `raw[11] * 10 + 1220`；保留未知状态原值。不假定 manufacturer ID 为 `AC00`。扫描桥先验证 ADV / RSP 各自长度不超过 31，分别解析，禁止将片段拼接或对 manufacturer 子串套 raw 偏移。仅校验成功的 ID 占候选槽；普通 BLE 不再挤满 20 项列表。

NVS 保留 `nvs_user_cfg_t.tpms[4]`（offset 96，单项 39 字节）的旧 MAC 布局；在旧完整 `sizeof(cfg)=252` 后追加 `tpms_sensor_id[4][7]`，空串未绑定，`000000` 是有效 ID，逐轮合并保存。grow 加载旧配置时新字段为零；旧 MAC 记录仅提示重配，不推算 ID。成功保存/删除才清除当前轮旧 MAC，失败回滚。保存 ID 不等于在线；后台接收、默认总览和主题绑定共用四轮缓存，不提供内置胎压报警。

INFO 级 `tpms_ble` 日志记录有效 ID、MAC、类型、RSSI、解码值及独立 ADV / RSP 原始 HEX；每个 ID 首次、首次非空响应、其后每 5 秒采样，重扫重置，列表满警告一次。解码、格式化和日志 I/O 在自旋锁外；不创建任务。`test/host/espnow/test_tpms_pairing.c` 覆盖构造报文 CRC/字段、短包/非法长度、分片拒绝、普通设备过滤、ID 去重/MAC 变化、满表更新、生命周期、日志限频与锁约束、NVS 边界及保存失败回滚。`-DEH_TPMS_LVGL=ON` 的真实 LVGL `tpms_ui` 测试覆盖四轮编号保存、精确 ID 行文本、取消/失败保留、旧记录重配、删除、重扫与退出清理；`TPMS_UI_CAPTURE_DIR` 可输出页面 PPM。测试不是实物传感器兼容性验证。

`lvgl_runtime.c` 每 250 ms 在 LVGL 控制上下文调用 `tpms_ble_service_step()`；从表、无 ELM 绑定或 ELM 已连接时允许后台无限扫描。手动配对页暂停后台，退出后恢复；ELM 重连优先，扫描停止后保留 500 ms 冷却、启动失败后 2 秒重试。广播回调按已绑定 ID 更新缓存，即使 20 个候选已满也不丢已绑定轮位更新；仅 CRC 通过的样本更新时间，120 秒后测量失效但保留详情历史。成功换绑清除该轮缓存，保存失败保持原数据；实时值不写 NVS、不经 ESP-NOW 同步。

`ui_tpms.c` 实现默认四轮简洁总览（bar 两位小数 / C / 估算电量，无编号）及点击详情。`tpms_theme_source.c` 提供每轮 16 个固件局部数据源：`tpms.fl.*` / `tpms.fr.*` / `tpms.rl.*` / `tpms.rr.*`，不追加 `contracts/signals.json`、OBD PID、NVS 槽位或线协议。主题按帧取一次快照，独立扩展数组与双 64 位使用/变化掩码（总源数不超过 128），条件与 change 规则可用；旧 `tr_inputs_t` / `tr_eval()` 不变，高位变化走 `tr_eval_extended()`。胎压不参与 OBD 扫表，不受 OBD link 缺失影响。电量函数复现 TireKing float32 电压分档，仅用于显示估算；字段/单位与 NaN 语义见 [THEMES.md](THEMES.md)。前端未改。

主机新增覆盖后台优先级/缓存过期/候选满表/换绑回滚、简洁页两位小数和详情、四轮所有主题字段、256 个电压字节与边界、高位 change 触发及扫表隔离；截图为合成数据，不是实车验证。

### 启动顺序（`main/app_main.c`）

```
cJSON hooks（PSRAM；内部 RAM 仅在 PSRAM 耗尽时兜底）
→ nvs_app_init → nvs_storage_init → signal_store_init → sd_card_init（失败只警告）
→ storage_service_boot（首启迁移 or 常规挂载 config；恢复 SD 主题选择；处理开机时落在
  /OBD/IMPORT 的 RESET.TXT / ACTIVATE.TXT / 车型 JSON）
→ coredump_store_boot_check（在迁移之后——迁移会先擦掉旧 bootmedia 占的转储区）
→ vp_manager_init（找不到待激活车型就回退 obd2.generic）
→ 任务看门狗 10s → I2C/EXIO/LCD/LVGL 初始化 → ble_hub_init（在 UI 之前）→ 开机动画
→ ui_init（含 theme_engine_init）→ ui_ext_init → app_event_init
→ 按角色分支：从表 = ESP-NOW slave + RaceChrono；主表/独立 = ELM327 BLE
  （仅已绑定 MAC 才自动连接）+ ADS1115 油压（仅 V1 板，且当前车型不走 OBD 油压时）
  + ESP-NOW master（仅 MASTER）+ RaceChrono
→ storage_service_start（按 SD 状态每 1/3/10s 轮询；卡任务在启动 5s 后 / 插卡时 / OTA 结束时触发）
→ bootmedia_serial_start（常驻 USB 接收任务，优先级 3）
→ ota_valid task（15s 自检标记槽位有效）
```

### 数据流

```
ELM327 BLE 适配器 ──BLE──→ elm327_ble_client（单线程状态机，车型 vp_engine 驱动请求/解码）
                                   │
                                   ▼
                            signal_store（obd.* / sys.* / veh.*，contracts/signals.json）
                                   │      ←── ads1115_oil_pressure（油压，V1 板，车型不走 OBD 油压时）
                 ┌─────────────────┼──────────────────┐
                 ▼                 ▼                  ▼
           LVGL 页面刷新        theme_tick()      espnow_link（主表广播，v6 TLV）
           （内置表盘页，经       （运行时主题页面）         │
            obd_data_cache 门面）                          ▼
                                              从表 espnow_link 排队接收 → 维护任务 → 渲染
```

每个车型 profile（`vehicles/catalog/*.vehicle.json`，编译态 `vp_profile_t`）
声明自己的请求/解码规则并写入 `signal_store` 对应的 `veh.*` 槽位；
`vp_engine` 是否发 ATMA 监听、CAN 头切换等都由 profile 决定，不再是按车型名写死的分支。

**ELM327 轮询是单线程状态机**：初始化 → （可选）协议自动检测 → 循环轮询各通道。
改轮询逻辑时不要引入并行请求 —— 一个适配器同时只服务一个请求流（AGENTS.md 红线）。
BLE 回调通过常驻静态信号量唤醒轮询任务；BLE 关闭时的回调也会唤醒任务退出，信号量不会随任务删除，避免迟到回调访问已释放的任务句柄。
BLE 连接和扫描的 64 位时间戳使用临界区同步，避免双核并发读写时读取不完整。

LVGL 常驻任务固定在 Core 1，ELM327 BLE 轮询状态机固定在 Core 0；任务参数统一由
`app_main.c` 中的策略表管理，并通过 `app_tasks.h` 接口创建。这样表盘/LVGL 绘制不会与 OBD 轮询任务在同一核心互相抢占，
BLE 协议栈仍按 ESP-IDF 配置运行。不要因此拆分或并行化 ELM 请求。

LCD 的 LVGL `wait_cb` 通过常驻静态二值信号量等待 DMA 完成，避免双缓冲刷屏期间在
`draw_buf->flushing` 上忙等。完成中断先调用 `lv_disp_flush_ready()` 释放缓冲，再发出唤醒；
任务醒来必须重新检查 `flushing`，不能把可能来自前次传输或全屏视频的旧信号当作本次完成。
该信号量在注册 LCD 回调前初始化，不随切页或视频启停释放，不占用 LVGL 通知槽 0。
ESP-IDF 5.5.3 的 SPI LCD 后端忽略完成回调的返回值，因此桥接函数在唤醒高优先级任务时
显式请求 ISR 调度。实现见 `app_obd_dsp/lvgl_flush_sync.c`。

USB 接收任务 `bootmedia_usb` 的优先级为 3，低于 OBD 轮询和 LVGL 的 4，避免 USB 通信繁忙时以更高优先级抢占实时数据与显示任务。USB 仍在正常开机后常驻，空闲时阻塞等待接收；网页刷写、资源上传和管理 RPC 流程保持不变。

## 编译配置

环境：ESP-IDF 5.5.3 + Python 工具链。组件依赖（`main/idf_component.yml`）：
`lvgl/lvgl`、`espressif/esp_lcd_touch`、`joltwallet/littlefs`、
`espressif/esp_new_jpeg`（1.0.2），首次构建自动拉到 `managed_components/`。
`button`/`knob` 组件已删除（未使用）。

```bash
idf.py set-target esp32s3
idf.py menuconfig     # OBD DSP Configuration
idf.py build
idf.py -p PORT flash monitor
cp build/bootmedia.bin /Volumes/SDCARD/OBD/BOOT/bootmedia.raw
```

> **改了 `sdkconfig.defaults` 之后，首次构建前先删本地 `sdkconfig`**
> （`defaults` 只在生成 `sdkconfig` 时生效一次，已存在的本地文件不会被覆盖）。
> 本次改动涉及的默认项：`CONFIG_FATFS_LFN_HEAP`/`CONFIG_FATFS_MAX_LFN=255`
> （SD 长文件名）、`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` + `_DATA_FORMAT_ELF` +
> `_CHECKSUM_CRC32`（崩溃转储）、`OBD_SD_SLOT_*`（见下表）。

设备自带网页（`device_web_page.html`，仅 OTA 模式可用）在编译期由
`tools/gzip_asset.py` gzip 压缩内嵌，压缩后体积硬上限 **48 KB**，超了构建直接失败。

`menuconfig → OBD DSP Configuration`：

| 选项 | 值 | 说明 |
|------|----|------|
| 硬件版本 | **V1**（默认，本次重构的目标板）| 微雪 ESP32-S3-Touch-LCD-1.85：ST77916 v2 init + TCA9554 + ADS1115 |
| | V2 | 新板 A：ST77916 **v1** init，直连 GPIO，无 TCA9554 / ADS1115 |
| | V3 | 新板 B：ST77916 v2 init，直连 GPIO，无 TCA9554 / ADS1115 |
| SD 卡槽 | **SDMMC 1-bit**（V1 默认）/ None | V1 板走 SDMMC 1-bit（CLK14/CMD17/D0 16）；选 None 的板所有 SD 功能报 "no card" |

> 刹车温度（RS485）与配套的 BLE OTA 服务已随本次重构删除，见 CHANGELOG.md。

构建系统在 CMake configure 阶段自动运行 `tools/gen_themes.py` 生成主题表，
并把 git 分支 / 提交数 / 短哈希注入 `OBD_GAUGE_BUILD_TAG`（设备清单和版本页显示的
build tag 就是它，所以发版前必须先 commit，见[发布流程](#发布流程)）。

## 适配新开发板

1. `Kconfig.projbuild` 加硬件版本选项（若 LCD / 触摸 / IO 扩展不同）
2. 重点检查 `bsp_obd_dsp/`：`lcd_driver/`（初始化序列、QSPI 参数）、`touch_driver/`、
   `exio/`（有无 IO 扩展）、引脚定义
3. 屏幕分辨率 / 色深宏在 `ST77916.h`，UI 按 360×360 设计，换屏要动 `export_path/`
4. 与车相关的都不用动 —— 车型逻辑在 `main/vehicle/`（引擎/生命周期）+
   `vehicles/catalog/*.vehicle.json`（具体车型），与板子无关

## 工具脚本

| 脚本 | 用途 |
|------|------|
| `tools/gen_themes.py` | 编译期主题代码生成（CMake 自动跑；`--check` 供 CI）|
| `tools/gen_signals.py` | 由 `contracts/signals.json` 生成 `signal_registry.{h,c}`（`--check` 供 CI）|
| `tools/gen_layout.py` | 由 `partitions.csv` 生成分区布局常量/CRC；`--check` 校验 FLASH.md/.en.md 地址表与 CSV 一致 |
| `tools/gen_release.py` | build/ 产物 → firmware/release/ + latest.json（校验固件大小、bootmedia 格式、build tag 匹配 HEAD）|
| `tools/release.sh` | 一键发版（commit 源码 → build → gen_release → commit 产物 `build:` → push）|
| `tools/make_video.py` | 编码开机 / 主题视频为 OBV1（`.obv`，MJPEG 容器）；`inspect`/`verify` 子命令 |
| `tools/make_bootmedia_raw.py` | 把 `bootmedia/slot_a/` 打成 raw 格式 `build/bootmedia.bin`（legacy 开机动画，CMake 自动跑）|
| `tools/gzip_asset.py` | gzip 压缩设备网页资源，超过体积上限直接失败（CMake 自动跑）|
| `tools/check_bss.py` | 对比两份 link map，禁止内部 DRAM 静态占用无提醒地增长超过阈值 |
| `tools/vehicles/dump_legacy.py` | 从旧版 `vehicle_profiles.c` / `vehicle_custom_config.h`（或指定 git 版本）导出中间 JSON |
| `tools/vehicles/legacy_to_v1.py` | 中间 JSON → `vehicles/catalog/*.vehicle.json`（`--check` 供 CI）|
| `tools/vehicles/import_car_scanner.py` | 把 Car Scanner `vehicle-profiles.json` 中可安全表达的 PID 转成社区可导入的车型 JSON；支持线性缩放、反序字节及简单算术/位运算公式，默认每份最多 12 个自定义信号以留出设备编译 blob 空间；条件公式、FLOAT32 和不支持的 ECU 命令会在 summary 中标注跳过原因 |
| `tools/vehicles/expr2rpn.py` | 把 profile 公式（infix）编译成设备执行的 RPN 数组；`--check` 交叉验证 |
| `tools/vehicles/gen_builtin.py` | `vehicles/catalog/obd2.generic.vehicle.json` → `main/vehicle/vp_builtin_generic.c` |
| `tools/vehicles/legacy_golden.py` | 生成/校验 `test/host/vehicle` 的旧固件行为 golden fixture |
| `tools/fixtures_node/run_ts_crosscheck.mjs` | 用编辑器仓库（`obd_theme_store`）自身 TS 源码交叉校验 `contracts/fixtures/`|
| `contracts/check_contracts.py` | 校验信号表 / 车型 JSON schema / fixtures 的一致性（CI `contracts` job）|
| `tools/fake_elm327.py` | 伪 ELM327 TCP 服务器：记录 App 发的每条请求，未知 PID 一律返回肯定应答 |
| `tools/one_shot.py` | 一键 PID 挖掘：起伪服务器 → 手机录一遍 → 自动出 pids_full.csv |
| `tools/analyze_probe.py` | 把注入信号与 Car Scanner 录制对上号，反推 PID→仪表映射与解码公式 |
| `tools/parse_carscanner_backup.py` | 解析 CarScanner 备份目录，导出全车 PID 清单 CSV |

Car Scanner 配置转换示例（PID 目录是聚合索引，按车型生成时使用各车型条目自带的 `pids`）：

```bash
python3 tools/vehicles/import_car_scanner.py \
  --profiles /path/to/vehicle-profiles.json \
  --output /tmp/carscanner-community
python3 contracts/check_contracts.py --profiles '/tmp/carscanner-community/*.vehicle.json'
```

转换器只接受直接 `21xx` / `22xxxx` 请求和固件可表达的数据解码：线性 `MUL/DIV/OFS`、反序字节，以及简单算术、`And`、`Shr`、单字节 `SIGNED` 公式。条件分支、`FLOAT32`、外置缩放与公式同时出现、复杂初始化命令和固件不支持的请求会在 `summary.json` 记录跳过原因。默认每份最多输出 12 个自定义信号；当前 1304 份输入中的 820 份输出均通过固件车型目录校验和 8 KB NVS blob 上限，更多信号可能超限。输出车型 JSON 可在车型社区逐个导入发布，多品牌配置会保留品牌列表供筛选，车型和年份元数据也会随配置发布。

> 已删除：`tools/convert_rpm_flash.py`（RPM 自定义闪烁现在是一条主题规则，
> 见 THEMES.md）、`tools/theme_packer/pack_theme.py`（运行时主题统一用编辑器
> 导出的 theme.bin）、`tools/make_boot_block.py`（被 `make_video.py` 取代）、
> `main/python_quick_rs485_check.py`（刹车温度模块已删除）。开机动画现在是
> 独立于主题的公共 SD 媒体（优先 `/OBD/BOOT/boot.obv`，见 THEMES.md），不再
> 使用编译进固件的静态图；没有可用 SD 媒体时跳过自定义动画。

### PID 挖掘工作流（不上车找私有 PID）

想给新车型找私有 PID（油温、油压、挡位…）时，用伪适配器反向记录手机 App
（Car Scanner 自带各车 profile）的请求：

```bash
python3 tools/one_shot.py
# 1. 自动起 fake_elm327.py --probe prbs --tick 2（注入已知伪信号）
# 2. 手机 Car Scanner 连 WiFi ELM327（本机 IP:35000），选目标车型 profile，录一段导出 CSV
# 3. 粘贴 CSV 路径，自动停服务器并跑 analyze → pids_full.csv
```

原理：伪服务器对未知 PID 全部返回"肯定应答"（App 不会标记不支持，会把 profile
里的私有 PID 问全），每条注入信号对应 App 的一次真实请求，**按顺序配对即可反推
哪个 PID 驱动哪个仪表、解码公式是什么**。挖到的结果怎么写成 `rpn` 公式见
[PID_GUIDE.md](PID_GUIDE.md)，再按 [VEHICLES.md](VEHICLES.md#新增修改车型)
加进车型配置。

## 主机单元测试与 CI

`main/vehicle/`、`main/theme_engine/`、`main/media/`（OBV1）、
`main/bsp_obd_dsp/espnow_v6.c` 等纯 C 模块不依赖 ESP-IDF/FreeRTOS，可以直接在
host 上编译测试，不需要上板也不需要 `idf.py`：

```bash
cmake -S test/host/<suite> -B build/host/<suite> -G Ninja
cmake --build build/host/<suite>
ctest --test-dir build/host/<suite> --output-on-failure
```

`<suite>` 对应 `test/host/` 下的目录：`rules`（规则引擎，38 组黄金向量 + 输出缓存对照）、
`vehicle`（车型 RPN 编译/校验/golden 请求序列）、`espnow`（v5/v6 编解码 + RaceChrono 筛选调度）、
`media`（OBV1 解析 + legacy delta 流 + CRC 对照 + 真实视频控制等待）、`engine`（车型轮询引擎，golden
请求序列）、`theme`（主题引擎：layout/overlay/sidecar/像素转换；theme_tick 缓存活动页并索引动态绑定项）、`signal_store`（信号批量读取、过期判定与性能统计分桶）、
`storage`（真实存储服务，有槽/无槽双配置，启动/热插拔/OTA/重试时序；SDK/RTOS 边界为 stub）。

`signal_store` 还包含 delivered-unit 换算对照测试：`SS_SANITIZE=ON`（默认）开启
ASAN/UBSAN，`SS_DELIVERY_BENCHMARK=ON` 构建单位换算基准。基准命令和 ESP32 目标代码
检查见 [PERFORMANCE.md](PERFORMANCE.md)；它只测换算函数，不代表锁、页面或设备端到端耗时。

`engine` 的 `EH_CAN_BENCHMARK=ON` 仅构建未接入固件的 CAN 规则选择实验，完整结果和
未采用原因见 [PERFORMANCE.md](PERFORMANCE.md)。`EH_REQUEST_BENCHMARK=ON` 构建车型请求
分发索引微基准；它验证线性扫描与 request→source 索引的 checksum，并报告固定输入下的
桌面选择耗时，不属于固件测试。完整命令和目标编译结果见 [PERFORMANCE.md](PERFORMANCE.md)。

`engine_poll_plan` 直接编入真实引擎，与旧规划循环进行 100,352 次完整状态/slot
序列对照；覆盖共享请求期限、轮次重置、source 切换和时间回绕。`EH_POLL_BENCHMARK=ON`
构建独立 driver 的局部掩码去重基准；完整输入、结果与测量边界见
[PERFORMANCE.md](PERFORMANCE.md)。生产引擎不包含测试 hook。

`engine_queue_copy` 直接编入真实引擎，与独立旧版进行 246,272 次完整状态、出队 step
和丢弃日志对照，覆盖全部队列 head/count、截断/NUL 边界与连续环绕。
`EH_QUEUE_BENCHMARK=ON` 构建独立 driver 的入队/出队/checksum 基准；默认运行正确性
测试，不运行计时。仅用于临时基线/负对照时可用 `EH_QUEUE_SOURCE` 指向引擎副本，正常
测试必须保持空值、编入生产文件；该覆盖不影响固件。命令和测量边界见
[PERFORMANCE.md](PERFORMANCE.md)。

`engine_state_format` 复用真实引擎/队列 probe，对照独立旧格式化实现进行 178,928 次
完整状态/step/日志比较，覆盖 ID 掩码/宽度、地址关闭哨兵、CAF 默认分支和 req 元数据。
`EH_STATE_BENCHMARK=ON` 构建独立 driver；默认只运行正确性测试。
`EH_STATE_BASELINE_SOURCE` 可指向布局一致的整份旧源码，仅在可选基准中以独立翻译
单元和符号别名链接，在同一进程交替计时两份实际生产包装；未设置时使用私有参考。
`EH_QUEUE_SOURCE` 仍只供临时源码基线/负对照，常规测试保持空值。完整重现命令、原始
数据、主机常量路径下降与目标 footprint 代价见 [PERFORMANCE.md](PERFORMANCE.md)。
生产引擎没有测试 hook。

`engine_rx_append` 对照独立旧接收循环，进行 219,423 次完整引擎字节和真实 CAN 监听
回调对照，覆盖 NUL/空输入、全部字节值、分片、容量后提示符、busy/kind 门控和时间回绕。
常规测试保持 `EH_RX_SOURCE` 为空；该覆盖仅供临时实验/负对照。
`EH_RX_BENCHMARK=ON` 必须配合布局一致的整份旧源码 `EH_RX_BASELINE_SOURCE`，
同进程/同地址交替计时真实接收包装，默认不跑计时。复测中全 NUL 长块有下降，
完整测量边界和重现命令见 [PERFORMANCE.md](PERFORMANCE.md)。这些选项不影响固件。

`vehicle_rpn_integer` 把实际生产 RPN 文件编入私有 probe，对照独立旧双精度规则，默认
检查 1,001,690 个位模式与 610,140 条完整公式。`VH_INTEGER_SOURCE` 只供临时 probe
实验/负对照，常规测试保持空值。Release 可选 `--exhaustive` 已覆盖全部 Binary32
位模式；不进入默认 CI。`VH_INTEGER_BENCHMARK=ON` 必须提供整份旧源码
`VH_INTEGER_BASELINE_SOURCE`，同进程交替计时完整执行器，也把旧执行器接入公式对照。
默认不跑计时，这些选项不影响固件。边界证明、主机小幅增减、目标软件浮点依赖和
静态栈帧变化见 [PERFORMANCE.md](PERFORMANCE.md)。

`vehicle_rpn_raw_float` 复用 RPN 私有 probe，默认进行 4,274,988 次完整公式对照，
检查独立逐 bit 提取、旧 64 位转换的 float 位模式以及错误输出哨兵。
同样保持 `VH_INTEGER_SOURCE` 为空；临时覆盖不影响正常 `vehicle_core`。
可选 Release `test_rpn_raw_float --exhaustive` 已覆盖全部 raw32 值的 signed/unsigned
完整公式；默认 CI 不跑穷举。`VH_INTEGER_BENCHMARK=ON` 与整份旧源码
`VH_INTEGER_BASELINE_SOURCE` 同时提供 `bench_rpn_raw_float` 和旧执行器对照，
共享 driver 按编译宏选择场景。主机回退、目标硬件转换依据和 32 字节固件代价见
[PERFORMANCE.md](PERFORMANCE.md)。这些选项不影响固件。

`engine_write_rounding` 编入真实私有写回路径，默认对照 1,720,944 个舍入输入与
33,280 次完整状态/回调，参考实现使用原双精度规则。`EH_WRITE_SOURCE` 只供临时负对照，
常规测试保持空值。Release 可选 `test_write_rounding --exhaustive` 检查全部 Binary32
舍入输入，不进入默认 CI；`EH_WRITE_BENCHMARK=ON` 构建舍入局部基准。
边界修复、主机退化与目标硬件指令证据见 [PERFORMANCE.md](PERFORMANCE.md)。
这些测试与选项不进入固件。

`signal_store` 的 `signal_lookup` 对真实 store、冻结旧扫描和独立线性 oracle
检查全部 16 位 ID 与每种车型注册数量；`signal_lookup_generator` 生成并编译 8 份
重排/稀疏注册表，校验 ID 与槽位映射。Release 可用 `SS_LOOKUP_BENCHMARK=ON`
构建局部查找及真实 ESP-NOW apply 基准，默认关闭。桌面锁为 stub；证据、RPM 局部
退化与首轮波动见 [PERFORMANCE.md](PERFORMANCE.md)。这些测试与基准不进入固件。

`espnow` 的 `snapshot_append` 将唯一追加与冻结旧插入函数对照，检查全部 16 位
ID、有效位、挡位和容量边界；`master_collect` 编入真实私有主表收集函数，比较
35,012 组输入的完整快照及所有 v5/v6 字节。追加接口要求调用方保证 ID 唯一，
覆盖值仍用通用插入。Release 可用 `EH_COLLECT_BENCHMARK=ON` 构建局部插入、
真实收集和收集加完整编码基准，默认关闭。stub 不包含真实锁或无线传输，桌面
编码用便携 CRC；目标代码成本和测量边界见 [PERFORMANCE.md](PERFORMANCE.md)。
这些测试和选项不进入固件。

`espnow` 新增便携 CRC 对照；找到 SDK 时另编译真实 `ESP_PLATFORM` codec，
运行完整协议套件、CRC 全两字节/全状态转换与完整分片字节对照。
`EH_ESP_ROM_DIR` 可指定 SDK，`EH_ROM_CRC=OFF` 可关闭兼容项；
`EH_CRC_BENCHMARK=ON` 在 Release 构建桌面 CRC/完整 codec 基准，默认关闭。
这些测试与 SDK Linux 查表不进入固件，目标使用 ROM；证据与测量边界见
[PERFORMANCE.md](PERFORMANCE.md)。

`media` 默认运行纯 C 的容器和 CRC 测试；找到 ESP-IDF `esp_rom` 源码时，还会
编译 `ESP_PLATFORM` 路径并对照 SDK 的 Linux CRC 实现。可用 `MH_ESP_ROM_DIR`
指定源码目录，或用 `MH_ROM_CRC=OFF` 关闭这两项兼容测试；无需安装 SDK 也能跑基础测试。
ROM 实机耗时与桌面基准的区别见 [PERFORMANCE.md](PERFORMANCE.md)。
`media_video_player_wait` / `media_video_player_stats` 编入真实 `video_player.c` 和
`lvgl_runtime.c`，用 SDK/RTOS/LVGL/JPEG/LCD stub 在普通/统计两配置各执行 47 个隔离
场景，覆盖满 ring、暂停/恢复、文件头/水位、UI 接帧、帧期限、停止唤醒，以及 LVGL
通知注册/合并/等待间隙、旧会话迟到通知、解码/发布交错和待显示帧生命周期。播放器控制在 LVGL task
或持有 `lvgl_mux` 的上下文调用，worker 不调用 LVGL。
虚拟 1 kHz tick 不模拟设备性能；生产文件没有测试 hook。仅用于负对照时可用
`MH_PLAYER_SOURCE` / `MH_RUNTIME_SOURCE` 指向临时实现副本，默认必须测试仓库生产文件。

`theme_resample` 对照真实颜色转换、旧轴构建器/完整缩放循环与公开生产路径，默认覆盖
295,186 个 Binary32 输入、10,464 组完整图片、100,303 组轴尺寸及分配失败。
Release `test_resample --exhaustive` 穷举全部 Binary32，`--axes-exhaustive` 穷举全部
4,190,209 组合法轴尺寸；`TE_RESAMPLE_BENCHMARK=ON` 构建带峰值请求字节追踪的完整
图片基准，默认比较新旧容量，`bench_resample --rounding` 保留上一轮颜色量化对照。
`TE_RESAMPLE_SOURCE` 仅供主机负对照，常规测试为空；目标代码/内存代价与测量限制见
[PERFORMANCE.md](PERFORMANCE.md)。

`theme` 默认还对照原逐像素算法测试 RGBA 圆屏按行裁剪，覆盖全部圆周边界与随机窗口；
可用 `TE_PLAN_BENCHMARK=ON` 构建图片规划基准，命令与测量范围见
[PERFORMANCE.md](PERFORMANCE.md)。

`theme` 可额外用真实 LVGL 8.4.0 编译 `theme_page.c`，对照逐帧应用外观的页面，
检查 LVGL 对象状态、绑定、告警恢复和视频切换/暂停调用。平台时间、信号快照与视频 I/O
由测试控制，不执行 LCD DMA 或视频解码。默认纯 C suite 不依赖 LVGL；开启方式如下：

```bash
cmake -S test/host/theme -B /tmp/obd-theme-page-tests -G Ninja -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-theme-page-tests
ctest --test-dir /tmp/obd-theme-page-tests --output-on-failure
```

`theme_navigation` 编译实际切页状态机与真实 LVGL 定时器/页面，控制 worker 调度，检查准备期间旧页定时器继续执行、仅 LVGL 发布、重复请求、启动/发布失败、离开/删除原页及离开后返回时的取消。`theme_page` 另外检查真实分阶段任务在运行前后丢弃时的资源回收；主机测试不覆盖实际 FreeRTOS 调度、SD 吞吐、DMA 或视频资源争用。

`theme_acquire` 同时编译真实获取/缓存路径，对照冻结旧获取函数，检查 1,006 组像素/
几何场景、全部 RGB565 颜色、RAM 像素寿命、预算、着色、SD 读取失败和分配失败。旧/新共用几何和
转换 helper；不模拟实际 SD/LCD 时序。Release 的 `test_acquire --benchmark` 计时
完整素材获取/释放，保存交替原始样本；计时与像素/峰值检查分离，范围见
[PERFORMANCE.md](PERFORMANCE.md)。

`theme_sd_loader_library` 编译真实 SD loader/library，覆盖超过 4 MiB 的包、字体段读取、
缺卡/坏包/越界/读锁超时、NVS 保存失败、当前包覆盖限制和旧中断标记恢复；不链接 Flash 读写实现。
`theme_media_migration` 编译真实迁移代码，验证旧分区查找、已有 SD 文件不被覆盖、
空旧分区不清选择、NVS 选择失败重试和超大动画救援锁释放。

默认读取 `managed_components/lvgl__lvgl`；未安装 managed components 时，用
`TE_LVGL_DIR` 指向 LVGL 8.4.0 源码。CI 的 `host-theme` 固定该版本的提交并开启这项测试。

`spi_lock` 提取固定 ESP-IDF v5.5.3 源文件中的真实 `schedule_core` / `bg_exit_core` 函数，编译旧版负对照与修复版，检查 242 组锁/后台传输状态以及补丁幂等、未知上下文拒绝。边界用 stub，不模拟实际中断时序或 LCD 性能。默认下载固定版本并校验 SHA-256；离线时传 `-DSPI_LOCK_SOURCE=/path/to/esp-idf/components/esp_hw_support/spi_bus_lock.c`（必须是原始 v5.5.3）。CI 在 `host-storage` 中一并执行。

固件构建在 `project()` 之后加载 `cmake/idf_spi_bus_lock.cmake`，把上游提交 `26263a36a2b3fe0649ccb20a02517c6c4b18e01b` 的 SPI 锁修复应用到构建目录副本，再替换 `esp_hw_support` 的该源文件；`cmake/idf_fatfs_exfat.cmake` 同样将 FatFs 的 `ff.c`、`ff.h`、`ffunicode.c`、`ffconf.h` 复制到构建目录并仅在本项目中打开 SDK 默认关闭的 `FF_FS_EXFAT`，因此 FAT32 仍可用且固件不会自动格式化 SD 卡。两个补丁都精确匹配上下文，未知 SDK 会停止配置并要求人工核对。升级 SDK 时先核对这些补丁及其负对照版本。

CI（`.github/workflows/ci.yml`）跑 9 个 job：`contracts`（
`contracts/check_contracts.py` + `tools/gen_layout.py --check --no-idf`）和
上述 8 个既有 host suite 对应的 job（`host-rules`/`host-vehicle`/
`host-espnow`/`host-media`/`host-engine`/`host-theme`/`host-signal-store`/`host-storage`）；`spi_lock` 回归在 `host-storage` 内执行。`gen_themes.py
--check` 目前只在本地手动跑，不在 CI 里。

## 发布流程

在开发分支（例如 `rebuild`）执行 `./tools/release.sh "fix: describe change"`，再将源码、产物与清单一起合并到 `main` 并推送 `main`，触发正式发布。脚本自动激活 EIM，提交源码 → `idf.py reconfigure build` → 生成清单 → 单独 `build:` 产物提交 → `git push --no-verify`。源码与产物提交均使用 `--no-verify`；自动源码提交排除 `build/`、`firmware/release/` 和两份 CHANGELOG，执行前检查其余暂存范围。本地与服务器分支不必相同：本地可选 `FIRMWARE_BRANCH` 只限制构建分支，服务器默认监听 `main`。

快进与普通 merge 保留原构建分支 tag；若合并修改源码，或 squash/rebase 改写构建历史，须在最终 `main` 重新构建。固件 build tag 与清单指向源码提交 N，产物提交是 N+1。服务器校验 N 是当前 checkout 的祖先，并要求 N 之后只有白名单产物变化；不会把旧二进制标成新源码版本。`gen_release.py` 先校验完整 NUL 结尾 tag、文件大小和可选 raw bootmedia，再暂存文件、最后替换 `latest.json`；校验失败不改现有发布目录。没有 `build/bootmedia.bin` 时清单不包含它，并移除旧发布副本。自定义媒体仍安装到 SD。

```text
firmware/release/
  latest.json
  obd_brz_gauge.bin
  bootloader/bootloader.bin
  partition_table/partition-table.bin
  ota_data_initial.bin
  bootmedia.bin                         # optional legacy SD payload
```

商店服务器使用专用固件 checkout，每 60 秒 fetch `origin/main`，从 `firmware/release/latest.json` 校验 SHA256/size 和源码版本。上传版本目录文件后写数据库，最后更新 `PUBLIC_BASE_URL/firmware/esp32/latest.json`；失败下周期重试，成功状态持久保存。Docker watcher 不编译 IDF。网页与 App 使用同一清单；完整部署和 Android APK 的源码提交、构建、产物提交与服务器拉取步骤见商店仓库 `docs/RELEASE_GUIDE.md`。

## 提交约定

- 行为变化与烧录方式变化要记 [CHANGELOG.md](../CHANGELOG.md)
- NVS `nvs_user_cfg_t` / 主题结构体新字段**只能追加到末尾**（老设备 NVS 兼容）
- 主题相关提交别动 `ui_theme_generated.c` / `theme_assets/`（生成物）

### 双语文档维护规则

- 文档成对维护：`X.md`（中文，**事实源**）↔ `X.en.md`（英文译文）。
  事实改动先改中文，英文在**同一个提交**里跟上
- 结构 1:1 镜像（章节、表格、代码块一一对应），方便对照排查漂移；
  命令、代码、JSON、日志输出、表格数据一律不翻译
- 英文文档内部互链指向对应的 `.en.md` 版本；链到代码和分区文件的路径不变
- 每份成对文档标题下都有语言切换行（`[English](X.en.md) | 简体中文`）；
  新增文档时中英两份一起建

三连表主表的从表在线与协议版本记录使用同一临界区，在无线回调和读取任务之间保持一致。

ESP-NOW 从表 DATA 回调复制原始数据到常驻的 8 帧队列，由从表维护任务统一解码、写入信号和检查超时；队列满时丢弃新帧并在任务中记录警告。消费时重新检查绑定 MAC 和接收时间，过期帧不能恢复在线状态。主从表 OTA 停止均使用任务自行退出的握手：`espnow_link_pause_for_ota()` 未完成时返回 false，LVGL 定时重试后再启动热点，不等待或强制删除持有资源的任务。队列和唤醒信号量在本次启动期间常驻。
