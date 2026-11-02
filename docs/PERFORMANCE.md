# 性能审计与测量

[English](PERFORMANCE.en.md) | 简体中文

本记录对应 2026-10-14 的当前工作树。已完成代码审计、十余处运行路径优化和可选实机统计；
桌面计时与主机测试不能证明 ESP32 的实际帧率、响应延迟或系统已经达到性能上限。

## 主题标签与需求掩码重复更新

普通左对齐、无缩放/旋转的运行时主题绑定标签保留 `LV_SIZE_CONTENT`。LVGL 8 的 `lv_label_set_text()` 会刷新文本并标记自适应尺寸布局，因此该路径不再额外调用 `lv_txt_get_size()`、固定尺寸和重复定位；居中、右对齐及变换标签保留原显式尺寸逻辑。真实 LVGL `test_page` 覆盖文本长度变化后的宽度增长与左锚点不变；这验证布局正确性，不是渲染耗时或实机帧率基准。

UI tick 仅在本地页需求掩码变化时发布 ESP-NOW 掩码；合并远端需求后，也仅在 OBD 总掩码变化时更新 ELM 客户端。远端需求超时或页面变化仍会导致新掩码立即发布。该去重减少稳定页面每帧的重复临界区写入；主机页面测试不覆盖这段 FreeRTOS 路径，设备端耗时和帧率仍待实测。

## 全项目路径审计

以下记录当前代码已有的机制。`host` 只代表纯 C 模块的正确性验证；
显示、DMA、BLE/WiFi 共存、SD 和任务调度效果仍需实机验证。

| Path | Current implementation | Verification |
|---|---|---|
| `signal_store.c` | batch metadata snapshots, sparse masks, generated numeric ID lookup, integer delivered units outside lock | host + device pending |
| `ui.c` / `app_main.c` | 16/33/200 ms ticks, conditional live snapshots, one NVS/linked-state view per RPM path, page-source sparse snapshots, 40/20-line DMA buffers | target compile + device pending |
| `theme_page.c` / `tr_eval.c` | coherent snapshot, binding cache, active-set output and applied-look reuse | host core + LVGL page + device pending |
| `theme_pixconv.c` | alpha row bounds, integer round-window/row spans, bounded channel quantization, mode-specific axis capacity | host differential + target compile + device pending |
| `vp_engine.c` / `vp_rpn.c` | serial polling, request-to-source dispatch index, poll candidate list, local-mask round deduplication, identity-factor single-precision writeback, hex lookup, bytewise bit fields, bounded command copies, fixed-format state commands, one RX-length commit per chunk, single-precision RPN integer checks, hardware raw32-to-float conversion | host + target compile + device pending |
| `racechrono_ble_diy.c` / `racechrono_filter.c` | event/deadline waits, due/subscribed snapshots, indexed filters, synchronized state | host + device pending |
| `espnow_link.c` | one broadcast/ramp worker, event-shortened rate-gated waits, batch signal reads/writes, shared config snapshot, cached IDs/MAC/hash | host codec + real worker/OTA + full target build + device pending |
| `sd_card.c` | 16 KB internal DMA bounce buffer, 40/20 MHz fallback | device pending |
| `video_player.c` / `video_ring_peek.h` | PSRAM ring, unlocked head reads, event-driven conditions, timed pacing/DMA, double buffers | host container + SPSC ring + real player control + full target build + device pending |
| `vp_common.c` / `obv1_parser.c` | device ROM CRC, portable host fallback | host + SDK Linux + device pending |
| `nvs_storage.c` / `storage_service.c` | cached configuration, serialized persistence, no SD worker without slot | host dual-config + full target build + device pending |
| `ota_wifi_server.c` / `upload_sink.c` | shared upload/storage pipeline | device pending |

已落地的优化是位字段提取、RaceChrono 到期筛选、主题规则输出复用、已应用外观复用、
主题页面活动页与绑定项缓存、RGBA 图片规划、ROM CRC 复用、信号输出单位整数换算，车型请求到 source 的静态索引、轮询请求局部掩码去重、ESP-NOW 主广播的标准信号静态 ID 查表与配置快照复用，以及 identity factor 的单精度写回快速路径；其他行是核对现有实现后的记录。
CPU/缓存/缓冲区配置不能单凭主机计时继续调高，下一步以对应路径的实际耗时和资源余量判断。

## ELM327 响应解析单次十六进制解码

`vp_engine.c` 的普通响应行和 ATMA 监视行都处在 BLE 接收后的热路径。普通响应的每个
十六进制 token 现在在校验时直接完成字节解码，避免先校验再重新转换；监视行的 token
在分词时缓存数值和合法性，后续识别 11 位 ID、29 位 ID、DLC 和数据字节直接复用缓存。
保留编号多帧、紧凑十六进制、空格分隔的 29 位 ID、CAF0 ISO-TP 以及扩展地址解析路径，
不改变 ELM327 单请求状态机或响应优先顺序。监视模式消费完整缓冲区时也跳过长度为 0 的
`memmove()`。

主机 `engine` 测试全部通过；使用当前 `build/compile_commands.json` 的 ESP-IDF 5.5.3
ESP32-S3 参数编译 `vp_engine.c` 到临时目标文件也通过。当前没有实车 BLE 接收耗时或
ESP32 周期测量，因此这里只记录重复扫描已移除，不宣称具体运行时间提升。

## BLE 命令发送热路径

`elm327_ble_client.c` 的 `sink_send()` 接收的命令都来自 `vp_engine`：车型 `bus.init` 在编译时已规范化为大写、无空格，标准 AT、状态切换和 OBD 请求由引擎直接生成，也不包含空格。发送路径因此改为一次有界长度计算、一次拷贝和一次回车追加，保留 20 字节 ATT 分片、写入顺序和错误处理；不再逐字符过滤空格并重复构造命令缓冲区。

这项改动减少的是固件侧发送前的 CPU 和内存访问，未测量 ESP32-S3 的具体周期收益。`vp_engine` 仍保持单线程、单请求状态机。

## UI tick 复用 NVS 配置与 linked 状态

`my_timerMain()` 每个 tick 开始时已经取得一份 `nvs_user_cfg_t` 快照。此前 RPM 警告的
“是否需要取完整 OBD 快照”判断和闪烁渲染又各自读取一次 NVS；主机模式下，
`espnow_link_linked_en()` 还会在这两条路径中重复读取 NVS。现在同一个 tick 将配置快照和
一次计算出的 linked-flash 状态传给两个 RPM 函数；standalone 模式直接跳过 ESP-NOW 查询。

这保持了配置在一个 UI tick 内的一致视图，同时移除了两个直接的重复配置复制和重复锁操作；
主机模式还避免了重复的 linked 状态查询及其内部 NVS 读取，最多减少四个额外的
`nvs_cfg_read()` 快照。RPM 阈值、闪烁条件、三连表 ramp 和刷新周期语义不变。

最初通过 `build/compile_commands.json` 中的 ESP-IDF 5.5.3 / ESP32-S3 参数编译
`espnow_link.c`、`ui.c` 和 `ui_ext.c` 到临时目标文件；本轮进一步使用可用的 IDF Python
环境完成普通和开启统计两种完整固件构建。实机周期测量仍未完成。

## 可配置页面按需读取 OBD 快照

`my_timerMain()` 的温度、信息、指针和趋势图页不再无条件读取全部 12 个 OBD slot。
先将当前页面的数据源映射为 slot 位掩码，相同数据源只读取一次，退役的显示槽位 9
不读取；需要 RPM 警告、闪烁或联动渐变时，再加入 RPM。指针页的数据源规划和渲染
使用同一份 tick 配置快照，移除每帧另一次 `nvs_cfg_read()`；公开指针更新接口保持不变。

| Page | Previous input slots/tick | Current input slots/tick |
|---|---|---|
| Needle / Chart | 12 | 1, at most 2 with RPM warning |
| Temp | 12 | at most 3, at most 4 with RPM warning |
| Info | 12 | at most 5, at most 6 with RPM warning |
| Gear / RPM / Speed | 3 | 3 (unchanged) |

重复映射和无效/退役项会使实际数量更小；只配置退役项且不需要 RPM 时，不读取信号。
新的 `obd_data_get_snapshot_mask()` 只进行一次批量读取，所有到期判断共享一个时间点，
未请求字段初始化为原有失效值。保留速度上限、正负压力 half-up 换算、链路/本地来源
规则与 RPM override；只有请求 RPM 时才访问 override。完整请求继续调用原有完整快照
实现，已有调用方不增加稀疏规划开销。扫表期间仍走原动画路径，轮询和无线频率不变。

新增主机测试逐项对照原完整快照，覆盖全部 4096 种 mask × 8 种有效性/override 场景、
两种压力的舍入边界、空 mask、额外位、NULL 输出、显示项映射 0..255、重复和退役项。
`signal_store` suite 的 4 项测试在 ASAN/UBSAN 下通过；全项目 7 套 host suite 共 36 项通过。

可重现的桌面微基准（AppleClang Release / `-O3`，关闭 sanitizer）命令：

```bash
cmake -S test/host/signal_store -B /tmp/obd-snapshot-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF
cmake --build /tmp/obd-snapshot-bench --target test_snapshot_mask
/tmp/obd-snapshot-bench/test_snapshot_mask --bench
```

每场景预热两条路径各 20,000 次，再交替运行 7 组、每组各 500,000 次，核对请求字段
checksum，取中位数。完整结果见 [CSV](performance/snapshot-mask-host-2026-10-14.csv)。

| scenario | requested_slots | full_ns | masked_ns | ratio |
|---|---|---|---|---|
| chart_oilp | 1 | 24.76 | 11.37 | 2.177 |
| chart_oilp_rpm | 2 | 23.63 | 12.53 | 1.886 |
| temp | 3 | 23.64 | 18.58 | 1.272 |
| temp_rpm | 4 | 23.62 | 19.11 | 1.236 |
| info | 5 | 23.60 | 22.87 | 1.032 |
| info_rpm | 6 | 23.60 | 24.44 | 0.966 |
| all | 12 | 23.62 | 24.96 | 0.946 |

`ratio = full_ns / masked_ns`。桌面锁为 stub，存储为普通主机内存，计时不含页面规划、
LVGL 绘制、PSRAM 延迟或 RTOS。6 项场景在桌面略慢，12 项对照包含新 API 包装开销，
不能据此声称所有路径都变快；当前固件收益的确定证据是减少请求 slot 数和一次指针配置
复制。实际耗时，特别是较密集的信息页，仍需用 ESP32-S3 的 `ui_tick` 统计复核。

## 主题页面缓存活动页与绑定项

`theme_tick()` 先使用缓存的活动主题页指针；只有当前 `lv_scr_act()` 变化、页面切换或缓存失效时，才回退到活动页链表查找。页面首次刷新仍遍历全部非图片元素，用于初始化未绑定的弧、条和指针；初始化完成后，只保留有 `data_source` 的非图片元素索引，静态装饰和未绑定控件不再每帧重复访问。

页面删除时清除活动页缓存，页面创建失败时索引分配失败会回退到完整遍历，不改变页面行为。主题规则、信号快照、页面切换动画和 LVGL 对象生命周期保持不变。该优化减少常态每帧的页面链表遍历和静态元素遍历；尚未在真实 ESP32 页面素材上测量帧时间，实机收益待测。

## 位字段提取基准

`vp_rpn_extract_raw()` 从逐 bit 提取改为逐字节片段提取：1–32 位字段最多读 5 个字节，
不使用未对齐的 word 读取，不越过声明的有效载荷，符号扩展去掉依赖数据正负的分支。
大小端、跨字节、32 位有符号值和错误时输出不变都与独立逐 bit 实现对照测试。

```bash
cmake -S test/host/vehicle -B /tmp/obd-perf-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVH_SANITIZE=OFF -DVH_BENCHMARK=ON
cmake --build /tmp/obd-perf-bench --target bench_raw
/tmp/obd-perf-bench/bench_raw
```

`bench_raw.c` 保留原生产逐 bit 算法作为基线，两边都使用运行时输入、每次一次间接调用、
相同数据和 checksum。预热后交替测量，每项 7 组、每组 1,000,000 次，取中位数。
2026-10-14 的 Apple M1 / arm64、Apple clang 21.0.0 Release 测量如下；
原始数据见 [CSV](performance/raw-host-2026-10-14.csv)。表中 ns 是桌面单次提取耗时，
不代表 ESP32 指令周期、OBD 请求吞吐或整机加速倍数。1 位 LE 的差异处于计时噪声范围。

| start | bits | endian | signed | bitwise_ns | bytewise_ns | speedup |
|---|---|---|---|---|---|---|
| 16 | 1 | le | 0 | 2.56 | 2.58 | 0.99 |
| 16 | 8 | le | 0 | 4.19 | 2.09 | 2.00 |
| 3 | 8 | le | 0 | 4.30 | 2.76 | 1.56 |
| 16 | 16 | le | 0 | 7.18 | 2.70 | 2.66 |
| 3 | 16 | le | 0 | 7.19 | 3.31 | 2.17 |
| 0 | 32 | le | 0 | 12.94 | 3.88 | 3.34 |
| 3 | 32 | le | 1 | 13.32 | 4.45 | 2.99 |
| 16 | 1 | be | 0 | 2.50 | 2.19 | 1.14 |
| 16 | 8 | be | 0 | 5.34 | 2.20 | 2.43 |
| 3 | 8 | be | 0 | 5.33 | 2.83 | 1.89 |
| 16 | 16 | be | 0 | 9.03 | 2.79 | 3.24 |
| 3 | 16 | be | 0 | 8.74 | 3.40 | 2.57 |
| 0 | 32 | be | 0 | 17.75 | 4.13 | 4.30 |
| 3 | 32 | be | 1 | 22.93 | 4.73 | 4.85 |

## RaceChrono 请求工作量

发送任务不再固定每 20 ms 唤醒：有效订阅存在时等待最早到期的通道；未连接、关闭
CCCD、deny-all 或仅订阅退役通道时，等待事件而不做周期检查。先计算订阅且到期的通道，
再进行一次批量信号读取；无到期通道时跳过信号存储。订阅状态按通道索引访问，
GATT 写入和发送任务用同一短临界区保护状态，读取信号和发送 BLE 都在锁外。

规划不更新发送时间；只有有效、仍符合最新筛选且连接/通知仍开启的样本才占用间隔。
保留 0=100 ms、至少 20 ms 的通道发送间隔、更新已有订阅保留时间戳、重连重置、
allow-all 独立时间戳等语义。批量采样间隔至少 20 ms：无效样本不占发送间隔，
但由独立重试期限限速，重复事件不能令信号存储忙轮询。`F109` 保留退役槽位，
不读取也不发送。

连接、断连、CCCD、筛选写入、服务就绪与 BLE shutdown 通过静态二值信号量唤醒任务。
令牌在阻塞前也保留并合并，避免“算好期限后、开始等待前”丢事件；信号量存储保留整个
boot，不随任务删除释放，晚到回调不会引用已删除的任务句柄。hub 先令 ready=false，
再在 hub 锁外调用已注册的 shutdown 唤醒函数；任务退出后仍保持单向 OTA teardown。
服务就绪在调用 start_service 后发布，采样中关闭通知也不会消耗发送期限。

期限向上换算到 RTOS tick；当前固件为 1000 Hz。非 20 ms 整数倍的订阅不再量化到旧
20 ms 检查网格，例如 101 ms 在虚拟时钟里按 101 ms 而非 120 ms 到期；实际执行仍受
调度、批量采样限速和 BLE 耗时影响，不保证精确发送周期。每个通知新增一个短状态快照
锁，等待规划也增加短锁与至多 8 个通道的期限计算，这是减少唤醒的代价，不是满负载
发送一定更快的证据。目标 ELF 中信号量存储 84 字节、指针 4 字节；hub 的 4 个 callback
槽各增加 4 字节，合计新增 104 字节静态 RAM，不为唤醒机制申请动态堆。

先前的按需快照对照仍以固定有效信号、1 秒、50 次检查为基线：

| Simulation | Before snapshots/s | After snapshots/s | Before input slots/s | After input slots/s | Notification candidates/s |
|---|---|---|---|---|---|
| all, 100 ms | 50 | 10 | 450 | 80 | 80 |
| one channel, 100 ms | 50 | 10 | 450 | 10 | 10 |
| deny all | 50 | 0 | 450 | 0 | 0 |

`Before input slots` 是旧批量 API 的输入项数，包含退役的 `SIG_SLOT_NONE`；
450 不代表 450 次真实信号读取。通知列是模拟的发送候选数，尚未测量 BLE 实际交付率。
本轮期限模型/真实 worker 的虚拟时钟对照如下；不计启动、控制事件和 shutdown 的唤醒：

| Simulation | Fixed-loop timed wakes/s | Deadline timed wakes/s | Snapshots/s | Notification candidates/s |
|---|---|---|---|---|
| all, 100 ms | 50 | 10 | 10 | 80 |
| one channel, 100 ms | 50 | 10 | 10 | 10 |
| deny all | 50 | 0 | 0 | 0 |
| disconnected / CCCD off / F109 only | 50 | 0 | 0 | 0 |
| all invalid, 100 ms | 50 | 46 | 46 | 0 |

无效场景在第 100 ms 首次采样，然后每 20 ms 重试。每毫秒一个筛选事件的测试仍只有
46 次采样，但事件自身仍会唤醒，不能把 0 次定时唤醒误读为 0 次任务运行。

`test_racechrono_filter.c` 与旧规则列表模型对照 20,000 步订阅/时间/有效性变化，
同时用独立绝对期限模型校验只读规划；覆盖 1 us 边界、0/最小/最大间隔、重加/重置、
独立历史、重试门控、无有效通道、64 位时间和有限等待饱和，269,071 项检查通过。
`test_racechrono_stream.c` 编译真实 worker、GATT 回调和 `ble_hub.c`，只替换 SDK/RTOS
原语与信号存储边界，在独立 boot 子进程中验证 12 个确定性场景：

