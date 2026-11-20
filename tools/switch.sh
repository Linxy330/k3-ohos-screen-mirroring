#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
die() { printf '%s\n' "$*" >&2; exit 1; }
size=${1:-}
codec=${2:-}
case "$size" in 720p|1080p) ;; *) die '请选择 1080p 或 720p。';; esac
case "$codec" in hw|sw) ;; *) die '请选择 hw（硬件编码）或 sw（软件编码）。';; esac
shift 2
consent=0
case "$#" in
    0) ;;
    1) [[ $1 == --reboot-if-needed ]] || die '可选参数为 --reboot-if-needed。'; consent=1;;
    *) die '启动参数过多。';;
esac
hdc=${HDC:-/Applications/鸿镜.app/Contents/Resources/hdc}
[[ -x "$hdc" ]] || die '请确认「鸿镜」安装在 Mac 的「应用程序」中。'
host() { "$hdc" -s 127.0.0.1:8710 "$@"; }
devices() { host list targets -v | tr -d '\r' | awk '$2=="USB" && $3=="Connected" {print $1}'; }
target=$(devices)
[[ -n "$target" && "$target" != *$'\n'* ]] || die '请通过 USB 连接一块已授权调试的 K3。'
dev() { host -t "$target" "$@"; }
board() { dev shell "$1" | tr -d '\r'; }
correct_board() {
    [[ "$(board 'param get const.product.brand' | sed 's/[[:space:]]*$//')" == 'Spacemit K3-Pico' &&
       "$(board 'uname -m')" == riscv64 ]]
}
correct_board || die '当前 USB 设备型号或架构不匹配。'
base=/data/local/tmp/k3-virtual-main
stage=''
cleanup() {
    if [[ -n "$stage" ]] && devices | grep -Fxq -- "$target"; then
        board "rm -rf -- '$stage'" >/dev/null || :
    fi
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
upload() {
    local file=$1 name=$2 hash observed
    [[ -f "$file" ]] || die "启动包缺少 $file，请完整解压。"
    hash=$(shasum -a 256 "$file" | awk '{print $1}')
    dev file send "$file" "$stage/$name" >/dev/null
    observed=$(board "openssl dgst -sha256 '$stage/$name'")
    [[ "${observed##* }" == "$hash" ]] || die "$name 在 USB 传输后校验失败。"
}
prepare() {
    local temp name
    temp=$(board 'umask 077; mktemp -d /data/local/tmp/k3-mirror-stage.XXXXXXXX')
    [[ "$temp" =~ ^/data/local/tmp/k3-mirror-stage\.[A-Za-z0-9]{8}$ ]] || die '未能建立板端临时传输目录。'
    stage=$temp
    board "mkdir -m 700 '$stage/720p' '$stage/1080p' '$stage/sw-720p' '$stage/sw-1080p'" >/dev/null
    upload tools/board.sh board.sh
    upload bin/k3-sw-wire sw-720p/k3-sw-wire
    upload bin/1080p/k3-sw-wire sw-1080p/k3-sw-wire
    for name in k3-virtual-primary k3-hw-wire k3-capture-pipe k3-surface-capture; do
        upload "bin/$name" "720p/$name"
        upload "bin/1080p/$name" "1080p/$name"
    done
}
invoke() {
    local approved=${1:-}
    # Some HDC versions do not return the remote shell's exit code.
    # Positive markers are emitted by the remote script for each transition.
    reply=$(board "umask 077; test ! -L '$base' && mkdir -p '$base' && /bin/sh '$stage/board.sh' '$size' '$codec' '$stage' '$approved' <'$base' && echo K3_SWITCH_OK") || :
}
ready() {
    printf '%s\n' "$reply" | sed '/^K3_SWITCH_OK$/d'
    echo '鸿镜显示「镜像中」时可继续使用。切换编码后若提示未连接，选择 K3 并点「连接」。'
}
wait_new_boot() {
    local previous=$1 current list elapsed=0 reminded=0
    local start=$SECONDS
    while (( SECONDS-start < 300 )); do
        list=$(devices) || list=''
        if printf '%s\n' "$list" | grep -Fxq -- "$target"; then
            current=$(board 'cat /proc/sys/kernel/random/boot_id') || current=''
            if [[ "$current" != "$previous" && "$current" =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}$ ]]; then
                if correct_board &&
                   board 'hidumper -s DisplayManagerService -a "-s -a"' | grep -q 'total screen num:' &&
                   board 'hidumper -s WindowManagerService -a "-a"' | grep -q -- '--WindowManagerService--'; then
                    return 0
                fi
            fi
        fi
        elapsed=$((SECONDS-start))
        if (( elapsed >= 12 && !reminded )); then
            echo '正在等待同一块 K3 回到 USB 调试模式。如一直离线，请重新连接 USB 调试线。'
            reminded=1
        fi
        sleep 1
    done
    return 1
}
prepare
invoke
if grep -Fqx K3_SWITCH_OK <<<"$reply"; then ready; exit 0; fi
line=$(printf '%s\n' "$reply" | awk '$1=="K3_SWITCH_REBOOT_REQUIRED" {print}')
read -r marker old_size new_size boot extra <<<"$line" || :
if [[ "$marker" != K3_SWITCH_REBOOT_REQUIRED || -n "$extra" ]] ||
   [[ "$old_size/$new_size" != 720p/1080p && "$old_size/$new_size" != 1080p/720p ]] ||
   [[ ! "$boot" =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}$ ]]; then
    printf '%s\n' "$reply" >&2
    die '启动未完成，请查看上方设备提示。'
fi
echo "K3 当前运行的是 $old_size 虚拟主屏，切换到 $new_size 需要重启开发板一次。"
if [[ "$consent" != 1 ]]; then
    [[ -t 0 ]] || die '请在终端重新运行所选入口，确认可以重启，或手动重启 K3 后再次运行。'
    printf '重启会中断板上正在运行的程序。保存工作后，输入 Y 确认现在重启：'
    read -r answer || exit 1
    case "$answer" in y|Y) ;; *) die '已取消切换。';; esac
fi
echo '正在提交重启请求。启动脚本会在 USB 重新连接后自动继续。'
invoke "$boot"
if grep -Fqx K3_SWITCH_OK <<<"$reply"; then ready; exit 0; fi
if ! grep -Fqx K3_SWITCH_REBOOT_SENT <<<"$reply"; then
    # HDC may lose the connection before forwarding the last line. A changed
    # boot ID still proves that the requested restart took place.
    if devices | grep -Fqx -- "$target" && [[ "$(board 'cat /proc/sys/kernel/random/boot_id')" == "$boot" ]]; then
        printf '%s\n' "$reply" >&2; die '重启未执行，已停止切换。'
    fi
fi
wait_new_boot "$boot" || die '等待开发板重新上线超时。USB 恢复后再次运行同一入口即可继续。'
cleanup
stage=''
prepare
invoke
if ! grep -Fqx K3_SWITCH_OK <<<"$reply"; then
    printf '%s\n' "$reply" >&2; die '开发板已重启。请检查上方的启动结果。'
fi
ready
