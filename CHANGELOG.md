# Changelog / 更新日志

Newest first. Entries cover behaviour and flashing changes; pure refactors and
comment cleanups are left to the git history.
最新的在最上面。这里只记录行为变化和烧录方式变化，纯重构和注释整理请查 git 历史。

---

## English

### Themes are now data, not code

Adding a UI theme no longer touches a single C file. A theme is a folder under `themes/` holding a `theme.toml` manifest — eight decorative colors plus optional artwork — and one appended line in `themes/registry.txt`. `tools/gen_themes.py` runs at CMake configure time: it validates every manifest, converts PNG artwork into LVGL image arrays, and emits `ui_theme_generated.c`.

- **Artwork.** `ring` (360x360, alpha) replaces the drawn bezel, `needle` replaces the drawn meter needle (LVGL rotates it around a pivot declared in the manifest; art must point right), and `dial` (360x360) becomes the page background. Anything omitted falls back to the drawn shape and its color role, so a colour-only theme is still a single file.
- **Slot stability.** `themes/registry.txt` pins slot -> id and is append-only. NVS stores the slot number, so reordering it would silently re-skin every existing device on the next OTA — invisible in local testing. The generator hard-fails on reordering, gaps, a non-`default` slot 0, or a registry line whose folder is missing.
- **Build-time validation** covers artwork dimensions, needle pivot bounds, missing files, duplicate roller names, unknown keys, and a total artwork budget (1536 KB, since every registered theme's art is linked in unconditionally). Every failure points at a file and line.
- Artwork conversion needs Pillow only when a PNG's SHA-256 changes; the generated C is checked in, so an ordinary build has no third-party Python dependency. `--check` verifies freshness for CI.
- **Fixed:** the Settings theme roller built its option list in a fixed 96-byte buffer and silently truncated once enough themes were registered. It now uses an exactly sized buffer, so truncation is structurally impossible.
- **Fixed:** leaving the RPM warning restored a hardcoded black background, which would permanently blank a themed dial face. All three restore paths now reapply the theme background.

Authoring guide: [themes/README.md](themes/README.md) (bilingual). Framework internals: [docs/THEMING.md](docs/THEMING.md).

### Multi-gauge linked RPM warning

Three gauges can now light up in sequence as revs climb, instead of all strobing at once. The 1000 rpm below the warning threshold is split into thirds; each gauge ramps black to red across its own third according to its configured position, and at the threshold all three strobe together. Every unit derives its own segment from the same ESP-NOW-synced RPM, so no extra inter-gauge messaging is needed and they stay in sync naturally.

- New LINKED toggle on the RPM warning page, mutually exclusive with the existing single-gauge flash.
- Changing the threshold on one gauge broadcasts it to the others (new ESP-NOW control packet).
- The test button drives a synthetic RPM ramp across all gauges (5 s rise, 0.8 s hold, 2.5 s fall).
- NVS config version 1 -> 2: adds `rpm_warn_linked_en`, and migrates the default theme index 1 -> 0 so existing devices keep their current look.
- RPM strobe redraw interval was 1 ms; it is now 25 ms.

### Partition layout and boot animation

- App partition shrunk 6 MB -> 4 MB; the bootmedia SPIFFS partition grew 9.8 MB -> 11.9 MB.
- **Flashing change: bootmedia moves from `0x620000` to `0x420000`.** Update your flash command and scripts.
- Boot animation re-encoded on a 300x300 grid (was 240x240) with a new `delta_varint_rgb565_black_v2` stream format.


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

---

## 中文

### 主题变成配置，不再是代码

加一套 UI 主题现在不用碰任何 C 文件。一个主题就是 `themes/` 下的一个文件夹，里面放一份 `theme.toml` 清单（8 个装饰色 + 可选的美术素材），再往 `themes/registry.txt` 末尾追加一行。`tools/gen_themes.py` 在 CMake configure 阶段自动运行：校验清单、把 PNG 素材转成 LVGL 图片数组、生成 `ui_theme_generated.c`。

- **美术素材**：`ring`（360x360，带透明通道）替换代码画的表框，`needle` 替换指针（LVGL 绕清单里声明的 pivot 旋转，素材必须画成朝右），`dial`（360x360）作为所有页面的背景。没声明的项自动回退到代码绘制 + 对应颜色角色，所以纯配色主题依然只要一个文件。
- **槽位稳定性**：`themes/registry.txt` 固定 槽位 -> id 的映射，且只能追加。NVS 存的是槽位号，一旦调序，所有已有设备在下次 OTA 后会被静默换成另一个主题 —— 而且本地测试根本发现不了。生成器对调序、跳号、槽位 0 不是 `default`、以及登记了却找不到文件夹的情况一律构建失败。
- **构建期校验**覆盖素材尺寸、指针 pivot 越界、文件缺失、滚轮重名、未知字段，以及素材总预算（1536 KB —— 因为所有已注册主题的素材都会被无条件链接进固件）。每条报错都精确到文件和行号。
- 素材转换只在 PNG 的 SHA-256 变化时才需要 Pillow；转换出来的 C 文件是入库的，所以常规编译不依赖任何第三方 Python 包。`--check` 模式供 CI 校验是否过期。
- **修复**：设置页的主题滚轮原来用一个固定 96 字节缓冲区拼选项，主题一多就会静默截断。现在按实际长度精确分配，结构上不可能再截断。
- **修复**：转速报警结束时会把背景恢复成硬编码的黑色，有表盘背景图的话会被永久抹掉。三条恢复路径现在都改为重新应用主题背景。

编写指南：[themes/README.md](themes/README.md)（中英双语）。框架内部实现：[docs/THEMING.md](docs/THEMING.md)。

### 三连表联动转速报警

三块表现在可以随转速上升**依次**亮起，而不是同时闪。报警阈值往下 1000 转被分成三段，每块表按自己配置的位置在自己那一段里由黑渐变到红，到阈值时三块一起闪。每块表都用同一份 ESP-NOW 同步过来的转速自行计算自己的区间，所以不需要额外的表间通信，天然同步。

- 转速报警页新增 LINKED 开关，与原有的单表闪烁互斥。
- 在任一块表上改阈值会广播同步给其它表（新增 ESP-NOW 控制包）。
- 测试按钮会在所有表上跑一段模拟转速曲线（5 秒上升、0.8 秒保持、2.5 秒回落）。
- NVS 配置版本 1 -> 2：新增 `rpm_warn_linked_en` 字段；默认主题索引从 1 改为 0，并带迁移逻辑，保证老设备升级后外观不变。
- 转速闪烁的重绘间隔原来是 1 毫秒，现在改为 25 毫秒。

### 分区调整与开机动画

- app 分区从 6 MB 缩到 4 MB，bootmedia SPIFFS 分区从 9.8 MB 扩到 11.9 MB。
- **烧录方式变更：bootmedia 的地址从 `0x620000` 变为 `0x420000`。** 请同步更新你的烧录命令和脚本。
- 开机动画按 300x300 网格重新编码（原来是 240x240），并改用新的 `delta_varint_rgb565_black_v2` 流格式。


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