- 全量/单通道、deny-all、断连、关闭 CCCD、仅 F109、无效值、每毫秒事件风暴、101 ms 订阅。
- 阻塞前 shutdown、等待中重连/筛选变更、采样中关闭/重新开启通知。
- 8 字节 wire 值/缩放不变、无 F109 读取、BT API/日志/信号读取均在锁外、服务就绪顺序、shutdown 幂等与删除后晚到唤醒。

ASan/UBSan 通过；在 `/tmp` 删除 shutdown 唤醒、删除筛选唤醒、提前发布服务就绪、
关闭通知仍占发送间隔的四种负对照都被测试拒绝。普通和统计固件完整构建通过，app
分别为 `0x2855c0` / `0x285e20`，3 MB 分区均余 16%。这些是路径、模型工作量、内存和
可构建性证据；没有模拟真实 FreeRTOS 双核竞争、射频拥塞或电流，实机收益仍待测。

## 主题规则输出复用

`tr_eval()` 每次仍按页面顺序更新全部规则的条件、滞回、去抖、定时/变化触发状态；
最终活动规则集合相同时复用排序结果和目标覆盖表，省掉排序、清零和动作重放。
活动集合改变时从空覆盖表重建，规则退出会撤销对应覆盖；`tr_reset()` 同时清除缓存。
编译后的规则/优先级/动作不可在两次 reset 之间修改，返回结果由程序拥有、只读。
缓存字段仅追加到 `tr_program_t` 末尾，不增加每帧分配，也不影响主题包或持久化格式。

对照测试以同一规则程序的强制重建路径为参照，运行 128 页 × 600 步，覆盖重复 id、
计时、滞回、去抖、无效值、稳定/变化集合、空集合、reset 和 NaN/Infinity 优先级；
比较全部运行状态、活动顺序和覆盖表，6,061,843 项检查通过。
编辑器黄金向量继续独立验证规则语义。

```bash
cmake -S test/host/rules -B /tmp/obd-perf-rules-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTR_SANITIZE=OFF -DTR_BENCHMARK=ON
cmake --build /tmp/obd-perf-rules-bench --target bench_rule_cache
/tmp/obd-perf-rules-bench/bench_rule_cache 0x63caf129
```

基准在同一 Apple M1 / arm64、Apple clang 21.0.0 Release 环境下运行，固定 seed，
预热后交替测量 7 组、每组 300,000 次，取中位数；两边 checksum 一致。
每条规则配置 8 个动作，命名不同目标；稳定场景全部活动，变化场景每步交替活动半数规则。
原始结果见 [CSV](performance/rules-host-2026-10-14.csv)。

| rules | targets | active_set | rebuild_ns | cached_ns | ratio |
|---|---|---|---|---|---|
| 0 | 0 | stable | 9.39 | 2.76 | 3.40 |
| 1 | 8 | stable | 27.15 | 4.52 | 6.00 |
| 8 | 64 | stable | 308.48 | 19.19 | 16.08 |
| 16 | 128 | stable | 673.94 | 32.66 | 20.63 |
| 16 | 128 | alternating | 319.07 | 319.15 | 1.00 |

`rebuild_ns` 是当前 evaluator 每次强制重建输出的时间，包含新的集合/缓存检查开销，
不是旧固件基线。比例表示此桌面合成场景中复用相对强制重建的收益，
不能当作整帧、ESP32 或修改前固件的加速倍数；活动集合每步变化时无法复用。

## 主题页面复用已应用外观

`theme_page.c` 每次仍更新信号变化窗口、数据绑定和 `tr_eval()`。活动规则集合与系统告警
抑制状态都不变时，跳过目标外观的拼装/比较；首次求值、reset 后首次更新、集合变化或
抑制切换仍应用外观，告警结束仍恢复 layout 背景。复用已有规则缓存状态，无新增持久化
字段或每帧分配。规则、目标和基础外观在页面生命周期内保持不变。

可选 `TE_PAGE_LVGL=ON` 主机测试直接编译实际 `theme_page.c` 和 LVGL 8.4.0；对照页面
每次额外执行原目标外观遍历，比较 LVGL 对象状态、已应用外观和规则运行状态。固定随机
seed `0x51fe9123`，10,000 步回放加定向场景共 680,850 项检查，覆盖去抖/计时、变化窗口
到期、无效输入、扫表、规则退出、报警恢复、空图片、指针角度及视频切换/暂停调用。
平台时间、信号快照和视频 I/O 使用主机替身；不验证 LCD DMA、视频解码或实际显示帧率。

```bash
cmake -S test/host/theme -B /tmp/obd-theme-page-tests -G Ninja -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-theme-page-tests
ctest --test-dir /tmp/obd-theme-page-tests --output-on-failure
```

默认读取 `managed_components/lvgl__lvgl`，可用 `TE_LVGL_DIR` 指定 LVGL 8.4.0 源码。
CI 固定上游提交 `4495f428630cc1741bd8bfd977f080e8460e8e8d` 并开启这项测试。
稳定页面基准：

```bash
cmake -S test/host/theme -B /tmp/obd-theme-page-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PAGE_LVGL=ON -DTE_PAGE_BENCHMARK=ON
cmake --build /tmp/obd-theme-page-bench --target bench_page
/tmp/obd-theme-page-bench/bench_page
```

Apple M1 / arm64、Apple clang 21.0.0 Release，无 LTO。合成页面交替放置 arc/bar/label，
最多 128 个目标、16 条规则，最多 32 个数据绑定。输入/时间在计时区间固定；每种路径预热
1,000 次，交替测量 7 组，每组 200,000 次更新，取中位数，checksum 一致。
测试包含当前信号快照替身、绑定和 evaluator，不包含页面创建/销毁或 LVGL 绘制。
全部 20 个场景（0/1/8/64/128 个目标 × 活动/不活动 × 抑制/不抑制）保存在
[CSV](performance/page-lvgl-host-2026-10-14.csv)。以下为活动且不抑制的代表行：

| targets | active | suppressed | forced_targets_ns | skipped_targets_ns | ratio |
|---|---|---|---|---|---|
| 1 | 1 | 0 | 33.74 | 28.60 | 1.18 |
| 8 | 1 | 0 | 73.42 | 40.09 | 1.83 |
| 64 | 1 | 0 | 453.33 | 173.78 | 2.61 |
| 128 | 1 | 0 | 866.99 | 327.64 | 2.65 |

`forced_targets_ns` 在当前更新路径后强制执行目标遍历；`skipped_targets_ns` 使用当前
跳过路径。两者都包含新的判断和已有 evaluator 缓存，并非旧固件前后对比。
比例仅表示这些固定输入桌面场景的更新路径收益；规则集合频繁改变时仍会执行目标遍历，
不能推导 ESP32 帧率、整机响应或无线/SD 吞吐。零目标对照也保留在 CSV 中。
正常固件 `0x2846d0` → `0x284700`（增加 48 字节），统计固件 `0x284ed0` → `0x284f10`
（增加 64 字节）；两者仍有 16% app 分区余量，实机耗时待测。

## RGBA 图片按行规划

`tpx_plan()` 先判定图片窗口是否完全位于圆屏内、圆屏外或与圆周相交。
完全在外时直接返回空规划；完全在内时无需圆周判断；相交时通过整数二分与圆对称
计算每行可见区间。包围盒只寻找每行首尾非透明像素，透明长段每次检查四个 alpha，
读入范围限定在当前可见区间内；裁剪后的不透明分类仍逐像素检查。
没有新增分配、静态查表或逐帧工作；RGB565 路径不变。

独立参考实现保留旧的逐像素圆周判断和包围盒更新。差分测试覆盖圆屏全部 129,600
个像素位置、圆周内外 alpha、所有裁剪模式、20,000 步固定种子 `0x70a9413b` 随机回放、
最大 2047×2047 尺寸及极端屏幕坐标，共 757,180 项检查，无失败。
极端坐标直接核对预期空规划，避免执行旧参考实现中会溢出的距离计算。
ASAN/UBSAN 下便携 suite 6/6 通过；本地 managed LVGL 与 CI 固定版本 LVGL
集成各 7/7 通过。后者是本地同配置验证，未运行远端 CI。

```bash
cmake -S test/host/theme -B /tmp/obd-plan-core-tests -G Ninja
cmake --build /tmp/obd-plan-core-tests
ctest --test-dir /tmp/obd-plan-core-tests --output-on-failure

cmake -S test/host/theme -B /tmp/obd-plan-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PLAN_BENCHMARK=ON
cmake --build /tmp/obd-plan-bench --target bench_plan
/tmp/obd-plan-bench/bench_plan
```

基准在 Apple M1 / arm64、Apple clang 21.0.0 Release、无 LTO 下运行，使用
16/64/360 px 方形合成图片，`opaque` / `transparent` / `mixed` / `rim`（4 px 边框）
四种 alpha 模式，全部三种裁剪方式。不启用圆裁剪的对照各运行一次；启用时覆盖
左上角 `(0,0)`、居中 `((360-w)/2, (360-w)/2)` 和完全在外 `(-w,-w)` 三种位置。
每条路径预热 20 次，交替测量 7 组取中位数，结果校验和一致；每组迭代次数为
`clamp(20000000/(w*w), 200, 20000)`。完整 144 个场景保存在
[CSV](performance/plan-host-2026-10-14.csv)。下表为 360×360、圆裁剪开启、位置 `(0,0)`、
`crop=0`（不裁剪包围盒）的代表行：

| pattern | per_pixel_ns | row_span_ns | ratio |
|---|---|---|---|
| opaque | 243285.00 | 53305.00 | 4.564 |
| transparent | 44300.00 | 25140.00 | 1.762 |
| mixed | 147875.00 | 5305.00 | 27.875 |
| rim | 50785.00 | 20875.00 | 2.433 |

`per_pixel_ns` 是旧规划器，`row_span_ns` 是当前规划器，`ratio` 为两者耗时比。
这些数值只覆盖校验、alpha 边界查找和格式分类，不包含分配、图像缩放、LVGL 绘制、
LCD DMA 或 ESP32 的内存访问成本。初次加载和缓存未命中时可减少图片规划工作，
不能据此推断整段主题加载时间或实机 FPS；全透明对照也保留在完整 CSV 中。

最终正常固件 `0x284700` → `0x2848f0`（增加 496 字节），统计固件
`0x284f10` → `0x2850f0`（增加 480 字节）；两个构建均通过，应用分区分别剩余
`0x7b710` / `0x7af10`（各约 16%）。实机图片加载耗时仍待测。

## 车型与视频校验复用 ROM CRC

ESP32 上的 `vp_crc32()` 和 `obv1_crc32()` 改用已有的 `esp_rom_crc32_le()`；
主机端仍保留车型逐 bit / 视频半字节表的纯 C 实现，不新增设备端查表数据或堆分配。
保留 IEEE CRC32 的初值、反射、最终异或、空输入和分段计算语义；已有车型 blob 和
OBV1 文件的 CRC 不变。这里只优化配置编译/校验和视频头/索引打开，不改变逐帧解码。

`test_crc.c` 用独立 Python zlib 固定向量、旧算法对照及随机分段，覆盖任意初值、
0–128 字节的全部长度、0–7 字节地址偏移、块边界、288,000 字节最大视频索引和单 bit 损坏。
纯 C 与 `ESP_PLATFORM` 两条路径各完成 421,932 项检查；后者链接 ESP-IDF 5.5.3 官方
Linux CRC 实现，另以原有容器测试验证损坏文件处理。SDK 兼容测试不等于实机 ROM 执行。

没有 SDK 时 `media` 仍可运行基础测试；设置 `MH_ESP_ROM_DIR` 可指定 `components/esp_rom`
目录，`MH_ROM_CRC=OFF` 可关闭 SDK 兼容测试。以下基准要求该目录可用：

```bash
cmake -S test/host/media -B /tmp/obd-perf-crc-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMH_SANITIZE=OFF -DMH_BENCHMARK=ON
cmake --build /tmp/obd-perf-crc-bench --target bench_crc
/tmp/obd-perf-crc-bench/bench_crc
```

旧逐 bit / 半字节算法在独立翻译单元中编译，和实际固件 wrapper 所调用的 SDK Linux
实现对比；Apple M1 / arm64、Apple clang 21.0.0 Release，无 LTO，固定 seed，预热后
交替测量 7 组、取中位数，checksum 一致。输入是合成字节缓冲区；大缓冲区用于算法
压力测量，不代表实际车型配置大小。保存结果时未并行运行构建或其他基准。
原始数据见 [CSV](performance/crc-sdk-host-2026-10-14.csv)，含偏移 0 和 3 的全部结果。

| path | bytes | offset | baseline_ns | sdk_host_ns | ratio |
|---|---|---|---|---|---|
| video_nibble | 60 | 0 | 283.06 | 151.32 | 1.87 |
| vehicle_bitwise | 1024 | 0 | 8079.83 | 2579.59 | 3.13 |
| vehicle_bitwise | 8192 | 0 | 64324.22 | 20525.39 | 3.13 |
| video_nibble | 8192 | 0 | 38539.06 | 20517.58 | 1.88 |
| vehicle_bitwise | 32768 | 0 | 257726.56 | 82234.38 | 3.13 |
| video_nibble | 288000 | 0 | 1351625.00 | 721250.00 | 1.87 |

`sdk_host_ns` 是桌面执行 SDK Linux 算法的耗时，不是 ESP32 ROM 的时间或导入/视频
总耗时。ESP32-S3 正常/开启统计的固件均构建通过，两处目标文件确实引用 ROM 接口，
最终 ELF 解析到 `0x40001c98`；视频私有 64 字节表不再出现在目标文件中。相同正常配置
下镜像由 `0x284790` 减至 `0x2846d0`（192 字节），这是体积变化，不是速度测量。
实机收益仍需比较导入与视频打开的实际耗时。

## 视频环形缓冲帧头移出临界区

`video_player.c` 的 `ring_peek()` 原来在 session 锁内读取 PSRAM 中的帧头。
现在使用 `video_ring_peek.h` 的内联读取器：锁内只快照内部 RAM 中的 `rpos` / `used`，
确认有已提交记录后，在锁外读取帧号和长度。缓冲只有一个生产者和一个消费者，生产者
只有在消费者释放记录后才能复用空间；因此快照后的帧头和回绕标记在读取期间不会被覆盖。
缓冲地址和容量在会话结束前不变，会话引用计数的释放规则不变。

回绕时重新入锁，从**当前** `used` 扣除尾部填充，保留快照之后生产者新提交的字节，
然后从头重新快照。空快照之后新提交的数据可以留到下一次 peek，不会读取未提交帧。
普通帧仍只进一次临界区；回绕多两次短元数据临界区，以换取所有 PSRAM 帧头读取均在锁外。
不改变视频格式、水位、帧率/丢帧策略、唤醒机制、DMA 或缓冲容量，不增加堆分配。

`test_video_ring_peek.c` 编译固件实际使用的内联读取器，仅替换 FreeRTOS 临界区 API：

- 与旧读取器对照 6 种容量共 240,000 步，覆盖空缓冲、重复 peek、填满与消费、显式/隐式回绕和非 4 字节帧长。
- 用 `mprotect(PROT_NONE)` 在每次入锁时保护环形数据页，出锁时恢复，验证普通帧和回绕帧头不在临界区内读取；临时负对照中的旧锁内读取器触发受保护页访问错误。
- 在快照出锁时注入生产者提交，检查空快照不暴露新帧、尾部扣减不丢失并发提交。
- pthread 双线程校验 200,000 条顺序记录，消费者释放前二次核对 payload，独立 ThreadSanitizer 运行通过。

普通和统计两份 ESP32-S3 固件均完整构建通过；目标 `video_player.c.obj` 的反汇编确认
`vPortExitCritical` 调用之后才执行两次 PSRAM 帧头 `l32i.n`。主机页保护和线程测试不能
复现 PSRAM cache miss、ESP32 双核中断或 SD 延迟；本项证明慢存储访问移出临界区，
不宣称具体 FPS 或实机临界区耗时改善幅度。

## 信号输出单位的整数换算

`signal_store_get_delivered_mask()` 的单位换算仍在临界区之外进行。油压在有效原始范围
0–2000 kPa 内，把 `floor(raw * 0.145038 + 0.5)` 改为
`(raw * 72519 + 250000) / 500000`，中间值不溢出 uint32；电压把
`floor(raw / 100.0)` 改为整数除法并修正负数余数，保持向下取整。
两条路径均检查注册表的操作、系数和取整策略；负数/超范围油压或不匹配的元数据仍使用
原双精度公式，其他信号保持原语义。不改变单位、有效性、过期和断连处理，不新增分配。

独立翻译单元保留原生产公式；对照所有 49 个槽位的整数边界、全部有效油压/电压、
负电压、200,000 步固定随机输入（seed `0x39049bd1`），逐 bit 比较 double 结果。
实际公开快照接口另穷举有效值并检查无效值 NaN 和断连门控；共 569,001 项检查，零失败。
`signal_store` suite 的 3 项测试在 ASAN/UBSAN 下通过。

```bash
cmake -S test/host/signal_store -B /tmp/obd-delivery-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF -DSS_DELIVERY_BENCHMARK=ON
cmake --build /tmp/obd-delivery-bench --target bench_delivery
/tmp/obd-delivery-bench/bench_delivery
```

Apple M1 / arm64、Apple clang 21.0.0 Release，无 LTO，禁止浮点收缩；对照和候选位于
独立翻译单元，通过间接调用读取 256 个运行时输入。预热 20,000 次，交替测量 7 组、
每组 1,000,000 次，取中位数，checksum 一致；计时期间没有并行构建或其他基准。
全部 6 个场景见 [CSV](performance/delivery-host-2026-10-14.csv)：

| case | iterations | floating_ns | integer_ns | ratio |
| --- | --- | --- | --- | --- |
| oil_pressure | 1000000 | 3.26 | 2.73 | 1.193 |
| battery_voltage | 1000000 | 2.60 | 2.29 | 1.133 |
| rpm_identity | 1000000 | 1.68 | 1.44 | 1.161 |
| veh_identity | 1000000 | 0.96 | 1.29 | 0.746 |
| oil_pressure_negative | 1000000 | 1.59 | 1.88 | 0.848 |
| battery_voltage_negative | 1000000 | 1.88 | 1.57 | 1.196 |

