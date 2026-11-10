# Performance audit and measurements

English | [简体中文](PERFORMANCE.md)

This record describes the working tree on October 14, 2026. Code inspection, over ten runtime
optimizations, and optional device statistics are in place. Desktop timings and host tests
do not establish ESP32 frame rate, response latency, or that the system has reached its performance limit.

## Theme labels and demand-mask deduplication

Ordinary left-aligned runtime-theme bindings without zoom or rotation retain `LV_SIZE_CONTENT`. In LVGL 8, `lv_label_set_text()` refreshes the text and marks self-size layout dirty, so this path no longer calls `lv_txt_get_size()`, fixes the dimensions or repositions the label on every update. Centered, right-aligned and transformed labels keep the existing explicit sizing logic. The real-LVGL `test_page` verifies width growth after a longer value and an unchanged left anchor; this checks layout correctness, not render time or device frame rate.

The UI tick now publishes the local ESP-NOW demand mask only when the local page mask changes. After merging remote demand, it updates the ELM client only when the aggregate OBD mask changes. Page changes and expiration of remote demand still publish the new mask immediately. This removes repeated critical-section writes on stable pages; host page tests do not cover this FreeRTOS path, and device timing and frame rate remain to be measured.

## Project-wide path audit

The table records mechanisms present in the current code. `host` covers correctness of pure C
modules only; display, DMA, BLE/WiFi coexistence, SD, and task scheduling still need device validation.

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

Implemented changes include bytewise field extraction, RaceChrono due-channel filtering, theme rule output reuse,
applied-look reuse, active-page and binding-item caching for theme pages, RGBA image planning, ROM CRC reuse,
integer delivered-unit conversion, a static vehicle request-to-source index, local-mask poll-request
deduplication, ESP-NOW master static standard-signal ID lookup and configuration snapshot reuse,
and an identity-factor single-precision writeback path. Other rows record existing implementations. CPU/cache/buffer settings need measured path costs and resource
headroom before further tuning; desktop timing alone cannot justify increasing them.

## Single-pass hexadecimal decoding for ELM327 responses

The ordinary response-line and ATMA monitor-line parsers in `vp_engine.c` run on the BLE receive
hot path. Ordinary response tokens now validate and decode their bytes in the same pass instead of
validating and converting them separately. Monitor tokens cache their numeric value and validity
while they are split, so 11-bit IDs, 29-bit IDs, DLC fields, and data bytes reuse that result.
Numbered multi-frame responses, compact hexadecimal, space-separated 29-bit IDs, CAF0 ISO-TP, and
extended-address parsing remain supported; the single-request ELM327 state machine and response
precedence are unchanged. Monitor consumption also skips a zero-length `memmove()` when the complete
receive buffer has already been consumed.

All host `engine` tests pass, and `vp_engine.c` compiles into a temporary object using the current
ESP-IDF 5.5.3 ESP32-S3 arguments from `build/compile_commands.json`. No real BLE receive timing or
ESP32 cycle measurement is available, so this records removal of duplicate scans without claiming a
specific runtime speedup.

## BLE command-send hot path

`elm327_ble_client.c` receives every command in `sink_send()` from `vp_engine`: profile `bus.init` commands are normalized to uppercase without whitespace at compile time, while standard AT commands, state changes, and OBD requests are generated without whitespace. The transport now performs one bounded length calculation, one copy, and one carriage-return append. It keeps the 20-byte ATT chunking, write order, and error handling, while removing per-character whitespace filtering and the duplicate command construction.

This reduces firmware-side CPU and memory traffic before each send; the concrete ESP32-S3 cycle gain has not been measured. `vp_engine` remains a single-threaded, one-command-in-flight state machine.

## Reuse NVS configuration and linked state in the UI tick

`my_timerMain()` already takes one `nvs_user_cfg_t` snapshot at the beginning of each tick. The RPM
warning gate for deciding whether to take the full OBD snapshot and the flash renderer previously
read NVS again independently. In master mode, `espnow_link_linked_en()` also read NVS internally
from both paths. The same tick now passes its configuration snapshot and one computed linked-flash
state to both RPM functions; standalone mode skips the ESP-NOW query entirely.

This keeps one consistent configuration view throughout the tick and removes two direct duplicate
configuration copies and lock operations. In master mode it also removes the repeated linked-state
query and its internal NVS reads, saving up to four extra `nvs_cfg_read()` snapshots. RPM thresholds,
flash conditions, the three-gauge ramp, and refresh-period semantics are unchanged.

The initial check reused ESP-IDF 5.5.3 ESP32-S3 arguments from `build/compile_commands.json` to
compile `espnow_link.c`, `ui.c`, and `ui_ext.c` into temporary objects. This round also used an
available IDF Python environment to build complete normal and instrumented firmware images.
ESP32-S3 cycle measurement is still pending.

## Read only the OBD fields needed by configurable pages

The Temp, Info, Needle, and Chart paths in `my_timerMain()` no longer read all 12 OBD slots
unconditionally. The current page's source mapping produces a slot mask: duplicate sources are read
once, retired display slot 9 is never read, and RPM is added when warning, flashing, or linked-ramp
rendering needs it. Needle planning and rendering share the same tick configuration snapshot,
removing another per-frame `nvs_cfg_read()`; the public needle update interface remains unchanged.

| Page | Previous input slots/tick | Current input slots/tick |
|---|---|---|
| Needle / Chart | 12 | 1, at most 2 with RPM warning |
| Temp | 12 | at most 3, at most 4 with RPM warning |
| Info | 12 | at most 5, at most 6 with RPM warning |
| Gear / RPM / Speed | 3 | 3 (unchanged) |

Duplicate mappings and invalid/retired items can reduce these counts further. A retired-only mapping
without an RPM requirement performs no signal reads. The new `obd_data_get_snapshot_mask()` performs
one batch read with a shared freshness timestamp and initializes unrequested fields to their existing
invalid sentinels. Speed clamping, signed pressure half-up conversion, link/local-source rules, and RPM
override are preserved; override is accessed only when RPM is requested. Full selections delegate to
the existing full-snapshot implementation, and existing callers incur no sparse-planning overhead.
Sweep animation, polling, and wireless frequencies are unchanged.

The new host test compares every field against the original full snapshot across all 4096 masks × 8
validity/override scenarios, pressure rounding boundaries, empty masks, extra bits, NULL output,
display-source mappings 0..255, duplicates, and retired items. All 4 `signal_store` tests pass under
ASAN/UBSAN; the 7 project host suites pass all 36 tests.

Reproduce the desktop microbenchmark (AppleClang Release / `-O3`, sanitizers disabled):

```bash
cmake -S test/host/signal_store -B /tmp/obd-snapshot-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF
cmake --build /tmp/obd-snapshot-bench --target test_snapshot_mask
/tmp/obd-snapshot-bench/test_snapshot_mask --bench
```

Each scenario warms both paths for 20,000 calls, then alternates 7 groups of 500,000 calls per path,
checks requested-field checksums, and takes the median. Full results are in the
[CSV](performance/snapshot-mask-host-2026-10-14.csv).

| scenario | requested_slots | full_ns | masked_ns | ratio |
|---|---|---|---|---|
| chart_oilp | 1 | 24.76 | 11.37 | 2.177 |
| chart_oilp_rpm | 2 | 23.63 | 12.53 | 1.886 |
| temp | 3 | 23.64 | 18.58 | 1.272 |
| temp_rpm | 4 | 23.62 | 19.11 | 1.236 |
| info | 5 | 23.60 | 22.87 | 1.032 |
| info_rpm | 6 | 23.60 | 24.44 | 0.966 |
| all | 12 | 23.62 | 24.96 | 0.946 |

`ratio = full_ns / masked_ns`. Desktop locks are stubs and storage is ordinary host memory; timing
excludes page planning, LVGL drawing, PSRAM latency, and RTOS scheduling. The six-field case is
slightly slower on this host, and the twelve-field control includes the new API wrapper overhead,
so these results do not claim every path runs faster. The established firmware changes are fewer
requested slots and one fewer needle configuration copy. Actual timing, especially for denser Info
pages, still needs ESP32-S3 `ui_tick` measurements.

## Theme page active-page and binding-item caching

`theme_tick()` first uses a cached pointer to the active theme page. It falls back to the live-page list only when `lv_scr_act()` changes, during a page transition, or after the cache is invalidated. The first page update still visits all non-image elements to initialize unbound arcs, bars, and pointers. Once seeded, the page stores only non-image elements with a `data_source`; static decoration and unbound widgets are no longer revisited on every frame.

Page deletion clears the active-page cache. If the binding-index allocation fails, the code falls back to the full scan and page creation still succeeds, so the optimization does not change page behavior. Theme rules, signal snapshots, page transition animations, and LVGL object lifetimes remain unchanged. This reduces the steady-state page-list lookup and static-element scan; real ESP32 page timing has not yet been measured, so device gains remain unmeasured.

## Bit-field extraction benchmark

`vp_rpn_extract_raw()` now consumes byte fragments instead of individual bits: a 1–32-bit
field touches at most 5 bytes, uses no unaligned word loads, and never reads beyond the declared
payload. Sign extension removes a branch dependent on the data's sign. Tests compare endianness,
unaligned fields, signed 32-bit values, and unchanged error output against an independent bitwise oracle.

```bash
cmake -S test/host/vehicle -B /tmp/obd-perf-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DVH_SANITIZE=OFF -DVH_BENCHMARK=ON
cmake --build /tmp/obd-perf-bench --target bench_raw
/tmp/obd-perf-bench/bench_raw
```

`bench_raw.c` retains the previous production bitwise algorithm as its baseline. Both paths
use runtime inputs, one indirect call per extraction, identical data, and matching checksums.
After warm-up, measurement order alternates over 7 samples of 1,000,000 extractions per case,
reporting medians. Results below were collected on October 14, 2026 with Apple M1 / arm64,
Apple clang 21.0.0, Release. Raw results are in the [CSV](performance/raw-host-2026-10-14.csv).
Nanoseconds measure one desktop extraction, not ESP32 cycles, OBD throughput, or whole-system
speedup. The difference for 1-bit LE is within timing noise.

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

## RaceChrono request workload

The stream no longer wakes every 20 ms: with eligible subscriptions it waits for the earliest
channel deadline; disconnected, CCCD-off, deny-all and retired-only states wait for events
without periodic checks. It first selects subscribed, due channels, then takes one batched signal
snapshot; no due channel means no signal-store access. Filters are indexed by channel. GATT
writes and the stream share a short critical section, while signal reads and BLE sends happen
outside the lock.

Planning does not update send timestamps; only valid samples still allowed by the latest filter
and enabled connection/notifications claim their interval. Semantics remain: 0=100 ms, at least
20 ms between channel sends, subscription updates retain timestamps, reconnection resets state,
and allow-all keeps independent timestamps. Batch snapshots are at least 20 ms apart: invalid
samples do not claim sending intervals, but a separate retry deadline prevents repeated events
from busy-polling signal storage. `F109` stays retired, is never sampled, and is never sent.

Connection, disconnection, CCCD, filter writes, service readiness and BLE shutdown wake the worker
through a static binary semaphore. Tokens persist and coalesce before blocking, preventing lost
events between deadline planning and waiting. Storage lasts for the entire boot rather than the
worker lifetime, so late callbacks do not reference a deleted task handle. The hub first makes
ready=false, then invokes registered shutdown wake functions outside its lock; teardown remains
one-way after worker exit. Service readiness is published after calling start_service, and turning
notifications off during a snapshot does not consume sending deadlines.

Deadlines round up to RTOS ticks; the current firmware uses 1000 Hz. Intervals not divisible by
20 ms no longer quantize to the old checking grid: a 101 ms subscription becomes due at 101 ms
rather than 120 ms on the virtual clock. Actual execution still depends on scheduling, batch
snapshot throttling and BLE cost, so exact sending periods are not guaranteed. Each notification
adds a short state-snapshot lock; wait planning also adds a short lock and deadline calculation
for up to eight channels. These are tradeoffs for fewer wakeups, not proof that full-load sending
is faster. The target ELF has 84 bytes of semaphore storage and a 4-byte pointer; four hub callback
slots each add 4 bytes, totaling 104 bytes of additional static RAM with no dynamic heap allocation
for the wake mechanism.

The earlier sparse-snapshot comparison retains its baseline of fixed valid signals, 1 second,
and 50 checks:

| Simulation | Before snapshots/s | After snapshots/s | Before input slots/s | After input slots/s | Notification candidates/s |
|---|---|---|---|---|---|
| all, 100 ms | 50 | 10 | 450 | 80 | 80 |
| one channel, 100 ms | 50 | 10 | 450 | 10 | 10 |
| deny all | 50 | 0 | 450 | 0 | 0 |

`Before input slots` counts inputs to the old batch API, including the retired `SIG_SLOT_NONE`.
450 does not mean 450 actual signal reads. Notifications are simulated sending candidates;
actual BLE delivery has not been measured. This round's deadline model / actual worker virtual
clock comparison follows, excluding startup, control-event and shutdown wakes:

| Simulation | Fixed-loop timed wakes/s | Deadline timed wakes/s | Snapshots/s | Notification candidates/s |
|---|---|---|---|---|
| all, 100 ms | 50 | 10 | 10 | 80 |
| one channel, 100 ms | 50 | 10 | 10 | 10 |
| deny all | 50 | 0 | 0 | 0 |
| disconnected / CCCD off / F109 only | 50 | 0 | 0 | 0 |
| all invalid, 100 ms | 50 | 46 | 46 | 0 |

The invalid scenario first samples at 100 ms, then retries every 20 ms. A filter event every
millisecond still produces only 46 snapshots, but events themselves wake the task: zero timed
wakes must not be mistaken for zero task executions.

`test_racechrono_filter.c` compares against the old rule-list model over 20,000 subscription/time/
validity steps and checks read-only planning against an independent absolute-deadline model.
Coverage includes 1 us boundaries, zero/minimum/maximum intervals, re-add/reset, independent
histories, retry gating, no eligible channels, 64-bit time and finite-wait saturation; 269,071 checks
pass. `test_racechrono_stream.c` compiles the actual worker, GATT callback and `ble_hub.c`, replacing
only SDK/RTOS primitives and the signal-store boundary. Separate boot subprocesses verify 12
deterministic scenarios:

- All/one channel, deny-all, disconnection, CCCD off, F109 only, invalid samples, per-millisecond event storms and 101 ms subscriptions.
- Shutdown before blocking, reconnection/filter changes while waiting, and notifications turned off/re-enabled during a snapshot.
- Unchanged 8-byte wire values/scaling, no F109 reads, BT APIs/logs/signal reads outside locks, service readiness order, shutdown idempotence and late wakeups after worker deletion.

ASan/UBSan pass. Four negative controls in `/tmp` are rejected: missing shutdown wake, missing
filter wake, readiness published before service start, and sending intervals claimed with
notifications off. Normal and profiling firmware builds pass, with apps `0x2855c0` / `0x285e20`,
both leaving 16% of the 3 MB partition free. These prove code paths, model workload, memory cost
and buildability, not actual FreeRTOS dual-core races, radio congestion or current consumption;
device gains remain unmeasured.

## Theme rule output reuse

Each `tr_eval()` still advances all rules in page order: conditions, hysteresis, debounce,
and timer/change triggers. An unchanged final active set reuses its sorted order and target
coverage, skipping sorting, clearing and action replay. A changed set rebuilds from an empty
override table, removing overrides of rules that exit; `tr_reset()` also clears the cache.
Compiled rules/priorities/actions remain immutable between resets; results are program-owned
and read-only. Cache fields are appended to `tr_program_t`, with no per-frame allocation
or theme package/persistence format change.

The comparison test forces output rebuilding in a second program for 128 pages × 600 steps,
covering shared IDs, timing, hysteresis, debounce, invalid values, stable/changing sets, empty
sets, resets, and NaN/Infinity priorities. It compares all runtime state, active order, and
coverage; 6,061,843 checks pass. Editor golden vectors independently verify rule semantics.

```bash
cmake -S test/host/rules -B /tmp/obd-perf-rules-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTR_SANITIZE=OFF -DTR_BENCHMARK=ON
cmake --build /tmp/obd-perf-rules-bench --target bench_rule_cache
/tmp/obd-perf-rules-bench/bench_rule_cache 0x63caf129
```

The benchmark uses the same Apple M1 / arm64, Apple clang 21.0.0 Release environment and a
fixed seed. After warm-up, it alternates order for 7 samples of 300,000 calls each and reports
medians; checksums match. Every rule has 8 actions on distinct targets. Stable scenarios
activate all rules; the changing scenario alternates half the rules each step.
Raw results are in [CSV](performance/rules-host-2026-10-14.csv).

| rules | targets | active_set | rebuild_ns | cached_ns | ratio |
|---|---|---|---|---|---|
| 0 | 0 | stable | 9.39 | 2.76 | 3.40 |
| 1 | 8 | stable | 27.15 | 4.52 | 6.00 |
| 8 | 64 | stable | 308.48 | 19.19 | 16.08 |
| 16 | 128 | stable | 673.94 | 32.66 | 20.63 |
| 16 | 128 | alternating | 319.07 | 319.15 | 1.00 |

`rebuild_ns` times the current evaluator with output rebuilding forced on every call, including
the new mask/cache checks; it is not the old firmware baseline. Ratios describe reuse relative
to forced rebuilding in these synthetic desktop cases, not whole-frame, ESP32, or pre-change
firmware speedups. A set that changes on every call cannot be reused.

