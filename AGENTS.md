# AGENTS.md

给在此仓库工作的 AI 编码代理的操作说明。本文件只放约束、红线和指路，
不重复文档内容；项目与架构细节见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)。

**会话开始时若存在 `AGENTS.local.md`（个人偏好，不入库），先读它。**

## 这是什么项目

ESP32-S3 圆形车载仪表（微雪 1.85" 圆屏）：ESP-IDF 5.5.3 + LVGL 8，
BLE 连 ELM327 兼容适配器读车况，ESP-NOW 三连表联动，双主题系统。

## 改代码必须同步更新文档

同一改动里代码和文档一起动，映射如下：

| 改了什么 | 必须同步 |
|----------|----------|
| 车型增删、PID / 协议 / 油温路径 | `docs/VEHICLES.md`（+.en.md）、`docs/PID_GUIDE.md`（+.en.md）、README 车型清单与数量 |
| 分区表 `partitions.csv` | `docs/FLASH.md`、`firmware/README.md`、README 快速开始——三处烧录地址必须一致，以 `partitions.csv` 为准 |
| OTA 端点 / BLE 服务 / 设备清单格式 | `docs/APP_PROTOCOL.md`（+.en.md）|
| 页面导航、设置项、三连表、开机动画行为 | `docs/USER_GUIDE.md`（+.en.md）|
| 主题系统 / 主题引擎 | `docs/THEMES.md`（+.en.md）|
| 任何行为或烧录方式变化 | `CHANGELOG.md` **和** `CHANGELOG.en.md` 各加一条 |

硬规则：

- 中文文档是**事实源**，英文版（`X.en.md`）在同一个提交里跟上；结构 1:1 镜像
- 命令、代码、JSON、日志输出、表格数据**永不翻译**
- 文档里的数字拿不准时读代码，事实源清单：分区地址 → `partitions.csv`；
  车型表 → `vehicles/catalog/*.vehicle.json`（固件只内置 `main/vehicle/vp_builtin_generic.c`，
  老 NVS 车型序号映射 → `vp_manager_legacy_id()`）；HTTP 端点 →
  `main/app_obd_dsp/ota_wifi_server.c`；NVS 配置键 → `main/bsp_obd_dsp/nvs_storage.h`；
  页面导航 → `main/export_path/ui.c` 手势处理函数

## 代码红线

- NVS `nvs_user_cfg_t` 和主题结构体**新字段只能追加到末尾**（偏移须 ≥ 旧 `sizeof`，不可读旧尾部 padding；见 `nvs_storage.c` 的 grow 逻辑与 `tpms` offset 断言）
- 退役的持久化/上线槽位**永不复用**：`DISP_ITEM_RESERVED_9`、`chartalarm[9]`、`nvs_user_cfg_t.rsv_brake_temp_warn_c`（offset 60）、ESP-NOW v5 offset 23、RaceChrono `RC_PID_F109_RETIRED`（0xF109）；`disp_item_t` 只能追加（数值存进 NVS，见 `ui_disp_item.h`）
- `themes/registry.txt` 只能在末尾追加、槽位 0 必须 `default`（NVS 存槽位号，改序会静默换肤）
- `main/export_path/ui_theme_generated.c`、`main/export_path/theme_assets/` 是生成物，永远不要手改
- `main/export_path/ui_ext.c` 是手写扩展区，SquareLine 重导出不得覆盖
- ELM327 轮询是**单线程状态机**，不要引入并行/交错请求
- ESP-NOW 从表 DATA 回调只入队，维护任务统一应用数据和处理超时；OTA 必须等 `espnow_link_pause_for_ota()` 返回 true，禁止从外部删除主从表维护任务（`espnow_link.c`）
- 全屏视频借用 LVGL DMA 缓冲时必须保护刷新回调；仅 `lv_timer_pause` 会被切页/布局更新自动恢复（`main/media/video_player.c`）
- LCD DMA 完成信号量开机常驻，不占 LVGL 通知槽 0；中断先释放缓冲再唤醒，等待端必须重查 `flushing`，防止旧信号提前复用在途缓冲（`main/app_obd_dsp/lvgl_flush_sync.c`）
- BLE 任务无限等待时必须注册 `shutdown_cb`；唤醒对象不得随任务删除释放（`ble_hub.h` / `racechrono_ble_diy.c`）
- 视频 ring 保持单生产者/单消费者；消费者释放前不得改写已提交帧（锁外读取依赖此约束，见 `main/media/video_ring_peek.h`）
- 视频无限条件等待必须配对 header/ring/UI/控制通知；stop 唤醒两 worker，event group 由 session 引用保活（`main/media/video_player.c`）
- 视频 start/stop/pause/resume 在 LVGL task 或持有 `lvgl_mux` 时调用；worker 不操作 LVGL（`main/media/video_player.h`）
- LVGL task 默认通知槽 0 专用于帧唤醒，绑定后永不删除/重绑；通知不带 session/view 指针（`main/app_obd_dsp/lvgl_runtime.h`）
- OTA 启停共用非阻塞生命周期保护；`ota_wifi_server_stop()` 返回 false 时不得继续 USB 接管或退出重启，稍后重试，避免持有 LVGL 锁等待 HTTP 清理造成互锁（`ota_wifi_server.c` / `ui_ScreenPageOTAMode.c`）
- 发版前必须先 commit——build tag 的 count 取 git 提交数（`tools/release.sh` 流程）

## 构建与验证

- `idf.py set-target esp32s3 && idf.py build`；换开发板先在 menuconfig → OBD DSP Configuration 选硬件版本（V1/V2/V3）
- 改了 `sdkconfig.defaults` 要先删本地 `sdkconfig` 再 build（defaults 只在生成 sdkconfig 时生效）
- 在 `SRC_DIRS` 内新增 C 文件后先 `idf.py reconfigure`（`main/CMakeLists.txt` 在 configure 时收集源文件）
- 改主题可独立快速验证：`python3 tools/gen_themes.py --check`
- 主机单元测试（无需 IDF）：`cmake -S test/host/<suite> -B build/host/<suite> -G Ninja && cmake --build build/host/<suite> && ctest --test-dir build/host/<suite>`，suite 清单见 `.github/workflows/ci.yml`
- `build/`、`firmware/release/` 下的二进制是构建产物，普通源码改动不要碰
- 没有实车/适配器时用 `tools/fake_elm327.py` 模拟 ELM327 调试

## 提交约定

- conventional commits（`feat:` `fix:` `docs:` `build:` `chore:`），与现有历史一致
- 源码改动与编译产物分开提交（产物单独用 `build:` 前缀）
- `CHANGELOG.md` / `CHANGELOG.en.md` 的提交由仓库所有者决定时机，不要自作主张带上

## 本文件的自更新协议

发现新的持久约束（比如踩了一次构建的坑、一条没写在这里的红线）时，
按三问归类后记录，**过时条目当场删除**，不保留:

1. 对**任何**在此仓库工作的人都成立、且能从代码/配置验证？ → 写进本文件（`AGENTS.md`）
2. 只对某个人的工作流、口味或本机环境成立（例："commit message 用中文"、"我用 eim 激活 IDF"、"我的串口是 /dev/cu.usbmodemXXX"）？ → 写进 `AGENTS.local.md`
3. 一次性 / 临时信息（某次调试的端口、临时绕过）？ → 哪里都不写

质量规则：新条目必须短、可验证（注明依据的文件或教训来源）；本文件总量
控制在 150 行内，超了先删旧的再加新的。
