#!/bin/sh
set -u
wanted=$1
codec=$2
stage=$3
reboot_boot_id=${4:-}
case "$wanted/$codec" in 720p/hw|720p/sw|1080p/hw|1080p/sw) ;; *) exit 2;; esac
mode="${wanted}_${codec}"
base=/data/local/tmp/k3-virtual-main
v2=$base/usb-h264-auto-v2
v3=$base/usb-h264-1080-v3
v4=$base/usb-h264-720-v4
sw720=$base/usb-mjpeg-720-v5
sw1080=$base/usb-mjpeg-1080-v5
sw720_sha=bd3c8196dff235968cbe0b1169fd31e338fa130b6e4bb243940c515e6e7c001c
sw1080_sha=d9d746ce852341be543665cca89aa67e89f536dd75fc3f6c1fa374c7b582bf9a
release=$base/usb-h264-720-v5
fhd=$base/usb-h264-1080-v4
oldrelease=$base/usb-h264-25-v1
trial=/data/local/tmp/k3-hw25-trial
bundle=com.ohos.scrcpy.server
primary=$base/k3-virtual-primary
legacy=$base/k3-usb-preview
jpeg=$v4/k3-usb-preview
wire=$release/k3-hw-wire
cap=$release/k3-surface-capture
small_primary=$release/k3-virtual-primary
small_surface_sha=ba957e384763a46481681a97e61b227b59c078e0beb9246f2a73745d6a66549c
primary_sha=434b6ed72bdf60a17e08d9260d5e4216d449abbb12e7d615e562e000abfc8189
oldjpeg_sha=3a8ddbfc72cdac5182ed9141898d9ba87103a02bb003a6f86efd247f9959f919
oldwire_sha=01965085b1b00f8569537cb57e74e401e2f983cf16e17b7896b05c6b15d6aba6
jpeg_sha=d73aa84a8424a80d8c7bc921fbb89305c93458e1fa8eccc557a7e46365fec5a9
wire_sha=80aa863ee655d454d37d2468f75f6f3a904618162aa206ead44c99cd4a0495a8
v2jpeg_sha=752ea9a6fb454c29cf359f075c97690e18fd1960b2630f1852f09e8edc5afe6d
v2wire_sha=fd17b0cff4e4e9a3a75ed3ba41554b3aff72d1f327632f55f8473209bd94ae54
oldcap_sha=12703c20bed69b5464d7d289caf0850aefd2746c32df6ee7d1d2b35eaa350933
cap_sha=3f5fea312272e41ce3bee221e87be7e74af0daac43b9cab718d878f9ff7c0d81
fprimary=$fhd/k3-virtual-primary
fwire=$fhd/k3-hw-wire
fcap=$fhd/k3-surface-capture
fsnapshot=$fhd/k3-capture-pipe
surface_sha=6d200e62140c9dfa200e9fb76bf578390d4d87d4a801691e05c3a08a1a1b1613
fprimary_sha=e074fee8c5ec088c747f3689e09d262a0c8d4590a532bdaa012b41b024bd7019
fwire_sha=6997184517f61dbc69ed89c4daad86c6104ba7cceaaf5961f020bb8657245e95
fcap_sha=263fdf62032c0231e8fc5739e1353a298510d4daf4d8b0c55de3210f29dbd79a
v3primary_sha=e074fee8c5ec088c747f3689e09d262a0c8d4590a532bdaa012b41b024bd7019
v3wire_sha=91008516f93b1e781e8d3f836c757e393ab8f7fcf28ec579f5bdda929f9d11da
v3cap_sha=223f180adb1e66d6319f598d982bebf04cf01a776ebae229da3615a87ee936d9
umask 077
fail() { echo "K3：$*" >&2; exit 1; }
digest() {
    openssl dgst -sha256 "$1" 2>/dev/null | busybox awk 'NF {print $NF}'
}
matches() { [ -f "$1" ] && [ ! -L "$1" ] && [ "$(digest "$1")" = "$2" ]; }
known_legacy() {
    case "$1" in
        "$oldjpeg_sha"|1054745dfbad3181a2d366d2317b8ef5b48e827e851d82b36f512d197963c8ee|d0a3fbb5d918036af14593d1bffda94efb38d1a0bf89005f65da09664585a0fe) return 0;;
        *) return 1;;
    esac
}
profile_files() {
    # The same check protects staged files, existing installations and rollback.
    case "$2" in
        720p) p_hash=$primary_sha; w_hash=$wire_sha; c_hash=$cap_sha; s_hash=$small_surface_sha;;
        1080p) p_hash=$fprimary_sha; w_hash=$fwire_sha; c_hash=$fcap_sha; s_hash=$surface_sha;;
        *) return 1;;
    esac
    [ -d "$1" ] && [ ! -L "$1" ] &&
    matches "$1/k3-virtual-primary" "$p_hash" && matches "$1/k3-hw-wire" "$w_hash" &&
    matches "$1/k3-capture-pipe" "$c_hash" && matches "$1/k3-surface-capture" "$s_hash"
}
install_profile() {
    if [ -e "$2" ] || [ -L "$2" ]; then
        profile_files "$2" "$1" || fail "板上 $1 程序校验失败。"
    else
        for name in k3-virtual-primary k3-hw-wire k3-capture-pipe k3-surface-capture; do
            chmod 700 "$stage/$1/$name" || fail "无法准备 $1 程序。"
        done
        mv "$stage/$1" "$2" && profile_files "$2" "$1" || fail "无法安装 $1 程序。"
    fi
    for name in k3-virtual-primary k3-hw-wire k3-capture-pipe k3-surface-capture; do
        [ -x "$2/$name" ] || fail "板上 $1 程序缺少执行权限。"
    done
}
install_software() {
    if [ ! -e "$2" ] && [ ! -L "$2" ]; then
        chmod 700 "$stage/sw-$1/k3-sw-wire" &&
        mv "$stage/sw-$1" "$2" || fail "无法安装 $1 软件编码程序。"
    fi
    [ -d "$2" ] && [ ! -L "$2" ] && [ -x "$2/k3-sw-wire" ] &&
    matches "$2/k3-sw-wire" "$3" || fail "板上 $1 软件编码程序校验失败。"
}
valid_trial() {
    [ -d "$trial" ] && [ ! -L "$trial" ] &&
    [ -f "$trial/.owner" ] && [ ! -L "$trial/.owner" ] &&
    [ "$(cat "$trial/.owner")" = 'k3-hongjing-experimental-h264-25-v1' ] &&
    matches "$trial/k3-hw-wire" "$oldwire_sha" && matches "$trial/k3-capture-pipe" "$oldcap_sha"
}
listener() {
    table=$(netstat -lntp) || { echo unknown; return; }
    row=$(printf '%s\n' "$table" | busybox awk '$1~/^tcp/ && $4~/:53535$/ && $6=="LISTEN" {n++; addr=$4; proc=$7} END{if(!n) print "free"; else if(n==1) print addr,proc; else print "unknown"}')
    case "$row" in free|unknown) echo "$row"; return;; esac
    addr=${row%% *}
    pid=${row#* }; pid=${pid%%/*}
    case "$pid" in ''|*[!0-9]*) echo unknown; return;; esac
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || { echo unknown; return; }
    if [ "$addr" = '127.0.0.1:53535' ]; then
        case "$exe" in
            "$v2/k3-hw-wire") echo "v2_h264 $pid"; return;;
            "$v2/k3-usb-preview") echo "v2_jpeg $pid"; return;;
            "$v3/k3-hw-wire") echo "v3_fhd $pid"; return;;
            "$sw720/k3-sw-wire") echo "720p_sw $pid"; return;;
            "$sw1080/k3-sw-wire") echo "1080p_sw $pid"; return;;
            "$wire") echo "720p_hw $pid"; return;;
            "$v4/k3-hw-wire") echo "v4_h264 $pid"; return;;
            "$fwire") echo "1080p_hw $pid"; return;;
            "$jpeg") echo "v4_jpeg $pid"; return;;
            "$oldrelease/k3-hw-wire") echo "old_h264 $pid"; return;;
            "$oldrelease/k3-usb-preview") echo "old_jpeg $pid"; return;;
            "$legacy") echo "legacy $pid"; return;;
            "$trial/k3-hw-wire") echo "trial $pid"; return;;
        esac
    fi
    name=$(busybox tr '\000' '\n' <"/proc/$pid/cmdline" | head -1)
    if [ "$name" = "$bundle" ]; then echo "hap $pid"; else echo unknown; fi
}
# Describe both current profiles and prior packaged services. Remembering the
# actual prior rate lets a failed encoder switch restore that exact session.
service_spec() {
    svc_kind=wire
    svc_codec=h264
    svc_name=k3-hw-wire
    svc_rate=auto
    case "$1" in
        720p_hw) svc_dir=$release; svc_hash=$wire_sha; svc_input=$cap; svc_input_hash=$small_surface_sha;;
        1080p_hw) svc_dir=$fhd; svc_hash=$fwire_sha; svc_input=$fcap; svc_input_hash=$surface_sha;;
        720p_sw) svc_dir=$sw720; svc_hash=$sw720_sha; svc_input=$cap; svc_input_hash=$small_surface_sha; svc_codec=mjpeg; svc_name=k3-sw-wire;;
        1080p_sw) svc_dir=$sw1080; svc_hash=$sw1080_sha; svc_input=$fcap; svc_input_hash=$surface_sha; svc_codec=mjpeg; svc_name=k3-sw-wire;;
        v4_h264) svc_dir=$v4; svc_hash=$wire_sha; svc_input=$v4/k3-capture-pipe; svc_input_hash=$cap_sha;;
        v4_jpeg) svc_dir=$v4; svc_kind=jpeg; svc_hash=$jpeg_sha;;
        v3_fhd) svc_dir=$v3; svc_hash=$v3wire_sha; svc_input=$v3/k3-capture-pipe; svc_input_hash=$v3cap_sha;;
        v2_h264) svc_dir=$v2; svc_hash=$v2wire_sha; svc_input=$v2/k3-capture-pipe; svc_input_hash=$oldcap_sha;;
        v2_jpeg) svc_dir=$v2; svc_kind=jpeg; svc_hash=$v2jpeg_sha;;
        old_h264) svc_dir=$oldrelease; svc_hash=$oldwire_sha; svc_input=$oldrelease/k3-capture-pipe; svc_input_hash=$oldcap_sha; svc_rate=25;;
        old_jpeg) svc_dir=$oldrelease; svc_kind=jpeg; svc_hash=$oldjpeg_sha;;
        legacy) svc_dir=$base; svc_kind=jpeg; svc_hash=$(digest "$legacy"); known_legacy "$svc_hash" || return 1;;
        trial)
            valid_trial || return 1
            svc_dir=$trial; svc_hash=$oldwire_sha; svc_input=$trial/k3-capture-pipe; svc_input_hash=$oldcap_sha; svc_rate=25;;
        hap) svc_kind=hap; return 0;;
        *) return 1;;
    esac
    case "$svc_kind" in
        wire) svc_exe=$svc_dir/$svc_name; svc_log=$svc_dir/$svc_codec.log;;
        jpeg) svc_exe=$svc_dir/k3-usb-preview; svc_log=$svc_dir/jpeg.log; svc_rate=legacy-default;;
    esac
    case "$1" in trial|legacy) svc_log=$svc_dir/preview.log;; esac
}
verify_owner() {
    service_spec "$1" || return 1
    if [ "$svc_kind" = hap ]; then [ "$(listener)" = "hap $2" ]; return; fi
    [ ! -L "$svc_dir" ] && matches "$svc_exe" "$svc_hash" && [ -x "$svc_exe" ] &&
    [ "$(digest "/proc/$2/exe")" = "$svc_hash" ] || return 1
    if [ "$svc_kind" = wire ]; then
        matches "$svc_input" "$svc_input_hash" && [ -x "$svc_input" ] || return 1
    fi
}
current_rate() {
    service_spec "$1" || return 1
    if [ "$svc_kind" = hap ]; then return 0; fi
    busybox tr '\000' '\n' <"/proc/$2/cmdline" | busybox awk -v family="$svc_kind" -v cap="${svc_input:-}" -v codec="$svc_codec" '
        NR == 2 { first=$0 }
        NR == 3 { rate=$0 }
        NR == 4 { encoding=$0 }
        NR == 5 { source=$0 }
        NR == 6 { fmt=$0 }
        END {
            if (family=="wire" && NR==6 && first=="53535" && encoding==codec && source==cap && fmt=="rgba" && (rate=="auto" || rate~/^[0-9]+$/)) print rate
            else if (family=="jpeg" && NR==1) print "legacy-default"
            else if (family=="jpeg" && NR==2 && (first=="auto" || first~/^[0-9]+$/)) print first
            else exit 1
        }'
}
screen_state() {
    display=$(hidumper -s DisplayManagerService -a '-d -a') || { echo unknown; return; }
    screens=$(hidumper -s DisplayManagerService -a '-s -a') || { echo unknown; return; }
    case "$display" in *'DisplayId ScreenId'*) ;; *) echo unknown; return;; esac
    case "$screens" in *'total screen num:'*) ;; *) echo unknown; return;; esac
    main=$(printf '%s\n' "$display" | busybox awk '$1~/^[0-9]+$/ && $2~/^[0-9]+$/ {gsub(/\[|\]/," "); print $1,$2,$(NF-1),$NF}')
    if [ -z "$main" ]; then
        if printf '%s\n' "$screens" | busybox grep -Eq 'total screen num:[[:space:]]*0([[:space:]]|$)'; then echo none; else echo foreign; fi
        return
    fi
    sid=$(printf '%s\n' "$screens" | busybox awk '$1=="K3" && $2=="USB" && $3=="desktop" && $4=="VIRTUAL" {print $6}')
    case "$sid" in ''|*[!0-9]*) echo foreign; return;; esac
    case "$main" in
        "0 $sid 1280 720") screen_size=720p;;
        "0 $sid 1920 1080") screen_size=1080p;;
        *) echo foreign; return;;
    esac
    for proc in $(pidof k3-virtual-primary); do
        owner=$(readlink "/proc/$proc/exe") || continue
        case "$screen_size:$owner" in
            "720p:$primary"|"720p:$small_primary") expected=$primary_sha;;
            "1080p:$v3/k3-virtual-primary") expected=$v3primary_sha;;
            "1080p:$fprimary") expected=$fprimary_sha;;
            *) continue;;
        esac
        if [ "$(digest "/proc/$proc/exe")" = "$expected" ]; then echo "$screen_size"; return; fi
    done
    echo stale
}
# A restarted display service may forget the virtual screen while its old
# helpers still own their process locks. Only retire recognized executables
# after confirming that this display service has no registered screen.
retire_orphaned_helpers() {
    for name in k3-surface-capture k3-virtual-primary; do
        for orphan in $(pidof "$name"); do
            exe=$(readlink "/proc/$orphan/exe") || continue
            case "$exe" in
                "$primary"|"$small_primary") expected=$primary_sha; argument=--run;;
                "$fprimary") expected=$fprimary_sha; argument=--run;;
                "$v3/k3-virtual-primary") expected=$v3primary_sha; argument=--run;;
                "$cap") expected=$small_surface_sha; argument=--serve;;
                "$fcap") expected=$surface_sha; argument=--serve;;
                *) continue;;
            esac
            [ "$(digest "/proc/$orphan/exe")" = "$expected" ] || continue
            cmd=$(busybox tr '\000' '\n' <"/proc/$orphan/cmdline" | tail -n +2)
            [ "$cmd" = "$argument" ] || continue
            [ "$(screen_state)" = none ] || fail '显示服务状态已变化，请重试。'
            [ "$(readlink "/proc/$orphan/exe")" = "$exe" ] || continue
            kill -TERM "$orphan" || fail '旧虚拟屏进程未能退出。'
            tries=0
            while [ "$(readlink "/proc/$orphan/exe" 2>/dev/null)" = "$exe" ] && [ "$tries" -lt 30 ]; do
                busybox usleep 100000
                tries=$((tries+1))
            done
            [ "$(readlink "/proc/$orphan/exe" 2>/dev/null)" != "$exe" ] || fail '正在等待旧虚拟屏进程退出。'
        done
    done
}
regular_log() {
    [ ! -L "$1" ] && { [ ! -e "$1" ] || [ -f "$1" ]; }
}
wait_for() {
    i=0
    while [ "$i" -lt 45 ]; do
        actual=$(listener)
        case "$actual" in "$1 "|"$1 "*) return 0;; free) [ "$1" = free ] && return 0;; esac
        busybox usleep 150000; i=$((i+1))
    done
    return 1
}
start_server() {
    [ "$(listener)" = free ] && service_spec "$1" || return 1
    wait_kind=$1
    start_rate=${2:-$svc_rate}
    case "$svc_kind" in
        wire) set -- "$svc_exe" 53535 "$start_rate" "$svc_codec" "$svc_input" rgba;;
        jpeg)
            if [ "$start_rate" = legacy-default ]; then set -- "$svc_exe"
            else set -- "$svc_exe" "$start_rate"; fi;;
        hap) aa start -b "$bundle" -a EntryAbility >/dev/null && wait_for hap; return;;
        *) return 1;;
    esac
    regular_log "$svc_log" || return 1
    nohup "$@" </dev/null >"$svc_log" 2>&1 &
    wait_for "$wait_kind"
}