## Theme pages reuse applied looks

`theme_page.c` still updates signal change windows, bindings, and `tr_eval()` on every tick.
When both the active rule set and system-alarm suppression are unchanged, it skips target
look assembly/comparison. First evaluations, first updates after reset, set changes, and
suppression transitions still apply looks; ending an alarm still restores the layout background.
This reuses existing rule cache state, with no new persisted fields or per-frame allocations.
Rules, targets, and base looks remain immutable during a page's lifetime.

The optional `TE_PAGE_LVGL=ON` host test compiles actual `theme_page.c` and LVGL 8.4.0.
Its reference page additionally executes the former target look loop on every tick, comparing
LVGL object state, applied looks, and rule runtime state. A fixed random seed `0x51fe9123`,
10,000 replay steps, and directed cases pass 680,850 checks. Coverage includes debounce/timing,
change-window expiry, invalid inputs, sweeps, rule exits, alarm recovery, empty images, pointer
angles, and video switch/pause calls. Platform time, signal snapshots, and video I/O use host
shims; this does not verify LCD DMA, video decoding, or actual display frame rate.

```bash
cmake -S test/host/theme -B /tmp/obd-theme-page-tests -G Ninja -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-theme-page-tests
ctest --test-dir /tmp/obd-theme-page-tests --output-on-failure
```

Sources default to `managed_components/lvgl__lvgl`; `TE_LVGL_DIR` can select LVGL 8.4.0 sources.
CI pins upstream commit `4495f428630cc1741bd8bfd977f080e8460e8e8d` and enables this test.
Stable-page benchmark:

```bash
cmake -S test/host/theme -B /tmp/obd-theme-page-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PAGE_LVGL=ON -DTE_PAGE_BENCHMARK=ON
cmake --build /tmp/obd-theme-page-bench --target bench_page
/tmp/obd-theme-page-bench/bench_page
```

Apple M1 / arm64, Apple clang 21.0.0 Release, no LTO. Synthetic pages alternate arc/bar/label
items, with up to 128 targets, 16 rules, and 32 bindings. Inputs/time stay fixed during timing.
Each path warms up for 1,000 calls; seven alternating samples of 200,000 updates use medians
and matching checksums. This includes the current signal-snapshot shim, bindings, and evaluator,
but excludes page creation/deletion and LVGL drawing. All 20 scenarios (0/1/8/64/128 targets ×
active/inactive × suppressed/unsuppressed) are in the
[CSV](performance/page-lvgl-host-2026-10-14.csv). Representative active, unsuppressed rows:

| targets | active | suppressed | forced_targets_ns | skipped_targets_ns | ratio |
|---|---|---|---|---|---|
| 1 | 1 | 0 | 33.74 | 28.60 | 1.18 |
| 8 | 1 | 0 | 73.42 | 40.09 | 1.83 |
| 64 | 1 | 0 | 453.33 | 173.78 | 2.61 |
| 128 | 1 | 0 | 866.99 | 327.64 | 2.65 |

`forced_targets_ns` forces a target pass after the current update path; `skipped_targets_ns`
uses the current skip path. Both include the new checks and existing evaluator cache;
this is not a before/after comparison of old firmware. Ratios only describe these fixed-input
desktop update cases. Frequently changing rule sets still execute the target pass, so the
results cannot predict ESP32 frame rate, whole-system response, or wireless/SD throughput.
Zero-target controls are retained in the CSV as well. The normal image changes from
`0x2846d0` to `0x284700` (+48 bytes), and the statistics image from `0x284ed0` to `0x284f10`
(+64 bytes). Both retain 16% app-partition headroom. Device timings remain unmeasured.

## RGBA image planning by row

`tpx_plan()` first classifies an image window as wholly inside, wholly outside, or intersecting
the round panel. Outside windows return an empty plan; inside windows avoid circle checks;
intersections use integer binary search and circle symmetry to compute each row's visible span.
The bounding box only searches the first and last nontransparent pixels per row. Transparent
runs check four alpha values at a time, with reads bounded to the current visible span;
opacity classification still examines individual pixels after cropping.
This adds no allocations, static lookup tables, or per-frame work. The RGB565 path is unchanged.

An independent reference retains the old per-pixel circle tests and bounding-box updates.
Differential tests cover all 129,600 panel pixel positions, alpha masks on both sides of the rim,
all crop modes, 20,000 randomized replay steps with seed `0x70a9413b`, maximum 2047×2047 images,
and extreme screen coordinates: 757,180 checks, zero failures. Extreme coordinates are checked
against expected empty plans without executing overflowing distance calculations in the old
reference. Under ASAN/UBSAN, the portable suite passes 6/6; local managed-LVGL and CI-pinned-LVGL
integration suites each pass 7/7. The latter reproduces CI locally; remote CI was not run.

```bash
cmake -S test/host/theme -B /tmp/obd-plan-core-tests -G Ninja
cmake --build /tmp/obd-plan-core-tests
ctest --test-dir /tmp/obd-plan-core-tests --output-on-failure

cmake -S test/host/theme -B /tmp/obd-plan-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PLAN_BENCHMARK=ON
cmake --build /tmp/obd-plan-bench --target bench_plan
/tmp/obd-plan-bench/bench_plan
```

The benchmark runs on Apple M1 / arm64, Apple clang 21.0.0 Release, without LTO. Synthetic square
images at 16/64/360 px use four alpha patterns: `opaque`, `transparent`, `mixed`, and `rim`
(a 4 px border), with all three crop modes. The no-round-clipping control runs once per case;
round clipping covers placements `(0,0)`, centered `((360-w)/2, (360-w)/2)`, and wholly outside
`(-w,-w)`. Each path warms up for 20 calls. Seven alternating samples use medians and matching
checksums, with `clamp(20000000/(w*w), 200, 20000)` iterations per sample. All 144 scenarios are
preserved in the [CSV](performance/plan-host-2026-10-14.csv). Representative rows below use
360×360 images, round clipping enabled, placement `(0,0)`, and `crop=0` (no bounding-box crop):

| pattern | per_pixel_ns | row_span_ns | ratio |
|---|---|---|---|
| opaque | 243285.00 | 53305.00 | 4.564 |
| transparent | 44300.00 | 25140.00 | 1.762 |
| mixed | 147875.00 | 5305.00 | 27.875 |
| rim | 50785.00 | 20875.00 | 2.433 |

`per_pixel_ns` measures the old planner, `row_span_ns` the current planner, and `ratio` their
time ratio. These numbers cover validation, alpha-bound searches, and format classification;
they exclude allocation, resampling, LVGL drawing, LCD DMA, and ESP32 memory-access costs.
Planning work is reduced on initial image loads and cache misses, but these measurements cannot
predict total theme-loading time or device FPS. Fully transparent controls are retained in the CSV.

The final normal image changes from `0x284700` to `0x2848f0` (+496 bytes), and the statistics image
from `0x284f10` to `0x2850f0` (+480 bytes). Both builds pass; their app partitions retain
`0x7b710` / `0x7af10` bytes (about 16% each). Device image-loading time remains unmeasured.

## Vehicle and video validation reuse ROM CRC

On ESP32, `vp_crc32()` and `obv1_crc32()` use the existing `esp_rom_crc32_le()` API.
Hosts keep the pure-C vehicle bitwise and video nibble-table implementations, with no new
lookup data or heap allocation on the device. IEEE CRC32 initialization, reflection, final
XOR, empty inputs, and incremental calls retain their semantics; existing vehicle blobs and
OBV1 files keep their CRC values. This optimizes profile compilation/validation and video
header/index opening, without changing per-frame decoding.

`test_crc.c` compares independently generated Python zlib vectors, the previous algorithms,
and randomized chunks. It covers arbitrary seeds, every length from 0–128, address offsets
0–7, block boundaries, the maximum 288,000-byte video index, and single-bit corruption.
The portable and `ESP_PLATFORM` paths each pass 421,932 checks. The latter links the official
ESP-IDF 5.5.3 Linux CRC implementation, with the existing container tests also checking damaged
files. SDK compatibility tests do not execute physical ROM.

The basic `media` tests still run without an SDK. Set `MH_ESP_ROM_DIR` to `components/esp_rom`,
or `MH_ROM_CRC=OFF` to disable SDK compatibility tests. The benchmark below requires that directory:

```bash
cmake -S test/host/media -B /tmp/obd-perf-crc-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMH_SANITIZE=OFF -DMH_BENCHMARK=ON
cmake --build /tmp/obd-perf-crc-bench --target bench_crc
/tmp/obd-perf-crc-bench/bench_crc
```

The previous bitwise/nibble algorithms compile in a separate translation unit and are compared
with the SDK Linux implementation called by the actual firmware wrappers. Measurements use
Apple M1 / arm64, Apple clang 21.0.0 Release, no LTO, a fixed seed, warm-up, seven alternating
samples, and medians; checksums match. Inputs are synthetic byte buffers; large buffers stress
the algorithms and do not represent actual vehicle profile sizes. The saved run had no concurrent
builds or other benchmarks. The [CSV](performance/crc-sdk-host-2026-10-14.csv) includes all results
at offsets 0 and 3.

| path | bytes | offset | baseline_ns | sdk_host_ns | ratio |
|---|---|---|---|---|---|
| video_nibble | 60 | 0 | 283.06 | 151.32 | 1.87 |
| vehicle_bitwise | 1024 | 0 | 8079.83 | 2579.59 | 3.13 |
| vehicle_bitwise | 8192 | 0 | 64324.22 | 20525.39 | 3.13 |
| video_nibble | 8192 | 0 | 38539.06 | 20517.58 | 1.88 |
| vehicle_bitwise | 32768 | 0 | 257726.56 | 82234.38 | 3.13 |
| video_nibble | 288000 | 0 | 1351625.00 | 721250.00 | 1.87 |

`sdk_host_ns` times the SDK Linux algorithm on a desktop, not ESP32 ROM or total import/video
opening time. Normal and profiling ESP32-S3 firmware builds pass. Both target objects reference
the ROM API, resolved to `0x40001c98` in the final ELF, and the private 64-byte video table is gone.
With the same normal configuration, the image shrinks from `0x284790` to `0x2846d0` (192 bytes).
This measures size rather than speed; device gains still need actual import and video-opening timings.

## Video ring headers outside critical sections

`ring_peek()` in `video_player.c` previously read PSRAM record headers while holding the session
lock. It now uses the inline reader in `video_ring_peek.h`: the lock only snapshots `rpos` / `used`
in internal RAM, and the frame number and length are read outside the lock after confirming a
committed record. The ring has one producer and one consumer; the producer can only reuse space
after the consumer releases the record, so the snapshotted header and wrap marker cannot be
overwritten during the read. The buffer address and capacity remain fixed until session teardown,
and the session-reference lifetime rules are unchanged.

On wrap, the reader re-enters the lock and subtracts tail padding from the **current** `used`,
preserving bytes committed by the producer after the snapshot, then snapshots again at the start.
A commit after an empty snapshot can be handled on the next peek without exposing an uncommitted
frame. Ordinary records still use one critical section; wraps use two additional short metadata
critical sections to keep all PSRAM header reads outside the lock. The format, watermark,
frame-rate/drop policy, wakeup mechanism, DMA, buffer capacity, and heap allocation are unchanged.

`test_video_ring_peek.c` compiles the actual firmware inline reader, replacing only the FreeRTOS
critical-section API:

- Compare with the old reader for 240,000 steps across six capacities, including empty buffers, repeated peeks, fill/consume, explicit/implicit wrap, and frame sizes not divisible by four.
- Protect the ring-data page with `mprotect(PROT_NONE)` on each lock entry and restore it on exit, verifying that ordinary and wrap headers are not read inside critical sections; the old lock-held reader faults in a temporary negative control.
- Inject producer commits at snapshot unlock, checking that empty snapshots do not expose new frames and padding subtraction preserves concurrent commits.
- Validate 200,000 ordered records with two pthreads, checking payload twice before consumer release; a separate ThreadSanitizer run passes.

Both normal and statistics-enabled ESP32-S3 firmware builds pass. Disassembly of the target
`video_player.c.obj` confirms that the two PSRAM header `l32i.n` loads execute after the
`vPortExitCritical` call. Host page protection and thread tests do not reproduce PSRAM cache
misses, ESP32 dual-core interrupts, or SD latency. This proves removal of slow-storage accesses
from the critical section, not a specific FPS or device lock-time improvement.

## Integer conversion of delivered signal units

Unit conversion in `signal_store_get_delivered_mask()` remains outside the critical section.
For oil pressure over its valid raw range of 0–2000 kPa,
`floor(raw * 0.145038 + 0.5)` becomes `(raw * 72519 + 250000) / 500000`,
with a uint32-safe intermediate. Voltage replaces `floor(raw / 100.0)` with integer
division and a negative-remainder correction, preserving floor rounding.
Both paths check registry operation, coefficient and rounding metadata. Negative/out-of-range
pressure or unmatched metadata retain the original double formula; other signals retain their
semantics. Units, validity, staleness and link gating are unchanged, with no new allocation.

An independent translation unit preserves the original production formula. Comparisons cover
integer corners across all 49 slots, every valid pressure/voltage value, negative voltage and
200,000 deterministic random steps (seed `0x39049bd1`), comparing double results bit for bit.
The public snapshot API additionally exhausts valid values and checks invalid NaN and link gating:
569,001 checks, zero failures. All 3 `signal_store` tests pass under ASAN/UBSAN.

```bash
cmake -S test/host/signal_store -B /tmp/obd-delivery-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF -DSS_DELIVERY_BENCHMARK=ON
cmake --build /tmp/obd-delivery-bench --target bench_delivery
/tmp/obd-delivery-bench/bench_delivery
```

Apple M1 / arm64, Apple clang 21.0.0 Release, no LTO, floating contraction disabled.
Reference and candidate use separate translation units and indirect calls with 256 runtime inputs.
After 20,000 warmups, 7 alternating samples of 1,000,000 calls use the median and matching
checksums; no builds or other benchmarks ran concurrently during timing.
All 6 scenarios are in the [CSV](performance/delivery-host-2026-10-14.csv):

| case | iterations | floating_ns | integer_ns | ratio |
| --- | --- | --- | --- | --- |
| oil_pressure | 1000000 | 3.26 | 2.73 | 1.193 |
| battery_voltage | 1000000 | 2.60 | 2.29 | 1.133 |
| rpm_identity | 1000000 | 1.68 | 1.44 | 1.161 |
| veh_identity | 1000000 | 0.96 | 1.29 | 0.746 |
| oil_pressure_negative | 1000000 | 1.59 | 1.88 | 0.848 |
| battery_voltage_negative | 1000000 | 1.88 | 1.57 | 1.196 |

`ratio = floating_ns / integer_ns`; values below 1 mean the candidate is slower. Custom identity
and negative-pressure controls do regress, and instruction placement can affect nanosecond-scale
host calls. These results do not establish a speedup for every path. The benchmark excludes
signal-store locks, clocks, PSRAM, page updates and ESP32 execution.

The [disassembly](performance/delivery-xtensa-2026-10-14.txt) uses the actual ESP32-S3
`signal_store.c` compiler flags for both wrappers: metadata guards become integer loads/comparisons,
with no `__eqdf2` or `memcmp` calls. Valid pressure uses integer multiplication/shifts; voltage
uses integer division/remainder. Both keep only the final integer-to-double conversion.
Generic fallbacks still reference double helpers and `floor`. This establishes removal of double
arithmetic on the target paths; device timing gains remain unmeasured.

Normal image: `0x2848f0` → `0x284aa0` (+432 bytes); statistics image:
`0x2850f0` → `0x285260` (+368 bytes). Both builds pass with `0x300000` app partitions,
leaving `0x7b560` / `0x7ada0` free (16%), respectively.

## `signal_store` metadata snapshots for batch reads

Current read and write paths sample the monotonic clock once while holding the signal lock, before updating or checking timestamps. A sample committed by another core while the caller waited for the lock is therefore not incorrectly treated as stale. Unit conversion remains outside the lock. The historical benchmark below predates this clock-placement change; its actual performance impact requires measurement.

For multi-slot reads, `signal_store_get_many()` and `signal_store_get_delivered_mask()` now read `local_mask`,
the number of registered vehicle signals, and `sys.obd_link` once after entering the critical section. Each slot
still checks its own validity, timestamp, range, and dynamic-slot bounds under that same critical section. Single-value
reads and single-bit requests retain a direct-read fast path, so one-value calls do not pay to build a snapshot.
Link gating, staleness, local-sensor priority, and return values are unchanged.

The benchmark used the current `signal_store.c` and a comparison copy with metadata snapshots removed. Both were built
on Apple M1 / arm64 with Apple clang 21.0.0 Release, `-O3`, and no LTO. Each case warmed up for 20,000 calls,
measured 7 samples of 500,000 calls, and reported the median. Checksums matched for every case; raw results are in the
[CSV](performance/signal-store-snapshot-host-2026-10-14.csv). This measures the shared read path with host lock and
fixed-allocation substitutes; it does not represent ESP32 PSRAM latency or whole-device frame rate.

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

`speedup = before_ns / after_ns`. Multi-slot batch reads improve as the requested slot count grows; single-slot
paths remain within timing noise. All 3 `signal_store` tests pass under ASAN/UBSAN.

## ESP-NOW master broadcast collection path

