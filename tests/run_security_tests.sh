#!/bin/bash
# 阶段 5 安全回归：TOCTOU 暂存、参数/暂存校验、Busy 与 fail-closed。
# 依赖：dbus-daemon、python3-gi（缺失时以 77 退出表示跳过）。
set -u

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
CLIENT=$REPO_DIR/tests/security_client.py
FAKE_POLKIT=$REPO_DIR/tests/fake_polkit.py
ALLOW_ALL="org.fdebm.helper.install,org.fdebm.helper.remove,org.fdebm.helper.update-cache,org.fdebm.helper.upgrade"

if ! python3 -c "import gi" >/dev/null 2>&1; then
    echo "SKIP: 需要 python3-gi"
    exit 77
fi
if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "SKIP: 需要 dbus-daemon"
    exit 77
fi
if [ -z "${BUILD_DIR:-}" ]; then
    echo "请通过 BUILD_DIR 指定构建目录，例如 BUILD_DIR=/tmp/build tests/run_security_tests.sh"
    exit 1
fi
BUILD_DIR=$(cd "$BUILD_DIR" && pwd) || exit 1
HELPER=$BUILD_DIR/fdebm-helperd
if [ ! -x "$HELPER" ]; then
    echo "未找到 $HELPER，请先构建"
    exit 1
fi

WORK=$(mktemp -d)
STAGING=$WORK/staging
BAD_STAGING=$WORK/bad-staging
mkdir -m 0700 "$STAGING"
mkdir -m 0777 "$BAD_STAGING"
chmod 0777 "$BAD_STAGING"

ADDR=""
PIDS=()
CHECKS=0
FAILURES=0

stop_stack() {
    local pid
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
    PIDS=()
}

cleanup() {
    stop_stack
    rm -rf "$WORK"
}
trap cleanup EXIT

start_stack() {
    local allow=$1 delay=$2 staging=$3 extra=${4:-}
    stop_stack

    local out
    out=$(dbus-daemon --session --fork --print-address=1 --print-pid=1) || return 1
    ADDR=$(printf '%s\n' "$out" | head -n1)
    PIDS+=("$(printf '%s\n' "$out" | tail -n1)")

    FAKE_POLKIT_ALLOW="$allow" FAKE_POLKIT_DELAY_MS="$delay" \
        python3 "$FAKE_POLKIT" "$ADDR" >"$WORK/polkit.log" 2>&1 &
    PIDS+=($!)

    DBUS_SYSTEM_BUS_ADDRESS="$ADDR" "$HELPER" --dry-run $extra --staging-dir "$staging" \
        >"$WORK/helper.log" 2>&1 &
    PIDS+=($!)

    local i
    for i in $(seq 1 100); do
        if timeout 2 gdbus introspect --address "$ADDR" --dest org.fdebm.Helper \
                --object-path /org/fdebm/Helper >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.05
    done
    echo "服务启动超时"
    cat "$WORK/helper.log" >&2
    return 1
}

check() {
    local name=$1
    shift
    CHECKS=$((CHECKS + 1))
    if "$@"; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        FAILURES=$((FAILURES + 1))
    fi
}

staging_empty() {
    if compgen -G "$STAGING/deb-*.deb" >/dev/null; then
        echo "暂存目录残留文件:" >&2
        ls -l "$STAGING" >&2
        return 1
    fi
    return 0
}

install_ok() {
    python3 "$CLIENT" "$ADDR" install "$WORK/pkg.deb" | tee "$WORK/install.log"
    return "${PIPESTATUS[0]}"
}

staged_path_used() {
    grep -qF "$STAGING/deb-" "$WORK/install.log"
}

run_reject() {
    local substring=$1
    shift
    python3 "$CLIENT" "$ADDR" reject-install "$substring" "$@" | tee "$WORK/reject.log"
    return "${PIPESTATUS[0]}"
}

# 授权前失败只能返回统一错误：不得出现路径或具体校验原因
reject_message_generic() {
    grep -qF "本地安装包校验失败" "$WORK/reject.log" \
        && ! grep -qF "$WORK" "$WORK/reject.log" \
        && ! grep -qE "文件不存在|不是普通文件|空文件|大小上限|文件权限不安全|必须是绝对路径|未选择任何 DEB" "$WORK/reject.log"
}

# ---------- 阶段 A：允许授权，验证暂存与并发 ----------
echo "== 阶段 A：允许授权 =="
start_stack "$ALLOW_ALL" 400 "$STAGING" || exit 1

printf 'dummy deb payload\n' >"$WORK/pkg.deb"
chmod 0644 "$WORK/pkg.deb"

check "正常本地安装成功" install_ok
check "安装后暂存目录已清空" staging_empty
check "apt 参数使用暂存路径" staged_path_used

printf 'x' >"$WORK/world.deb"
chmod 0666 "$WORK/world.deb"
check "拒绝组/其他用户可写的 deb" run_reject "本地安装包校验失败" "$WORK/world.deb"

: >"$WORK/empty.deb"
chmod 0644 "$WORK/empty.deb"
check "拒绝空 deb 文件" run_reject "本地安装包校验失败" "$WORK/empty.deb"

rm -f "$WORK/big.deb"
truncate -s 5G "$WORK/big.deb"
chmod 0644 "$WORK/big.deb"
check "拒绝超过单包上限的 deb" run_reject "本地安装包校验失败" "$WORK/big.deb"
rm -f "$WORK/big.deb"

mkfifo "$WORK/fifo.deb"
check "拒绝 FIFO" run_reject "本地安装包校验失败" "$WORK/fifo.deb"