ensure_surface() {
    # The virtual display requires a running consumer even between USB sessions.
    # The v4 1080p relay is already the correct binary and remains attached.
    for p in $(pidof k3-surface-capture); do
        if [ "$(readlink "/proc/$p/exe")" = "$relay" ] &&
           [ "$(digest "/proc/$p/exe")" = "$relay_hash" ] &&
           [ "$(busybox tr '\000' '\n' <"/proc/$p/cmdline" | tail -n +2)" = "--serve" ]; then
            return 0
        fi
    done
    regular_log "$relay_log" || fail '无法写入显示采集日志。'
    nohup "$relay" --serve </dev/null >"$relay_log" 2>&1 &
    relay_pid=$!
    attempts=0
    while [ "$attempts" -lt 40 ] && kill -0 "$relay_pid" 2>/dev/null; do
        busybox grep -q 'SetVirtualScreenSurface result=0' "$relay_log" && return 0
        busybox usleep 100000
        attempts=$((attempts+1))
    done
    fail "$wanted 虚拟主屏直读尚未就绪，请查看 $relay_log。"
}

previous=''
prior_rate=''
recover() {
    if [ -n "$previous" ] && [ "$(listener)" = free ]; then
        if start_server "$previous" "$prior_rate"; then echo '已恢复切换前的投屏服务。' >&2
        else echo '原投屏服务未能重新启动。请查看板端日志并重试。' >&2; fi
    fi
}
trap recover EXIT
trap 'exit 130' HUP INT TERM
flock -n 0 || fail '另一项投屏切换正在运行。'