`master_collect()` gathers values for broadcast every 100 ms. Values are still read in OBD and vehicle ranges
using the batch API. Standard signal numeric IDs now come directly from the static `sig_registry` instead of doing
one runtime ID lookup per valid slot. `sys.obd_link` is read in the same `signal_store_get_many()` call as the standard
OBD range, removing one extra timestamp read and critical-section entry. The NVS configuration field needed by the
broadcast is copied once at the start of the collection and reused. Vehicle signals still use
`SIG_VEH_ID_BASE + index` in registration order; a slave also invalidates all vehicle slots in one batch when its
vehicle profile changes. ESP-NOW v5/v6 encoding, validity masks, the special gear field, and the single-threaded send
order are unchanged. The active profile hash is cached when ESP-NOW starts, so broadcast and slave receive validation
do not repeatedly look up the profile object and walk the ID string. A slave also caches its bound-master MAC when
the link starts and updates it through the explicit bind/unbind APIs; the receive callback no longer copies the full
NVS configuration or takes its mutex for every wireless frame.

This removes repeated lookup and lock work from snapshot collection and slave receive filtering without changing the
broadcast interval or wire protocol. All 30 tests across the ESP-NOW codec, signal_store, engine, rules, theme, and vehicle host suites pass;
the current target compile commands also compile `espnow_link.c` and `signal_store.c` successfully. No target-device
cycle sample is available, so no percentage speedup is claimed.

## One ESP-NOW task for broadcasts and the linked-test ramp

Previously the master used two tasks: broadcasts ran at 10 Hz when idle and 50 Hz during TEST,
while the independent ramp task woke every 20 ms whether TEST was active or not. The broadcast
worker now computes the RPM ramp once before sending and uses the same active/RPM sample for
the local override, packet flag, and v5/v6 RPM. The independent task is removed. One broadcast's
NVS snapshot supplies both the warning threshold and linked-enable; TEST no longer reads the
configuration again from a second task.

The 5000 ms rise, 800 ms hold, 2500 ms fall, threshold + 200 peak and uint16 saturation remain.
Rise/fall products are below `65735 * 5000`, so they now use uint32 arithmetic rather than 64-bit
multiplication/division. The timestamp remains 64-bit, protected with restart/completion state
in a short critical section. NVS, override, signal reads and radio APIs stay outside the lock.
Each packet uses one ramp sample; requests arriving after calculation take effect in the next
packet, preventing flag/RPM values from different phases.

TEST requests update the latest start time and give a boot-lifetime static binary token. An
idle wait can shorten to a deadline at least 20 ms after the previous send batch finished; if
already due, the next send starts immediately. Active waits remain 20 ms and idle waits 100 ms,
both measured from the end of the preceding send batch. Tokens cannot bypass the deadline:
repeated triggers restart the timeline without increasing broadcast frequency. Start/stop
boundaries are no longer sampled by two independent tasks, so the first ramp value and final
restored value are coherent with the same packet's flag. This does not claim identical
instant-by-instant output to the old two-task schedule.

OTA closes TEST admission before deleting tasks and clearing the override; late local/slave
requests cannot reactivate the test. The static token survives task deletion and callbacks do
not notify a task handle. Task-creation failure also closes admission. Slave TEST control
packets, ramp reception, presence reports and v5/v6 wire formats remain unchanged.

The following is a 1 s ideal-clock workload model, not CPU utilization or actual RTOS wake
counts. It includes the first execution at t=0 but excludes t=1 s. New-path counts are checked
by host replay of the real worker; old-path counts follow the periods of the original loops.

| case | before_task_iterations | after_task_iterations | before_config_reads | after_config_reads | data_batches |
|---|---:|---:|---:|---:|---:|
| idle_1s | 60 | 10 | 10 | 10 | 10 |
| test_1s | 100 | 50 | 100 | 50 | 50 |
| test_requests_every_1ms | - | 50 | - | 50 | 50 |

The event-storm scenario also has 1000 requests and 1001 semaphore takes. It proves that signal
sampling and radio sends do not scale with event frequency, not that events have zero cost.
An ordinary TEST start may also consume an extra token.

Under the target configuration, removal eliminates allocation requests for a `3072`-byte task
stack and `340`-byte TCB (`3412` bytes total, excluding allocator overhead). Stack units for
`xTaskCreate()` are bytes in this IDF; target compilation confirms `sizeof(StackType_t)=1` and
`sizeof(StaticTask_t)=340`, with an SDK check that the latter matches the TCB. New static storage
is an `84`-byte semaphore, `4`-byte handle, `8`-byte lock and `1`-byte admission flag; removing the
old `4`-byte task handle and `2`-byte shared RPM makes the net static-symbol increase `91` bytes
(excluding alignment). This is a code/ABI ledger, not measured boot free heap. The merged task
still has a `3584`-byte stack. Target `-fstack-usage` reports a `672`-byte static worker frame,
**excluding callees**; actual stack high-water margin still needs device measurement.

Host tests compile `espnow_link.c` and the real codec, replacing only SDK/RTOS, NVS and signal-store
boundaries. The 17 independent-boot scenarios cover idle, local/slave start, v6, restart before
completion, threshold changes, zero/maximum thresholds, per-ms events, an event before waiting,
active/idle OTA, task-creation failure, slave requests, 64-bit time, and restart after completion
sampling/during packet assembly. An independent formula checks the ramp, override, decoded
RPM/flag and minimum send interval. Both ordinary and Release + ASAN/UBSAN wireless suites pass
4/4. Five `/tmp` negative controls are rejected: missing event wake, a token bypassing the rate
gate, accepting after stop, ignoring restart, and packet RPM inconsistent with the sample.
This deterministic injection does not cover real dual-core preemption, WiFi/BLE coexistence
or concurrent OTA stability.

```bash
cmake -S test/host/espnow -B /tmp/obd-link-host -G Ninja -DEH_SANITIZE=ON
cmake --build /tmp/obd-link-host
ctest --test-dir /tmp/obd-link-host --output-on-failure
/tmp/obd-link-host/test_espnow_link
```

Both full ESP32-S3 builds pass: ordinary image `0x2855c0` → `0x285720` (up `352` bytes), statistics
image `0x285e20` → `0x285f60` (up `320` bytes), both leaving 16% of the 3 MB app partition free.
Neither ELF contains `master_linktest_task` / `s_linktest_task_handle` anymore. Actual power,
broadcast delivery, CPU and memory margins have not been measured; this is not evidence that
the whole system has reached its performance limit.

## Vehicle request dispatch and polling candidate indexes

`vp_engine_create()` builds a static request-to-source linked list after the profile is fixed and caches signals that contain request sources. `req_done()` walks only sources that can reference the completed request, then verifies that the source is still the signal's current source. `start_round()` checks only signals that can issue requests; CAN-only, derived, unmapped, and requestless signals no longer enter every round scan. The list preserves profile signal/source order, so fallback selection and response handling keep their existing order. The ELM327 loop remains a single-threaded, one-request state machine.

The index adds four `uint8_t` arrays with capacity limits of 32 + 32 + 96 + 48 slots, or 208 bytes; the actual object size also depends on layout and the other fields. The index is built once when a profile engine is created and is released with that engine, so response processing does not allocate dynamically.

The microbenchmark measures only request selection/dispatch loops. Production functions and timing loops are compiled in separate translation units to prevent the compiler from folding a path around fixed request counts. Each scenario uses 256 fixed requests, warms up for 20,000 calls, alternates 7 samples of 1,000,000 calls, and reports medians; linear and indexed checksums must match. Results collected on October 14, 2026 with Apple M1 / arm64, Apple clang 21.0.0, Release are below; raw data is in the [CSV](performance/request-index-host-2026-10-14.csv).

| case | signals | sources | requests | linear_ns | indexed_ns | ratio |
|---|---:|---:|---:|---:|---:|---:|
| catalog_like | 12 | 24 | 12 | 13.35 | 3.39 | 3.940 |
| mixed_max | 48 | 96 | 32 | 39.31 | 3.79 | 10.365 |
| sparse_max | 48 | 48 | 32 | 36.27 | 2.10 | 17.232 |
| can_heavy | 48 | 96 | 32 | 38.35 | 2.68 | 14.299 |

Here `ratio = linear_ns / indexed_ns`; it describes desktop selection-loop speed for fixed inputs, not whole-device speed, ESP32 instruction timing, or OBD round-trip latency. The 12/12 host engine suite and ASAN/UBSAN passed; compiling `vp_engine.c` alone with the existing ESP32-S3 command also passed, with 12,836 bytes of target object `.text`. Without a vehicle and adapter, real polling interval, Bluetooth throughput, and device-side gains remain unmeasured.

Benchmark command:

```bash
cmake -S test/host/engine -B /tmp/obd-engine-request-bench \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEH_SANITIZE=OFF -DEH_REQUEST_BENCHMARK=ON
cmake --build /tmp/obd-engine-request-bench --target bench_request_index
/tmp/obd-engine-request-bench/bench_request_index
```

## Local-mask deduplication in poll planning

`start_round()` previously scanned the collected request array for each due candidate signal,
requiring up to 48 × 32 membership checks. A local `uint32_t` now records collected requests;
validated request indices are 0..31, and increasing capacity beyond 32 is rejected at compile time.
The request array still follows first occurrence in candidate-signal order, not bit order; every
due signal sharing a request still updates its own `last_poll_ms`. No engine fields, allocations,
tasks, or persistent state are added. Array deduplication remains for the at-most-two
`rate: every` signals, avoiding mask overhead in that short path.

`engine_poll_plan` includes production `vp_engine.c` directly in a test translation unit and
compares it against an independent pre-change planning loop; production has no test hooks.
Coverage includes 0..48 signals, descending unique/shared/paired/mixed requests, request 31,
period/slow defaults and deadlines, unmapped/derived/source-less signals, CAN/request source
switches, every-only/empty rounds, and 32-bit time wrap. Each of 392 configurations replays
256 rounds: 100,352 full engine-state and slot-sequence comparisons pass with ASAN/UBSAN.
Five successfully compiled negative controls (low-16-bit aliasing, no deduplication, updating
only unique-request deadlines, retaining the mask across rounds, and reversed output) are rejected.

A separate driver benchmarks actual production planning and the old loop. Each case first
checks full state, warms up 10,000 iterations, then alternates seven samples of 200,000 iterations
and reports medians. Timings include planning and an order-sensitive checksum, not parsing,
transport, or response waits. Configuration: Apple M1 / arm64, Apple clang 21.0.0 Release,
no sanitizers/LTO. Raw data for 28 cases is in the [CSV](performance/poll-plan-host-2026-10-14.csv).
`unique` cycles downward through 32 request indices, so the 48-signal case repeats earlier
indices; `shared` uses request 31 throughout. Zero/one-signal cases are essentially equal
(an initial single-signal run showed a regression below 1 ns); high-signal cases alone must
not be presented as the benefit for every vehicle.

| signals | pattern | linear_ns | mask_ns | ratio |
|---:|---|---:|---:|---:|
| 9 | unique | 36.60 | 24.62 | 1.486 |
| 9 | shared | 19.06 | 15.75 | 1.210 |
| 32 | unique | 283.67 | 86.94 | 3.263 |
| 32 | shared | 66.31 | 48.38 | 1.371 |
| 48 | paired | 322.60 | 98.88 | 3.263 |
| 48 | mixed_refs | 249.65 | 83.51 | 2.989 |

`ratio = linear_ns / mask_ns`. The nine-signal planning loop takes about 1/1.21..1/1.50 of
its former time; 32/48-signal cases take about 1/1.30..1/3.26. These are desktop local-algorithm
results, not vehicle polling periods or ESP32 gains. Across normal/instrumented/no-slot
ESP-IDF 5.5.3 / Xtensa GCC 14.2.0 configurations, the engine remains 5016 bytes and the
compiler-reported static frame for `vp_engine_run()` remains 128 bytes; other emitted functions'
static frames are also unchanged. Target-object `size` text/readonly totals fall from 12317 to
12288 bytes. [Raw target compilation record](performance/poll-plan-xtensa-2026-10-14.txt). All three complete firmware builds pass; app sizes are `0x2857a0`, `0x286000`,
and `0x276160`. Device cycles, task maximum stack depth, and vehicle effective-update intervals
have not been measured and remain pending.

```bash
cmake -S test/host/engine -B /tmp/obd-poll-plan-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_POLL_BENCHMARK=ON
cmake --build /tmp/obd-poll-plan-bench
ctest --test-dir /tmp/obd-poll-plan-bench --output-on-failure
/tmp/obd-poll-plan-bench/bench_poll_plan
```

## Bounded command copying on queue insertion

`q_push()` formerly copied commands with `snprintf("%s")`. It now copies at most 19 bytes
into the zeroed step, keeping byte 20 and the remaining tail zero. Whole-step clearing,
kind/delay, `req = -1`, queue head/count, wraparound, full-queue drops and their logs remain
unchanged. No engine fields, tasks, allocations or persistent state are added. The single-request
state machine keeps its command order, send delays and polling deadlines.

`engine_queue_copy` includes real `vp_engine.c` in a private test translation unit and compares
complete engine bytes, returned slots, popped steps and drop logs against independent old
push/pop implementations. It covers all 24 heads, 0..24 occupied slots, eight kinds, empty
strings, 18/19/20-byte boundaries, long strings, non-ASCII bytes, embedded NULs, delay boundaries,
and 256 sequences of 512 fill/drop/pop/wrap operations: 246,272 comparisons pass with
ASAN/UBSAN. Strings are allocated at their exact length, placing an ASAN boundary after NUL.
The independent old source also passes this test. Six successfully compiled negative controls
are rejected: copying 20 bytes, truncating at 18, skipping clearing, `req = 0`, incorrect wrap,
and changing head on a drop. Production code has no test hooks.

A separate driver calls actual production/reference push/pop wrappers and consumes a step
checksum; timing excludes transport, response waits and full-queue logging. Each case first
checks complete state, warms up for 10,000 calls, alternates seven samples of 1,000,000 calls,
and reports medians. No builds or other benchmarks ran concurrently during timing. Environment:
Apple M1 / arm64, Apple clang 21.0.0 Release, no sanitizers/LTO. Raw results for 24 cases are in
the [CSV](performance/queue-copy-host-2026-10-14.csv). Inputs longer than 19 bytes test truncation,
not actual transmitted command lengths. Successful insertion paths are about 4.03..5.08 times
faster in this benchmark, not across the whole device.

| head | length | format_ns | copy_ns | ratio |
|---:|---:|---:|---:|---:|
| 0 | 0 | 45.62 | 9.78 | 4.666 |
| 0 | 4 | 50.86 | 10.02 | 5.075 |
| 0 | 12 | 50.86 | 10.92 | 4.656 |
| 0 | 19 | 52.07 | 12.86 | 4.049 |
| 23 | 20 | 51.74 | 12.83 | 4.032 |
| 23 | 255 | 56.58 | 12.80 | 4.420 |

`ratio = format_ns / copy_ns`. Normal/instrumented/no-slot ESP-IDF 5.5.3 / Xtensa GCC 14.2.0
compilation confirms that `q_push()` no longer calls `snprintf`, retaining `memset` and full-queue
logging. The engine remains 5016 bytes; compiler-reported static frames for `q_push()` and
`vp_engine_run()` remain 48 and 128 bytes, with other emitted static frames unchanged. Target
object `size` text/readonly totals are 12288 → 12285 bytes; this does not establish final-image
savings or maximum task stack depth. [Raw target compilation record](performance/queue-copy-xtensa-2026-10-14.txt).
All three complete firmware builds pass; app sizes are in that record. Device CPU cycles and
effective sampling intervals remain unmeasured; ELM/ECU waits have not been reduced.

```bash
cmake -S test/host/engine -B /tmp/obd-queue-copy-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_QUEUE_BENCHMARK=ON
cmake --build /tmp/obd-queue-copy-bench
ctest --test-dir /tmp/obd-queue-copy-bench --output-on-failure
/tmp/obd-queue-copy-bench/bench_queue_copy
```

## Fixed-format adapter state commands

`push_state()` no longer uses dynamic `snprintf` to build CAN IDs for `ATSH` / `ATCRA`,
`ATCEA` addresses, or `ATCAF` switches. IDs retain the 11-bit mask with three uppercase hex
digits or the 29-bit mask with eight; addresses retain their low eight bits with two digits.
Leading zeros, NUL, off sentinels, the default CAF branch for unknown states, and step
st/val/req metadata remain unchanged. `push_req()` and state encoding share one read-only hex
table. Command order, send gaps, polling deadlines, full-queue logs and the single-request
state machine are unchanged. No engine fields, allocations, tasks or persistent state are
added; a compile-time check protects capacity for the longest command.

Target flags include `-fno-builtin-memcpy`, so short-prefix `memcpy` operations retain real
function calls. Fixed byte stores avoid them and save another 28 object text/readonly bytes
relative to the intermediate version. Constant `snprintf("ATCRA")` / `snprintf("ATCEA")`
remain; all three target configurations confirm they fold into fixed stores. Final
`push_state()` / `fmt_id()` have no `snprintf` or `memcpy` calls.

