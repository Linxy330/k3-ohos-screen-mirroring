#!/bin/bash
set -eu
cd "$(dirname "$0")"
if (( $# )); then exec /bin/bash tools/switch.sh "$@"; fi

echo '选择投屏分辨率'
echo '  1) 1080p（默认）'
echo '  2) 720p'
while :; do
    printf '选择 [1/2]，或输入 q 退出：'
    IFS= read -r choice || exit 0
    case "$choice" in
        ""|1) size=1080p; break;;
        2) size=720p; break;;
        q|Q) exit 0;;
        *) echo '请输入 1、2 或 q。';;
    esac
done

echo '选择编码方式'
echo '  1) 硬件编码（默认）'
echo '  2) 软件编码'
while :; do
    printf '选择 [1/2]，或输入 q 退出：'
    IFS= read -r choice || exit 0
    case "$choice" in
        ""|1) codec=hw; break;;
        2) codec=sw; break;;
        q|Q) exit 0;;
        *) echo '请输入 1、2 或 q。';;
    esac
done
exec /bin/bash tools/switch.sh "$size" "$codec"