profile_files "$stage/720p" 720p && profile_files "$stage/1080p" 1080p &&
matches "$stage/sw-720p/k3-sw-wire" "$sw720_sha" && matches "$stage/sw-1080p/k3-sw-wire" "$sw1080_sha" ||
    fail '启动包的程序校验失败。'
case "$codec" in
    hw) encoder=h264_stcodec; label='H.264 硬件编码';;
    sw) encoder=mjpeg; label='MJPEG 软件编码';;
esac
[ -x /vendor/bin/ffmpeg ] || fail '开发板缺少厂商 FFmpeg。'
enc=$(/vendor/bin/ffmpeg -hide_banner -encoders 2>/dev/null) || fail '无法读取编码器列表。'
printf '%s\n' "$enc" | busybox awk -v enc="$encoder" '$2==enc {found=1} END{exit !found}' ||
    fail "当前系统没有可用的 $label 编码器。"

active=$(listener); kind=${active%% *}
case "$kind" in
    free) :;;
    unknown) fail '53535 端口归属不明，已保留原进程。';;
    *)
        verify_owner "$kind" "${active#* }" || fail '当前监听程序的身份校验失败。'
        prior_rate=$(current_rate "$kind" "${active#* }") || fail '当前投屏服务参数无法识别。';;
esac
display_state=$(screen_state)
case "$display_state" in
    1080p|720p|none) :;;
    foreign) fail '开发板已有其他显示屏，请在无外接显示器时启动。';;
    stale) fail '默认虚拟屏的创建进程已退出，请在方便时重启 K3 再运行。';;
    *) fail '无法读取显示服务状态。';;