`ratio = floating_ns / integer_ns`；小于 1 表示候选更慢。自定义恒等值和负油压对照确有
退化，几纳秒的桌面函数计时也会受指令布局影响，不能据此宣称所有路径都加速。
此基准不含信号存储锁、时钟、PSRAM、页面更新或 ESP32 执行。

采用真实 ESP32-S3 `signal_store.c` 编译参数生成两套 wrapper 的
[反汇编](performance/delivery-xtensa-2026-10-14.txt)：元数据比较被展开为整数读取/比较，
没有 `__eqdf2` 或 `memcmp` 调用；有效油压路径使用整数乘法/移位，电压路径使用整数
除法/余数，仅保留最终整数到 double 的转换。通用回退仍引用双精度 helper 与 `floor`。
这证明目标路径移除了双精度运算，尚未测得实际设备的时间收益。

正常镜像 `0x2848f0` → `0x284aa0`（增加 432 字节）；统计镜像
`0x2850f0` → `0x285260`（增加 368 字节）。两种构建通过，app 分区均为 `0x300000`，
分别剩余 `0x7b560` / `0x7ada0`（16%）。

## signal_store 批量读取的元数据快照

当前读写路径在信号锁内读取一次单调时钟，再更新或检查信号时间戳。这样等待锁时另一核心提交的新样本不会因读取时间较早而被误判过期；单位换算仍在锁外。下面的历史基准不包含这次时钟位置调整，实际性能影响需重新测量。

`signal_store_get_many()` 和 `signal_store_get_delivered_mask()` 在多 slot 读取时，进入临界区后只读取一次
`local_mask`、已注册车型信号数量和 `sys.obd_link` 状态；每个 slot 仍在同一临界区内检查自己的
`valid`、时间戳、范围和动态信号槽位。单值读取和单 bit 请求保留直读快速路径，避免为一次读取建立
快照。没有改变 link gating、过期判断、局部传感器优先级或返回值。

基准使用当前 `signal_store.c` 与去掉元数据快照的对照副本，均为 Apple M1 / arm64、Apple clang 21.0.0
Release、`-O3`、无 LTO；每项预热 20,000 次，测量 7 次，每次 500,000 个调用，取中位数。每个场景
的 checksum 一致，原始结果见 [CSV](performance/signal-store-snapshot-host-2026-10-14.csv)。这是主机锁替身和固定
PSRAM 替身下的共享读取路径测量，不代表 ESP32 的 PSRAM 延迟或整机帧率。

| case | before_ns | after_ns | speedup |
|---|---:|---:|---:|
| many_1 | 7.55 | 6.89 | 1.096 |
| many_8 | 16.62 | 13.21 | 1.258 |
| many_16 | 29.37 | 20.02 | 1.467 |
| many_48 | 87.28 | 56.26 | 1.551 |
| delivered_1 | 5.75 | 5.49 | 1.047 |
| delivered_8 | 22.28 | 19.96 | 1.116 |
| delivered_16 | 41.18 | 33.96 | 1.213 |
| delivered_48 | 118.00 | 107.23 | 1.100 |

`speedup = before_ns / after_ns`。多 slot 批量读取收益随 slot 数量增加而增大；单 slot 路径保持在计时噪声内。
`signal_store` suite 的 3 项测试在 ASAN/UBSAN 下通过。

## ESP-NOW 主广播收集路径

`master_collect()` 每 100 ms 收集一次待广播信号。信号值继续按 OBD 和车型区间批量读取，标准信号的数字 ID
直接从静态 `sig_registry` 取得，不再为每个有效 slot 调用一次运行时 ID 查询；`sys.obd_link` 也和标准 OBD
区间在同一次 `signal_store_get_many()` 中读取，避免单独一次时间读取和临界区进入。本来只需要一个字段的
NVS 配置也在一次广播开始时复制并复用。车型信号仍按注册顺序生成 `SIG_VEH_ID_BASE + index`；从机车型切换时也一次批量失效全部车型 slot。
ESP-NOW v5/v6 编码、有效位、gear 特殊字段和单线程发送顺序均不变。活动车型的 profile hash 在 ESP-NOW 启动时缓存，广播和从机接收校验不再重复读取车型对象和遍历 ID 字符串。从机的绑定主机 MAC 也在链路启动时缓存，并在显式绑定/解绑时同步更新；接收回调不再对每个无线帧复制整份 NVS 配置或获取配置锁。

这只减少广播收集阶段和从机接收过滤阶段的重复锁/查表开销，不改变轮询频率或无线协议。ESP-NOW codec、signal_store、engine、
rules、theme 和 vehicle host suite 共 30 项测试全部通过；当前目标编译命令也已单独编译
`espnow_link.c` 和 `signal_store.c`。没有目标设备的周期采样，因此不记录具体百分比收益。

## ESP-NOW 广播与联动渐变合并为一个任务

主表原先有两个任务：广播任务闲时 10 Hz、TEST 时 50 Hz，独立渐变任务无论是否 TEST
都每 20 ms 唤醒。现在由广播任务在发包前计算一次 RPM 渐变，并将同一份 active/RPM
样本用于本地 override、packet flag 和 v5/v6 RPM；独立任务完全移除。一次广播的 NVS
快照同时提供报警阈值和 linked-enable，TEST 期间不再由第二个任务重复读取配置。

保留 5000 ms 上升、800 ms 保持、2500 ms 下降，阈值 + 200 的峰值及 uint16 饱和。
上升/下降的乘积上限小于 `65735 * 5000`，改用 uint32 运算，不再为这些乘积做 64 位乘除。
时间戳仍为 64 位，由短临界区和重启/结束状态一起保护；NVS、override、信号读取和无线 API
均在锁外。每包使用一份渐变样本，计算之后到达的新请求从下一包开始生效，避免 flag/RPM
来自不同阶段。

TEST 请求更新最近的开始时间并给 boot 生命周期的静态二值令牌。闲时等待可缩短到距离
上次发送结束至少 20 ms 的期限；已过期限时立即开始下一次发送。活动时仍以 20 ms 等待，
闲时仍以 100 ms 等待，两者都从上一批发送结束计时。令牌不会绕过期限，重复触发只重启
时间线，不增加广播频率。启动/停止边界不再由两个任务独立采样；因此第一份渐变值和结束
恢复值会与同包 flag 同步，不能声称与旧双任务调度的逐时刻输出完全相同。

OTA 先关闭 TEST 请求接收，再删除任务和清除 override；晚到的本地/从表请求不能重新开启
测试。静态令牌不随任务删除而释放，回调也不通知任务 handle。任务创建失败同样关闭请求
接收。从表的 TEST 控制包、渐变接收、存在报告和 v5/v6 wire 格式不变。

以下是 1 s 理想时钟工作量模型，不是 CPU 利用率或真实 RTOS 唤醒计数；含 t=0 首次执行，
不含 t=1 s。新路径由真实 worker 的主机回放验证，旧路径计数来自原两个循环的周期。

| case | before_task_iterations | after_task_iterations | before_config_reads | after_config_reads | data_batches |
|---|---:|---:|---:|---:|---:|
| idle_1s | 60 | 10 | 10 | 10 | 10 |
| test_1s | 100 | 50 | 100 | 50 | 50 |
| test_requests_every_1ms | - | 50 | - | 50 | 50 |

事件风暴场景另有 1000 次请求与 1001 次 semaphore take；它证明信号采样和无线发送不会
随事件频率放大，不意味着请求事件本身零开销。正常 TEST 启动也可能多消费一次令牌。

目标配置下移除 `3072` 字节动态任务栈和 `340` 字节 TCB 的分配请求（合计 `3412` 字节，
不含堆分配器开销）。`xTaskCreate()` 的栈单位在当前 IDF 中是字节；目标编译确认
`sizeof(StackType_t)=1`、`sizeof(StaticTask_t)=340`，SDK 检查其与 TCB 大小一致。新增静态
semaphore 为 `84` 字节、handle `4` 字节、锁 `8` 字节、接收开关 `1` 字节；同时删去旧任务
handle `4` 字节与共享 RPM `2` 字节，静态符号净增 `91` 字节（不含对齐）。这是代码/ABI
账本，不是启动后 free-heap 实测。合并任务仍使用 `3584` 字节栈；目标 `-fstack-usage`
显示 worker 自身静态帧 `672` 字节，**不含下层调用链**，实际栈高水位仍需实机测量。

主机测试直接编译 `espnow_link.c` 和真实 codec，仅替换 SDK/RTOS、NVS 与信号存储边界。
17 个独立 boot 场景覆盖闲置、本地/从表启动、v6、结束前重启、阈值变化、零/最大阈值、
每毫秒事件、等待前事件、active/idle OTA、任务创建失败、从表请求、64 位时间和计算结束
后/组包中的重启；独立公式检查渐变、override、解码后的 RPM/flag 和最小发送间隔。
普通及 Release + ASAN/UBSAN 无线 suite 均 4/4 通过。`/tmp` 下五种负对照全部被拒绝：
去掉事件唤醒、令牌绕过限速、停止后继续接收、忽略重启及 packet RPM 与样本不一致。
这是确定性注入，不覆盖真实双核抢占、WiFi/BLE 共存或 OTA 并发稳定性。

```bash
cmake -S test/host/espnow -B /tmp/obd-link-host -G Ninja -DEH_SANITIZE=ON
cmake --build /tmp/obd-link-host
ctest --test-dir /tmp/obd-link-host --output-on-failure
/tmp/obd-link-host/test_espnow_link
```

ESP32-S3 普通和统计两种完整构建均通过：普通镜像 `0x2855c0` → `0x285720`（增加 `352`
字节），统计镜像 `0x285e20` → `0x285f60`（增加 `320` 字节），3 MB app 分区均剩 16%。
两个 ELF 均不再含 `master_linktest_task` / `s_linktest_task_handle`。未测量真实功耗、广播
交付率、CPU 或内存余量，不据此宣称整机已达到极限。

## 车型请求分发与轮询候选索引

`vp_engine_create()` 在 profile 固定后建立 request → source 的静态链表，并缓存包含请求 source 的轮询信号。`req_done()` 只遍历当前请求可能引用的 source，再确认该 source 仍是信号当前 source；`start_round()` 只检查可能发起请求的信号，纯 CAN、derived、未映射和无请求 source 的信号不再进入每轮扫描。链表按 profile 的 signal/source 顺序建立，因此 fallback 选择和响应处理顺序不变；ELM327 仍保持单线程、单请求状态机。

新增索引的容量上限为 32 + 32 + 96 + 48 个 `uint8_t` 槽位，即 208 字节；对象实际大小还受布局和其他字段影响。请求索引只在 profile 创建时建立，车型切换时随 engine 一起释放，不增加每次响应的动态分配。

微基准只测请求选择/分发循环，生产函数和计时循环放在不同翻译单元，避免编译器利用固定请求数折叠路径。每个场景使用 256 个固定请求、预热 20,000 次、交替测量 7 组，每组 1,000,000 次，报告中位数；linear 和 indexed 的 checksum 必须一致。2026-10-14 的 Apple M1 / arm64、Apple clang 21.0.0 Release 结果如下，原始数据见 [CSV](performance/request-index-host-2026-10-14.csv)。

| case | signals | sources | requests | linear_ns | indexed_ns | ratio |
|---|---:|---:|---:|---:|---:|---:|
| catalog_like | 12 | 24 | 12 | 13.35 | 3.39 | 3.940 |
| mixed_max | 48 | 96 | 32 | 39.31 | 3.79 | 10.365 |
| sparse_max | 48 | 48 | 32 | 36.27 | 2.10 | 17.232 |
| can_heavy | 48 | 96 | 32 | 38.35 | 2.68 | 14.299 |

这里的 `ratio = linear_ns / indexed_ns`，只表示固定输入下的桌面选择循环收益，不代表整机、ESP32 指令周期或 OBD 往返延迟。主机 engine 12/12 测试和 ASAN/UBSAN 通过；使用现有 ESP32-S3 编译命令单独编译 `vp_engine.c` 也通过，目标对象 `.text` 为 12,836 字节。没有实车和适配器，仍未测量真实轮询周期、蓝牙吞吐和设备端收益。

基准命令：

```bash
cmake -S test/host/engine -B /tmp/obd-engine-request-bench \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEH_SANITIZE=OFF -DEH_REQUEST_BENCHMARK=ON
cmake --build /tmp/obd-engine-request-bench --target bench_request_index
/tmp/obd-engine-request-bench/bench_request_index
```

## 轮询规划用局部位掩码去重

`start_round()` 原来对每个到期候选信号扫描已收集的请求数组，最坏需要
48 × 32 次成员判断。现在使用一个局部 `uint32_t` 记录已收集请求；验证后的请求
索引只可能为 0..31，容量扩大到 32 以上会在编译时拒绝。请求数组仍按候选信号的
首次出现顺序填充，不按 bit 顺序遍历；共享请求的每个到期信号仍更新自己的
`last_poll_ms`。不新增 engine 字段、动态分配、任务或持久化状态。最多只有两个
`rate: every` 信号的小数组去重继续保留，不为这条短路径增加掩码开销。

`engine_poll_plan` 在测试翻译单元中直接编入生产 `vp_engine.c`，与改动前的独立
规划循环对照；生产文件没有测试 hook。覆盖 0..48 信号、倒序唯一/共享/成对/混合
请求、请求 31、period/slow 默认与期限、未映射/derived/无 source、CAN/request
source 切换、仅 every/空轮次和 32 位时间回绕。392 组状态各回放 256 轮，共
100,352 次完整 engine 状态及 slot 序列比较，ASAN/UBSAN 通过。5 个成功编译的
负对照（低 16 位混叠、不去重、仅唯一请求更新期限、跨轮保留 mask、逆序输出）均被拒绝。

基准从独立 driver 调用实际生产规划函数和旧循环，每组先验证完整状态，再预热
10,000 次、交替计时 7 组各 200,000 次，取中位数；包含规划与顺序敏感 checksum，
不含解析、传输或响应等待。测量配置为 Apple M1 / arm64、Apple clang 21.0.0 Release，
无 sanitizer/LTO。28 场景的原始数据见 [CSV](performance/poll-plan-host-2026-10-14.csv)。
`unique` 在 32 个请求索引内倒序循环，48 信号场景会重复前面的索引；`shared` 全部使用
请求 31。0/1 信号基本相当（初次单信号测量曾出现不足 1 ns 的退化），不能只引用
高信号数场景当作所有车型的收益。

| signals | pattern | linear_ns | mask_ns | ratio |
|---:|---|---:|---:|---:|
| 9 | unique | 36.60 | 24.62 | 1.486 |
| 9 | shared | 19.06 | 15.75 | 1.210 |
| 32 | unique | 283.67 | 86.94 | 3.263 |
| 32 | shared | 66.31 | 48.38 | 1.371 |
| 48 | paired | 322.60 | 98.88 | 3.263 |
| 48 | mixed_refs | 249.65 | 83.51 | 2.989 |

`ratio = linear_ns / mask_ns`。9 信号的规划循环约为旧循环的 1/1.21..1/1.50，
32/48 信号场景约为 1/1.30..1/3.26；这些是桌面局部算法结果，不代表实车轮询周期
或 ESP32 收益。ESP-IDF 5.5.3 / Xtensa GCC 14.2.0 的普通/统计/无槽三种配置中，
engine 对象仍为 5016 字节，`vp_engine_run()` 的编译器静态栈帧仍为 128 字节，其他
已生成函数的静态帧大小也未变化；目标对象 `size` 的 text/readonly 合计从 12317
降至 12288 字节。 [目标编译原始记录](performance/poll-plan-xtensa-2026-10-14.txt)。三种完整固件均构建通过；app 大小分别为 `0x2857a0`、`0x286000`、
`0x276160`。没有设备周期/任务最大栈深和实车有效更新间隔数据，仍待实机测量。

```bash
cmake -S test/host/engine -B /tmp/obd-poll-plan-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_POLL_BENCHMARK=ON
cmake --build /tmp/obd-poll-plan-bench
ctest --test-dir /tmp/obd-poll-plan-bench --output-on-failure
/tmp/obd-poll-plan-bench/bench_poll_plan
```

## 命令入队改为有界复制

`q_push()` 原来用 `snprintf("%s")` 复制命令，现在在清零的 step 中最多复制
19 字节，保留第 20 字节及尾部为零。保留整步清零、kind/delay、`req = -1`、
队列 head/count、环绕、满队列丢弃及其日志；不新增 engine 字段、任务、分配或
持久化状态，不改变单请求状态机的发包顺序、发送间隔或轮询期限。

`engine_queue_copy` 在私有测试翻译单元中编入真实 `vp_engine.c`，与独立旧版
入队/出队实现比较完整 engine 字节、返回 slot、出队 step 和丢弃日志。覆盖全部
24 个 head、0..24 个已占槽、8 个 kind、空串、18/19/20 字节边界、超长串、非 ASCII
字节、嵌入 NUL、delay 边界，以及 256 组各 512 步填满/丢弃/出队/环绕序列；共
246,272 次对照，ASAN/UBSAN 通过。字符串按实际长度分配，NUL 后就是 ASAN 边界。
独立旧版源码也通过同一测试；6 个成功编译的负对照（复制 20 字节、截短到 18 字节、
不清零、`req = 0`、错误环绕、丢弃时改 head）均被拒绝。生产代码无测试 hook。

基准从独立 driver 调用实际生产和参考入队/出队包装，计入 step checksum，但不包含
传输、响应等待或满队列日志。每组先做完整状态对照，再预热 10,000 次、交替计时
7 组各 1,000,000 次，取中位数；测量时不并行构建或运行其他基准。Apple M1 / arm64、
Apple clang 21.0.0 Release，无 sanitizer/LTO。24 场景原始数据见
[CSV](performance/queue-copy-host-2026-10-14.csv)。长于 19 字节的输入测试截断路径，
不代表实际发出的命令长度；正常成功入队路径约快 4.03..5.08 倍，不是整机速度收益。

