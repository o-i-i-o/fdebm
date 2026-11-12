#!/bin/bash
# GUI 冒烟测试：私有总线 + dry-run helper，offscreen 平台启动 GUI 并确认进程稳定存活。
set -u

if [ -z "${BUILD_DIR:-}" ]; then
    echo "请通过 BUILD_DIR 指定构建目录，例如 BUILD_DIR=/tmp/build tests/run_gui_smoke.sh" >&2
    exit 1
fi
BUILD_DIR=$(cd "$BUILD_DIR" && pwd) || exit 1
GUI=$BUILD_DIR/fdebm
HELPER=$BUILD_DIR/fdebm-helperd

if [ ! -x "$GUI" ] || [ ! -x "$HELPER" ]; then
    echo "未找到构建产物，请先构建（GUI=$GUI，HELPER=$HELPER）" >&2
    exit 1
fi
if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "SKIP: 需要 dbus-daemon" >&2
    exit 77
fi

WORK=$(mktemp -d)
BUS_PID=""
HELPER_PID=""
GUI_PID=""

cleanup() {
    [ -n "$GUI_PID" ] && kill "$GUI_PID" 2>/dev/null
    [ -n "$HELPER_PID" ] && kill "$HELPER_PID" 2>/dev/null
    [ -n "$BUS_PID" ] && kill "$BUS_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -m 0700 "$WORK/staging"

OUT=$(dbus-daemon --session --fork --print-address=1 --print-pid=1) || exit 1
ADDR=$(printf '%s\n' "$OUT" | head -n1)
BUS_PID=$(printf '%s\n' "$OUT" | tail -n1)

DBUS_SYSTEM_BUS_ADDRESS="$ADDR" "$HELPER" --dry-run --staging-dir "$WORK/staging" \
    >"$WORK/helper.log" 2>&1 &
HELPER_PID=$!

if command -v gdbus >/dev/null 2>&1; then
    for _ in $(seq 1 100); do
        if timeout 2 gdbus introspect --address "$ADDR" --dest org.fdebm.Helper \
                --object-path /org/fdebm/Helper >/dev/null 2>&1; then
            break
        fi
        sleep 0.05
    done
else
    sleep 1
fi

QT_QPA_PLATFORM=offscreen DBUS_SYSTEM_BUS_ADDRESS="$ADDR" \
    "$GUI" >"$WORK/gui.log" 2>&1 &
GUI_PID=$!

sleep 6
if ! kill -0 "$GUI_PID" 2>/dev/null; then
    echo "FAIL: GUI 在 offscreen 平台启动后退出" >&2
    cat "$WORK/gui.log" >&2
    exit 1
fi

echo "PASS: GUI 在 offscreen 平台稳定运行"
exit 0