`engine_state_format` includes the actual engine and compares complete engine bytes, popped
steps and full-queue logs with independent old formatting. Both use the same production
`q_push()`, avoiding double-counting the preceding copy optimization. Coverage includes all
2048 11-bit IDs, 24 heads, empty/nonempty/almost-full/full queues, 29-bit flag/mask boundaries
and leading zeros, all 256 addresses plus the off sentinel, every uint8 state default branch,
int16 req boundaries, and 256 sequences of 512 fill/drop/pop/wrap operations: 178,928
comparisons pass with ASAN/UBSAN. The complete old production source passes the same test.
Eight successfully compiled negative controls are rejected: wrong 11-bit mask, 29-bit flag
removal, both ID widths, lowercase, address off sentinel, CAF switch, and req metadata.
Production source has no test hooks; all 45 tests across eight host suites pass.

A separate driver calls construction/push/pop wrappers and consumes a checksum, excluding
transport, response waits and full-queue logging. Each case validates complete state, warms
up for 10,000 calls, and takes medians of seven samples with 1,000,000 calls each. Compiler
context affects the private old-format reference; that column must not be mistaken for actual
old production timing. `EH_STATE_BASELINE_SOURCE` places the complete old source in a separate
translation unit, renaming public symbols only. Both actual production wrappers alternate
samples in one process on the same engine address. Baseline object `__text` bytes match the
independently compiled baseline; engine/probe layouts must match. This override is only for
this round's host benchmark. Environment: Apple M1 / arm64, Apple clang 21.0.0 Release,
no sanitizers/LTO. This agent ran no concurrent builds or other benchmarks, but background
system load was present; absolute ns and ratios are not device-speed or polling-rate conclusions.
Raw results for 24 cases and a repeat run are in the [CSV](performance/state-format-host-2026-10-14.csv)
and [repeat CSV](performance/state-format-host-repeat-2026-10-14.csv).

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

`baseline_to_production = baseline_ns / production_ns`. Dynamic-argument/CAF paths were about
3.32..4.90 times faster across these two runs. The two constant off paths instead showed a
roughly 5..7 ns desktop slowdown; those results are retained, so not every path can be claimed
faster. Compiler/link layout and system load affect host microbenchmarks; neither ESP32
regression nor gains follow directly. Target constant paths still use fixed stores and the
`push_state()` static frame does not grow. Device A/B measurements must determine overall
benefit; ELM/ECU waits have not shortened.

Normal/instrumented/no-slot ESP-IDF 5.5.3 / Xtensa GCC 14.2.0 configurations retain a 5016-byte
engine. Compiler-reported static frames for `q_push()` / `push_state()` / `vp_engine_run()`
remain 48 / 64 / 128 bytes. A newly emitted standalone `fmt_id()` has a 32-byte static frame
(the old helper was inlined); other emitted frames do not grow. This does not establish maximum
task stack depth. Object text/readonly totals are 12285 → 12349 bytes (+64), not flash savings.
Relevant function bytes match between evidence objects and all three complete-build objects.
ELFs contain real helpers and no test/baseline symbols. App sizes are `0x2857d0` / `0x286030` /
`0x276190`, increases of 32 / 16 / 32 bytes over the preceding round.
The [target/regression record](performance/state-format-xtensa-2026-10-14.txt) includes
baseline/current source SHA256; the [source diff](performance/state-format-source-2026-10-14.diff)
reconstructs this round's baseline from its matching production source.

The ordinary optional benchmark uses an independent reference. To reproduce actual production
A/B, reverse-apply the saved diff to this round's matching source and enable the whole-source
baseline. Source hashes must match the record; leave override fields empty for normal tests:

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

## Commit receive length once per chunk

`vp_engine_on_rx()` accumulates accepted bytes in a local `size_t` and writes `rx_len`
once after scanning the chunk. The old target loop stored and truncated a 16-bit length
for each appended non-NUL byte; the new loop removes both operations and one register
copy, moving truncation/writeback outside the loop. It still scans all input beyond
capacity to preserve later `>` prompts and CR/LF detection. NUL filtering, the 512-byte
capacity, terminators, empty input and one end-of-call monitor consumption remain the same.
No fields, allocations, tasks or concurrent requests are added.

`engine_rx_append` includes the real engine and compares it with an independently saved
old receive implementation. Every call checks complete engine bytes and the events,
values, ordering and clock-call counts from actual monitor decoding. The 219,423 comparisons
cover all 256 byte values, five busy/kind states, both prompt states, five initial lengths,
twelve input lengths, NUL, NULL, non-NULL zero length, prompts beyond capacity, every CAN
stream split, and continuous random streams with timestamp wrap. Explicit assertions check
80°C coolant/90°C oil, valid-frame counts, refusal flags and overflow-consumption timing.
ASAN/UBSAN pass, as does complete old source. Eight successfully compiled negative controls
break NUL filtering, scanning beyond capacity, prompt detection, length writeback, full-buffer
consumption, busy/kind gating and the post-monitor terminator; all are rejected. All 46 tests
across eight host suites pass. The production engine contains no test hooks.

The desktop benchmark links complete old and production sources in separate translation
units with symbol aliases, alternating them in one process on the same engine address.
Both source and probe layouts must match. Each call includes length/prompt reset, the actual
receive function and a result read; full-state copying, monitor decoding, BLE transport and
response waits are excluded. After validation and 10,000 warmups, seven samples of 200,000
calls use the median. Apple M1 / arm64, Apple clang 21.0.0 Release `-O3`, no sanitizer/LTO.
This agent ran no concurrent build or other benchmark, but the system had background load.
Across two runs, 20/128-byte ASCII, mixed and prompt cases improve by 1.328–2.116 times.
Empty/single-byte and all-NUL cases have no guaranteed gain, and long chunks vary. The repeated
1024-byte all-NUL case is 0.874 times, versus 1.032 in the first run. Do not claim every input
gets faster or convert this timing to device throughput or polling frequency. Complete data:
[CSV](performance/rx-append-host-2026-10-14.csv) and
[repeat CSV](performance/rx-append-host-repeat-2026-10-14.csv). Repeat-run excerpt:

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

Across all three ESP32-S3 configurations, RX function text is 174 → 170 bytes and complete
engine object text/readonly is 12349 → 12345 bytes. Compile-time probes show
`sizeof(vp_engine_t)` remains 5016 bytes and the RX static frame remains 32 bytes; these
are not maximum task stack or device high-water measurements. Standalone RX instruction
bytes match the complete firmware objects. ELFs contain no probe/baseline symbols, and
instrumentation symbols appear only in the instrumented configuration. Complete normal,
instrumented and no-slot firmware builds pass: app sizes are `0x2857c0` / `0x286030` /
`0x276190`, respectively -16 / 0 / 0 bytes from the preceding round. Linked app changes
include alignment and cannot be inferred directly from function sizes. Source hashes,
disassembly, negative controls and regressions are in the
[target evidence](performance/rx-append-xtensa-2026-10-14.txt). The
[source diff](performance/rx-append-source-2026-10-14.diff) reconstructs the old baseline.

Normal tests leave `EH_RX_SOURCE` empty; only temporary experiments/negative controls point
to source copies. Timing is off by default. The optional benchmark requires
`EH_RX_BASELINE_SOURCE`; these options do not affect firmware. Reproduction:

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

## Single-precision range checks for RPN integer operations

`to_u32()` / `to_shift()` previously promoted formula-stack `float` values to `double` for
range checks and integer conversion. ESP32-S3 compilation therefore repeatedly referenced
software double helpers. The checks now use single precision: negative values truncate to
`int32_t` before converting to a 32-bit pattern; nonnegative values convert to `uint32_t`.
Finite-value, range and untouched-on-failure rules remain the same. Shifts still accept
negative fractions in `(-1, 0)` and truncate them to zero. Binary32 represents no value
between `-2^31-1` and `-2^31`, so changing the lower check to `< -2^31` preserves the accepted
set of bit patterns. Formula opcodes, profile blobs, polling order and NVS layouts are unchanged.

The default `vehicle_rpn_integer` test compares independent old double rules against
1,001,690 float bit patterns and 610,140 real complete formulas. It covers signed/unsigned
boundaries and neighboring bit patterns, zero/negative zero/subnormals, NaN/Inf, negative
fractions, truncation, invalid shifts and untouched failure sentinels. Large operands are
built from legal constants and exact power-of-two multiplications, checking success and
error results for all six integer opcodes in the evaluator. Optional Release `--exhaustive`
compared all 4,294,967,296 Binary32 bit patterns: both private helpers match acceptance and
output. Including routine samples, this is 4,295,968,986 bit-pattern comparisons. The whole
old source passes the same ASAN/UBSAN test; the optional Release test also compares complete
formulas to the whole old evaluator in a separate translation unit. Seven successfully
compiled negative controls break integer lower/upper bounds, negative truncation, fractional
negative shifts, the shift upper bound, NaN checks and failure sentinels; all are rejected.
This round passes all 47 tests across eight host suites.

The host benchmark alternates two whole evaluators in one process on the same formula and
input addresses, Release `-O3`, sanitizers/LTO off, 10,000 warmup calls and seven samples of
500,000 calls each, taking medians. Twelve formulas cover integer operations, failure paths
and byte/raw-field/RPM controls; outputs and checksums match. Initial/repeat Apple M1 results
mostly show small differences; `sext8` / `sext16` are slightly slower in both runs, so these
results do not establish that every formula is faster. No other local builds/tests overlap
timing, but background system load is uncontrolled. The numbers do not represent ESP32
latency or OBD polling rates. Raw results are in the [initial CSV](performance/rpn-integer-host-2026-10-14.csv)
and [repeat CSV](performance/rpn-integer-host-repeat-2026-10-14.csv); this table uses the repeat:

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

In all three ESP32-S3 configurations, `vp_rpn_eval` text is 1908 → 1502 bytes and complete RPN
object text/readonly is 3736 → 3210 bytes. The evaluator's compile-time static frame is
176 → 96 bytes, not a task maximum or measured device high-water mark. The RPN object no
longer references `__extendsfdf2`, `__fixsfdi`, `__gtdf2`, `__ledf2` or `__unorddf2`; division/raw
fields still use `__divsf3` / `__floatdisf`. This does not mean the entire firmware stops using
double precision. Standalone evaluator instruction and literal bytes match the full-build
object. ELFs contain no integer probe/baseline symbols, and instrumentation appears only in
the statistics configuration. Normal/statistics/no-slot full builds pass with app sizes
`0x285620` / `0x285e80` / `0x275fe0`, respectively 416 / 432 / 432 bytes smaller than the previous
round. Object sizes, disassembly, hashes and regressions are in the
[target evidence](performance/rpn-integer-xtensa-2026-10-14.txt); the old baseline can be rebuilt
from the [source diff](performance/rpn-integer-source-2026-10-14.diff).

Normal tests leave `VH_INTEGER_SOURCE` empty; this override is only for temporary private
probe experiments/negative controls. `VH_INTEGER_BENCHMARK=ON` requires the complete old
source via `VH_INTEGER_BASELINE_SOURCE`. Exhaustive checks/timing are off by default; these
options do not affect firmware. Reproduction commands:

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

## 32-bit floating-point conversion of raw fields

`PUSH_RAW` extraction returns `int64_t`, but fields contain at most 32 bits. Signed results
already fit `int32_t`; unsigned results fit `uint32_t`. The evaluator now selects these
narrow conversions using `VP_RAW_SIGNED`, keeping the same extractor, field checks and error
returns. ESP32-S3 replaces the old `__floatdisf` software call with `float.s` / `ufloat.s`
instructions. Opcodes, profile blobs, NVS and request scheduling are unchanged.

`vehicle_rpn_raw_float` reuses the private probe of the actual RPN source, comparing independent
per-bit extraction and the old `int64_t`-to-`float` rule in 4,274,988 complete formula comparisons
by default, including 1,319,950 valid field cases. It covers widths 0..33, LE/BE, signed/unsigned,
unknown flag bits, unaligned fields, exact payload ends and one-byte-short payloads, fields near
`UINT16_MAX`, null pointers, integer rounding and half-ULP neighborhoods, and real multiplication/
subtraction postprocessing. Short payload allocations match the declared size with a one-byte
start offset so ASAN can catch out-of-bounds reads. Error codes and untouched failure sentinels
are also compared. The whole old source passes the same ASAN/UBSAN test.

Optional Release `--exhaustive` covered all 4,294,967,296 raw 32-bit values, running signed and
unsigned complete formulas. Float bit patterns match both the independent reference and the
whole old evaluator in a separate translation unit. Including routine samples, this is
8,594,209,580 formula comparisons. Seven successfully compiled negative controls break sign
selection, force signed/unsigned conversion, narrow to 16 bits, change flag checks, offset results
and clobber failure sentinels; all are rejected. This round passes all 48 tests across eight host
suites; optional paired Release tests pass 14/14.

Host timing uses the same process and formula/input addresses, Release `-O3`, sanitizers/LTO
off, 10,000 warmup calls and seven alternating samples of 500,000 calls, taking medians. Sixteen
formulas cover widths, byte order, signedness, unaligned/error paths, temperature conversion and
byte/RPM controls; outputs and checksums match. In both Apple M1 runs, single raw-field formulas
mostly regress: repeat ratios are 0.956–0.969, with the temperature formula at 1.016. This host
has hardware 64-bit conversion, so its results do not establish the cost of ESP32 software calls.
Adoption is based on removing `__floatdisf` from the target path and using hardware conversion
instructions. No other local builds/tests overlap timing, but background system load is
uncontrolled. ESP32 latency and polling-rate improvements remain unmeasured. Raw results are in
the [initial CSV](performance/rpn-raw-float-host-2026-10-14.csv) and
[repeat CSV](performance/rpn-raw-float-host-repeat-2026-10-14.csv); this table uses the repeat:

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

In all three ESP32-S3 configurations, `vp_rpn_eval` text is 1502 → 1534 bytes and complete RPN
object text/readonly is 3210 → 3238 bytes. The static frame remains 96 bytes. This change adds
local branches/instructions to remove a software conversion call per field; it does not mean
the entire firmware stops referencing `__floatdisf`. Standalone function instruction/literal
bytes match the full-build object. ELFs contain no probe/baseline symbols, and instrumentation
appears only in the statistics configuration. Normal/statistics/no-slot full builds pass with
app sizes `0x285640` / `0x285ea0` / `0x276000`, each 32 bytes larger than the previous round.
Static frames are not task maxima or measured device high-water marks. Target objects,
disassembly, hashes, exhaustive results and negative controls are in the
[target evidence](performance/rpn-raw-float-xtensa-2026-10-14.txt); the old baseline can be rebuilt
from the [source diff](performance/rpn-raw-float-source-2026-10-14.diff).

The test reuses the temporary `VH_INTEGER_SOURCE` probe override; leave it empty in normal CI.
`VH_INTEGER_BENCHMARK=ON` also builds optional `bench_rpn_raw_float` and requires complete old
source via `VH_INTEGER_BASELINE_SOURCE`. It shares the integer benchmark driver, selecting cases
by a compile definition. Exhaustive checks/timing are off by default; these options do not affect
firmware. Reproduction commands:

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

## Identity-factor writeback fast path

Engine creation uses one `uint64_t` mask to record signals with unit factor `1.0`, allowing
common standard signals to bypass double unit conversion. The initial `floorf(v + 0.5f)`
implementation passed golden samples but had uncovered rounding boundaries; the exact hardware
conversion in the next section replaces it. Other factors retain the double expression.
The mask preserves profile blob/NVS layouts; actual CPU/poll gains still need device measurement.

## Exact hardware rounding for identity writeback

The performance audit found early rounding in the former `floorf(v + 0.5f)` fast path:
input `0x3effffff` (immediately below `0.5`) produces `1`, while the original
`floor((double)v + 0.5)` produces `0`; `8388609.0f` also carries incorrectly.
Routine vehicle samples and golden request tests did not cover these boundaries.

The path now checks the signed 32-bit conversion range, truncates to an integer, and computes
the difference between the original value and that integer. Positive fractions `>= 0.5f`
increment; negative fractions `< -0.5f` decrement; negative half ties retain the truncated value.
Fractional Binary32 values are too small for the final increment/decrement to overflow, and
large integers convert back to float exactly. Nonfinite and out-of-range inputs are rejected
before conversion. The creation-time identity mask, double expression for other factors,
post/valid/registry/filter/writeback order and invalidation callbacks are preserved.
No RAM fields or profile/NVS layouts change.

The default `engine_write_rounding` compiles the actual production engine into a private probe,
checking 1,720,944 rounding inputs and 33,280 complete engine-state/callback comparisons.
Coverage includes half ties and neighbors, large integers, subnormals, NaN/Inf, conversion
boundaries, failure sentinels, low/high mask bits, standard/custom slots, 5 unit factors,
speed scaling, post/valid rules and both filters. Reference writeback uses the original double rules.
Optional Release `--exhaustive` checks the rounding helper for all 4,294,967,296 Binary32 bit
patterns, totaling 4,296,688,240 with routine samples. Complete pipeline replay remains sampled,
not exhaustive. All 8 negative controls compile successfully and are rejected: the old fast path,
positive/negative half boundaries, early carry, corrupted failure sentinel, and production-pipeline-only
mutations restoring old rounding, omitting the unit factor or changing valid-range checks.

Eight ASAN/UBSAN suites pass all 49 tests; a separate Release engine run passes 6/6.
Apple M1 / arm64 / Apple clang 21, Release `-O3`, sanitizers/LTO off: identical input addresses,
indirect calls in one process, 10,000 warmup calls, medians of 7 alternating samples of
1,000,000 calls; outputs and checksums agree. No other local builds/tests overlap timing;
background OS load is uncontrolled. Both the
[first run](performance/write-rounding-host-2026-10-14.csv) and
[repeat](performance/write-rounding-host-repeat-2026-10-14.csv) show finite host cases regressing.
Repeat results, with `double_to_production = double_ns / production_ns`:

| case | double_ns | production_ns | double_to_production |
| --- | --- | --- | --- |
| rpm_integer | 1.25 | 1.57 | 0.798 |
| temperature_fraction | 1.25 | 1.57 | 0.797 |
| half_neighbors | 1.25 | 1.57 | 0.799 |
| large_identity | 1.25 | 1.56 | 0.802 |
| mixed_patterns | 1.63 | 1.62 | 1.003 |
| invalid | 1.25 | 1.25 | 0.999 |

Finite host cases take about 25% longer in the repeat run; random/invalid cases are close. M1 double `floor` is a
hardware instruction. This benchmark measures only rounding, excluding unit selection, filtering,
storage, BLE and polling waits; it cannot prove ESP32 timing gains.

Actual ESP32-S3 compile commands confirm `trunc.s` / `float.s` in the identity path, without
`floorf` or double-helper calls. The general path in `sample_ok` still uses double helpers,
and EMA still calls `floorf`; the entire engine has not removed them. In all three configurations,
engine object text/readonly is `12345 → 12350`; inlined `sample_ok` text is `1058 → 1066`,
with its static frame unchanged at `64` bytes. Static frames are not task stack maxima.
Standalone and full-firmware instruction/literal bytes match exactly; ELFs contain no test-probe
symbols. Normal/statistics/no-slot full builds pass with app sizes
`0x285650` / `0x285eb0` / `0x276010`, each adding `16` bytes.

[Target evidence](performance/write-rounding-xtensa-2026-10-14.txt) records disassembly,
source/image hashes, exhaustive and negative-control results;
the [source patch](performance/write-rounding-source-2026-10-14.diff) reconstructs the old fast path.
Adoption fixes boundary behavior and removes library calls from the target identity path;
device CPU/poll timing remains unmeasured.

```bash
cmake -S test/host/engine -B /tmp/obd-write-rounding-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_WRITE_BENCHMARK=ON
cmake --build /tmp/obd-write-rounding-bench
ctest --test-dir /tmp/obd-write-rounding-bench --output-on-failure
/tmp/obd-write-rounding-bench/test_write_rounding --exhaustive
/tmp/obd-write-rounding-bench/bench_write_rounding
```

## Rejected CAN index experiment

`vp_engine.c` scans at most 16 CAN rules per frame. A host experiment uses a stable sorted index
and binary search, preserving original order for equal CAN ids; below 5 rules it uses linear
scanning. The index has not been integrated into firmware.

```bash
cmake -S test/host/engine -B /tmp/obd-can-index-review -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_CAN_BENCHMARK=ON
cmake --build /tmp/obd-can-index-review --target bench_can_index
/tmp/obd-can-index-review/bench_can_index
```

Apple M1 / arm64, Apple clang 21.0.0 Release, no LTO. Both selection functions live in a translation
unit separate from timing loops, preventing known rule counts from folding one path away.
0/1/2/4/8/16 rules × unique/paired/same ids × all miss/25% hit/all hit yields 54 scenarios.
Each uses 256 fixed queries, verifies order-sensitive checksums, warms up 20,000 calls, then
alternates 7 samples of 500,000 calls and takes the median. The complete
[CSV](performance/can-index-host-2026-10-14.csv) includes zero-rule and low-count controls.
These all-hit scenarios enter the indexed path:

| rules | pattern | stream | linear_ns | indexed_ns | ratio |
| --- | --- | --- | --- | --- | --- |
| 8 | unique | hit | 4.10 | 7.06 | 0.580 |
| 8 | paired | hit | 3.67 | 8.24 | 0.446 |
| 8 | same | hit | 3.76 | 17.74 | 0.212 |
| 16 | unique | hit | 5.40 | 9.00 | 0.600 |
| 16 | paired | hit | 5.31 | 11.72 | 0.453 |
| 16 | same | hit | 7.56 | 33.00 | 0.229 |

`ratio = linear_ns / indexed_ns`; the index is slower in these scenarios, so linear scanning
remains in production. Timing covers rule selection and order-sensitive checksums, excluding
serial parsing, RPN, callbacks and RTOS. It does not prove the same regression on ESP32,
and the experiment is not counted as an adopted optimization.

## No storage background task without an SD slot

When `CONFIG_OBD_SD_SLOT_SDMMC_1BIT` is disabled, `sd_card_is_mounted()` always returns false.
The former `storage` task polled once a second but could never execute card jobs. The worker,
private task handle and creation path are now compiled out according to the **slot configuration**,
not hardcoded V1/V2/V3 choices or temporary card absence. Without a slot, `storage_service_start()`
returns `ESP_OK` idempotently. With a slot it keeps the existing task, initial 5-second delay,
insertion/OTA-stop reruns and retries 10 seconds after busy jobs finish.

`storage_service_boot()` and `storage_service_busy()` remain outside the conditional compilation:
config mount/first-boot migration, interrupted-theme recovery, the mounted check for boot import
scanning, legacy-vehicle retry and UI busy reporting remain available. This does not remove storage
bring-up or change Flash configuration, HTTP uploads or NVS persistence layouts.

Target compilation with the current ESP-IDF 5.5.3 ESP32-S3 configurations confirms
`sizeof(StackType_t) = 1`, `sizeof(StaticTask_t) = 340` (the SDK asserts equality with the real TCB)
and `sizeof(TaskHandle_t) = 4`. Without a slot this removes `8192 + 340 = 8532` bytes of dynamic
task-allocation requests, excluding allocator overhead, plus the `4`-byte static handle. No task or
synchronization object is added. The former once-per-second periodic wake also disappears. This is
source/target-ABI accounting, not measured boot free heap, CPU utilization or power consumption.

The new `storage` host suite compiles real `storage_service.c` with project API headers; only SDK /
RTOS and storage-operation boundaries are stubbed. Its original 15 slot and 7 no-slot scenarios run in separate
child processes, without resetting production boot-lifetime state. Virtual time covers recovery
ordering after boot errors, idempotency/creation-failure retry, prolonged card absence,
insertion/removal/reinsertion, OTA pause/stop reruns, insertion during OTA, per-job busy retries,
retry deadlines after expensive jobs finish, non-busy errors, deadlines beyond 32-bit millisecond
rollover and UI busy reporting.

```bash
cmake -S test/host/storage -B /tmp/obd-perf-storage-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/obd-perf-storage-host
ctest --test-dir /tmp/obd-perf-storage-host --output-on-failure
```

Both Release + ASAN/UBSAN tests pass, and CI now runs the same dual-configuration suite. Five temporary
negative controls are rejected: unconditional creation restored, boot skipped without a slot,
startup skipped for an empty slot, OTA ignored and busy retries dropped. Target-object comparisons
confirm identical worker/start/boot/busy instruction bytes with a slot, and identical boot/busy
bytes without a slot. The no-slot worker, private handle and task-create/delay references disappear.
The final no-slot ELF has no `service_task` and retains all three public storage APIs.

Complete slot-normal, slot-instrumented and no-slot-normal firmware builds pass with image sizes
`0x285720`, `0x285f60` and `0x2760e0`, respectively; both slot sizes are unchanged this round. The
no-slot build retains the V1 hardware selection and changes only the SD slot option, checking that
this optimization does not depend on board numbering. Cross-configuration image sizes do not measure
this change's isolated size benefit. Builds, SDKCONFIG files and negative controls remain under
`/tmp`, without touching repository `build/` or release artifacts. SD hotplug/file import, task
scheduling, long-run stability and heap headroom remain unmeasured and need device-matrix validation.

## SD state-driven storage waits

`sd_card_poll_wait_ms()` returns the remaining time until the next useful probe for the current card
state: 1 second when mounted, 3 seconds when absent or ejected, and 10 seconds when a card is present
but unusable. After each probe, the storage worker sleeps for this duration; if jobs are pending, it
uses the earlier of the retry deadline and SD probe deadline. Mounted operation remains capped at
1 second to preserve CMD13 checks and OTA pause/resume detection. Actual probes still follow the
existing 3-second absent/ejected and 10-second unusable-card deadlines. Removing intervening empty
polls reduces steady-state worker wakeups with an empty slot by roughly two thirds.

The storage host suite has 25 service scenarios covering 3-second absent and 10-second error waits,
the 1-second mounted cap, minimum tick waits, and existing insertion/OTA/retry behavior. New tests
execute real `sd_card.c` in slot/no-slot configurations, covering RTOS tick rollover, no probes before
deadlines, two failures before removal, eject/reinsert behavior, and remaining deadlines after costly
probes. All 4 Release + ASAN/UBSAN tests pass. This verifies policy with virtual time, not real SD bus
response, CPU use or power changes; physical hotplug testing is still required.

## Event-driven video condition waits

`video_player.c` reuses the session's two existing wake bits and uses `portMAX_DELAY` for
conditions without deadlines. No task, lock, timer or event object is added. Producers update
conditions before notifying, with one consumer per wake bit; notifications latch across the
predicate-to-wait gap. Worker session references retain the event group until exit.
`session_halt()` sets `quit` before waking both reader and decoder.

| Wait condition | Previous timeout | Event producers |
|---|---|---|
| Reader ring full | 20 ms | ring release, stop |
| Decoder header pending | 100 ms | header publication, reader completion/error, stop |
| Decoder paused | 100 ms | resume, reader/UI notifications, stop |
| Decoder watermark pending | 20 ms | ring commit/full, EOF/error, stop |
| Composite UI slot pending | 20 ms | UI handoff, pause/resume, stop |

A reader with a full ring still notifies the decoder first: fullness permits consumption even
below the frame watermark, preventing a reader/decoder circular wait. Previously paused decoding
had a 100 ms timeout, but a full-ring reader also notified it every 20 ms. Both paths must change
to remove this periodic wake chain. Only timeout polling disappears, not useful notifications;
other producers sharing the decoder bit may still wake it to recheck conditions.

**Deadline waits stay timed**: `wait_until()` retains microsecond-to-millisecond flooring and
`pdMS_TO_TICKS()`, with pause/stop able to interrupt it. The `fs_wait_idle()` panel timeout,
stop's `EV_DECODER_DONE` waits and DMA-buffer lifetime remain unchanged. The COMPOSITE 10 ms
LVGL handoff timer and `video_elem` 100 ms owner timer remain, so the entire video path cannot
be described as free of periodic wakeups. Reader retention/session teardown after decoder
errors is also unchanged.

`media_video_player_wait` compiles the real production `video_player.c`, substituting the
SDK/RTOS/LVGL/JPEG/LCD boundaries while using the real OBV1 parser, temporary containers and
virtual 1 kHz ticks. Its 28 subprocess-isolated scenarios cover full-ring backpressure,
pre-wait notifications, resume after a 10-second pause, isolated header publication/error/stop,
watermark/EOF/fullness, UI handoff/pause/stop, frame deadlines and interruption, fullscreen
finish/stop and reader creation failure. Frame markers check ordering; allocation tracking
checks final releases and borrowed-strip ownership. Existing concurrent SPSC head tests remain.
These tests do not model ESP32 scheduling, real JPEG, DMA or SD throughput.

```bash
cmake -S test/host/media -B /tmp/obd-perf-video-waits-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/obd-perf-video-waits-host
ctest --test-dir /tmp/obd-perf-video-waits-host --output-on-failure
```

All 6 media tests pass with Release + ASAN/UBSAN. Ten temporary negative controls compile
successfully and are then rejected: old polling, omitted resume/ring-commit/ring-release/UI/header/reader-
completion/stop/full-ring notifications, and an indefinite frame-deadline wait.
`MH_PLAYER_SOURCE` points them at `/tmp` copies without mutating production files.

Objects and final ELFs were checked in all three target configurations. Disassembly confirms
infinite ticks for condition waits, calculated finite ticks for frame deadlines, and retained
panel/stop timeouts. `sizeof(vp_session_t)` remains `264`, private writable storage is unchanged,
and no compiler static stack frame grows (reader `192`, decoder `256` bytes). Task stacks remain
`4096` / `6144` bytes, with allocation paths unchanged. This is not a runtime stack-watermark
or actual free-heap measurement.

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285720` | `0x285710` | `-16` |
| SD slot, statistics | `0x285f60` | `0x285f60` | `0` |
| No SD slot, normal | `0x2760e0` | `0x2760d0` | `-16` |

All three full firmware builds pass. Sizes compare before/after images of the same configuration
and record footprint only, not speed. Builds, SDKCONFIGs, baselines and negative controls stay in
`/tmp`, without touching repository build artifacts. Device CPU/power, paused-task wakeups, FPS,
SD underrun recovery and long-term stability remain unmeasured; follow the device acceptance matrix.

## Pause the composite-video LVGL handoff timer

> Historical intermediate baseline; the next section replaces this handoff with LVGL task notifications.

Pausing a COMPOSITE session also calls `lv_timer_pause()` on its existing 10 ms handoff timer;
resume calls `lv_timer_resume()` before notifying the decoder. The timer is not deleted/recreated,
and no allocations, tasks or synchronization objects are added. All player controls run in the
LVGL task or while holding `lvgl_mux`; workers do not call LVGL.

Frames published before pause, or racing with pause, remain in the session's `pending` slot.
Presentation freezes while paused; resume hands the frame to the view through the original path
and notifies the decoder. Pause during decode still retains the ring record for retry after
resume. Stop while paused still deletes the timer and releases buffers through the existing
reference lifetime. FULLSCREEN has no handoff timer; its display refresh must remain paused until
stop, rather than being resumed by a playback resume.

The current LVGL 8 `lv_timer.c` skips paused callbacks and excludes paused timers from its next
wake-deadline calculation. This removes the nominal 100 Hz empty handoff callback/deadline while
paused, not a measured number of task wakeups. LVGL still traverses the timer node, and other
UI/touch/theme-owner timers still run. In particular, `video_elem` keeps its 100 ms owner check,
including error handling while paused. The playing 10 ms handoff period, frame pacing, panel
timeouts and single-session model remain unchanged.

`media_video_player_wait` expands from 28 to 36 scenarios. New cases cover pause during loading,
existing pending frames, the pause/publication gap, control interleavings during actual decode
and publication, stop while paused, FULLSCREEN controls and invalid controls. Scripts provide
1000 virtual timer-handler opportunities, checking zero paused callbacks/presentations/handoff
notifications, resumed frame markers and final resource releases. This is not a real LVGL/ESP32
throughput simulation; production has no test hooks. Release + ASAN/UBSAN passes media 6/6 and
all 41 tests across 8 suites. The original 10 wait negative controls and 5 new timer controls all
compile before being rejected: missing pause/resume, discarded pending frames, incorrectly
resumed fullscreen refresh and an unguarded null timer operation.

All three ESP32-S3 full builds pass. Final ELFs confirm guarded session handoff-timer operations
and retained decoder notifications in pause/resume. Target object instruction bytes for
`ui_timer_cb`, `session_halt`, ring release and panel wait/draw remain unchanged.
`sizeof(vp_session_t)` remains `264`; private writable storage and allocation paths are unchanged,
and no compiler static stack frame grows (reader `192`, decoder `256` bytes).

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285710` | `0x285730` | `32` |
| SD slot, statistics | `0x285f60` | `0x285f80` | `32` |
| No SD slot, normal | `0x2760d0` | `0x2760f0` | `32` |

Same-configuration sizes record footprint, not speed. Builds/baselines/negative controls stay in
`/tmp`; repository artifacts are untouched. CPU, power, actual LVGL wakeups/resume latency and
long-term stability still require device measurement; this cannot establish a whole-device limit.

## Composite handoff through LVGL task notifications

Replacing the previous section's intermediate design, COMPOSITE no longer creates a per-session
10 ms handoff timer. Publishing `pending` only notifies the boot-lifetime LVGL task. With
`lvgl_mux` held, that task runs `video_player_ui_step()` before `lv_timer_handler()`. Workers
never call LVGL; notifications carry no session/view pointer. Front/pending transfer remains
under the session lock, followed by a decoder notification. The loop still checks for a current
session when no video is playing; this is not a zero-overhead LVGL loop.

Default direct-notification slot 0 belongs exclusively to this wakeup. The task handle is
published with atomic release/acquire, bound once and never deleted. `eSetBits` coalesces repeated
wakes; no entry clear and an exit clear preserve notifications in the predicate/wait gap. The
first loop unconditionally checks the current session, including a frame published before task
registration. Events may return earlier than the original 2..500 ms wait deadline; without an
event, ordinary timers retain their deadlines and the same bounds. No new task/semaphore or
runtime allocation is introduced.

Pause retains pending without presenting it; resume notifies LVGL and the decoder. After stop
or replacement, a retired worker's late notification only wakes LVGL, which checks the current
session and never retains the old session/view. Fullscreen refresh, DMA wait/draw, frame deadlines
and the 100 ms `video_elem` owner check remain unchanged. This removes the nominal 100 Hz
handoff timer's callback/deadline/node, not other LVGL/touch/UI timers. A frame event can add an
early loop iteration; this does not prove fewer measured task wakeups overall.

