#!/usr/bin/env bash
# Build the K3-Pico / OpenHarmony 6.1 helpers from the bundled sources.
set -euo pipefail

usage() {
    cat <<'HELP'
Usage: ./build.sh [output-directory]

Defaults to ./build beside this script. Requires Docker and a running
ubuntu2204-x64 container with the K3 RISC-V OpenHarmony native SDK.
Override with K3_BUILD_CONTAINER and K3_SDK_NATIVE.
On the first build, connect the target K3 over HDC to read its runtime libraries.
To select a target or HDC client, set K3_TARGET or HDC_BIN.
Subsequent builds reuse output-directory/system-libs. Use a new output directory
when building for another board image.
HELP
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if (( $# > 1 )); then usage >&2; exit 2; fi
root=$(cd "$(dirname "$0")" && pwd -P)
container=${K3_BUILD_CONTAINER:-ubuntu2204-x64}
sdk=${K3_SDK_NATIVE:-/opt/k3-camera/sdk/native}
out=${1:-"$root/build"}
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
case "$out" in
    "$root"|"$root"/bin|"$root"/bin/*|"$root"/src|"$root"/src/*|"$root"/vendor|"$root"/vendor/*)
        echo "Choose a separate build output directory." >&2; exit 2 ;;
esac

docker exec "$container" test -x "$sdk/llvm/bin/clang++" || {
    echo "K3 compiler unavailable in $container at $sdk/llvm/bin/clang++." >&2; exit 1;
}

stage=$(mktemp -d "$out/.k3-build.XXXXXX")
remote=''
cleanup() {
    if [[ -n "$remote" ]]; then docker exec "$container" rm -rf -- "$remote" >/dev/null || true; fi
    rm -rf -- "$stage"
}
trap cleanup EXIT

libs="$out/system-libs"
mkdir -p "$libs"
libpaths=(
    /system/lib64/libdm.z.so
    /system/lib64/platformsdk/libmmi-client.z.so
    /system/lib64/libaccessibleability.z.so
    /system/lib64/platformsdk/libaccessibility_common.z.so
    /system/lib64/chipset-sdk-sp/libsurface.z.so
    /system/lib64/chipset-sdk-sp/libutils.z.so
    /system/lib64/platformsdk/libnative_image.so
    /system/lib64/chipset-sdk-sp/libc++.so
)
hdc=''
serial=''
select_target() {
    hdc=${HDC_BIN:-}
    if [[ -z "$hdc" ]]; then
        if command -v hdc >/dev/null; then hdc=$(command -v hdc)
        else hdc='/Applications/鸿镜.app/Contents/Resources/hdc'
        fi
    fi
    if [[ ! -x "$hdc" ]]; then echo "Set HDC_BIN to the HDC executable." >&2; return 1; fi
    serial=${K3_TARGET:-}
    if [[ -z "$serial" ]]; then
        local targets
        targets=$("$hdc" list targets | tr -d '\r' | awk 'NF == 1 && $0 != "[Empty]"')
        if [[ -z "$targets" || "$targets" == *$'\n'* ]]; then
            echo "Connect one K3 board or select it with K3_TARGET." >&2; return 1
        fi
        serial=$targets
    fi
    local arch
    arch=$("$hdc" -t "$serial" shell 'uname -m' | tr -d '\r\n')
    if [[ "$arch" != riscv64 ]]; then echo "Expected a riscv64 OpenHarmony board." >&2; return 1; fi
}

for path in "${libpaths[@]}"; do
    name=${path##*/}
    if [[ ! -s "$libs/$name" ]]; then
        if [[ -z "$hdc" ]]; then select_target; fi
        echo "Reading $name from the K3"
        "$hdc" -t "$serial" file recv "$path" "$stage/$name" >/dev/null
        if [[ ! -f "$stage/$name" ]] || ! file -b "$stage/$name" | grep 'RISC-V' >/dev/null; then
            echo "Could not read a valid RISC-V library at $path." >&2; exit 1
        fi
        mv -f "$stage/$name" "$libs/$name"
    fi
done

remote=$(docker exec "$container" mktemp -d /tmp/k3-hongjing-build.XXXXXXXX)
docker cp "$root/src" "$container:$remote/src"
docker cp "$root/vendor" "$container:$remote/vendor"
docker exec "$container" mkdir "$remote/system-libs"
for path in "${libpaths[@]}"; do
    name=${path##*/}
    docker cp "$libs/$name" "$container:$remote/system-libs/$name"
done

docker exec -i "$container" bash -se -- "$remote" "$sdk" <<'COMPILE'
set -euo pipefail
cd "$1"
sdk=$2
readelf="$sdk/llvm/bin/llvm-readelf"
for lib in system-libs/*.so; do
    if ! "$readelf" -h "$lib" | grep 'Machine:.*RISC-V' >/dev/null; then
        echo "Not a RISC-V ELF library: $lib" >&2; exit 1
    fi
done
# The system libraries use the OpenHarmony platform libc++ ABI namespace __h.
# The application NDK uses a different namespace when left at its default.
cat > platform-cxx.h <<'CPP'
#include <__config_site>
#undef _LIBCPP_ABI_NAMESPACE
#define _LIBCPP_ABI_NAMESPACE __h
CPP
flags=(--target=riscv64-linux-ohos --sysroot="$sdk/sysroot" -std=c++17 -O2
       -fPIE -pie -D__OHOS__ -DROSEN_OHOS -DUSE_M133_SKIA -Ivendor
       -isystem "$sdk/sysroot/usr/include/asm-riscv"
       -isystem "$sdk/sysroot/usr/include/riscv64-linux-ohos" -include platform-cxx.h)
while IFS= read -r dir || [[ -n "$dir" ]]; do flags+=("-Ivendor/$dir"); done < vendor/include-dirs.txt
link=(-nostdlib++ -Wl,--as-needed -Wl,--allow-shlib-undefined)
cc="$sdk/llvm/bin/clang++"
# One set of geometry constants is used by the virtual display, screenshot
# bounds, input mapping, encoder and Hongjing protocol for each profile.
build_profile() {
    local prefix=$1
    shift
    local profile=("$@")
    local hwflags=(-O3 -march=rv64gcv)
    "$cc" "${flags[@]}" "${profile[@]}" src/main.cpp "${link[@]}" system-libs/libdm.z.so \
        system-libs/libsurface.z.so system-libs/libutils.z.so system-libs/libnative_image.so \
        system-libs/libc++.so -o "${prefix}k3-virtual-primary"
    for encoder in hw sw; do
        local encoder_flags=()
        if [[ $encoder == sw ]]; then encoder_flags+=(-DK3_SOFTWARE_ONLY); fi
        "$cc" "${flags[@]}" "${hwflags[@]}" "${profile[@]}" "${encoder_flags[@]}" -DWITH_LOCAL_INPUT \
            src/wire.cpp src/screenshot.cpp src/touch_backend.cpp src/focused_text.cpp "${link[@]}" \
            system-libs/libdm.z.so system-libs/libmmi-client.z.so system-libs/libaccessibleability.z.so \
            system-libs/libaccessibility_common.z.so system-libs/libutils.z.so system-libs/libc++.so \
            -o "${prefix}k3-${encoder}-wire"
    done
    "$cc" "${flags[@]}" "${hwflags[@]}" "${profile[@]}" src/screenshot.cpp src/pipe.cpp "${link[@]}" \
        system-libs/libdm.z.so system-libs/libutils.z.so system-libs/libc++.so -o "${prefix}k3-capture-pipe"
    "$cc" "${flags[@]}" "${hwflags[@]}" -fno-rtti "${profile[@]}" src/surface_capture.cpp src/screenshot.cpp "${link[@]}" \
        system-libs/libdm.z.so system-libs/libnative_image.so system-libs/libsurface.z.so \
        system-libs/libutils.z.so system-libs/libc++.so -o "${prefix}k3-surface-capture"
}
mkdir 1080p
build_profile ""
build_profile "1080p/" -DK3_FULL_HD
for executable in k3-virtual-primary k3-hw-wire k3-sw-wire k3-capture-pipe k3-surface-capture 1080p/*; do
    "$readelf" -h "$executable" | grep 'Machine:.*RISC-V'
    "$readelf" -l "$executable" | grep 'program interpreter: /lib/ld-musl-riscv64.so.1'
    "$readelf" -d "$executable" | grep '(NEEDED)'
done
COMPILE

mkdir -p "$stage/1080p" "$out/1080p"
programs=(k3-virtual-primary k3-hw-wire k3-sw-wire k3-capture-pipe k3-surface-capture
          1080p/k3-virtual-primary 1080p/k3-hw-wire 1080p/k3-sw-wire 1080p/k3-capture-pipe 1080p/k3-surface-capture)
for name in "${programs[@]}"; do
    docker cp "$container:$remote/$name" "$stage/$name"
done
for name in "${programs[@]}"; do mv -f "$stage/$name" "$out/$name"; done
echo "Built the K3 720p and 1080p programs in $out"