| head | length | format_ns | copy_ns | ratio |
|---:|---:|---:|---:|---:|
| 0 | 0 | 45.62 | 9.78 | 4.666 |
| 0 | 4 | 50.86 | 10.02 | 5.075 |
| 0 | 12 | 50.86 | 10.92 | 4.656 |
| 0 | 19 | 52.07 | 12.86 | 4.049 |
| 23 | 20 | 51.74 | 12.83 | 4.032 |
| 23 | 255 | 56.58 | 12.80 | 4.420 |

`ratio = format_ns / copy_ns`。ESP-IDF 5.5.3 / Xtensa GCC 14.2.0 的普通/统计/无槽
三种配置都确认 `q_push()` 不再调用 `snprintf`，保留 `memset` 和满队列日志。
engine 对象仍为 5016 字节，`q_push()` / `vp_engine_run()` 的编译器静态栈帧仍为
48 / 128 字节，其他已生成静态帧大小不变；目标对象 `size` 的 text/readonly 合计
12288 → 12285 字节，不据此推断最终镜像节省或任务最大栈深。
[目标编译原始记录](performance/queue-copy-xtensa-2026-10-14.txt)。三种完整固件构建
通过；app 大小见原始记录。没有设备 CPU 周期和有效采样间隔实测，ELM/ECU 等待未减少。

```bash
cmake -S test/host/engine -B /tmp/obd-queue-copy-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_QUEUE_BENCHMARK=ON
cmake --build /tmp/obd-queue-copy-bench
ctest --test-dir /tmp/obd-queue-copy-bench --output-on-failure
/tmp/obd-queue-copy-bench/bench_queue_copy
```

## 适配器状态命令的固定格式编码

`push_state()` 不再用动态 `snprintf` 拼接 `ATSH` / `ATCRA` 的 CAN ID、`ATCEA`
地址和 `ATCAF` 开关。ID 保持 11 位掩码 + 3 位大写十六进制或 29 位掩码 + 8 位
大写十六进制，地址仍取低 8 位、输出 2 位；前导零、NUL、关闭哨兵、未知 state 的
CAF 默认分支和 step 的 st/val/req 均保留。`push_req()` 与状态编码共用只读十六进制表。
不改变发包顺序、请求间隔、轮询期限、满队列日志或单请求状态机，不新增 engine 字段、
分配、任务或持久化状态。最长命令所需容量由编译期检查保护。

目标编译参数含 `-fno-builtin-memcpy`，短前缀的 `memcpy` 会留下真实函数调用；固定
字节写入避免了这类调用，相比中间版本又减少 28 字节对象 text/readonly。常量
`snprintf("ATCRA")` / `snprintf("ATCEA")` 保留，三种目标配置均确认它们已折叠成
固定存储；最终 `push_state()` / `fmt_id()` 没有 `snprintf` 或 `memcpy` 调用。

`engine_state_format` 编入真实引擎，对照独立旧格式化实现的完整 engine 字节、出队
step 与满队列日志，双方使用相同生产 `q_push()`，不重复计入上一项复制优化。
覆盖全部 2048 个 11 位 ID、24 个 head、空/非空/临满/满队列、29 位 flag/mask 边界与
前导零、全部 256 个地址与关闭哨兵、全部 uint8 state 默认分支、int16 req 边界及
256 组各 512 步连续填满/丢弃/出队/环绕；共 178,928 次对照，ASAN/UBSAN 通过。
整份旧生产源码也通过同一测试；8 个成功编译的负对照（11 位掩码、29 位标志清除、
两种 ID 宽度、小写、地址关闭哨兵、CAF 开关、req 元数据错误）均被拒绝。
生产文件无测试 hook；8 套 host suite 45/45 通过。

桌面基准由独立 driver 调用构造/入队/出队包装并消费 checksum，不含传输、响应等待
或满队列日志。每组先验证完整状态，再预热 10,000 次，7 组各 1,000,000 次取中位数。
仅对照私有旧格式化参考函数会受编译上下文影响，不能把该列当作真实旧生产耗时；
`EH_STATE_BASELINE_SOURCE` 将整份旧源码放在独立翻译单元中，只重命名公开符号，
在同一进程、同一 engine 地址逐组交替计时两份实际生产包装。旧对象的 `__text` 字节
与独立编译基线一致；engine/probe 布局必须一致，此覆盖仅供本轮主机基准。
Apple M1 / arm64、Apple clang 21.0.0 Release，无 sanitizer/LTO。本代理未并行构建或
运行其他基准，但系统有后台负载；绝对 ns 和倍率不是设备速度或轮询频率结论。
24 场景及重复测量见 [CSV](performance/state-format-host-2026-10-14.csv) 和
[重复 CSV](performance/state-format-host-repeat-2026-10-14.csv)。

| head | case | baseline_ns | production_ns | baseline_to_production |
|---:|---|---:|---:|---:|
| 0 | hdr11 | 93.37 | 21.76 | 4.292 |
| 0 | hdr29 | 88.84 | 19.74 | 4.501 |
| 0 | rx_off | 13.54 | 19.20 | 0.705 |
| 0 | rx29 | 93.69 | 24.53 | 3.820 |
| 0 | cea_off | 12.95 | 19.22 | 0.674 |
| 0 | cea_f1 | 86.47 | 22.75 | 3.800 |
| 0 | caf0 | 144.74 | 43.54 | 3.324 |
| 23 | hdr11 | 91.18 | 19.97 | 4.565 |
| 23 | caf1 | 118.23 | 27.43 | 4.310 |

`baseline_to_production = baseline_ns / production_ns`。两次测量的动态参数/CAF 路径
约快 3.32..4.90 倍；两个常量 off 路径却呈现约 5..7 ns 的桌面下降，已完整保留，
不能宣称每条路径均提速。编译/链接布局和系统负载会影响主机微基准，不能直接推断
ESP32 回归或收益；目标常量路径仍是固定存储，`push_state()` 静态帧不增。
只有实机 A/B 才能决定总体收益，ELM/ECU 等待并未缩短。

ESP-IDF 5.5.3 / Xtensa GCC 14.2.0 的普通/统计/无槽配置中，engine 对象仍为 5016
字节；`q_push()` / `push_state()` / `vp_engine_run()` 的编译器静态栈帧仍为
48 / 64 / 128 字节。新增独立生成的 `fmt_id()` 静态帧为 32 字节（旧函数被内联），
其他已生成帧不增；不能由此判断任务最大栈深。对象 text/readonly 为 12285 → 12349
字节（+64），没有宣称节省 flash。证据对象与三份完整构建对象的相关函数字节一致，
ELF 含真实 helper 而不含测试/基线符号。app 大小为 `0x2857d0` / `0x286030` /
`0x276190`，相较上一轮增加 32 / 16 / 32 字节。
[目标与回归记录](performance/state-format-xtensa-2026-10-14.txt) 包含基线/当前源码
SHA256；[源码差异](performance/state-format-source-2026-10-14.diff) 可从本轮对应生产源码
重建本轮基线。

普通可选基准使用独立参考；重现实际生产 A/B 时，再从本轮对应源码反向应用保存的
差异、启用整份基线。源文件哈希必须与记录一致；常规测试的覆盖字段保持空值：

```bash
cmake -S test/host/engine -B /tmp/obd-state-format-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_STATE_BENCHMARK=ON \
  -DEH_QUEUE_SOURCE= -DEH_STATE_BASELINE_SOURCE=
cmake --build /tmp/obd-state-format-bench
ctest --test-dir /tmp/obd-state-format-bench --output-on-failure
/tmp/obd-state-format-bench/bench_state_format

cp main/vehicle/vp_engine.c /tmp/obd-state-format-before.c
patch -R /tmp/obd-state-format-before.c < docs/performance/state-format-source-2026-10-14.diff
cmake -S test/host/engine -B /tmp/obd-state-format-paired-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_STATE_BENCHMARK=ON \
  -DEH_QUEUE_SOURCE= -DEH_STATE_BASELINE_SOURCE=/tmp/obd-state-format-before.c
cmake --build /tmp/obd-state-format-paired-bench
ctest --test-dir /tmp/obd-state-format-paired-bench --output-on-failure
/tmp/obd-state-format-paired-bench/bench_state_format
```

## 接收块的长度统一写回

`vp_engine_on_rx()` 在局部 `size_t` 中累计已接收字节数，扫描完一块数据后才写回
`rx_len`。目标旧循环每追加一个非 NUL 字节都存储 16 位长度并做截断；新循环移除这两项
操作与一次寄存器复制，截断/写回移到循环外。仍扫描超出容量的全部输入，以保留后续
`>` 提示符和 CR/LF 检测；NUL 过滤、512 字节容量、终止符、空输入以及监听模式的
单次调用末尾消费时机不变。没有增加字段、内存分配、任务或请求并发。

`engine_rx_append` 编入真实引擎，对照独立保存的旧接收实现，逐次比较完整引擎字节与
真实监听解码产生的回调事件/数值/顺序和时钟调用数。219,423 次对照覆盖全部 256 个
字节值、5 种 busy/kind 状态、prompt 开关、5 个初始长度、12 个输入长度、NUL、NULL、
非 NULL 零长度、提示符在容量之外、每个 CAN 报文分片点，以及连续随机流和时间戳回绕。
显式断言监听得到 80°C 水温/90°C 油温、有效帧计数、拒绝标记及溢出消费时机。
ASAN/UBSAN 通过；整份旧源码也通过；8 个成功编译的负对照分别破坏 NUL 过滤、容量后
扫描、提示符、长度写回、满缓冲消费、busy/kind 门控和监听后的终止符，均被测试拒绝。
8 套主机 suite 46/46 通过；生产引擎无测试 hook。

桌面基准将整份旧源码和生产源码放在独立翻译单元，用符号别名链接，在同一进程、同一
engine 地址交替计时。两份源码和 probe 布局必须一致。每次包含长度/prompt 复位、
真实接收函数及结果读取；不包含完整状态复制、监听解码、BLE 传输或响应等待。
先校验并预热 10,000 次，7 组各 200,000 次取中位数；Apple M1 / arm64、Apple clang
21.0.0 Release `-O3`，无 sanitizer/LTO。本代理没有并行运行构建或其他基准，但系统
有后台负载。两次测量中 20/128 字节 ASCII、混合和提示符场景为 1.328–2.116 倍；
空/单字节和全 NUL 不保证收益，长块也有波动。复测全 NUL 1024 字节为 0.874 倍，
第一轮为 1.032 倍；不能声称所有输入都加速，也不能据此推算设备吞吐或轮询频率。
完整数据见 [CSV](performance/rx-append-host-2026-10-14.csv) 和
[重复 CSV](performance/rx-append-host-repeat-2026-10-14.csv)。以下摘自复测：

| case | bytes | baseline_ns | production_ns | baseline_to_production |
|---|---:|---:|---:|---:|
| ascii | 1 | 4.03 | 4.12 | 0.978 |
| ascii | 20 | 49.14 | 27.07 | 1.815 |
| ascii | 128 | 318.75 | 154.07 | 2.069 |
| mixed | 20 | 38.40 | 22.89 | 1.678 |
| mixed | 128 | 259.25 | 152.67 | 1.698 |
| prompt | 128 | 339.30 | 169.92 | 1.997 |
| ascii | 1024 | 2208.95 | 2115.81 | 1.044 |
| nul | 1024 | 599.57 | 686.04 | 0.874 |

三种 ESP32-S3 配置的 RX 函数 text 均为 174 → 170 字节，完整引擎对象 text/readonly
为 12349 → 12345 字节；编译期测得 `sizeof(vp_engine_t)` 保持 5016 字节，RX 静态栈帧
保持 32 字节。这不是最大任务栈或实机高水位测量。独立编译后的 RX 指令字节与完整
固件对象一致，ELF 不含 probe/基线符号；性能统计符号仅存在于统计配置。
普通/统计/无槽完整固件构建通过，app 分别为 `0x2857c0` / `0x286030` / `0x276190`，
相对上一轮分别为 -16 / 0 / 0 字节；链接对齐后的整包变化不能用函数字节直接推算。
源码哈希、反汇编、负对照与回归记录见
[目标证据](performance/rx-append-xtensa-2026-10-14.txt)，修改可用
[源码差异](performance/rx-append-source-2026-10-14.diff) 重建旧基线。

常规测试保持 `EH_RX_SOURCE` 为空，只有临时实验/负对照才指向源码副本；默认不跑计时。
可选基准必须提供 `EH_RX_BASELINE_SOURCE`，这些选项不影响固件。重现命令：

```sh
cp main/vehicle/vp_engine.c /tmp/obd-rx-append-baseline.c
patch -R /tmp/obd-rx-append-baseline.c < docs/performance/rx-append-source-2026-10-14.diff
cmake -S test/host/engine -B /tmp/obd-rx-append-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_RX_SOURCE= \
  -DEH_RX_BENCHMARK=ON -DEH_RX_BASELINE_SOURCE=/tmp/obd-rx-append-baseline.c
cmake --build /tmp/obd-rx-append-bench
ctest --test-dir /tmp/obd-rx-append-bench --output-on-failure
/tmp/obd-rx-append-bench/bench_rx_append
```

## RPN 整数操作的单精度范围检查

`to_u32()` / `to_shift()` 原来先把公式栈中的 `float` 扩展到 `double`，再做范围检查和
整数转换。ESP32-S3 目标编译因此反复引用双精度软件辅助函数。现在直接用单精度比较，
负值先截断到 `int32_t` 再转成 32 位模式，非负值直接转 `uint32_t`。检查保留有限值、
范围和失败时不改写输出的规则；移位仍接受 `(-1, 0)` 的负小数并截断为 0。
Binary32 在 `-2^31-1` 与 `-2^31` 之间没有可表示值，因此下界改为 `< -2^31` 接受的
位模式集合不变。公式 opcode、profile blob、轮询顺序和 NVS 布局不变。

`vehicle_rpn_integer` 默认对照独立旧双精度规则，检查 1,001,690 个浮点位模式和
610,140 条真实完整公式。范围覆盖有符号/无符号边界、全部边界附近的相邻位模式、
零/负零/次正规数、NaN/Inf、负小数、整数截断、非法移位与失败输出哨兵。
大数由合法常量和精确的二次幂乘法构造，覆盖六种整数 opcode 在执行器中的成功与错误
结果。可选 Release `--exhaustive` 已对照全部 4,294,967,296 种 Binary32 位模式，
两个私有转换的接受状态与输出均一致；连同默认样本共 4,295,968,986 次位模式对照。
整份旧源码通过同一套 ASAN/UBSAN 测试；可选 Release 测试还对照独立翻译单元中的整份
旧执行器。7 个成功编译的负对照分别破坏整数下/上界、负值截断、负小数移位、移位上界、
NaN 检查和失败输出哨兵，全部被拦住。本轮 8 套主机 suite 47/47 通过。

主机基准使用同进程、同一份公式和输入地址上的两份完整执行器，Release `-O3`、关闭
sanitizer/LTO，预热 10,000 次后交替测量 7 组，每组 500,000 次，取中位数。
12 种公式包含整数操作、失败路径和纯字节/位字段/转速对照；两份结果与 checksum 一致。
Apple M1 的首次/复测大多只有小幅差异，`sext8` / `sext16` 两次均略慢，不能宣称所有
公式更快。没有本轮其他构建/测试并发计时，但机器后台负载未受控。这些数据不代表
ESP32 耗时或 OBD 轮询频率。原始数据见 [首次 CSV](performance/rpn-integer-host-2026-10-14.csv)
和 [复测 CSV](performance/rpn-integer-host-repeat-2026-10-14.csv)，下表摘自复测：

| case | baseline_ns | production_ns | baseline_to_production |
|---|---:|---:|---:|
| byte_and | 12.49 | 12.20 | 1.023 |
| raw_or | 15.65 | 15.67 | 0.999 |
| raw_shr | 15.31 | 15.01 | 1.020 |
| signed_shl | 15.33 | 15.01 | 1.021 |
| negative_shift_fraction | 15.32 | 15.02 | 1.020 |
| invalid_shift | 14.49 | 14.11 | 1.027 |
| raw_sext8 | 11.47 | 11.59 | 0.989 |
| raw_sext16 | 11.47 | 11.57 | 0.991 |
| fraction_and | 17.65 | 17.65 | 1.000 |
| rpm_control | 23.42 | 23.12 | 1.013 |
| byte_control | 5.00 | 5.03 | 0.994 |
| raw_control | 9.41 | 9.40 | 1.001 |

三种 ESP32-S3 配置的 `vp_rpn_eval` text 均为 1908 → 1502 字节，完整 RPN 对象的
text/readonly 均为 3736 → 3210 字节；执行函数的编译期静态栈帧为 176 → 96 字节，
不是任务最大栈或实机高水位测量。RPN 对象不再引用 `__extendsfdf2`、`__fixsfdi`、
`__gtdf2`、`__ledf2`、`__unorddf2`，仍保留除法/原始字段所需的 `__divsf3` / `__floatdisf`；
不代表整个固件不再使用双精度。独立编译后的执行函数指令和 literal 字节与完整构建对象
一致，ELF 不含整数 probe/基线符号，性能统计符号只存在于统计配置。
普通/统计/无槽完整固件构建通过，app 为 `0x285620` / `0x285e80` / `0x275fe0`，
相对上一轮分别减少 416 / 432 / 432 字节。目标对象、反汇编、哈希和回归记录见
[目标证据](performance/rpn-integer-xtensa-2026-10-14.txt)，旧基线可由
[源码差异](performance/rpn-integer-source-2026-10-14.diff) 重建。

常规测试保持 `VH_INTEGER_SOURCE` 为空；该选项只供临时私有 probe 实验/负对照。
`VH_INTEGER_BENCHMARK=ON` 必须提供整份旧源码 `VH_INTEGER_BASELINE_SOURCE`。
默认不跑穷举/计时，这些选项不影响固件。重现命令：