The real player and real `lvgl_runtime.c` execute 47 isolated host scenarios in both normal and
statistics configurations. New cases cover pre-registration publication, gap/blocked publication,
coalescing, paused late frames/resume, late notifications after stop, session replacement,
fullscreen guarding and ordinary timer deadlines. All original 36 reader/decoder/control cases,
allocation tracking, the real OBV1/parser and the concurrent SPSC test remain. Statistics tests
also check one mutex-wait/handler sample per loop. RTOS/LVGL/JPEG/LCD boundaries are stubs, not
ESP32 scheduling or throughput simulation. All 27 negative controls compile before being rejected.
The 8 host suites pass 42/42 with Release + ASAN/UBSAN, and optional real-LVGL theme tests pass
7/7. Production has no test hooks.

All three ESP32-S3 full builds pass. Final ELFs verify task/mutex/handoff/handler/notification
integration and absence of per-session timer allocation. Ring-release and panel wait/draw target
instruction bytes are unchanged. `sizeof(vp_session_t)` stays `264` (alignment absorbs the removed
field); one LVGL timer allocation is removed. The unused task-handle output slot is replaced by
the atomic handle, keeping relevant private writable storage unchanged. Compiler static frames
remain `192`/`256` bytes for reader/decoder. Normal LVGL task: `32` → `32`; statistics: `32` → `48`;
new ui-step/wake: `32` each. Task-stack budgets remain unchanged; these are not measured peak
stack depths. `lvgl_handler` now explicitly covers UI handoff step + timer handler; a metric-boundary
change must not be presented as speedup.

| Configuration | Before app bytes | After app bytes | Delta bytes |
|---|---|---|---|
| SD slot, normal | `0x285730` | `0x2857b0` | `128` |
| SD slot, statistics | `0x285f80` | `0x286020` | `160` |
| No SD slot, normal | `0x2760f0` | `0x276180` | `144` |

Same-configuration sizes record footprint, not speed. Builds/baselines/negative controls remain in
`/tmp`; repository artifacts are untouched. Actual CPU/power, LVGL wakeups, handoff/display latency,
video dropped/underruns and long-term stability still require same-configuration on-device A/B
measurement. This cannot establish a whole-device performance limit.

## On-device performance statistics

Enable `CONFIG_OBD_PERF_STATS` under menuconfig → OBD DSP Configuration; it defaults off.
Copy the existing configuration for the correct board into a temporary path before building
to preserve the normal local configuration:

```bash
cp sdkconfig /tmp/obd-perf.sdkconfig
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig menuconfig
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig reconfigure build
idf.py -B /tmp/obd-perf -D SDKCONFIG=/tmp/obd-perf.sdkconfig -p PORT flash monitor
```

The option creates a low-priority logger with a 4096-byte stack, reporting about every 5 seconds.
Each sample only measures time and aggregates into fixed storage. Formatting, heap queries, and
UART output happen outside the statistics lock. Disabled timing and wrapper functions are not
compiled; the normal firmware ELF was checked for their absence. Enabled diagnostics add timing,
critical-section, memory, and logging costs. Validate final speed with external measurements after
disabling diagnostics.

| Metric | Scope | Units |
|---|---|---|
| `lvgl_wait` | LVGL mutex wait | calls |
| `lvgl_handler` | UI handoff step + `lv_timer_handler()` | calls |
| `lcd_submit` | LVGL `esp_lcd_panel_draw_bitmap()` submission | pixels |
| `lcd_wait` | LVGL wait_cb wall time waiting for LCD DMA completion | calls |
| `ui_tick` | whole `my_timerMain()` call | calls |
| `theme_tick` | whole `theme_tick()` call | calls |
| `theme_nav_begin` | navigation layout/rules snapshot on LVGL thread | calls |
| `theme_nav_images` | worker image reads/conversion/cache planning | calls |
| `theme_nav_publish` | LVGL finish, cache adoption and widget creation | calls |
| `elm_run` | RX drain + `vp_engine_run()` | calls |
| `sd_read` | `sd_card_read()`, including bounce-buffer wait/copy | bytes |
| `sd_write` | `sd_card_write()`, including bounce-buffer wait/copy | bytes |
| `video_fullscreen` | `fs_present()`, including transfer waits | frame attempts |
| `video_composite` | `comp_decode()` | frame attempts |
| `rc_snapshot` | due-channel `signal_store_get_many()` | input slots |

Output includes `n`, `total_us`, `min_us`, integer `avg_us`, `p95_le_us`, `max_us`, and `units`.
`p95_le_us` is a power-of-two histogram upper bound, not an exact P95. Durations above
1,048,576 us enter the `UINT32_MAX` upper-bound bucket. Metrics with no samples are omitted;
a missing metric cannot be interpreted as zero cost.

The following demonstrates the format and is not a device measurement:

```text
I (...) perf: window_us=5000000 metric=rc_snapshot n=50 total_us=300 min_us=4 avg_us=6 p95_le_us=8 max_us=8 units=400
```

Heap output includes current internal/PSRAM free space, minimum free space since boot, largest
contiguous free blocks, and logger stack headroom. `ui_tick` and `theme_tick` are nested within
`lvgl_handler`; summing durations does not yield CPU utilization. `lcd_submit` excludes asynchronous
DMA completion; `elm_run` does not measure ECU round-trip time; video counts include processing
attempts and are not screen FPS. SD metrics only cover calls through `sd_card_read/write()`.

## Verification and remaining measurements

Host validation covers 8 suites. `theme` adds optional real-LVGL page comparisons. `media` adds portable CRC comparisons, optional SDK compatibility tests, an SPSC ring-header test, and 47 real-player/LVGL-task control/event-wait scenarios in each of the normal/statistics configurations. `rules` adds rule output-cache comparison tests. `espnow` adds RaceChrono filter/deadline-state and deterministic actual-worker/GATT/hub tests, plus a 17-scenario real ESP-NOW worker/control/OTA replay;
`signal_store` adds histogram boundaries, percentiles, 64-bit totals, count saturation, delivered-unit
conversion, and sparse OBD snapshot comparisons. Vehicle tests continue covering legacy request
sequences and default OBD2 inheritance. `engine` adds 100,352 actual planning-state/slot comparisons
and a local-deduplication benchmark; `engine_queue_copy` adds 246,272 complete-state/step/log comparisons and an optional command-copy benchmark. `engine_state_format` adds 178,928 state-command comparisons and optional whole-source A/B timing. `storage` adds 25 real-service scenarios in slot/no-slot
configurations, plus actual SD state-machine deadline tests in both configurations; `engine_rx_append` adds 219,423 complete RX-state/real-monitor-callback comparisons and whole-source A/B timing. `vehicle_rpn_integer` adds 1,001,690 conversion patterns and 610,140 complete formulas, with optional exhaustive Binary32 checks and whole-source A/B timing. `vehicle_rpn_raw_float` adds 4,274,988 complete formula comparisons with independent extraction/old-conversion rules, optional full-range raw32 checks and whole-source A/B timing. `engine_write_rounding` adds 1,720,944 identity-rounding inputs and 33,280 complete writeback state/callback comparisons against original double rules, plus optional Binary32 exhaustion and a rounding-only benchmark. The earlier run passed all 49 tests across 8 host suites; this SD-wait change separately passes its 4 storage/SD tests.
Complete slot-normal, slot-instrumented and no-slot-normal ESP32-S3 builds pass with ESP-IDF 5.5.3. Build
directories and SDKCONFIG files are under `/tmp`, without writing repository `build/` or release
artifacts. All three ELFs contain the sparse-snapshot entry point. The normal ELF has no
`ui_timer_main_run`, `perf_stats_record`, or `perf_stats_start`; the instrumented ELF contains these
symbols. These checks establish compatibility and buildability, not device performance.

Device acceptance must cover static settings, RPM animation, complex theme rules, fullscreen/composite
video, standard/special vehicle polling, all-channel/single-channel RaceChrono subscriptions,
three linked gauges, and SD import/OTA. Capture baseline and modified results with identical hardware,
assets, vehicle/adapter, and settings. Compare duration distributions, valid-data update intervals,
screen FPS, video dropped/underruns, wireless delivery, and long-run heap/stack headroom.

No connected ESP32 gauge and adapter have been obtained. Device statistics, frame rate,
wireless/SD throughput, and long-run stability remain incomplete. Those measurements determine
further optimization; green host tests cannot establish completion of the project-wide goal.

## Remove channel floorf calls from theme pointer resampling

`theme_page.c` calls `tpx_resample()` when acquiring an uncached pointer sprite. It uses
area coverage for shrinking and bilinear expansion, then writes premultiplied weighted
accumulation as RGBA. Each scaled pixel converts up to four channels. Same-size RGBA
still copies directly; same-size RGB565 still expands directly without this quantizer.

`px_u8()` already excludes NaN, nonpositive values and inputs `>= 255` before conversion.
The remaining float `v + 0.5f` is nonnegative and below `255.5f`, so a direct `uint8_t`
conversion is equivalent to the original conversion after `floorf`. The original float
addition rounding remains: for example, `0x3effffff` plus `0.5f` rounds to `1.0f`.
Changing the addition to double would change boundary pixels. Sampling weights,
accumulation/division order, transparent pixels, dimension limits and allocation/free
policy remain compatible, with no additional RAM fields.

`theme_resample` includes the real private conversion and freezes the original full
resample loop using `floorf`. Both share unchanged axis-weight and pixel-fetch helpers;
this comparison verifies the conversion change, not the independent correctness of the
whole sampling algorithm. That round’s default test checks 295,186 Binary32 inputs and 10,448
image cases, covering 10,978,368 output bytes against the private real path, old loop
and public production `tpx_resample()`. Cases include expansion/shrinking/mixed/same
sizes, single-pixel and maximum-dimension axes, RGBA/RGB565 and fully transparent/
opaque/low-alpha/random pixels. All nine allocation failure points, same-size allocation
failure and invalid arguments also match without leaks. Release `--exhaustive` checks
all 4,294,967,296 Binary32 bit patterns, totaling 4,295,262,482 including routine inputs;
image comparisons remain a finite set. Four negative controls first compile, then fail
the tests: double addition, omitted half, and red-channel/alpha changes only in the real
image path. ASAN/UBSAN theme 7/7 and Release theme 7/7 pass; probes stay out of firmware.

The complete-image benchmark includes axis planning, allocation, sampling, quantization
and freeing. It excludes PSRAM, Flash/cache, LVGL page acquisition, BLE, screen refresh
and system scheduling. Apple M1 / arm64 / Apple clang 21, Release `-O3`, no sanitizer/LTO:
first compare bytes and checksums, warm each path four times, then alternate seven groups
and take the median. Each group processes at least 4,000,000 output pixels. No other local
builds/tests run during timing; background system load is uncontrolled. Two runs of 16
cases are in the [first run](performance/pixel-rounding-host-2026-10-14.csv) and
[repeat](performance/pixel-rounding-host-repeat-2026-10-14.csv). Fourteen host cases are close to parity; two RGBA cases regress in both runs.
The repeat results below use `ratio = actual_ns / reference_ns`:

| format | sw | sh | dw | dh | reference_ns | actual_ns | ratio |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rgba8888 | 512 | 128 | 128 | 512 | 923209.7 | 957241.9 | 1.036863 |
| rgba8888 | 127 | 129 | 63 | 65 | 64854.7 | 72754.4 | 1.121806 |

These host cases take roughly 4% / 12% longer. Host timing does not support a speedup claim;
target timing still needs device measurements.

The full ESP32-S3 build removes four `floorf` call sites from `tpx_resample`, using
`utrunc.s`. Text changes `1353 → 1201`, literals `68 → 52`, saving 168 bytes together.
The static frame remains 144 bytes, not a runtime task-stack peak. Every text/literal
section from old/new standalone compilation matches the corresponding full firmware
object byte for byte; the ELF contains no probe symbols. Normal/stats/no-SD-slot builds pass:

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x285650 | 0x2855b0 | -160 |
| stats | 0x285eb0 | 0x285e10 | -160 |
| no-slot | 0x276010 | 0x275f60 | -176 |

[Target evidence](performance/pixel-rounding-xtensa-2026-10-14.txt) preserves disassembly,
source/image hashes, exhaustive and negative-control results. The
[source patch](performance/pixel-rounding-source-2026-10-14.diff) reconstructs the old
path. Adoption rests on byte-compatible images and fewer target library calls/code bytes;
initial artwork load time, CPU, PSRAM bandwidth and page FPS still need device measurements.

```bash
cmake -S test/host/theme -B /tmp/obd-pixel-rounding-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_RESAMPLE_BENCHMARK=ON
cmake --build /tmp/obd-pixel-rounding-bench
ctest --test-dir /tmp/obd-pixel-rounding-bench --output-on-failure
/tmp/obd-pixel-rounding-bench/test_resample --exhaustive
/tmp/obd-pixel-rounding-bench/bench_resample --rounding
```

## Tighten temporary axis-weight capacity by scaling mode

`px_axis_build()` previously reserved `sn + 2*dn + 2` taps for indices and weights
in every mode. It now chooses a smaller safe capacity before the existing four allocations:

| mode | cap |
| --- | --- |
| sn == dn | dn |
| sn < dn | 2*dn |
| sn > dn && sn % dn == 0 | sn |
| other shrink | sn+dn-1 |

Identity axes need one tap per output; bilinear retains both taps, including repeated
indices and zero weights. Area-coverage intervals are partitioned by source-pixel and
output boundaries, giving at most `sn+dn-1` intervals. Integer shrink ratios have exactly
representable double boundaries and one tap per source pixel. Noninteger shrinking uses
a bound rather than a rational gcd count, allowing tiny additional floating-point overlaps.
Weight generation, sampling accumulation, channel quantization, the same-size full-image
fast path, allocation counts and failure cleanup remain compatible. Only index/weight
allocation requests shrink, with no new per-pixel computation or structure fields.

`theme_resample` now freezes the old axis builder and compares complete start/count/used
idx/float weight bits. The default 100,303 axis pairs cover every 1..2047 dimension against
boundary sizes in both directions plus identity. Optional Release `--axes-exhaustive`
passes all 4,190,209 legal dimension pairs, with smaller new requests in every pair.
This proves host axis tables/capacity bounds, not device RAM or timing. Default tests also
check 295,186 channel inputs and 10,464 full-image cases (13,182,976 output bytes) against
the old full loop, private real path and public production path. Mixed scaling with a
maximum-size identity axis, old/new peak requests, all nine allocation failure points,
invalid arguments and final zero leaks pass. Shared fetch/free helpers are not independently
revalidated. Three negative controls first compile, then fail: restored overallocation,
undersized bilinear capacity (ASAN catches the overflow), and undersized integer shrinking.
ASAN/UBSAN and Release theme each pass 7/7.

Full-image peaks trace actual allocation/free callbacks. They include RGBA output and
axis arrays, excluding source images, heap metadata/alignment, LVGL caches and later render
memory. These requested bytes are the same for RGBA/RGB565 sources, not measured device
heap peaks:

| sw | sh | dw | dh | previous_peak_bytes | actual_peak_bytes | saved_bytes |
| --- | --- | --- | --- | --- | --- | --- |
| 360 | 360 | 180 | 180 | 140424 | 136080 | 4344 |
| 90 | 150 | 180 | 300 | 226104 | 224640 | 1464 |
| 180 | 300 | 90 | 150 | 61224 | 58320 | 2904 |
| 2047 | 1 | 2047 | 2 | 65570 | 40976 | 24594 |
| 360 | 90 | 360 | 180 | 271644 | 266760 | 4884 |


Timing uses the complete-image method: Apple M1 / arm64 / Apple clang 21, Release `-O3`,
no sanitizer/LTO, byte/checksum comparisons, four warmup calls per path and seven alternating
groups of at least 4,000,000 output pixels each, taking the median. The baseline retains the
previous integer channel quantizer and old capacity, isolating the earlier `floorf` change.
`--rounding` preserves the historical old/new channel comparison with old capacity in both
paths. No other local builds/tests run during timing; background system load is uncontrolled.
The [first run](performance/axis-capacity-host-2026-10-14.csv) and
[repeat](performance/axis-capacity-host-repeat-2026-10-14.csv) record all 20 cases.
Host results cannot prove an ESP32 speedup. All 20 repeat cases are within ±0.4%,
with `ratio = actual_ns / reference_ns` ranging from `0.996039..1.001950`. One first-run case
was about 8.5% faster, which did not recur. The two exploratory 16-case runs before expanding
the scenarios are preserved as [first](performance/axis-capacity-host-exploratory-2026-10-14.csv)
and [repeat](performance/axis-capacity-host-exploratory-repeat-2026-10-14.csv); some RGBA cases
were about 9–10% slower, also not reproduced in the final repeat. Adoption rests on smaller
memory requests. These desktop timing variations establish neither a stable speedup nor
absence of a device artwork-load regression.

Target `px_axis_build` text changes `839 → 895`, with literals staying `164`, adding 56
code bytes. The static frame remains 112 bytes. Capacity modulo uses hardware `remu`,
checked once before building an axis. `tpx_resample` text/literal sizes stay 1201/52 bytes,
with a 144-byte static frame. Compiler instruction/directive sequences match after removing
debug metadata and renumbering local labels. Assembler alignment and branch offsets differ,
so target text bytes are not identical; the source per-pixel path is unchanged. Every text/literal section from old/new standalone
compilation matches its full firmware object, with no test probes in the ELF. Normal/stats/
no-SD-slot builds pass, each adding 64 app bytes:

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855b0 | 0x2855f0 | 64 |
| stats | 0x285e10 | 0x285e50 | 64 |
| no-slot | 0x275f60 | 0x275fa0 | 64 |