esac
if [ "$display_state" != none ] && [ "$display_state" != "$wanted" ]; then
    boot_id=$(cat /proc/sys/kernel/random/boot_id) || fail '无法读取当前启动会话。'
    if [ -z "$reboot_boot_id" ]; then
        printf 'K3_SWITCH_REBOOT_REQUIRED %s %s %s\n' "$display_state" "$wanted" "$boot_id"
        exit 44
    fi
    [ "$reboot_boot_id" = "$boot_id" ] || fail '开发板已经重启，请重新运行所选配置。'
fi

# Install all choices independently of which display is currently active.
install_profile 720p "$release"
install_profile 1080p "$fhd"
install_software 720p "$sw720" "$sw720_sha"
install_software 1080p "$sw1080" "$sw1080_sha"
case "$wanted" in
    720p) new_primary=$small_primary; profile_dir=$release; relay=$cap; relay_hash=$small_surface_sha;;
    1080p) new_primary=$fprimary; profile_dir=$fhd; relay=$fcap; relay_hash=$surface_sha;;
esac
primary_log=$profile_dir/run.log
relay_log=$profile_dir/surface.log
if [ "$display_state" != none ] && [ "$display_state" != "$wanted" ]; then
    [ "$(screen_state)" = "$display_state" ] && [ "$(listener)" = "$active" ] ||
        fail '显示屏或投屏服务刚刚发生变化，请重新运行启动入口。'
    echo K3_SWITCH_REBOOT_SENT
    sync && reboot || fail '开发板重启请求失败。'
    exit 45
