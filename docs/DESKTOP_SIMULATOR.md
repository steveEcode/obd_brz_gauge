# Desktop Triple-Gauge Simulator

The desktop simulator runs the original LVGL gauge UI on Linux or WSL2.

桌面模拟器直接运行项目原有的 LVGL 仪表 UI，用于页面开发、三联表通信、掉线和自动恢复测试。

## Features / 功能

- Original project LVGL UI
- One Master and two Slave windows
- Shared mock OBD data
- Mouse click and swipe support
- Master timeout detection
- Automatic recovery after Master restart
- No ESP32 or ELM327 required

## Requirements / 依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libsdl2-dev
```

## Submodules / 子模块

```bash
git submodule update --init --recursive
```

Simulator dependencies:

- `simulator/third_party/lvgl`
- `simulator/third_party/lv_drivers`

## Build / 编译

```bash
cmake \
  -S simulator \
  -B simulator/build-ui-probe \
  -DSIMULATOR_ENABLE_GITHUB_UI_PROBE=ON

cmake --build \
  simulator/build-ui-probe \
  --target BRZGaugeSimulator \
  -j"$(nproc)"
```

Successful output:

```text
[100%] Built target BRZGaugeSimulator
```

## Run / 运行

```bash
./tools/run_triple_simulator.sh
```

The launcher starts:

- `BRZ Gauge - MASTER`
- `BRZ Gauge - SLAVE LEFT`
- `BRZ Gauge - SLAVE RIGHT`

Press `Ctrl+C` in the launcher terminal to close all windows.

## Controls / 控制

| Input | Action |
|---|---|
| `W` or `Up` | Throttle |
| `S` or `Down` | Brake |
| `A` or `Left` | Steer left |
| `D` or `Right` | Steer right |
| `T` | Toggle traffic mode |
| `Esc` | Close focused instance |
| Mouse | Click and swipe UI |

Focus the Master window when controlling the mock ECU.

## Logs / 日志

```text
${XDG_RUNTIME_DIR:-/tmp}/obd_brz_gauge_simulator-${USER}/logs
```

Log files:

- `master.log`
- `slave-left.log`
- `slave-right.log`

## Disconnect Test / 掉线测试

Start the simulator:

```bash
./tools/run_triple_simulator.sh
```

In a second Ubuntu terminal, stop only the Master:

```bash
MASTER_PID="$(
  pgrep -f 'BRZGaugeSimulator.*--role master' |
  head -n 1
)"

kill "$MASTER_PID"
```

After about 2 seconds, both Slaves display:

```text
MASTER OFFLINE
WAITING FOR MASTER
```

Restart the Master:

```bash
LOG_DIR="${XDG_RUNTIME_DIR:-/tmp}/obd_brz_gauge_simulator-${USER}/logs"

env SDL_VIDEODRIVER=x11 \
  ~/projects/obd_brz_gauge/simulator/build-ui-probe/BRZGaugeSimulator \
  --role master \
  >"$LOG_DIR/master-restart.log" 2>&1 &
```

The Slaves reconnect automatically and hide the offline overlay.

## Validation / 验证

Verified:

- Clean desktop simulator build
- Triple-window synchronization
- Master timeout and automatic recovery
- ESP-IDF v5.5.4 full build
- ESP32-S3 target

The ESP-IDF build validates compilation and linking only. Physical hardware, BLE, LCD and wireless behavior still require real-device testing.

## Scope / 范围

The mock ECU is intended for UI testing, not exact vehicle dynamics.

Gear display is currently estimated from RPM and vehicle speed. Real `P/R/N/D/M1-M6` support requires vehicle-specific TCM or CAN development.