[Target evidence](performance/axis-capacity-xtensa-2026-10-14.txt) preserves source/image/test/benchmark
hashes, disassembly, exhaustive and negative-control results; the
[source patch](performance/axis-capacity-source-2026-10-14.diff) reconstructs old capacity.
Adoption rests on definitely smaller allocation requests and compatible legal axis tables/
images. Actual PSRAM free heap, fragmentation, page-artwork load delay, CPU and FPS still
need device verification; this does not establish the whole project's performance limit.

```bash
cmake -S test/host/theme -B /tmp/obd-axis-capacity-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_RESAMPLE_BENCHMARK=ON
cmake --build /tmp/obd-axis-capacity-bench
ctest --test-dir /tmp/obd-axis-capacity-bench --output-on-failure
/tmp/obd-axis-capacity-bench/test_resample --axes-exhaustive
/tmp/obd-axis-capacity-bench/bench_resample
```

## Crop and convert identity-size pointers directly

This section records the Flash mmap implementation and measurements before SD migration.
The current SD path owns RAM pixels, budgets untinted RGB565 and adds a temporary SD source
segment; zero-copy savings and old acquisition benchmarks below do not describe the current
storage path. Identity-size pixel/geometry behavior still skips resampling; see [THEMES.en.md](THEMES.en.md).

`te_acquire()` previously requested `4*w*h` bytes even when source and element dimensions
matched, copying RGBA or expanding RGB565 before scanning alpha, retaining the pivot and
converting to the display format. It now calls `tpx_resample()` only for different source/target
dimensions. Identity-size sources go directly through validated planning and the original
crop/pivot retention. RGBA format, pixels, alpha, placement and cache keys stay compatible.

Identity RGB565 has no transparent pixels and retains the entire image. Without tint, the
existing full-row zero-copy path borrows mapped artwork, with `owned=NULL` and zero pixel
budget; a referenced cache entry still requires allocation. Tint forces an independent RGB565
buffer with the original color mixing, never writing mapped flash. `te_est_cost()` accordingly
estimates zero without tint or `2*w*h` with tint; other pointers keep the conservative `3*w*h`
estimate. Lower costs can admit artwork previously skipped by pixel budgets/preflight.
Cache hits still count their actual pixel cost. Reference counts, LRU and deferred release of
cleared entries keep their original rules. Public `theme_release_resources()` defers while
pages live, clearing the cache and unmapping after the last page is deleted, reusing the
mapped-artwork lifetime constraint already used by image elements.

These pixel requests are deterministically removed; this is not device free-heap or allocator
overhead measurement:

| w | h | temporary_request_removed | persistent_rgb565_untinted_removed | pixel_allocations_removed_rgba_or_tinted | pixel_allocations_removed_rgb565_untinted |
| --- | --- | --- | --- | --- | --- |
| 90 | 150 | 54000 | 27000 | 1 | 2 |
| 180 | 300 | 216000 | 108000 | 1 | 2 |
| 360 | 360 | 518400 | 259200 | 1 | 2 |

New `theme_acquire` compiles real page/pixel conversion/cache/LVGL against the frozen entire
old acquisition function. Both share geometry, pixel and cache helpers; this does not independently
reimplement those algorithms. The old function does not call the estimator; the new estimator
is separately checked as an upper bound on actual pixel cost. The 1,006 pixel/geometry cases
cover both formats, tint, transparent/low/random alpha and transparent margins, single pixels
and maximum legal single axes, actual scaling and ordinary images, negative/outside/fractional
pivots and dimension rounding. Additional checks cover all 65,536 RGB565 codes, mapped offsets,
source immutability, allocation-free hits, ref rollback on over-budget hits, clearing pinned
entries, final release, zero budgets/simulated low free heap, failures of the removed temporary,
persistent final-buffer/entry failures, transient failures at every scaled allocation, invalid
dimensions/formats/byte counts and complete release. ASAN/UBSAN and Release each pass 9/9.
Three compiled negative controls (skip actual scaling, tint mapped data, drop pivot bounds)
are rejected by 1,022 / 151 / 330 assertions respectively.

[First run](performance/pointer-identity-host-2026-10-14.csv) and
[repeat](performance/pointer-identity-host-repeat-2026-10-14.csv) each preserve 20 whole-acquisition
cases with seven alternating groups of at least 20 ms, raw samples and medians. Timing includes
allocation, conversion, cache insertion, release and clearing; input preparation, output comparison
and peak tracing are outside timing. Ratios across both runs are:

| path | new_over_old_min | new_over_old_max |
| --- | --- | --- |
| identity_rgb565_untinted | 0.000186 | 0.001706 |
| identity_rgb565_tinted | 0.139949 | 0.152151 |
| identity_rgba_untinted | 0.942976 | 0.969428 |
| identity_rgba_tinted | 0.948424 | 0.973395 |
| scaled_all | 0.992949 | 1.004261 |

The zero-copy desktop result removes every pixel loop, excludes flash reads during actual
drawing and is not a whole-page/FPS benchmark. Sources are in host RAM, capabilities/free heap
are simulated and the desktop allocator includes a test header; both paths use that allocator.
Background load is uncontrolled; desktop ratios cannot establish ESP32 speedups. Requested-byte
tracing includes the host's 112-byte cache entry but excludes allocator headers. In 360×360
examples, RGBA peak is `691920 → 173632`, untinted RGB565 is `777600 → 112`, and tinted RGB565
is `777600 → 259312`. The temporary is freed before entry allocation, so peak savings differ
from the sum of removed requests. ESP32 entry size, heap overhead and mapped reads differ;
device peaks, fragmentation and load time remain unmeasured.

On ESP32-S3 acquisition is inlined into `theme_create_page`, whose text is `6594 → 6638` and
static frame stays 624 bytes; estimator text is `76 → 108` with an unchanged 80-byte frame.
Whole-page object text is `14646 → 14723`, literals stay 2020 bytes, with no added `.bss` / `.data`.
All standalone old/new text/literal/bss/data sections match their corresponding firmware objects;
host probes are absent from ELF. Three firmware builds pass:

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x285640 | 80 |
| stats | 0x285e50 | 0x285e80 | 48 |
| no-slot | 0x275fa0 | 0x276000 | 96 |

[Target/test evidence](performance/pointer-identity-xtensa-2026-10-14.txt) preserves disassembly,
hashes and results; the [source patch](performance/pointer-identity-source-2026-10-14.diff)
reconstructs the old acquisition path. Adoption is based on removing redundant full-image
conversion and deterministic pixel-request savings. Device timing, display behavior and
long-term stability still need verification; the whole-project performance goal remains open.

```bash
cmake -S test/host/theme -B /tmp/obd-pointer-acquire -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DTE_SANITIZE=OFF -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-pointer-acquire
ctest --test-dir /tmp/obd-pointer-acquire --output-on-failure
/tmp/obd-pointer-acquire/test_acquire --benchmark
```

## Use ROM CRC for ESP-NOW v6

CRC-16/CCITT-FALSE in `espnow_v6.c` previously ran eight bit iterations per byte,
repeated on both encode and decode. The ESP32 path now uses SDK `esp_rom_crc16_be`.
The SDK complements its state, requiring seed 0 and a complemented return value;
empty input still returns `0xFFFF`. Wide `size_t` inputs are chunked at `UINT32_MAX`
while retaining continuous state. Device `size_t` is 32-bit, so the compiler removes
the chunk loop. No allocation or RAM lookup table is added. Ordinary host builds
retain the old bit loop. v5, v6 DATA fields and CRC byte order, HELLO and receive
acceptance remain compatible. This change does not alter send frequency or measure RF throughput.

New `espnow_crc_portable` / `espnow_crc_rom` compare against the frozen old CRC,
covering all 65,536 two-byte inputs, lengths 0..512/eight offsets/four patterns,
random long input, NULL with zero length and the standard vector. The SDK branch
also covers 16,777,216 state/byte transitions and every split of a 257-byte input;
the public wrapper has not been tested with an allocated input above 4 GiB.
Real encode/decode compare against the portable codec with shared helpers for
0..64 records and eight offsets, checking complete output, independent old CRC,
decoded fields, truncation, byte-position corruption and insufficient capacity.
The existing full v5/v6 protocol suite also runs with the ROM branch. Shared helper
comparison does not independently verify unchanged protocol layouts. Portable
and ROM tests pass 643,162 and 17,420,636 assertions respectively. ASAN/UBSAN and
Release each pass 7/7; disabling SDK compatibility leaves a portable 5/5 suite.
Three compiled negative controls (wrong seed, missing final complement, reflected
algorithm) produce 84,410 / 84,410 / 84,314 failures respectively.

The [first run](performance/espnow-crc-host-2026-10-14.csv) and
[repeat](performance/espnow-crc-host-repeat-2026-10-14.csv) each save 18 cases,
seven alternating groups per case and at least 20 ms per group, with all raw
samples and medians. CRC lengths are 0/38/44/104/236/248 bytes. Complete encode/decode
covers 0/1/11/33/43/64 records and includes every fragment. Input preparation and
correctness checks stay outside timing; CRC/frame results are consumed to prevent
elimination. Ratio ranges across both runs:

| path | new_over_old_min | new_over_old_max |
| --- | --- | --- |
| crc_empty | 0.985754 | 0.988935 |
| crc_nonempty | 0.174370 | 0.291654 |
| encode | 0.201994 | 0.319280 |
| decode | 0.196878 | 0.300662 |

The host executes the SDK Linux table implementation with RAM input, measuring
neither chip ROM nor wireless tasks or end-to-end latency. Background load is
uncontrolled; desktop ratios are not ESP32 speedups. Adoption rests on removing
target bit loops, reusing existing ROM and reducing target code. Actual CPU time,
power and multi-gauge throughput still require device measurements.

Standalone objects built with the real ESP32-S3 commands match all corresponding
firmware text/literal/bss/data sections. Code and static entry frames follow;
entry frames exclude the ROM's internal call stack:

| function | before_text | after_text | before_frame | after_frame |
| --- | --- | --- | --- | --- |
| espnow_crc16 | 66 | 32 | 32 | 32 |
| espnow_v6_encode | 476 | 452 | 64 | 64 |
| espnow_v6_decode | 446 | 385 | 48 | 32 |

Whole-codec text is `4234 → 4116`, literal `156 → 148`, with zero `.bss` / `.data`.
`espnow_crc16` is inlined into encode/decode and its standalone function is removed
by the linker. In all three ELFs both indirect calls load `0x40001cbc`, matching
the absolute ROM symbol `esp_rom_crc16_be`. Firmware contains neither host
reference functions nor SDK Linux CRC table symbols. All three builds pass:

| config | before | after | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x285640 | 0x2855f0 | -80 |
| stats | 0x285e80 | 0x285e20 | -96 |
| no-slot | 0x276000 | 0x275fa0 | -96 |

[Target and test evidence](performance/espnow-crc-xtensa-2026-10-14.txt) saves
assembly, hashes and results; the [source patch](performance/espnow-crc-source-2026-10-14.diff)
restores the original CRC path. Set `EH_ESP_ROM_DIR` to select SDK sources;
`EH_ROM_CRC=OFF` retains SDK-free base tests. Timing requires the official SDK Linux
backend and is included in neither default CI nor firmware:

```bash
cmake -S test/host/espnow -B /tmp/obd-espnow-crc -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_CRC_BENCHMARK=ON
cmake --build /tmp/obd-espnow-crc
ctest --test-dir /tmp/obd-espnow-crc --output-on-failure
/tmp/obd-espnow-crc/bench_espnow_crc
```

## Generated signal ID lookup

`signal_store_slot_by_id()` previously scanned 17 static signals; vehicle and unknown
IDs also traversed the entire table. ESP-NOW receive processing calls it for clearing
valid bits and writing records. `tools/gen_signals.py` now emits
`sig_registry_slot_by_id()` from the registry. Entries coalesce into range arithmetic
only when both IDs and slots are adjacent; other entries use individual comparisons.
The current registry produces two ranges without adding a runtime mapping table;
future holes or reordered entries remain exact through generation.
Vehicle ID bounds, registered-count checks and critical sections are preserved,
with static/dynamic slot numbers and protocol IDs unchanged.

`signal_lookup` includes real `signal_store.c` and the frozen original complete lookup
on identical registration state, with a separate linear table/registered-count oracle.
All 65,536 IDs are checked before initialization, after initialization, for each vehicle
count 1..32, after clearing and after re-registration: 7,077,955 assertions pass.
`signal_lookup_generator` generates and compiles C for eight registries: current,
reversed, rotated, shuffled, sparse, sparse shuffled, subset and reversed IDs. Independent
linear-table oracles cover 524,288 IDs. The `signal_store` suite passes 6/6 under both
ASAN/UBSAN and Release, the related ESP-NOW suite 7/7 and the vehicle suite 14/14;
contracts pass for 37 profiles. Four temporary negative controls (missing endpoint,
wrong slot offset, accepting a hole and ignoring vehicle registration count) detect
72 / 864 / 72 / 591 failures. Tests and frozen references do not enter firmware.

The [first](performance/signal-id-host-2026-10-14.csv),
[repeat](performance/signal-id-host-repeat-2026-10-14.csv) and
[third](performance/signal-id-host-third-2026-10-14.csv) runs each save 24 cases,
with seven alternating groups per case, at least 20 ms per group, all samples and
medians retained. Twelve local cases cover standard/system/vehicle/unknown IDs,
before initialization, unregistered slots and a mixed sequence. Twelve receive cases
include real `espnow_rx_apply()`: empty frames, valid masks, vehicle invalidation,
0/1/32 vehicle slots, unknown records, v5, profile mismatch and complete 64-record
snapshots including every fragment. Receive callbacks update only test arrays and
counters. Write counts, final values, validity and callback counts are compared after
each fragment outside timing; frame preparation and decoding are also excluded.
Local ns is per lookup; receive ns is per complete snapshot including all fragments.

| scope | path | new_over_old_min | new_over_old_max |
| --- | --- | --- | --- |
| all_3_runs | lookup_rpm | 1.139780 | 1.163550 |
| all_3_runs | lookup_mixed | 0.251269 | 0.251687 |
| all_3_runs | lookup_registered_dynamic | 0.289161 | 0.291045 |
| repeat_and_third | apply_empty_all_valid | 0.947191 | 0.955869 |
| repeat_and_third | apply_other_cases | 0.340574 | 0.475689 |
| all_3_runs | apply_full64 | 0.442505 | 0.445516 |

RPM previously matched the first table entry; its desktop single-ID lookup increases
from about 1.24..1.27 ns to 1.41..1.45 ns, a 14%..16% regression that does not describe
all paths. The first `standard_invalidate32` case has substantial variation: old samples
about 691..26,867 ns, new samples about 209..69,939 ns, median ratio 2.759025. That run
includes a clear regression and is retained unchanged. The subsequent ratios are
0.368565 / 0.368298, with improvements in other ordinary receive cases. System
background load and lock contention were uncontrolled; desktop ratios cannot establish
device timing improvements. Host FreeRTOS locks are empty stubs, and test array writes
exclude real signal-table locks, timestamps and validity checks. Decoding, CRC, radio
transmission and task scheduling are also excluded. Actual ESP32 receive time and power
remain to be measured on the device.

Actual ESP32-S3 compilation removes the static scan's `loop` / `l16ui` and registry
address reference while preserving both critical-section calls for dynamic IDs.
Target lookup sizes follow; entry frames exclude stack inside lock calls:

| function | text_before | text_after | literal_before | literal_after | frame_before | frame_after |
| --- | --- | --- | --- | --- | --- | --- |
| signal_store_slot_by_id | 92 | 92 | 20 | 16 | 32 | 32 |

All text/literal/bss/data/rodata sections match between standalone and normal firmware
objects. Only lookup text/literal sections change across versions; other functions and
storage match byte for byte. Total object text is 4,429 bytes, literal 544 to 540 bytes,
with bss/data/rodata unchanged at 4/8/131 bytes. Generated `signal_registry.c` is byte
identical to the baseline. Normal, statistics and no-SD-slot firmware builds pass, with
identical lookup functions and retained lock calls in each ELF, no reference or benchmark
symbols, and image sizes unchanged because of alignment:

| build | before_hex | after_hex | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x2855f0 | 0 |
| stats | 0x285e20 | 0x285e20 | 0 |
| no-slot | 0x275fa0 | 0x275fa0 | 0 |

Adoption is based on removing repeated static table scans from receive processing while
maintaining target code size and dynamic locking semantics.
[Target evidence](performance/signal-id-xtensa-2026-10-14.txt) contains compile commands,
section comparisons, disassembly, test/negative-control results and hashes. The
[source diff](performance/signal-id-source-2026-10-14.diff) contains only this production
change; reverse application restores the original generator, generated header and lookup.

```sh
cmake -S test/host/signal_store -B /tmp/obd-signal-lookup -G Ninja -DCMAKE_BUILD_TYPE=Release -DSS_SANITIZE=OFF -DSS_LOOKUP_BENCHMARK=ON
cmake --build /tmp/obd-signal-lookup
ctest --test-dir /tmp/obd-signal-lookup --output-on-failure
/tmp/obd-signal-lookup/bench_signal_lookup
python3 tools/gen_signals.py --check
```

## Avoid repeated ID scans in ESP-NOW master snapshots