check "拒绝不存在的路径" run_reject "本地安装包校验失败" "$WORK/missing.deb"
check "拒绝信息不泄露路径与具体原因" reject_message_generic
check "详细拒绝原因写入助手日志" \
    grep -qF "拒绝本地安装请求" "$WORK/helper.log"
check "拒绝空参数" run_reject "本地安装包校验失败"

printf 'not a deb\n' >"$WORK/pkg.txt"
chmod 0644 "$WORK/pkg.txt"
check "拒绝非 .deb 后缀" run_reject "本地安装包校验失败" "$WORK/pkg.txt"
check "拒绝相对路径" run_reject "本地安装包校验失败" "relative.deb"

check "拒绝选项形式的包名" \
    python3 "$CLIENT" "$ADDR" install-names "非法包名" "-oDPkg::Pre-Install-Pkgs"
check "拒绝含空格的包名" \
    python3 "$CLIENT" "$ADDR" install-names "非法包名" "bad name"
check "拒绝空包名列表" \
    python3 "$CLIENT" "$ADDR" install-names "未选择任何软件包"
check "接受多架构包名的源安装" \
    python3 "$CLIENT" "$ADDR" install-names-ok libc6:amd64
check "更新缓存事务完成" python3 "$CLIENT" "$ADDR" update-cache
check "系统升级事务完成" python3 "$CLIENT" "$ADDR" upgrade

printf 'swap payload\n' >"$WORK/swap.deb"
chmod 0644 "$WORK/swap.deb"
check "授权期间替换为符号链接被拒绝" \
    python3 "$CLIENT" "$ADDR" swap-during-auth "$WORK/swap.deb" /etc/passwd
check "符号链接替换拒绝后无暂存残留" staging_empty

printf 'original payload\n' >"$WORK/replace.deb"
chmod 0644 "$WORK/replace.deb"
check "授权期间替换为另一个普通文件被拒绝" \
    python3 "$CLIENT" "$ADDR" replace-during-auth "$WORK/replace.deb"
check "普通文件替换拒绝后无暂存残留" staging_empty

printf 'late payload\n' >"$WORK/late.deb"
chmod 0644 "$WORK/late.deb"
check "授权期间放宽权限被拒绝" \
    python3 "$CLIENT" "$ADDR" chmod-during-auth 666 "$WORK/late.deb"

check "事务进行中并发请求返回 Busy" \
    python3 "$CLIENT" "$ADDR" busy "$WORK/pkg.deb"
check "Busy 拒绝后暂存目录为空" staging_empty

check "接受多架构包名 libc6:amd64" \
    python3 "$CLIENT" "$ADDR" remove libc6:amd64
check "拒绝非法多架构后缀" \
    python3 "$CLIENT" "$ADDR" reject-remove "非法包名" libc6:amd64:extra

polkit_subject_ok() {
    ! grep -qE 'SUBJECT_(MISMATCH|INVALID|MISSING)' "$WORK/polkit.log"
}

polkit_actions_seen() {
    local action
    for action in org.fdebm.helper.install org.fdebm.helper.remove \
            org.fdebm.helper.update-cache org.fdebm.helper.upgrade; do
        grep -q "action=$action .*subject=ok" "$WORK/polkit.log" || {
            echo "polkit 日志缺少 $action" >&2
            return 1
        }
    done
}

check "polkit subject 与 /proc 一致" polkit_subject_ok
check "polkit 收到全部四类 action" polkit_actions_seen

# ---------- 阶段 B：拒绝授权，验证 fail-closed ----------
echo "== 阶段 B：拒绝授权 =="
start_stack "" 0 "$STAGING" || exit 1
printf 'denied payload\n' >"$WORK/denied.deb"
chmod 0644 "$WORK/denied.deb"
check "未授权时拒绝安装" run_reject "未获得授权" "$WORK/denied.deb"
check "未授权时拒绝更新缓存" \
    python3 "$CLIENT" "$ADDR" reject-update-cache "未获得授权"
check "未授权时拒绝系统升级" \
    python3 "$CLIENT" "$ADDR" reject-upgrade "未获得授权"
check "未授权时 subject 仍然一致" polkit_subject_ok
check "未授权时不产生事务与暂存文件" staging_empty

# ---------- 阶段 C：暂存目录不安全 ----------
echo "== 阶段 C：暂存目录权限不安全 =="
start_stack "$ALLOW_ALL" 0 "$BAD_STAGING" || exit 1
check "拒绝组/其他用户可写的暂存目录" \
    run_reject "暂存目录" "$WORK/pkg.deb"

# ---------- 阶段 D：取消长事务 ----------
echo "== 阶段 D：取消长事务 =="
# 加长 dry-run 事务窗口，确保取消请求在事务结束前到达
start_stack "$ALLOW_ALL" 0 "$STAGING" "--dry-run-delay-ms 5000" || exit 1
check "发起方可以取消自己的事务" \
    python3 "$CLIENT" "$ADDR" cancel
check "其他连接不能取消他人事务" \
    python3 "$CLIENT" "$ADDR" cancel-foreign
check "取消不存在的事务被拒绝" \
    python3 "$CLIENT" "$ADDR" cancel-missing

echo "----------------------------------------"
echo "共 $CHECKS 项，失败 $FAILURES 项"
if [ "$FAILURES" -ne 0 ]; then
    echo "== helper 日志 =="
    cat "$WORK/helper.log"
    exit 1
fi
exit 0