```sh
cp main/vehicle/vp_rpn.c /tmp/obd-rpn-integer-baseline.c
patch -R /tmp/obd-rpn-integer-baseline.c < docs/performance/rpn-integer-source-2026-10-14.diff
cmake -S test/host/vehicle -B /tmp/obd-rpn-integer-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVH_SANITIZE=OFF -DVH_INTEGER_SOURCE= \
  -DVH_INTEGER_BENCHMARK=ON -DVH_INTEGER_BASELINE_SOURCE=/tmp/obd-rpn-integer-baseline.c
cmake --build /tmp/obd-rpn-integer-bench
ctest --test-dir /tmp/obd-rpn-integer-bench --output-on-failure
/tmp/obd-rpn-integer-bench/test_rpn_integer --exhaustive
/tmp/obd-rpn-integer-bench/bench_rpn_integer
```

## 原始位字段的 32 位浮点转换

`PUSH_RAW` 的提取函数用 `int64_t` 返回结果，但字段宽度最多 32 位。有符号值已经
处于 `int32_t` 范围，无符号值处于 `uint32_t` 范围。执行器现在按 `VP_RAW_SIGNED`
选择这两种窄转换，保留同一提取函数、字段检查和错误返回。ESP32-S3 原来的
`__floatdisf` 软件调用被 `float.s` / `ufloat.s` 指令替代；opcode、profile blob、
NVS 和请求调度不变。

`vehicle_rpn_raw_float` 复用实际 RPN 文件的私有 probe，对照独立逐 bit 提取和旧
`int64_t` 转 `float` 规则，默认进行 4,274,988 次完整公式比较，其中有 1,319,950 个
有效字段案例。覆盖 0..33 位宽、LE/BE、有符号/无符号、未知 flag 位、非对齐字段、
精确 payload 末端与一字节不足、`UINT16_MAX` 附近字段、空指针、整数舍入
边界与半 ULP 附近值，以及真实乘法/减法后处理。短 payload 采用只分配声明长度且
起始地址偏移一字节的缓冲区，ASAN 可捕获读取越界；错误码和失败输出哨兵也逐一对照。
整份旧源码通过同一套 ASAN/UBSAN 测试。

可选 Release `--exhaustive` 已覆盖全部 4,294,967,296 个 32 位原始值，分别执行有符号
和无符号完整公式，输出 float 位模式同时与独立参考和独立翻译单元中的整份旧执行器
一致；含常规样本共 8,594,209,580 次公式比较。7 个成功编译的负对照分别破坏有符号
选择、强制 signed/unsigned、截断为 16 位、flag 检查、结果偏移和失败哨兵，全部被捕获。
本轮 8 套主机 suite 48/48 通过；Release 可选配对测试 14/14 通过。

主机基准继续使用同进程和同一公式/输入地址，Release `-O3`，sanitizer/LTO 关闭，
10,000 次预热后交替测量 7 组，每组 500,000 次，取中位数。16 种公式包含不同宽度、
字节序、符号、非对齐/错误路径、温度换算和 byte/RPM 对照；结果和 checksum 相同。
Apple M1 两轮中单个 raw 字段多为回退，复测比值 0.956–0.969，温度公式为 1.016。
该主机有硬件 64 位转换，不能用它的结果推断 ESP32 的软件调用代价；采用依据是
目标路径移除 `__floatdisf` 并使用硬件转换指令。没有本轮其他构建/测试并发计时，
但机器后台负载未受控；ESP32 实际耗时与轮询频率改善仍未测量。
原始结果见 [首次 CSV](performance/rpn-raw-float-host-2026-10-14.csv) 和
[复测 CSV](performance/rpn-raw-float-host-repeat-2026-10-14.csv)，下表摘自复测：

| case | baseline_ns | production_ns | baseline_to_production |
|---|---:|---:|---:|
| raw8_unsigned_le | 7.82 | 8.17 | 0.956 |
| raw8_signed_le | 7.84 | 8.19 | 0.958 |
| raw16_unsigned_be | 8.47 | 8.78 | 0.965 |
| raw16_signed_be | 8.45 | 8.76 | 0.965 |
| raw21_unsigned_unaligned | 8.75 | 9.06 | 0.965 |
| raw21_signed_unaligned | 8.75 | 9.07 | 0.965 |
| raw32_unsigned_le | 9.39 | 9.70 | 0.968 |
| raw32_signed_le | 9.37 | 9.68 | 0.968 |
| raw32_unsigned_be | 9.37 | 9.68 | 0.968 |
| raw32_signed_be | 9.38 | 9.68 | 0.969 |
| raw32_short_error | 5.64 | 5.62 | 1.003 |
| raw_width_error | 4.50 | 4.49 | 1.001 |
| raw_start_error | 4.71 | 4.70 | 1.003 |
| raw_temperature | 19.23 | 18.92 | 1.016 |
| byte_control | 5.10 | 5.09 | 1.001 |
| rpm_control | 23.63 | 23.20 | 1.019 |

三种 ESP32-S3 配置的 `vp_rpn_eval` text 均为 1502 → 1534 字节，完整 RPN 对象
text/readonly 为 3210 → 3238 字节，静态栈帧保持 96 字节。这项修改增加本地分支/指令，
以移除逐字段的软件转换调用；不代表整个固件不再引用 `__floatdisf`。独立编译的函数
指令/literal 与完整构建对象字节一致，ELF 不含 probe/基线符号，统计符号只出现在
统计配置。普通/统计/无槽固件完整构建通过，app 分别为 `0x285640` / `0x285ea0` /
`0x276000`，相对上一轮均增加 32 字节；静态帧不是任务最大栈或设备高水位测量。
目标对象、反汇编、哈希、穷举结果和负对照见
[目标证据](performance/rpn-raw-float-xtensa-2026-10-14.txt)，旧基线可由
[源码差异](performance/rpn-raw-float-source-2026-10-14.diff) 重建。

该测试复用 `VH_INTEGER_SOURCE` 临时 probe 覆盖，常规 CI 保持空值。
`VH_INTEGER_BENCHMARK=ON` 同时构建可选 `bench_rpn_raw_float`，必须提供整份旧源码
`VH_INTEGER_BASELINE_SOURCE`；与整数转换基准共享 driver，场景按编译宏选择。
默认不跑穷举/计时，这些选项不影响固件。重现命令：

```sh
cp main/vehicle/vp_rpn.c /tmp/obd-rpn-raw-float-baseline.c
patch -R /tmp/obd-rpn-raw-float-baseline.c < docs/performance/rpn-raw-float-source-2026-10-14.diff
cmake -S test/host/vehicle -B /tmp/obd-rpn-raw-float-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVH_SANITIZE=OFF -DVH_INTEGER_SOURCE= \
  -DVH_INTEGER_BENCHMARK=ON -DVH_INTEGER_BASELINE_SOURCE=/tmp/obd-rpn-raw-float-baseline.c
cmake --build /tmp/obd-rpn-raw-float-bench
ctest --test-dir /tmp/obd-rpn-raw-float-bench --output-on-failure
/tmp/obd-rpn-raw-float-bench/test_rpn_raw_float --exhaustive
/tmp/obd-rpn-raw-float-bench/bench_rpn_raw_float
```

## Identity factor 写回快速路径

创建 engine 时用一个 `uint64_t` 位图记录单位因子为 `1.0` 的信号，常见标准信号由此
绕过双精度单位换算。首版使用 `floorf(v + 0.5f)`，golden 样本通过但存在未覆盖的
舍入边界；当前实现已由下一节的精确硬件转换替代。其他单位因子保留双精度表达式。
位图不改变 profile blob 或 NVS 布局，实际 CPU/轮询收益仍待设备测量。

## 恒等单位写回的精确硬件舍入

性能审查发现上一版 `floorf(v + 0.5f)` 快速路径会提前舍入：输入
`0x3effffff`（紧邻且小于 `0.5`）得到 `1`，原始 `floor((double)v + 0.5)` 得到 `0`；
`8388609.0f` 也会错误进位。常规车型样本与 golden 请求测试没有覆盖这些边界。

现在先检查输入位于有符号 32 位转换范围，再截断为整数并计算原值与整数的差。
正小数部分 `>= 0.5f` 加一，负小数部分 `< -0.5f` 减一；负半值保留截断值。
Binary32 有小数的值幅度不足以让最终加减一溢出；大整数转换回 float 可精确恢复输入。
非有限值和越界值在转换前拒绝。保留创建时的 identity 位图、非恒等单位的双精度公式、
post/valid/registry/filter/写回次序与失效回调；不增加 RAM 字段，也不改变 profile/NVS。

默认 `engine_write_rounding` 将实际生产 engine 编入私有 probe，进行 1,720,944 个
舍入输入对照与 33,280 次完整 engine 状态/回调对照。覆盖半值及相邻值、大整数、
次正规数、NaN/Inf、转换边界、失败哨兵、位图低/高位、标准/自定义槽位、
5 种单位因子、速度缩放、post/valid 与两个过滤器。参考写回使用原始双精度规则。
可选 Release `--exhaustive` 对全部 4,294,967,296 种 Binary32 位模式检查舍入 helper，
含常规样本总计 4,296,688,240 次；完整 pipeline 对照仍是采样而非穷举。
8 个负对照均先编译成功再被拒绝，包含旧快速路径、正/负半值边界、提前进位、失败哨兵破坏，
以及仅修改真实 pipeline 的旧舍入、单位因子遗漏和有效范围判断。

8 套 ASAN/UBSAN suite 共 49 项通过，另一次 Release engine 6/6 通过。
Apple M1 / arm64 / Apple clang 21，Release `-O3`、无 sanitizer/LTO：相同输入地址、
同进程间接调用、预热 10,000 次，交替 7 组各 1,000,000 次取中位数，结果和 checksum 一致。
计时期间没有其他本地构建/测试，后台系统负载未控制。
[首轮](performance/write-rounding-host-2026-10-14.csv)和
[复测](performance/write-rounding-host-repeat-2026-10-14.csv)均显示有限值场景主机退化；
复测如下，`double_to_production = double_ns / production_ns`：

| case | double_ns | production_ns | double_to_production |
| --- | --- | --- | --- |
| rpm_integer | 1.25 | 1.57 | 0.798 |
| temperature_fraction | 1.25 | 1.57 | 0.797 |
| half_neighbors | 1.25 | 1.57 | 0.799 |
| large_identity | 1.25 | 1.56 | 0.802 |
| mixed_patterns | 1.63 | 1.62 | 1.003 |
| invalid | 1.25 | 1.25 | 0.999 |

主机复测有效值耗时约增加 25%，随机/无效值接近持平。M1 的 double `floor` 是硬件指令；
此基准只测舍入，不含单位选择、过滤、存储、BLE 或轮询等待，不能证明 ESP32 的时间收益。

ESP32-S3 真实编译命令确认恒等路径使用 `trunc.s` / `float.s`，不再调用 `floorf`
或双精度 helper。`sample_ok` 中的非恒等路径仍使用 double helper，EMA 仍使用 `floorf`，
不能声称整个 engine 移除了它们。三种配置的 engine 对象 text/readonly
`12345 → 12350`，内联后的 `sample_ok` text `1058 → 1066`，静态 frame 保持 `64` 字节。
静态 frame 不是任务栈峰值。独立编译与完整固件对象的指令/常量字节完全一致；
ELF 不含测试 probe 符号。普通/统计/无槽完整构建通过，app 分别为
`0x285650` / `0x285eb0` / `0x276010`，各增加 `16` 字节。

[目标证据](performance/write-rounding-xtensa-2026-10-14.txt)保存反汇编、源码/镜像哈希、
穷举与负对照结果；[源码补丁](performance/write-rounding-source-2026-10-14.diff)可还原旧快速路径。
采用依据是修复边界行为并移除目标恒等路径的库调用，设备 CPU/轮询时间仍待实测。

```bash
cmake -S test/host/engine -B /tmp/obd-write-rounding-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_WRITE_BENCHMARK=ON
cmake --build /tmp/obd-write-rounding-bench
ctest --test-dir /tmp/obd-write-rounding-bench --output-on-failure
/tmp/obd-write-rounding-bench/test_write_rounding --exhaustive
/tmp/obd-write-rounding-bench/bench_write_rounding
```

## 未采用的 CAN 索引实验

`vp_engine.c` 对每帧最多扫描 16 条 CAN 规则。主机实验使用稳定排序索引和二分查找，
保留相同 CAN id 的原规则顺序；少于 5 条时回到线性扫描。未接入固件。

```bash
cmake -S test/host/engine -B /tmp/obd-can-index-review -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_CAN_BENCHMARK=ON
cmake --build /tmp/obd-can-index-review --target bench_can_index
/tmp/obd-can-index-review/bench_can_index
```

Apple M1 / arm64、Apple clang 21.0.0 Release，无 LTO；两种选择函数放在独立于计时循环
的翻译单元，避免已知规则数把其中一条路径折叠。0/1/2/4/8/16 条规则 × 唯一/成对/相同
id × 全 miss/25% hit/全 hit，共 54 个场景；每场景 256 个固定查询，先核对顺序敏感
checksum，再预热 20,000 次、交替测量 7 组，每组 500,000 次，取中位数。
完整结果含零规则与低规则数对照，见 [CSV](performance/can-index-host-2026-10-14.csv)。
以下为进入索引路径的全命中场景：

| rules | pattern | stream | linear_ns | indexed_ns | ratio |
| --- | --- | --- | --- | --- | --- |
| 8 | unique | hit | 4.10 | 7.06 | 0.580 |
| 8 | paired | hit | 3.67 | 8.24 | 0.446 |
| 8 | same | hit | 3.76 | 17.74 | 0.212 |
| 16 | unique | hit | 5.40 | 9.00 | 0.600 |
| 16 | paired | hit | 5.31 | 11.72 | 0.453 |
| 16 | same | hit | 7.56 | 33.00 | 0.229 |

`ratio = linear_ns / indexed_ns`；这些场景中的索引更慢，因此继续保留当前线性扫描。
这里只计时规则选择和顺序敏感 checksum，不含串口解析、RPN、回调或 RTOS；不证明
ESP32 必然同样退化，也不把索引实验算作已落地优化。

## 无 SD 插槽时不创建存储后台任务

`CONFIG_OBD_SD_SLOT_SDMMC_1BIT` 未启用时，`sd_card_is_mounted()` 永远返回 false，
原来的 `storage` 任务每秒轮询一次，但永远不能进入卡任务。现在根据**插槽配置**编译掉
该 worker、任务 handle 和任务创建路径，不按 V1/V2/V3 硬编码，也不因暂时没插卡而跳过。
`storage_service_start()` 在无插槽时幂等返回 `ESP_OK`；有插槽时保留原来的任务、首次
5 秒延迟、插卡/OTA 结束重跑及忙任务完成后 10 秒重试。

`storage_service_boot()` 和 `storage_service_busy()` 不受条件编译影响：配置挂载/首启迁移、
主题断电恢复、启动导入扫描的挂载判断、旧车型重试以及 UI 忙状态查询仍保留。
这不是删除存储服务，也不改变 Flash 配置、HTTP 上传或 NVS 的持久化布局。

在 ESP-IDF 5.5.3 的当前 ESP32-S3 配置下，目标编译证实 `sizeof(StackType_t) = 1`、
`sizeof(StaticTask_t) = 340`（SDK 断言与真实 TCB 等大）、`sizeof(TaskHandle_t) = 4`。
无插槽时消除 `8192 + 340 = 8532` 字节动态任务分配请求（不含分配器开销），并删除
`4` 字节静态 handle，没有新增任务或同步对象。它也不再产生原来每秒一次的周期唤醒。
这是源代码/目标 ABI 账本，不是启动后 free-heap、CPU 利用率或功耗的实测值。

新增 `storage` host suite，直接编译真实 `storage_service.c` 和项目 API 头文件，只有 SDK /
RTOS 与存储操作边界为 stub。该优化最初验证的带插槽 15 场景、无插槽 7 场景分别在独立子进程运行，
避免重置生产代码的 boot 生命周期状态。虚拟时钟覆盖启动错误后的恢复顺序、幂等/创建失败
重试、长期无卡、插入/移除/重插、OTA 暂停/结束重跑、OTA 中插卡、忙任务按位重试、
耗时任务完成后的重试期限、非 busy 错误、32 位毫秒溢出后的期限及 UI 忙状态。

```bash
cmake -S test/host/storage -B /tmp/obd-perf-storage-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/obd-perf-storage-host
ctest --test-dir /tmp/obd-perf-storage-host --output-on-failure
```

Release + ASAN/UBSAN 的两个测试通过，CI 新增同一双配置 suite。5 个临时负对照均被拒绝：
恢复无条件创建、无槽跳过 boot、空槽跳过启动、忽略 OTA 和丢掉 busy 重试。
目标对象比较确认有槽配置的 worker/start/boot/busy 指令字节均未变化；无槽配置的
boot/busy 字节未变化，worker、私有 handle 和任务创建/延迟引用消失。无槽最终 ELF
不含 `service_task`，保留 3 个公开 storage API。

有槽普通、有槽统计和无槽普通三种固件完整构建通过，镜像分别为 `0x285720`、
`0x285f60`、`0x2760e0`；有槽两种大小与本轮前相同。无槽构建保留 V1 硬件选择，仅改变
SD 插槽选项，验证该优化不绑定板卡编号。不同配置的镜像大小不能作为本改动的独立体积收益。
所有构建、SDKCONFIG 和负对照在 `/tmp`，未改仓库 `build/` 或发布产物。
未实测 SD 热插拔/文件导入、任务调度、长期稳定性或 heap 余量，仍需按实机验收矩阵复核。

## SD 状态驱动的 storage 等待