Each `master_collect()` starts with an empty snapshot and visits the 12 unique
`obd.*` slots and at most 32 `veh.*` slots. Their numeric ID ranges are disjoint.
Gear occupies a separate byte, leaving at most 43 ordinary records. Original
`espnow_snapshot_put()` searched existing IDs for each insertion, scanning
`43*42/2 = 903` records at full load. Only these two initial collection loops now
use `espnow_snapshot_append_unique()`, preserving order, validity masks, invalid-ID,
capacity and gear bounds. Callers must guarantee that an ordinary ID is absent;
replacement still uses `put()`. The final linked-test RPM override keeps the general
replacement API and cannot introduce duplicate RPM records.

Append handles gear internally without a nested call; valid gear can still update a
full snapshot. No heap allocation, index table or task is added. Batched store reads,
locking, polling/broadcast rates and v5/v6 bytes retain their behavior. The change
reduces snapshot construction work; it does not change wireless capacity.

`snapshot_append` compares against frozen old insertion over all 65,536 IDs, empty/full
snapshots, mixed validity masks, 0..64 shuffled unique sequences, invalid IDs/gears,
replacement at capacity and RPM override: 1,187,778 assertions. `master_collect` includes
the real private collector with deterministic store/UI stubs and compares against its
frozen predecessor over 35,012 inputs and 210,616 assertions. It covers all 4,096 OBD
validity masks, 0/1/16/32 vehicle slots, gear bounds, linked-test switches, sequence wrap,
UI step conversion, and 0..33 reported slots with holes or removal between batches.
Full snapshots, v5 bytes, every v6 fragment and decoding are checked. ASAN/UBSAN and
Release wireless suites each pass 9/9; the portable suite with SDK compatibility disabled
passes 7/7. Temporary wrong vehicle masks, gear upper bound,
IDs and append-instead-of-RPM-replacement mutations produce 258,392 / 4 / 464,057 /
31,650 failures respectively; sanitizer detects a wrong capacity bound. Frozen references,
tests and benchmarks never enter firmware.

[First run](performance/snapshot-append-host-2026-10-14.csv) and
[repeat](performance/snapshot-append-host-repeat-2026-10-14.csv) each retain 33 cases,
seven alternating groups per case of at least 20 ms each, all 14 samples and ratio ranges.
Twelve insertion cases cover 0/1/11/32/43/64 records with and without RPM override.
Twenty-one real collection/collection-plus-encode cases cover empty, standard, one vehicle,
full, sparse, removed slots and linked override. Complete snapshots and all encoded bytes
are compared outside timing; v6 includes every fragment. ns is per complete snapshot.
Desktop ratios follow:

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

Full collection takes substantially less time. The reduction is smaller with v6 encoding,
where encoding and CRC contribute more work. Empty v5 can regress slightly; both results
are retained. The desktop benchmark uses the production portable bit-loop CRC, excluding
device ROM CRC, actual store locks, radio transmission and RTOS scheduling. System background
load is uncontrolled. These ratios cannot directly establish ESP32-S3 latency, throughput
or power improvements.

Real ESP32-S3 `-O2` append is 140 text bytes, with no literal, record search or child call.
Its entry frame is 32 bytes, matching the old general insertion. General insertion stays
200 text bytes and byte-identical; the master task frame stays 672 bytes. After adding the
function, the compiler emits 2 / 1 / 1 fewer bytes for unchanged v5 encode, hello encode and
receive apply source respectively, for a net 136-byte code increase. Object totals follow:

| object | section | before_bytes | after_bytes |
| --- | --- | --- | --- |
| espnow_v6 | text | 4116 | 4252 |
| espnow_v6 | literal | 148 | 148 |
| espnow_link | text | 4969 | 4969 |
| espnow_link | literal | 1568 | 1568 |

Codec bss/data/rodata stays 0/0/0; the link object retains 253/16/1,428 bytes. Standalone
objects exactly match real firmware text/literal/data/rodata bytes and bss sizes. In the
link object, only two error-check source-line immediates move by two lines after added
comments; other instruction bytes match. Real ELF verifies two append calls and one general
RPM replacement in the master, without reference/benchmark symbols. Normal, statistics and
no-SD-slot firmware builds pass; alignment leaves every image 144 bytes larger:

| build | before_hex | after_hex | delta_bytes |
| --- | --- | --- | --- |
| normal | 0x2855f0 | 0x285680 | 144 |
| stats | 0x285e20 | 0x285eb0 | 144 |
| no-slot | 0x275fa0 | 0x276030 | 144 |

Adoption removes quadratic snapshot scans while retaining static RAM, collector call-stack
usage and protocol behavior, trading a small amount of Flash for fewer loop iterations.
[Target evidence](performance/snapshot-append-xtensa-2026-10-14.txt) retains actual commands,
section comparisons, disassembly, tests/negative controls and hashes for three builds.
The narrow [source patch](performance/snapshot-append-source-2026-10-14.diff) reverses exactly
to the three captured preceding files. Device collection, encode and transmission timing
remain to be measured.

```sh
cmake -S test/host/espnow -B /tmp/obd-snapshot-append -G Ninja -DCMAKE_BUILD_TYPE=Release -DEH_SANITIZE=OFF -DEH_COLLECT_BENCHMARK=ON
cmake --build /tmp/obd-snapshot-append
ctest --test-dir /tmp/obd-snapshot-append --output-on-failure
/tmp/obd-snapshot-append/bench_master_collect
```

## Driver-input priority scheduling validation

2026-10-28: `test_responsive_driver_inputs` in `test/host/engine/test_engine.c` replays old and new scheduling against the same simulated adapter. The baseline is `vp_engine.c` at `10f780ad86a471c4b63e07b1c7fd4d2790fd281a`. The host Release build enables ASan/UBSan; protocol 6, a 2 ms response delay, the default 30 ms request gap and ten distinct PIDs are used, with no explicit boost/throttle periods. Each demand mask is observed for five seconds after startup settles. These are simulated clock intervals, not host execution timings or measured BLE/ECU/LCD latency.

| Demanded signals | Before: RPM / speed / boost / TPS reads per 5 s | After: RPM / speed / boost / TPS reads per 5 s | Before: max RPM / speed / boost / TPS request gap (ms) | After: max RPM / speed / boost / TPS request gap (ms) |
|---|---|---|---|---|
| RPM + speed + boost + TPS + 6 background | 63 / 31 / 7 / 7 | 52 / 26 / 26 / 26 | 96 / 160 / 640 / 640 | 128 / 192 / 192 / 192 |
| RPM + speed + boost + 6 background | 63 / 31 / 9 / 0 | 63 / 32 / 31 / 0 | 98 / 162 / 576 / — | 96 / 160 / 160 / — |
| RPM + speed + boost only | 63 / 32 / 62 / 0 | 63 / 31 / 63 / 0 | 96 / 162 / 98 / — | 96 / 162 / 98 / — |

Boost no longer waits for the entire background round on mixed pages; a three-input-only page retains its sampling cadence. Four simultaneous priority inputs still share serial bandwidth: RPM and speed rates decrease slightly in the table, so this is not a simultaneous speedup of every parameter. The test also checks background service and stopping unneeded requests after page changes. Separate cases cover explicit periods, slow throttle polling, shared-PID deduplication and indivisible before/after command chains. Slower adapters, timeouts and proprietary multicommand requests increase these intervals.

`test/host/signal_store/test_display_response.c` extracts the production display update function and checks first-refresh latest-sample output for RPM, speed, boost (including negative values), throttle and load; unchanged samples avoid rewriting labels, and invalid/recovery behavior remains correct. Temperature retains its ramp. The same-frame case in `test/host/media/test_video_player_wait.c` verifies that 1000 sub-frame updates do not wake the SD reader or decoder, while frame changes and rapid round trips still wake them. The player already deduplicated target frames; this change removes unnecessary worker wakeups and does not demonstrate fewer nonexistent duplicate decodes or higher video frame rate.

All 24 host tests across engine, signal_store and media pass for this round. The four changed production C files pass the ESP32-S3 compiler's `-fsyntax-only` check. No full firmware build or vehicle-latency measurement was performed.

## Event-driven LCD DMA wait validation

2026-11-02: The LCD driver previously did not install an LVGL `wait_cb`; real
LVGL 8 `draw_buf_flush()` repeatedly checked `flushing` while a partial
double-buffered transfer was pending. `main/app_obd_dsp/lvgl_flush_sync.c`
now blocks on a boot-lifetime static binary semaphore. The completion ISR
releases the buffer before signaling; every signal only prompts a state recheck,
not permission to reuse a buffer in the current DMA transfer. It does not use
LVGL notification slot 0 or change fullscreen refresh guards, buffer ownership,
or the single-threaded ELM request stream. The ESP-IDF 5.5.3 SPI LCD completion
backend ignores callback return values, so the bridge explicitly requests an
ISR yield when necessary.

`test/host/media/test_lvgl_flush_sync.c` runs the real bridge source with a
pthread-condition-variable binary semaphore. It deterministically waits for
the task to block before completing DMA, rather than inferring correctness from
sleep duration. Both normal/statistics variants check 391 completions and 259
blocking wakeups, covering already idle, completion before wait, stale signals,
coalesced consecutive video completions, an interrupt between the flag check and
take, and 256 repeated transfers. Three temporary negative controls remove the
state recheck, reverse release/signal order, or remove the ISR yield; assertions
reject all three. Existing real-player lifecycle tests also pass in both
normal/statistics variants; this is not a real LCD/RTOS test.

At the DMA-wait stage (before the sweep-clock changes below), the four Release + ASan/UBSan suites `theme` (including real LVGL pages), `media`,
`signal_store`, and `rules` pass all 38 tests; theme generation checks pass.
Complete ESP-IDF 5.5.3 ESP32-S3 V1 dual-core normal/statistics firmware builds pass,
with image sizes `0x28e520` / `0x28ee30`, both retaining 15% application-partition
headroom. In the normal ELF the wait function calls `xQueueSemaphoreTake` and
rechecks state; the static semaphore plus its handle occupy 88 bytes BSS. The
normal ELF contains neither `perf_stats_record` nor `perf_stats_start`.
Builds/configurations are isolated in `/tmp/obd-smooth-20261102/`; statistics
remain disabled in the local normal `sdkconfig`.

```sh
cmake -S test/host/media -B /tmp/obd-flush-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/obd-flush-host
ctest --test-dir /tmp/obd-flush-host --output-on-failure
cmake -S test/host/theme -B /tmp/obd-flush-theme -G Ninja -DCMAKE_BUILD_TYPE=Release -DTE_PAGE_LVGL=ON
cmake --build /tmp/obd-flush-theme
ctest --test-dir /tmp/obd-flush-theme --output-on-failure
```

The statistics metric `lcd_wait` measures wall time waiting for DMA in LVGL,
including wakeup/scheduling time; already-idle calls produce no sample. It is
nested within `lvgl_handler`, is not CPU usage or complete panel FPS, and does
not cover fullscreen video's own wait path. LVGL / OBD tasks are also assigned
to Core 1 / Core 0: this is an unverified scheduling policy, not evidence of a
frame-rate gain. No gauge serial port has been identified and nothing has been
flashed. Live data,
swipes, sweep animations, video, and long-run stability still require device A/B
tests with the same theme, SD card, and adapter. Normal firmware also requires
external observation to exclude statistics-logging overhead.


## Sweep clock and presentation cadence validation

2026-11-02: The old self-test sweep reduced the main UI timer to 40 ms and
advanced one step per callback, so rendering or scheduling delays stretched the
animation. `main/export_path/ui_sweep.c` now derives progress from milliseconds
of `esp_timer_get_time()`, targeting a 16 ms sweep refresh interval. The master
retains 48 coarse 40 ms sync slots, a nominal 1920 ms duration, and delayed
callbacks sample the current position. A 16 ms time grid produces 61 distinct
ramp ratios versus 25 on the old 40 ms grid. These are deterministic algorithm
outputs, not measured panel FPS. TEMP / CHART / INFO no longer reconstruct a
ratio from the already-incremented slot; they share the same sampled ratio as
the theme, RPM, and needle.

Slaves fill the ratio over time between received slots, but flash and termination
still follow received master state only. Packet loss cannot locally advance a
flash or end the sweep. An accepted lower slot is treated as a new sweep, matching
the existing snapshot contract; a slot alone cannot distinguish restart from
reordering, and this adds no network ordering guarantee. The master publishes
the actual current slot, not the old post-draw incremented slot, so mixed firmware
may differ by one 40 ms slot. Protocol fields and showroom 200+ encoding remain
unchanged; use the same firmware version on master and slaves. Live vehicle
values are not interpolated this way and simulated values never enter the store.

`test_sweep_clock.c` compiles the production clock and checks every old 40 ms
ratio/brightness sample, 16 ms sampling, repeated same-time callbacks, skipping
and expiry after long stalls, late join/duplicate/jumping/lower slave slots,
master-authoritative flash/end, invalid values, and uptime beyond 32-bit
milliseconds. `test_sweep_runtime.c` extracts the real `ui_ext.c` state/wrappers
and `ui.c` refresh policy to check connection triggering/boot deferral, atomic
slot publication, target intervals, clearing active and restoring brightness
in the ending callback, writing brightness only when changed, and showroom
interruption. Final boot-page loading and actual UI snapshot switching still
need device integration validation; these tests do not run the complete LVGL
timer, FreeRTOS, or ESP-NOW link.

With the two additions, all 40 Release + ASan/UBSan tests in `theme` / `media` /
`signal_store` / `rules` pass, as do theme generation checks. After adding the C
file, `idf.py reconfigure build` passes for complete ESP-IDF 5.5.3 ESP32-S3 V1
dual-core normal/statistics firmware. Images at that stage are `0x28e630` / `0x28ef40`,
both retaining 15% application-partition headroom. Builds remain isolated in
`/tmp/obd-smooth-20261102/{normal,stats}`, without modifying repository `build/`
or the local `sdkconfig`. No gauge has been identified in the current port list;
nothing has been flashed, committed, or pushed. This does not establish overall
smoothness completion: live data, swipes, sweeps, video, and long-run stability
still require device A/B under matching conditions.


## Background image preparation for theme navigation validation

2026-11-02: Navigation inside a theme previously called `theme_create_page()`
synchronously in the gesture callback, blocking LVGL during SD image reads and
conversion. `ui_theme_navigation.c` now reuses staged creation: LVGL captures a
layout/rules snapshot, `APP_TASK_THEME_PAGE_PREP` (6144-byte stack, priority 3,
unpinned) prepares an isolated image cache, then wakes LVGL for a 16 ms polling
timer to publish widgets and switch. Preparation neither holds the theme lock
nor calls LVGL, allowing the old page to refresh. This is not a device measurement
of total page-creation time or FPS.

Only one request is in flight; requests are not queued and tasks are not killed.
Explicit cancellation, origin unload-start/deletion, or a changed active page or
index prevents publication. Leaving and returning cannot revive a cancelled
transition. After the worker completes, `theme_page_prepare_discard()` frees the
result without creating stale widgets or taking a video session. Start or
publication failure keeps the old page; installation/library reload wait for
preparation resources. Layout snapshots, fonts and widget creation remain on the
LVGL path. The isolated cache can duplicate origin images, increasing transient
PSRAM pressure under existing image budgets/degradation. Slow-card/video I/O
contention and large-theme memory peaks need device validation; this does not
establish that all navigation stalls are gone.

Statistics builds add `theme_nav_begin`, `theme_nav_images` and
`theme_nav_publish`, measuring wall time for request-side layout/rules capture,
worker image planning/reads/conversion, and LVGL finish/cache adoption/widget
creation. They exclude complete request-to-visible latency and do not measure
FPS. The first/last are within the LVGL handler, and worker time can include SD
metrics, so these must not be summed as CPU utilization. Normal builds omit the
timing code.

`theme_navigation` uses real LVGL timers/screens and the production navigation
state machine with controlled worker/engine shims. It checks old-page timer
progress while pending, no worker publication, duplicate/invalid requests,
begin/task/publication failures, explicit cancellation, leaving/deleting the
origin, changed index and leaving/returning, without early disposal or stale
delete callbacks. `theme_page` additionally exercises real-engine resource
cleanup when discarding before/after running. Release + ASan/UBSan passes:
`theme` 15/15, `media` 10/10, `signal_store` 10/10 and `rules` 6/6, totaling 41/41;
theme generation and whitespace checks excluding existing `build/` artifacts
also pass. Tests do not simulate actual FreeRTOS scheduling, SD timing, DMA or
video-session contention.

After adding the C source, isolated `idf.py reconfigure build` passes for
ESP-IDF 5.5.3 / ESP32-S3 V1 dual-core normal/statistics firmware. Images are
`0x28e8b0` / `0x28f250`, both with 15% application-partition headroom. Logs are
`/tmp/obd-smooth-20261102/{normal,stats}-navigation.log`; the only warning is the
existing touch API deprecation. Binary SHA256:

```text
normal 23a1c3f5987d8833215f3876b5b2ba34060c3b12b8a37db1c02b0672a0161f1f
stats  29c738a84cc500408ced8bf906bdc837088a294372355f377c150393439b0c34
```

Repository `build/`, local `sdkconfig` and generated theme sources were not
modified. Ports remain Bluetooth, debug-console and wlan-debug only, with no
gauge identified; nothing has been flashed, committed or pushed. Next is device
A/B of normal/statistics builds using the same SD, theme and data source,
covering live refresh, repeated navigation, sweeps, video and memory stability.
Host tests and builds do not establish completion of overall smoothness.