fi
if [ "$display_state" = none ]; then
    retire_orphaned_helpers
    regular_log "$primary_log" || fail '无法写入虚拟主屏日志。'
    nohup "$new_primary" --run </dev/null >"$primary_log" 2>&1 &
    i=0
    while [ "$i" -lt 35 ] && [ "$(screen_state)" != "$wanted" ]; do busybox usleep 200000; i=$((i+1)); done
    [ "$(screen_state)" = "$wanted" ] || fail "虚拟主屏尚未就绪，请查看 $primary_log。"
fi
ensure_surface
[ "$(listener)" = "$active" ] || fail '端口状态刚刚发生变化，请重试。'
if [ "$kind" = "$mode" ] && [ "$prior_rate" = auto ]; then
    echo "$wanted $label 投屏已就绪，虚拟主屏全速直读。"
    exit 0
fi
if [ "$kind" != free ]; then
    pid=${active#* }
    verify_owner "$kind" "$pid" && [ "$(listener)" = "$active" ] || fail '端口状态已改变，已停止切换。'
    previous=$kind
    if [ "$kind" = hap ]; then
        aa stop-service -b "$bundle" -a ScrcpyService >/dev/null
        i=0
        while [ "$i" -lt 8 ] && [ "$(listener)" = "$active" ]; do busybox usleep 200000; i=$((i+1)); done
        [ "$(listener)" != "$active" ] || aa force-stop "$bundle" >/dev/null
    else
        kill -TERM "$pid" || fail '未能停止旧投屏服务。'
    fi
    wait_for free || fail '原投屏服务还在使用 53535 端口。'
fi
start_server "$mode" auto || fail '新投屏服务未能开始监听。'
previous=''
echo "$wanted $label 投屏已就绪，虚拟主屏全速直读。"