`sd_card_poll_wait_ms()` 按卡状态给出下一次有效探测的剩余时间：已挂载 1 秒、缺卡或
已弹出 3 秒、存在但不可挂载 10 秒。storage worker 在每次探测后按此时间休眠；若有
待重试作业则取重试期限与 SD 探测期限中较早者。挂载时等待仍限制为最多 1 秒，以保持
CMD13 检查和 OTA 暂停/恢复检测频率。缺卡/弹出的实际探测仍按原来的 3 秒期限、错误卡
按原来的 10 秒期限执行；省掉中间无效轮询，空卡槽常态 worker 唤醒约减少三分之二。

storage host suite 的 25 个服务场景覆盖缺卡 3 秒、错误卡 10 秒等待、挂载时的 1 秒上限、
最小 tick 等待及原有插卡/OTA/重试行为；新增真实 `sd_card.c` 有槽/无槽测试，覆盖 RTOS
tick 回绕、到期前不探测、两次失败移除、弹出后的只探测/重插路径以及耗时探测后的剩余
期限。4 项 Release + ASAN/UBSAN 测试通过。它只验证策略和虚拟时钟，不证明真实 SD 总线
响应、CPU 占用或功耗变化；仍需实机热插拔验收。

## 视频条件等待改为事件驱动

`video_player.c` 复用会话已有的两个 wake bit，让没有期限的条件等待使用
`portMAX_DELAY`，不新增任务、锁、定时器或事件对象。先改变条件再发通知，每个 wake bit
只有一个消费者；通知在“检查条件 → 进入等待”的间隙到达也会锁存。worker 的会话引用
保留 event group 到退出，`session_halt()` 先置 `quit`，再唤醒 reader 和 decoder。

| Wait condition | Previous timeout | Event producers |
|---|---|---|
| Reader ring full | 20 ms | ring release, stop |
| Decoder header pending | 100 ms | header publication, reader completion/error, stop |
| Decoder paused | 100 ms | resume, reader/UI notifications, stop |
| Decoder watermark pending | 20 ms | ring commit/full, EOF/error, stop |
| Composite UI slot pending | 20 ms | UI handoff, pause/resume, stop |

满 ring 的 reader 仍先通知 decoder：即使缓存帧数未达水位，满缓冲也足够启动消费，
避免 reader/decoder 互相等待。旧暂停状态不仅有 decoder 的 100 ms 超时，reader 满 ring
时还会每 20 ms 通知 decoder；两处一起改，才能去掉这条周期唤醒链。这里取消的是
超时轮询，不屏蔽有意义的事件；共享 decoder wake bit 的其他通知仍可能使它重查条件。

**有期限的等待不改**：`wait_until()` 保留微秒转毫秒向下取整与 `pdMS_TO_TICKS()`，
暂停/停止事件可提前打断帧等待；`fs_wait_idle()` 的面板超时、停止时的
`EV_DECODER_DONE` 等待和 DMA buffer 生命周期不变。COMPOSITE 的 10 ms LVGL 接帧
定时器以及 `video_elem` 的 100 ms owner 定时器仍保留，不能声称整个视频路径没有周期唤醒。
解码器报错后的 reader 留存/会话清理时机也未改变。

`media_video_player_wait` 把真实生产 `video_player.c` 编入测试，替代 SDK/RTOS/LVGL/
JPEG/LCD 边界，使用真实 OBV1 解析器、临时容器和虚拟 1 kHz tick。28 个子进程隔离场景
覆盖满 ring 背压、先于等待的通知、暂停 10 秒后恢复、文件头独立发布/错误/停止、
水位/EOF/满缓冲、UI 接帧/暂停/停止、帧期限与中断、全屏结束/停止以及 reader 创建失败。
帧标记核对顺序；分配跟踪核对最终释放和借用 strip 不被释放。原有 SPSC 并发帧头测试
继续保留。这些测试不模拟 ESP32 调度、真实 JPEG、DMA 或 SD 吞吐。

```bash
cmake -S test/host/media -B /tmp/obd-perf-video-waits-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/obd-perf-video-waits-host
ctest --test-dir /tmp/obd-perf-video-waits-host --output-on-failure
```

Release + ASAN/UBSAN 的 6 项 media 测试通过。10 个临时负对照先成功编译，再均被测试拒绝：
恢复旧轮询、删除 resume/ring commit/ring release/UI/header/reader completion/stop/full-ring 通知，
以及把帧期限改成无限等待。负对照用 `MH_PLAYER_SOURCE` 指向 `/tmp` 副本，未修改生产文件。

三种目标配置的对象与最终 ELF 已复核；反汇编确认条件等待传无限 tick，帧期限仍计算
有限 tick，面板/停止超时仍保留。`sizeof(vp_session_t)` 前后均为 `264`，私有可写存储
不变，编译器静态栈帧没有增加（reader `192`、decoder `256` 字节）；任务栈仍为
`4096` / `6144` 字节，分配路径不变。这不是运行时栈水位或实际空闲堆测量。

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285720` | `0x285710` | `-16` |
| SD slot, statistics | `0x285f60` | `0x285f60` | `0` |
| No SD slot, normal | `0x2760e0` | `0x2760d0` | `-16` |

三种固件完整构建通过。尺寸差来自同配置的本轮前后镜像，只作体积记录，不代表速度。
构建、SDKCONFIG、基线和负对照均在 `/tmp`，不写仓库构建产物。没有实机 CPU/功耗、
暂停期间任务唤醒、帧率、SD 缺帧恢复或长期稳定性数据；仍需按实机验收矩阵验证。

## 暂停合成视频的 LVGL 接帧定时器

> 历史阶段基线；当前接帧实现已被下一节的 LVGL 任务通知方案替代。

暂停 COMPOSITE 会话时，同时 `lv_timer_pause()` 已有的 10 ms 接帧 timer；恢复时先
`lv_timer_resume()`，再通知 decoder。不删除/重建 timer，不新增分配、任务或同步对象。
所有播放器控制入口都在 LVGL task 或持有 `lvgl_mux` 的上下文调用；worker 不调用 LVGL。

暂停前已经发布、或恰好在暂停期间发布的 frame 保留在会话的 `pending` 槽中，暂停期间
不换前台画面，恢复后按原接帧路径交给 view，再通知 decoder。解码途中被暂停仍保留
ring 记录供恢复后重试；暂停后直接 stop 仍删除 timer 并按既有引用生命周期释放 buffer。
FULLSCREEN 没有这条 timer；其 display refresh 必须保持暂停直到 stop，不能随 resume 恢复。

检查当前 LVGL 8 的 `lv_timer.c`：paused timer 既不执行 callback，也不参与下一次期限
计算。因此这里去掉暂停期间这条名义 100 Hz 的空接帧 callback/deadline；不是实测任务
唤醒次数。timer 节点仍会被 LVGL 遍历，其他 UI/触摸/主题 owner 定时器也仍运行。
尤其 `video_elem` 的 100 ms owner 检查保留，仍能处理暂停期间发生的解码错误；播放中
10 ms 接帧周期、帧 pacing、面板超时及单会话机制不变。

`media_video_player_wait` 从 28 扩展为 36 个场景。新增 loading/已有 pending 暂停、
暂停与发布间隙、真实解码/发布中的控制交错、暂停后 stop、FULLSCREEN 控制及无效控制。
脚本提供 1000 次虚拟 timer-handler 机会，核对暂停期间零 callback/呈现/接帧通知，
恢复后帧标记及资源释放；这不是真实 LVGL/ESP32 吞吐仿真。生产代码没有测试 hook。
Release + ASAN/UBSAN 的 media 6/6 与全套 8 suite 41/41 通过；原 10 个等待负对照与
新增 5 个 timer 负对照全部先编译成功再被拒绝，覆盖遗漏 pause/resume、丢弃 pending、
错误恢复全屏 refresh 及无条件操作空 timer。

三种 ESP32-S3 完整构建通过。最终 ELF 确认 pause/resume 对非空会话接帧 timer 操作，
并保留 decoder 通知；目标对象的 `ui_timer_cb`、`session_halt`、ring release 和面板
等待/绘制指令字节未变。`sizeof(vp_session_t)` 仍为 `264`，私有可写存储与分配路径
不变，所有编译器静态栈帧无增长（reader `192`、decoder `256` 字节）。

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285710` | `0x285730` | `32` |
| SD slot, statistics | `0x285f60` | `0x285f80` | `32` |
| No SD slot, normal | `0x2760d0` | `0x2760f0` | `32` |

同配置尺寸只记 footprint，不代表速度。构建/基线/负对照在 `/tmp`，未修改仓库产物。
CPU、功耗、LVGL 实际唤醒/恢复延迟及长期稳定性仍需实机测量，不能据此宣称整机已达极限。

## 合成视频接帧改为 LVGL 任务通知

接替上一节的阶段方案，COMPOSITE 不再创建每会话的 10 ms 接帧 timer。decoder 发布
`pending` 后只通知开机生命周期的 LVGL task；该 task 持有 `lvgl_mux` 时先执行
`video_player_ui_step()`，再运行 `lv_timer_handler()`。worker 不调用 LVGL，通知中
没有 session/view 指针。接帧仍在 session 锁下交接 front/pending，最后通知 decoder。
没有视频时也会做一次当前会话空检查；不是完全零开销的 LVGL 循环。

LVGL task 的默认 direct-notification slot 0 专用于这个唤醒。task handle 用原子
release/acquire 发布，只绑定一次且永不删除；用 `eSetBits` 合并重复唤醒，等待入口不清位、
出口清位，避免 predicate/wait 间隙丢通知。首次循环无条件检查当前会话，注册前发布的
首帧也能接到。通知可早于原 2..500 ms 等待期限返回；无通知时仍保留普通 timer 的
期限和同样的上下限，没有新 task/semaphore 或运行时分配。

暂停仍保留 pending，UI step 不呈现；恢复通知 LVGL 和 decoder。停止/换会话后，旧
worker 的迟到通知只唤醒 LVGL，后者只查看当前会话，不持有旧 session/view。全屏
refresh、DMA wait/draw、视频帧期限及 `video_elem` 的 100 ms owner 检查不变。
去掉的是这条名义 100 Hz 接帧 timer 的 callback/deadline/节点；其他 LVGL/触摸/UI
定时器仍运行，帧事件可能增加一次提前循环，不等于实测整条任务的唤醒数量下降。

真实 player + 真实 `lvgl_runtime.c` 在主机执行 47 个隔离场景，普通和统计两种配置各跑
一遍；新增注册前发布、等待间隙/阻塞发布、重复通知合并、暂停迟到帧/恢复、停止迟到
通知、替换会话、全屏防护及普通 timer 期限。保留原 36 场景的 reader/decoder/控制
测试、分配跟踪、真实 OBV1/parser 和 SPSC 并发测试。统计配置还检查每轮的 mutex
wait/handler 样本；RTOS/LVGL/JPEG/LCD 边界是 stub，不是 ESP32 调度或吞吐模拟。
27 个负对照均先编译成功，再被测试拒绝。8 套 host suite 42/42（Release + ASAN/UBSAN）
及可选真实 LVGL theme 7/7 通过；生产代码没有测试 hook。

