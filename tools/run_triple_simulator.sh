#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(
    cd "$(dirname "${BASH_SOURCE[0]}")/.."
    pwd
)"

BUILD_DIR="${SIM_BUILD_DIR:-$ROOT_DIR/simulator/build-ui-probe}"
BINARY="$BUILD_DIR/BRZGaugeSimulator"

# WSLg 默认 Wayland 后端会忽略或重置 SDL 窗口位置。
# 使用 X11 后端，让 SDL_SetWindowPosition() 稳定生效。
export SDL_VIDEODRIVER=x11

USER_TAG="${USER:-$(id -u)}"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}/obd_brz_gauge_simulator-$USER_TAG"
LOG_DIR="$RUNTIME_DIR/logs"

MASTER_PID=""
LEFT_PID=""
RIGHT_PID=""

cleanup()
{
    trap - EXIT INT TERM

    echo
    echo "[SIM] 正在关闭三联表……"

    for pid in "$MASTER_PID" "$LEFT_PID" "$RIGHT_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    for pid in "$MASTER_PID" "$LEFT_PID" "$RIGHT_PID"; do
        if [[ -n "$pid" ]]; then
            wait "$pid" 2>/dev/null || true
        fi
    done

    rm -f /dev/shm/obd_brz_gauge_bus

    echo "[SIM] 已关闭。"
}

trap cleanup EXIT INT TERM

if [[ ! -x "$BINARY" ]]; then
    echo "[SIM] 找不到模拟器程序："
    echo "      $BINARY"
    echo
    echo "请先编译："
    echo "cmake --build \"$BUILD_DIR\" --target BRZGaugeSimulator -j\"$(nproc)\""
    exit 1
fi

mkdir -p "$LOG_DIR"

# 清理之前遗留的模拟器进程。
pkill -9 -f 'BRZGaugeSimulator.*--role' 2>/dev/null || true
sleep 0.2

# 清理上一轮共享内存。
rm -f /dev/shm/obd_brz_gauge_bus

: > "$LOG_DIR/master.log"
: > "$LOG_DIR/slave-left.log"
: > "$LOG_DIR/slave-right.log"

echo "[SIM] 启动 MASTER"
env SDL_VIDEODRIVER=x11 "$BINARY" --role master \
    >"$LOG_DIR/master.log" 2>&1 &
MASTER_PID=$!

sleep 0.35

if ! kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "[SIM] MASTER 启动失败："
    tail -n 30 "$LOG_DIR/master.log" || true
    exit 1
fi

echo "[SIM] 启动 SLAVE LEFT"
env SDL_VIDEODRIVER=x11 "$BINARY" --role slave-left \
    >"$LOG_DIR/slave-left.log" 2>&1 &
LEFT_PID=$!

sleep 0.5

echo "[SIM] 启动 SLAVE RIGHT"
env SDL_VIDEODRIVER=x11 "$BINARY" --role slave-right \
    >"$LOG_DIR/slave-right.log" 2>&1 &
RIGHT_PID=$!

sleep 0.5

# WSLg/Wayland 通常忽略 SDL_SetWindowPosition。

echo
echo "[SIM] 三联表已启动"
echo "      MASTER PID:     $MASTER_PID"
echo "      SLAVE LEFT PID:  $LEFT_PID"
echo "      SLAVE RIGHT PID: $RIGHT_PID"
echo
echo "[SIM] 日志目录：$LOG_DIR"
echo "[SIM] 在这个终端按 Ctrl+C 可关闭全部窗口。"
echo
echo "[SIM] 单独关闭 MASTER 时，两个 SLAVE 会继续运行，"
echo "      便于下一步测试掉线和自动恢复。"

# 保持启动器运行，直到三个窗口全部退出。
while true; do
    alive=0

    for pid in "$MASTER_PID" "$LEFT_PID" "$RIGHT_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            alive=1
            break
        fi
    done

    if [[ "$alive" -eq 0 ]]; then
        break
    fi

    sleep 0.5
done