三种 ESP32-S3 完整构建通过，最终 ELF 检查了 task/锁/接帧/handler/通知集成和没有
每会话 timer 分配。ring release 及面板 wait/draw 的目标指令字节不变。
`sizeof(vp_session_t)` 仍为 `264`（移除字段被对齐吸收）；去掉一条 LVGL timer 分配。
原未使用的 task-handle 输出槽被新的原子 handle 替代，相关私有可写存储总量不变。
reader/decoder 的编译器静态栈帧仍为 `192`/`256` 字节；普通 LVGL task 为 `32` → `32`，
统计配置为 `32` → `48`，新增 ui-step/wake 各 `32`。任务栈预算不变，这不是实际峰值栈量。
`lvgl_handler` 现在明确覆盖 UI 接帧 step + timer handler，不能把指标边界变化当加速。

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285730` | `0x2857b0` | `128` |
| SD slot, statistics | `0x285f80` | `0x286020` | `160` |
| No SD slot, normal | `0x2760f0` | `0x276180` | `144` |

同配置尺寸只记 footprint，不代表速度。构建/基线/负对照在 `/tmp`，未修改仓库产物。
实际 CPU/功耗、LVGL 唤醒数、接帧/显示延迟、视频 dropped/underruns 及长时间稳定性
仍需同配置实机 A/B 测量；不能宣称整机达到性能上限。

## 实机性能统计

在 menuconfig → OBD DSP Configuration 开启 `CONFIG_OBD_PERF_STATS`，默认关闭。
复制当前正确硬件版本的 `sdkconfig` 到临时路径后构建，避免更改本地正常配置：

```bash
cp sdkconfig /tmp/obd-perf.sdkconfig
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig menuconfig
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig reconfigure build
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig -p PORT flash monitor
```

该选项创建一个低优先级、4096 字节栈的日志任务，每约 5 秒输出一轮统计。
每个样本只有计时和固定空间聚合，日志格式化、堆查询和串口输出都在统计锁外。
关闭时计时代码和包装函数不参与编译；普通固件 ELF 已核对没有统计/包装函数符号。
开启时会有计时、临界区、内存和日志开销，最终速度仍需在关闭统计后用外部测量复核。

| Metric | Scope | Units |
|---|---|---|
| `lvgl_wait` | LVGL mutex wait | calls |
| `lvgl_handler` | UI handoff step + `lv_timer_handler()` | calls |
| `lcd_submit` | LVGL `esp_lcd_panel_draw_bitmap()` submission | pixels |
| `ui_tick` | whole `my_timerMain()` call | calls |
| `theme_tick` | whole `theme_tick()` call | calls |
| `elm_run` | RX drain + `vp_engine_run()` | calls |
| `sd_read` | `sd_card_read()`, including bounce-buffer wait/copy | bytes |
| `sd_write` | `sd_card_write()`, including bounce-buffer wait/copy | bytes |
| `video_fullscreen` | `fs_present()`, including transfer waits | frame attempts |
| `video_composite` | `comp_decode()` | frame attempts |
| `rc_snapshot` | due-channel `signal_store_get_many()` | input slots |

输出 `n`、`total_us`、`min_us`、整数 `avg_us`、`p95_le_us`、`max_us` 和 `units`。
`p95_le_us` 是 2 的幂分桶上界，不是精确 P95；超过 1,048,576 us 的样本进入
`UINT32_MAX` 上界桶。没有样本的指标不输出，不能把缺失指标解释为零耗时。

以下为格式示例，并非实机结果：

```text
I (...) perf: window_us=5000000 metric=rc_snapshot n=50 total_us=300 min_us=4 avg_us=6 p95_le_us=8 max_us=8 units=400
```

另输出 internal/PSRAM 当前空闲量、启动以来最小空闲量、最大连续空闲块和日志任务栈余量。
`ui_tick`、`theme_tick` 嵌套在 `lvgl_handler` 中，指标总耗时不能相加当作 CPU 利用率。
`lcd_submit` 不覆盖异步 DMA 完成；`elm_run` 不是 ECU 往返时间；视频计数含解码/展示尝试，
不是实际屏幕 FPS；SD 指标只覆盖经过 `sd_card_read/write()` 的路径。

## 验证与尚待完成的测量

主机验证覆盖 8 套 suite；`theme` 新增可选真实 LVGL 页面对照；`media` 新增纯 C CRC 对照、可选 SDK 兼容测试、SPSC 环形缓冲帧头测试与普通/统计两配置各 47 场景真实视频/LVGL 任务控制与事件等待测试；`rules` 新增规则输出缓存对照测试；`espnow` 新增 RaceChrono 筛选/期限状态机和真实 worker/GATT/hub 确定性测试，以及 17 场景真实 ESP-NOW worker/control/OTA 回放，
`signal_store` 新增统计分桶边界、百分位、64 位总量、计数饱和、输出单位换算与按需 OBD 快照对照测试。
`storage` 新增真实存储服务的有槽/无槽双配置 25 场景对照，并直接运行真实 SD 状态机的有槽/无槽期限测试。
车型测试继续覆盖旧请求序列和默认 OBD2 继承；`engine` 新增 100,352 次真实规划状态/slot 对照与局部去重基准，`engine_queue_copy` 新增 246,272 次完整状态/step/日志对照与可选命令复制基准；`engine_state_format` 新增 178,928 次状态命令对照与可选整份源码 A/B 基准。`engine_rx_append` 新增 219,423 次完整接收状态/真实监听回调对照与整份源码 A/B 基准。`vehicle_rpn_integer` 新增 1,001,690 个转换位模式与 610,140 条完整公式对照，并提供可选 Binary32 穷举与整份源码 A/B 基准。`vehicle_rpn_raw_float` 新增 4,274,988 次独立提取/旧转换规则的完整公式对照，以及可选全范围 raw32 检查和整份源码 A/B 计时。`engine_write_rounding` 新增 1,720,944 个恒等舍入输入与 33,280 次原双精度完整写回状态/回调对照，提供可选 Binary32 穷举与舍入局部基准。此前 8 套 host suite 的 49 项测试通过；本次 SD 等待改动的 4 项 storage/SD 测试单独复核通过。
使用 ESP-IDF 5.5.3 完成有槽普通/统计和无槽普通三种 ESP32-S3 固件的完整构建，构建目录和
SDKCONFIG 均在 `/tmp`，没有写入仓库的 `build/` 或发布产物。三份 ELF 都含按需快照入口；
普通 ELF 不含 `ui_timer_main_run`、`perf_stats_record` 或 `perf_stats_start`，统计 ELF
包含这些符号。这些检查证明兼容性和统计代码可构建，未证明实机性能。

实机验收至少覆盖静态设置页、转速动画、丰富主题规则、全屏/合成视频、标准/特殊车型轮询、
RaceChrono 全量/单通道订阅、三连表和 SD 导入/OTA。对每个场景保存相同硬件、素材、
车辆/适配器和设置下的基线与修改后日志，比较耗时分布、有效数据更新间隔、屏幕 FPS、
视频 dropped/underruns、无线交付率以及长期堆/栈余量。

当前没有获得连接的 ESP32 仪表和适配器，实机统计、帧率/无线/SD 吞吐和长时间稳定性
仍是未完成项。后续优化方向由这些测量决定，不能把绿色主机测试当作全项目目标已经完成。

## 主题指针缩放移除颜色通道 floorf

`theme_page.c` 在指针素材获取/缓存缺失时调用 `tpx_resample()`，按面积覆盖缩小、
双线性放大，再将预乘权重累积结果写为 RGBA。缩放像素最多转换四个颜色通道；
同尺寸 RGBA 仍直接复制，同尺寸 RGB565 仍直接展开，不经过该量化函数。

`px_u8()` 已在转换前排除 NaN、非正值和 `>= 255` 的输入。其余 `v + 0.5f` 为
非负且小于 `255.5f` 的 float，直接转 `uint8_t` 与原 `floorf` 后转换完全等价。
保留原来的 float 加法舍入：例如 `0x3effffff` 加 `0.5f` 后舍入到 `1.0f`；
不能改成 double 加法，否则图片边界会改变。取样权重、累积/除法顺序、透明像素、
尺寸限制与分配/释放策略不变，不增加 RAM 字段。

`theme_resample` 编入真实私有转换，并冻结原始 `floorf` 版本的完整 resample 循环；
两者共享未改变的轴权重和像素读取 helper，因此此对照验证转换变化，不是独立验证
整个取样算法。该轮默认检查 295,186 个 Binary32 输入和 10,448 组图片，输出共
10,978,368 字节，同时比较私有真实路径、旧循环和公开生产 `tpx_resample()`。
覆盖放大/缩小/混合/同尺寸、1 像素轴、最大尺寸轴、RGBA/RGB565、全透明/全不透明/
低 alpha/随机像素。9 个分配失败点、同尺寸分配失败与非法参数也一致且无泄漏。
Release `--exhaustive` 检查全部 4,294,967,296 种 Binary32 位模式，含常规输入总计
4,295,262,482 次；图片对照仍是有限场景。4 个负对照先编译成功再被测试拒绝：
改为 double 加法、遗漏半值、只改真实图片路径的红通道和 alpha。ASAN/UBSAN theme
7/7 与 Release theme 7/7 通过，测试 probe 不进入固件。

完整图片基准包含权重构建、分配、取样、量化与释放；不含 PSRAM、Flash/cache、
LVGL 页面获取、BLE、屏幕刷新或系统调度。Apple M1 / arm64 / Apple clang 21，
Release `-O3`、无 sanitizer/LTO，先逐字节和 checksum 对照，每路径预热 4 次，
交替 7 组取中位数，每组至少处理 4,000,000 个输出像素。计时期间没有其他本地
构建/测试；后台系统负载未控制。两轮 16 个场景的结果见
[首轮](performance/pixel-rounding-host-2026-10-14.csv)与
[复测](performance/pixel-rounding-host-repeat-2026-10-14.csv)。主机 14 个场景接近持平，两个 RGBA 场景连续两轮退化，
复测如下（`ratio = actual_ns / reference_ns`）：

| format | sw | sh | dw | dh | reference_ns | actual_ns | ratio |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rgba8888 | 512 | 128 | 128 | 512 | 923209.7 | 957241.9 | 1.036863 |
| rgba8888 | 127 | 129 | 63 | 65 | 64854.7 | 72754.4 | 1.121806 |

这两项主机耗时约增加 4% / 12%；不能用主机计时声称性能收益，目标耗时仍待实测。

ESP32-S3 完整构建的 `tpx_resample` 移除四处 `floorf` 调用，使用 `utrunc.s`。
text `1353 → 1201`、literal `68 → 52`，合计减少 168 字节；静态 frame 保持
144 字节，这不是运行时任务栈峰值。旧/新独立编译的全部 text/literal section 与
对应完整固件对象逐字节一致，ELF 无 probe 符号。普通/统计/无 SD 槽三种构建通过：

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x285650 | 0x2855b0 | -160 |
| stats | 0x285eb0 | 0x285e10 | -160 |
| no-slot | 0x276010 | 0x275f60 | -176 |

[目标证据](performance/pixel-rounding-xtensa-2026-10-14.txt)保存反汇编、源码/镜像哈希、
穷举及负对照结果；[源码补丁](performance/pixel-rounding-source-2026-10-14.diff)
可还原旧路径。采用依据为保持图片字节兼容并减少目标库调用/代码量；首次素材加载
耗时、CPU、PSRAM 带宽与页面帧率仍待实机验证。

```bash
cmake -S test/host/theme -B /tmp/obd-pixel-rounding-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_RESAMPLE_BENCHMARK=ON
cmake --build /tmp/obd-pixel-rounding-bench
ctest --test-dir /tmp/obd-pixel-rounding-bench --output-on-failure
/tmp/obd-pixel-rounding-bench/test_resample --exhaustive
/tmp/obd-pixel-rounding-bench/bench_resample --rounding
```

## 按缩放方式收紧临时轴权重容量

`px_axis_build()` 原来为索引和权重统一预留 `sn + 2*dn + 2` 个 tap。
现在在原有四次分配前选择更小的安全容量：

| mode | cap |
| --- | --- |
| sn == dn | dn |
| sn < dn | 2*dn |
| sn > dn && sn % dn == 0 | sn |
| other shrink | sn+dn-1 |

等尺寸每个输出仅一个 tap；双线性仍保留两个 tap（包括重复索引与零权重）。
面积覆盖的小区间由源像素边界与输出边界共同划分，最多 `sn+dn-1` 个。
整数倍缩小的 double 边界可精确表示，每个源像素只对应一个 tap。非整数缩小
使用上界，不用理想有理数的 gcd 计算精确数量，避免浮点边界带来的额外微小覆盖。
不改变权重生成、取样累积、颜色量化、同尺寸整图快路径、分配次数或失败释放流程；
只减少两个轴的索引/权重分配请求，没有新增逐像素计算或结构体字段。

`theme_resample` 现在冻结旧轴构建器，并检查完整 start/count/已用 idx/float 权重位模式。
默认 100,303 组轴对照覆盖所有 1..2047 尺寸与边界尺寸双向组合/等尺寸；可选 Release
`--axes-exhaustive` 对全部 4,190,209 组合法尺寸组合通过，每组新分配请求都小于旧值。
这证明主机轴表及容量边界，不是设备端 RAM/耗时测量。默认还检查 295,186 个颜色输入、
10,464 组完整图片（13,182,976 个输出字节），同时比较旧完整循环、私有真实路径与
公开生产路径。最大等尺寸轴的混合缩放、新旧峰值请求、九个分配失败点、非法参数
与最终零泄漏也通过；共享 fetch/free helper 未作独立算法验证。
三个负对照先编译成功再被拒绝：恢复旧过量分配、双线性容量不足（ASAN 检出越界）、
整数缩小容量不足。ASAN/UBSAN 与 Release theme 各 7/7 通过。

完整图片峰值由实际分配/释放回调追踪：包含 RGBA 输出和轴数组，不含源图、堆元数据、
对齐、LVGL 缓存或后续 render 的内存。以下请求字节对 RGBA/RGB565 输入相同；
不是设备实测堆峰值：

| sw | sh | dw | dh | previous_peak_bytes | actual_peak_bytes | saved_bytes |
| --- | --- | --- | --- | --- | --- | --- |
| 360 | 360 | 180 | 180 | 140424 | 136080 | 4344 |
| 90 | 150 | 180 | 300 | 226104 | 224640 | 1464 |
| 180 | 300 | 90 | 150 | 61224 | 58320 | 2904 |
| 2047 | 1 | 2047 | 2 | 65570 | 40976 | 24594 |
| 360 | 90 | 360 | 180 | 271644 | 266760 | 4884 |


计时沿用完整图片方法：Apple M1 / arm64 / Apple clang 21，Release `-O3`、无 sanitizer/LTO，
逐字节/checksum 对照、每路径预热 4 次、交替 7 组，每组至少 4,000,000 个输出像素，
取中位数。基准旧路径保留上一版整数颜色量化和旧容量，隔离上轮 `floorf` 改动；
`--rounding` 保留旧容量对旧/新颜色量化的历史对照。计时期间无其他本地构建/测试，
后台系统负载未控制；[首轮](performance/axis-capacity-host-2026-10-14.csv)和
[复测](performance/axis-capacity-host-repeat-2026-10-14.csv)记录全部 20 个场景。
主机结果不能证明 ESP32 提速。复测全部 20 项在 ±0.4% 内，
`ratio = actual_ns / reference_ns` 的范围为 `0.996039..1.001950`；首轮一项约快 8.5%，
未在复测重现。扩充场景前的两轮 16 项探索结果也保留在
[探索首轮](performance/axis-capacity-host-exploratory-2026-10-14.csv)和
[探索复测](performance/axis-capacity-host-exploratory-repeat-2026-10-14.csv)，其中有
RGBA 场景约慢 9–10%，同样未在最终复测重现。采用依据是减少内存请求，
不能把这些桌面计时波动当作稳定提速或证明设备加载延迟不回退。

目标 `px_axis_build` text `839 → 895`、literal 保持 `164`，代码增加 56 字节，
静态 frame 保持 112 字节；容量取模使用硬件 `remu`，只在轴构建前判断一次。
`tpx_resample` 的 text/literal 大小保持 1201/52 字节，静态 frame 保持 144 字节；
编译器汇编去除调试信息并统一局部标签后，指令/伪指令序列相同。汇编器对齐与
分支偏移导致 text 字节存在差异，不能声称目标函数逐字节相同；源码逐像素路径不变。
旧/新独立编译的全部 text/literal 与各自完整固件对象一致，测试 probe 不进入 ELF。
普通/统计/无 SD 槽三种构建通过，app 都增加 64 字节：

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855b0 | 0x2855f0 | 64 |
| stats | 0x285e10 | 0x285e50 | 64 |
| no-slot | 0x275f60 | 0x275fa0 | 64 |

[目标证据](performance/axis-capacity-xtensa-2026-10-14.txt)保存源码/镜像/测试/基准哈希、反汇编、
穷举与负对照结果；[源码补丁](performance/axis-capacity-source-2026-10-14.diff)可还原旧容量。
采用依据是确定减少分配请求并保持所有合法轴表/图片兼容；实际 PSRAM 空闲堆、
碎片、页面素材加载延迟、CPU 与帧率仍待设备验证，不能声称全项目性能已经达到极限。

```bash
cmake -S test/host/theme -B /tmp/obd-axis-capacity-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_RESAMPLE_BENCHMARK=ON
cmake --build /tmp/obd-axis-capacity-bench
ctest --test-dir /tmp/obd-axis-capacity-bench --output-on-failure
/tmp/obd-axis-capacity-bench/test_resample --axes-exhaustive
/tmp/obd-axis-capacity-bench/bench_resample
```

## 等尺寸指针直接裁剪与转换

此节记录迁移到 SD 之前的 Flash mmap 实现与当时的测量。当前 SD 主题路径强制持有
RAM 像素，未染色 RGB565 也计预算，并增加临时 SD 源段；下表零拷贝节省与旧获取基准
不适用于当前存储路径。等尺寸省去重采样的像素/几何行为仍保留，现行行为见 [THEMES.md](THEMES.md)。

`te_acquire()` 原来即使素材尺寸与元素尺寸一致，也先申请 `4*w*h` 字节，
复制 RGBA 或展开 RGB565，随后扫描 alpha、保留旋转中心、转换成最终显示格式。
现在仅在源/目标尺寸不同的时候调用 `tpx_resample()`；等尺寸直接对已验证的素材
执行原来的裁剪和旋转中心保留。RGBA 输出的格式、像素、透明度、位置与缓存键保持一致。

等尺寸 RGB565 没有透明像素，保留全图；无 tint 时沿用已有的整行零拷贝路径，
最终像素直接引用映射素材，`owned=NULL`、像素预算为 0，仍需分配和引用缓存条目。
有 tint 时强制独立的 RGB565 缓冲区，保留颜色混合规则，不写映射 Flash。
`te_est_cost()` 相应估算为无 tint 的 0 或有 tint 的 `2*w*h`；其他指针仍保守估算
`3*w*h`。页面像素预算/预检可能因减少成本而允许以前被跳过的素材。
缓存命中仍计入实际像素成本；条目引用计数、LRU、清除后的延迟释放保持原有规则。
公开 `theme_release_resources()` 在页面存在时延迟释放，最后页面删除后才清缓存和
解除映射，沿用图片元素已经使用的素材寿命约束。

确定移除的像素申请如下；不是实机空闲堆或分配器开销测量：

| w | h | temporary_request_removed | persistent_rgb565_untinted_removed | pixel_allocations_removed_rgba_or_tinted | pixel_allocations_removed_rgb565_untinted |
| --- | --- | --- | --- | --- | --- |
| 90 | 150 | 54000 | 27000 | 1 | 2 |
| 180 | 300 | 216000 | 108000 | 1 | 2 |
| 360 | 360 | 518400 | 259200 | 1 | 2 |

新 `theme_acquire` 编译真实页面/像素转换/缓存/LVGL，对照冻结的完整旧获取函数。
两者共用几何、像素和缓存 helper，测试不独立重写这些算法；旧函数不调用估算器，
新估算器单独检查实际像素成本上界。1,006 组像素/几何场景覆盖两种格式、着色、
透明/低 alpha/随机 alpha/透明边缘、单像素与最大合法单轴、真正缩放及普通图片、
负/超界/小数旋转中心和尺寸取整。另覆盖全部 65,536 种 RGB565 颜色、映射偏移、
源数据不变、缓存命中无分配、预算不足命中后的引用回滚、带引用清除、最后释放、
零预算/模拟低空闲堆、移除的中间缓冲区失败、最终缓冲区/条目持续失败、
缩放路径每个分配点的瞬时失败、非法尺寸/格式/字节数和完整释放。
ASAN/UBSAN 与 Release 各 9/9 通过；三个编译负对照（跳过真正缩放、给映射素材着色、
丢掉旋转中心边界）分别被 1,022 / 151 / 330 条断言检出。

[首轮](performance/pointer-identity-host-2026-10-14.csv)和
[复测](performance/pointer-identity-host-repeat-2026-10-14.csv)各保存 20 个完整获取场景，
每个场景交替 7 组，每组至少 20 ms，保留原始样本和中位数。计时包含分配、转换、
缓存插入、释放与清除；输入准备、输出对照和峰值追踪在计时之外。两轮比例如下：

| path | new_over_old_min | new_over_old_max |
| --- | --- | --- |
| identity_rgb565_untinted | 0.000186 | 0.001706 |
| identity_rgb565_tinted | 0.139949 | 0.152151 |
| identity_rgba_untinted | 0.942976 | 0.969428 |
| identity_rgba_tinted | 0.948424 | 0.973395 |
| scaled_all | 0.992949 | 1.004261 |

零拷贝桌面结果省掉所有像素循环，不包括实际显示时读取 Flash 的开销，也不是
整页构建/FPS 基准。主机源在 RAM，内存能力/空闲堆模拟，桌面分配器携带测试头；
旧/新用同一分配器。后台负载未控制，不能把桌面比例当作 ESP32 提速。
追踪仅计算申请字节（包含主机 112 字节缓存条目，排除分配器头）。360×360 示例
RGBA 峰值 `691920 → 173632`，RGB565 无 tint `777600 → 112`，有 tint
`777600 → 259312`；临时像素在条目申请前释放，所以峰值减少量并不等于移除申请的总和。
实际 ESP32 条目大小/堆开销及映射读耗时不同，设备峰值、碎片与加载时间尚未测量。

ESP32-S3 中获取函数内联到 `theme_create_page`，其 text `6594 → 6638`、静态 frame
保持 624 字节；估算器 text `76 → 108`、frame 保持 80 字节。整个页面对象 text
`14646 → 14723`，literal 保持 2020，没有新增 `.bss` / `.data`。
旧/新独立对象的全部 text/literal/bss/data 与对应完整固件一致，测试 probe 不进入 ELF。
三种固件通过：

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x285640 | 80 |
| stats | 0x285e50 | 0x285e80 | 48 |
| no-slot | 0x275fa0 | 0x276000 | 96 |

[目标与测试证据](performance/pointer-identity-xtensa-2026-10-14.txt)保存反汇编、哈希与结果；
[源码补丁](performance/pointer-identity-source-2026-10-14.diff)可恢复旧获取路径。
采用依据是移除重复整图转换和确定减少像素申请；设备时序、实机显示和长期稳定性
仍待验证，全项目性能目标仍未完成。

```bash
cmake -S test/host/theme -B /tmp/obd-pointer-acquire -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-pointer-acquire
ctest --test-dir /tmp/obd-pointer-acquire --output-on-failure
/tmp/obd-pointer-acquire/test_acquire --benchmark
```

## ESP-NOW v6 CRC 使用 ROM 实现

`espnow_v6.c` 的 CRC-16/CCITT-FALSE 原来每字节循环 8 次，编码/解码都在重复执行。
ESP32 路径改用 SDK `esp_rom_crc16_be`；SDK 的状态取反约定要求初始状态为 0、
返回值取反，空输入仍返回 `0xFFFF`。宽 `size_t` 按 `UINT32_MAX` 分块保留连续状态；
设备 `size_t` 为 32 位，编译器消掉分块循环。没有新增分配或 RAM 查表。
普通主机保留原位循环；v5、v6 DATA 的字段与 CRC 字节序、HELLO、接收判定保持一致。
此修改不改变无线发送频率，也未测量射频吞吐。

新增 `espnow_crc_portable` / `espnow_crc_rom` 对照冻结的原 CRC，覆盖全部 65,536 种
两字节输入、0..512 长度/8 个偏移/4 种模式、随机长输入、空指针零长度与标准向量。
SDK 分支另覆盖 16,777,216 种状态/字节转换和 257 字节数据的全部分块点；公共包装器的
超 4 GiB 单次输入没有实际分配测试。真实编码/解码在 0..64 条记录、8 个偏移上对照
共享 helper 的便携 codec，检查完整输出、独立旧 CRC、解析字段、截断、逐字节翻转和
容量不足；原完整 v5/v6 协议套件也以 ROM 分支运行。共享 helper 对照不独立验证未改动的
协议布局。便携测试 643,162 条断言、ROM 测试 17,420,636 条断言均通过。
ASAN/UBSAN 与 Release 各 7/7；关闭 SDK 兼容项时纯便携 suite 5/5。
三个编译负对照（错误种子、遗漏最终取反、使用反射算法）分别检出
84,410 / 84,410 / 84,314 条失败。

[首轮](performance/espnow-crc-host-2026-10-14.csv)与
[复测](performance/espnow-crc-host-repeat-2026-10-14.csv)各保存 18 个场景，
每场景交替 7 组，每组至少 20 ms；保存全部样本和中位数。CRC 测 0/38/44/104/236/248
字节，完整编码/解码测 0/1/11/33/43/64 条记录并包含全部分片。输入准备和正确性检查
在计时外；消费 CRC/帧结果防止编译器消除工作。两轮比例范围：

| path | new_over_old_min | new_over_old_max |
| --- | --- | --- |
| crc_empty | 0.985754 | 0.988935 |
| crc_nonempty | 0.174370 | 0.291654 |
| encode | 0.201994 | 0.319280 |
| decode | 0.196878 | 0.300662 |

主机执行的是 SDK Linux 表实现，输入在 RAM，不是芯片 ROM、无线任务或端到端延迟。
未控制后台负载；不能把桌面比例当作 ESP32 提速。采用依据是移除固件中的逐位循环、
复用已有 ROM 和减少目标代码占用；实际 CPU 时间、功耗与多表吞吐仍待设备测量。

实际 ESP32-S3 编译命令的独立对象与对应固件对象全部 text/literal/bss/data 相同。
代码和静态入口 frame 如下；入口 frame 不包含 ROM 内部调用栈：

| function | before_text | after_text | before_frame | after_frame |
| --- | --- | --- | --- | --- |
| espnow_crc16 | 66 | 32 | 32 | 32 |
| espnow_v6_encode | 476 | 452 | 64 | 64 |
| espnow_v6_decode | 446 | 385 | 48 | 32 |

整个 codec 对象 text `4234 → 4116`、literal `156 → 148`，`.bss` / `.data` 均为 0。
`espnow_crc16` 内联到编码/解码后独立函数被链接回收；三种 ELF 中两处间接调用加载
`0x40001cbc`，与绝对 ROM 符号 `esp_rom_crc16_be` 一致。固件没有主机参考函数或 SDK
Linux CRC 表符号。三种固件通过：

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x285640 | 0x2855f0 | -80 |
| stats | 0x285e80 | 0x285e20 | -96 |
| no-slot | 0x276000 | 0x275fa0 | -96 |

[目标与测试证据](performance/espnow-crc-xtensa-2026-10-14.txt)保存反汇编、哈希与结果；
[源码补丁](performance/espnow-crc-source-2026-10-14.diff)可恢复原 CRC 路径。
`EH_ESP_ROM_DIR` 可指定 SDK 源码；`EH_ROM_CRC=OFF` 保留无需 SDK 的基础测试。
基准需要官方 SDK Linux 后端，不进入默认 CI 或固件：

```bash
cmake -S test/host/espnow -B /tmp/obd-espnow-crc -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_CRC_BENCHMARK=ON
cmake --build /tmp/obd-espnow-crc
ctest --test-dir /tmp/obd-espnow-crc --output-on-failure
/tmp/obd-espnow-crc/bench_espnow_crc
```

## 信号 ID 查找由生成器展开

`signal_store_slot_by_id()` 原来逐项扫描 17 个静态信号；车型 ID 与未知 ID 也要
扫完整表。ESP-NOW 接收的有效位清除和记录写入都会调用它。现在由
`tools/gen_signals.py` 从注册表生成 `sig_registry_slot_by_id()`；只有 ID 和槽位
同时相邻的条目才合并为范围运算，其他条目生成单值判断。当前表生成两个范围，
不增加运行时映射表；以后注册表出现空洞或重排仍由生成器保持准确。
车型 ID 的范围、注册数量判断与临界区保留，未改变静态/动态槽位编号或协议 ID。

`signal_lookup` 编入真实 `signal_store.c` 和冻结的原完整查找函数，两者共用同一
注册状态；另以线性表和已注册数量作为 oracle。对全部 65,536 个 ID，分别在
初始化前、初始化后、每种 1..32 个车型信号数量、清空后和重新注册后检查，
共 7,077,955 条断言通过。`signal_lookup_generator` 对当前、反序、轮转、打乱、
稀疏、稀疏打乱、子集与反向 ID 共 8 份注册表生成 C，再编译运行独立线性表 oracle，
覆盖 524,288 个 ID。ASAN/UBSAN 和 Release 的 `signal_store` suite 各 6/6，
相关 ESP-NOW suite 7/7，车型 suite 14/14；37 份车型配置契约校验通过。
四个临时负对照（遗漏范围端点、错误槽位偏移、接受空洞 ID、忽略车型注册数量）
分别检出 72 / 864 / 72 / 591 条失败。测试与冻结参考不进入固件。

[首轮](performance/signal-id-host-2026-10-14.csv)、
[复测](performance/signal-id-host-repeat-2026-10-14.csv)和
[第三轮](performance/signal-id-host-third-2026-10-14.csv)各保存 24 个场景，
每个场景交替测 7 组，每组至少 20 ms，保存全部样本和中位数。12 个局部场景覆盖
标准/系统/车型/未知 ID、初始化前与未注册情况及混合序列；12 个接收场景编入真实
`espnow_rx_apply()`，覆盖空帧、有效位、车型失效、0/1/32 个车型槽位、未知记录、
v5、profile 不匹配和包含全部分片的 64 记录快照。接收回调只更新测试数组和计数，
两条路径的写入数量、最终值、有效位与回调计数在计时外逐片对照；生成和解码也在
计时外。局部 ns 按单次查找计，接收 ns 按完整快照的全部分片计。

| scope | path | new_over_old_min | new_over_old_max |
| --- | --- | --- | --- |
| all_3_runs | lookup_rpm | 1.139780 | 1.163550 |
| all_3_runs | lookup_mixed | 0.251269 | 0.251687 |
| all_3_runs | lookup_registered_dynamic | 0.289161 | 0.291045 |
| repeat_and_third | apply_empty_all_valid | 0.947191 | 0.955869 |
| repeat_and_third | apply_other_cases | 0.340574 | 0.475689 |
| all_3_runs | apply_full64 | 0.442505 | 0.445516 |

RPM 原来在表的首项命中，桌面单项查找从约 1.24..1.27 ns 到 1.41..1.45 ns，
退化约 14%..16%；它没有代表全部路径。首轮 `standard_invalidate32` 波动很大，
旧样本约 691..26,867 ns，新样本约 209..69,939 ns，中位数比值为 2.759025，
该轮包含明显退化；原样保留。后两轮该场景比值 0.368565 / 0.368298，其余常规
接收场景也改善。没有控制系统后台负载或锁竞争，不能由桌面比例断言设备时间收益。
主机 FreeRTOS 锁为空 stub，测试数组写入不包含真实信号表锁、时间戳、失效检查，
也不包含解码、CRC、无线传输或任务调度。实际 ESP32 收包耗时与功耗仍待设备测量。

真实 ESP32-S3 编译结果去掉静态扫描的 `loop` / `l16ui` 和注册表地址引用，
保留动态 ID 的两个临界区调用。目标查找大小如下，入口 frame 不含锁内部调用栈：

| function | text_before | text_after | literal_before | literal_after | frame_before | frame_after |
| --- | --- | --- | --- | --- | --- | --- |
| signal_store_slot_by_id | 92 | 92 | 20 | 16 | 32 | 32 |

独立对象与普通固件对象的全部 text/literal/bss/data/rodata 逐节相同；前后只有
查找函数的 text/literal 改变，其他函数和存储逐节相同。对象总 text 为 4,429 字节，
literal 从 544 到 540 字节，bss/data/rodata 保持 4/8/131 字节。生成的
`signal_registry.c` 与改动前逐字节相同。普通、统计、无 SD 槽三种固件构建通过，
ELF 中查找函数一致且仍调用锁函数，没有参考或基准符号；镜像因对齐未缩小：

| build | before_hex | after_hex | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x2855f0 | 0 |
| stats | 0x285e20 | 0x285e20 | 0 |
| no-slot | 0x275fa0 | 0x275fa0 | 0 |

采用依据是去掉接收路径每次静态表扫描，维持目标代码大小与动态锁语义。
[目标证据](performance/signal-id-xtensa-2026-10-14.txt)保存编译命令、逐节对照、
反汇编、测试/负对照结果和哈希；[源码差异](performance/signal-id-source-2026-10-14.diff)
只包含本项生产改动，逆应用可恢复原生成器、生成头文件与查找函数。

```sh
cmake -S test/host/signal_store -B /tmp/obd-signal-lookup -G Ninja -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF -DSS_LOOKUP_BENCHMARK=ON
cmake --build /tmp/obd-signal-lookup
ctest --test-dir /tmp/obd-signal-lookup --output-on-failure
/tmp/obd-signal-lookup/bench_signal_lookup
python3 tools/gen_signals.py --check
```

## ESP-NOW 主表快照省去重复 ID 扫描

主表 `master_collect()` 每次从空快照依次读取唯一的 12 个 `obd.*` 槽位和最多
32 个 `veh.*` 槽位；两组数值 ID 不重叠。挡位存进单独的 gear 字节，所以最多
43 条普通记录。原 `espnow_snapshot_put()` 每插入一条记录都查找已有 ID，满载时
累计扫描 `43*42/2 = 903` 条记录。现在仅这两个初始收集循环调用
`espnow_snapshot_append_unique()`，按原顺序直接追加，保留有效位、无效 ID、容量
和挡位范围检查。调用方必须保证普通 ID 尚未存在；需要覆盖已有值时仍用 `put()`。
联动测试最后写 RPM 继续用通用替换接口，不会产生重复 RPM。

挡位在追加函数内部处理，不产生嵌套调用；满快照仍能更新合法挡位。没有新建
堆内存、索引表或任务，信号表批读和锁、轮询/广播频率、v5/v6 字节保持原行为。
这项改动的收益是减少构造快照的工作，不改变无线容量。

`snapshot_append` 对照冻结的旧插入函数，覆盖全部 65,536 个 ID、空/满快照、
混合有效位、0..64 条打乱的唯一序列、非法 ID/挡位、容量后的替换与 RPM 覆盖，
共 1,187,778 条断言。`master_collect` 编入真实私有收集函数，使用确定的信号表/UI
stub，与冻结旧收集函数对照 35,012 组输入、210,616 条断言：全部 4,096 组 OBD
有效位、0/1/16/32 个车型槽位、挡位边界、联动开关、序号回绕、UI 步数转换，以及
0..33 个报告槽位、车型空洞与两次批读之间移除槽位。完整快照、v5 字节、v6 的
每个分片及解码均检查。ASAN/UBSAN 和 Release 的无线 suite 各 9/9 通过；
关闭 SDK 兼容项的便携 suite 7/7 通过。
临时负对照中，错误车型有效位、错误挡位上界、错误 ID 和用追加替代 RPM 覆盖
分别检出 258,392 / 4 / 464,057 / 31,650 条失败；错误容量上界由 sanitizer 检出。
冻结参考、测试与基准均不进入固件。

[首轮](performance/snapshot-append-host-2026-10-14.csv)和
[复测](performance/snapshot-append-host-repeat-2026-10-14.csv)各保存 33 个场景，
每场景交替测 7 组，每组至少 20 ms，保留全部 14 个样本和比值范围。12 个局部
场景覆盖 0/1/11/32/43/64 条记录及 RPM 覆盖；21 个真实收集/收集加编码场景覆盖
空/标准/1 车型/满载/稀疏/中途移除/联动覆盖。计时外先比较完整快照和所有编码字节；
v6 计入全部分片。ns 按一个完整快照计，桌面比值如下：

| case | first_new_over_old | repeat_new_over_old |
| --- | --- | --- |
| insert_0 | 0.996716 | 0.989475 |
| insert_1 | 0.943990 | 0.940549 |
| insert_43 | 0.272469 | 0.273173 |
| insert_64 | 0.189608 | 0.187584 |
| collect_empty | 0.991215 | 1.003685 |
| collect_standard | 0.591070 | 0.592015 |
| collect_full | 0.318660 | 0.345474 |
| collect_override | 0.319288 | 0.320284 |
| v5_full | 0.357242 | 0.358656 |
| v6_full | 0.903655 | 0.905750 |
| v5_empty | 1.018155 | 1.010981 |

满载收集耗时明显减少；加入 v6 编码后收益较小，因为编码和 CRC 占用更多工作。
空 v5 路径可能轻微退化，保留两个测量结果。桌面基准使用生产便携位循环 CRC，
没有设备 ROM CRC、真实信号表读锁、无线传输或 RTOS 调度；未控制系统后台负载。
上述比例不能直接作为 ESP32-S3 实机延迟、吞吐或功耗结论。

真实 ESP32-S3 `-O2` 编译的追加函数为 140 字节 text，无 literal、记录查找或
子调用；入口 frame 为 32 字节，与旧通用插入函数一致。通用插入的 text 保持
200 字节且逐字节相同；主表任务 frame 保持 672 字节。添加函数后编译器对未修改
源码的 v5 编码、hello 编码和接收 apply 分别少发出 2 / 1 / 1 字节，合计净增
136 字节代码。对象统计如下：

| object | section | before_bytes | after_bytes |
| --- | --- | --- | --- |
| espnow_v6 | text | 4116 | 4252 |
| espnow_v6 | literal | 148 | 148 |
| espnow_link | text | 4969 | 4969 |
| espnow_link | literal | 1568 | 1568 |

codec 的 bss/data/rodata 为 0/0/0，链路对象保持 253/16/1,428 字节。独立对象与
真实固件对象的所有 text/literal/data/rodata 字节及 bss 大小一致；链路中仅两处
错误检查的源文件行号随注释移后 2 行，其他指令字节相同。实际 ELF 验证主表
两处追加调用及一处 RPM 通用覆盖，没有参考或基准符号。普通、统计、无 SD 槽
三种固件通过，对齐后镜像都增加 144 字节：

| build | before_hex | after_hex | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x285680 | 144 |
| stats | 0x285e20 | 0x285eb0 | 144 |
| no-slot | 0x275fa0 | 0x276030 | 144 |

采用依据是直接消除构造快照的平方扫描，静态 RAM、收集调用栈和协议保持原行为，
用少量 Flash 换取更少的循环工作。[目标证据](performance/snapshot-append-xtensa-2026-10-14.txt)
保留实际编译命令、逐节对照、反汇编、测试/负对照与三种固件哈希；
[源码差异](performance/snapshot-append-source-2026-10-14.diff)只包含这项生产改动，
逆应用可精确恢复此次改动前的三个文件。设备端收集、编码与发送耗时仍待测量。

```sh
cmake -S test/host/espnow -B /tmp/obd-snapshot-append -G Ninja -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_COLLECT_BENCHMARK=ON
cmake --build /tmp/obd-snapshot-append
ctest --test-dir /tmp/obd-snapshot-append --output-on-failure
/tmp/obd-snapshot-append/bench_master_collect
```

## 油门相关数据优先调度验证

2026-10-28：`test/host/engine/test_engine.c` 的 `test_responsive_driver_inputs` 用同一模拟适配器回放修改前后调度。基线为 `10f780ad86a471c4b63e07b1c7fd4d2790fd281a` 的 `vp_engine.c`；主机 Release 构建启用 ASan/UBSan，协议 6、每个应答延迟 2 ms、默认请求间隙 30 ms、十个独立 PID，压力和节气门不设置显式周期。启动稳定后，每种需求掩码各观察 5 秒。此处时间为模拟时钟，不是桌面程序运行耗时，也不是 BLE/ECU/LCD 实测。

| Demanded signals | Before: RPM / speed / boost / TPS reads per 5 s | After: RPM / speed / boost / TPS reads per 5 s | Before: max RPM / speed / boost / TPS request gap (ms) | After: max RPM / speed / boost / TPS request gap (ms) |
|---|---|---|---|---|
| RPM + speed + boost + TPS + 6 background | 63 / 31 / 7 / 7 | 52 / 26 / 26 / 26 | 96 / 160 / 640 / 640 | 128 / 192 / 192 / 192 |
| RPM + speed + boost + 6 background | 63 / 31 / 9 / 0 | 63 / 32 / 31 / 0 | 98 / 162 / 576 / — | 96 / 160 / 160 / — |
| RPM + speed + boost only | 63 / 32 / 62 / 0 | 63 / 31 / 63 / 0 | 96 / 162 / 98 / — | 96 / 162 / 98 / — |

压力在混合页面不再等待整轮后台参数；只读三个参数时保持原有采样节奏。四个优先信号全开仍共享串行带宽，表中转速与车速频率略降，不能宣称所有参数同时提速。该测试另验证后台参数没有饥饿、切页停止不需要的请求；显式周期、慢速节气门、共用 PID 合并及请求前后命令的不可分割性由独立用例检查。更慢适配器、超时或厂商多命令请求会拉长间隔。

`test/host/signal_store/test_display_response.c` 抽取生产显示更新函数，验证转速、车速、压力（含负值）、节气门及负载在首次刷新显示最新样本，数值未变时不重写标签，失效与恢复仍正确；温度保留原有平滑。`test/host/media/test_video_player_wait.c` 的同帧测试验证 1000 次未跨帧数值更新不唤醒读卡或解码，跨帧与快速往返仍唤醒。原播放器已按目标帧去重，这次减少的是无效任务唤醒，不能据此宣称减少了原本不存在的重复解码或提高了视频帧率。

本轮 engine、signal_store、media 三组共 24 项主机测试通过；四个修改后的生产 C 文件通过 ESP32-S3 编译器 `-fsyntax-only` 检查，未执行完整固件构建或实车延迟测量。
