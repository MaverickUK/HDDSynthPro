#!/usr/bin/env bash
# Boots a copy of the benchmark image in QEMU (in Docker: Homebrew has no
# QEMU for macOS 14) for SECONDS (default 75), then prints its C:\BENCH.LOG.
# Usage: tools/bench-image/test_qemu.sh [cpu, default 486] [seconds]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cpu="${1:-486}"
seconds="${2:-75}"
[ -f "$here/build/DOSBENCH.IMG" ] || python3 "$here/build_image.py"
docker build -q -t hddsynth-qemu "$here/qemu" >/dev/null
cp "$here/build/DOSBENCH.IMG" "$here/build/qemu-test.img"
(sleep "$seconds"; echo quit) | docker run --rm -i -v "$here/build":/w hddsynth-qemu \
    qemu-system-i386 -cpu "$cpu" -m 8 -drive file=/w/qemu-test.img,format=raw,if=ide,index=0 \
    -boot c -display none -monitor stdio >/dev/null 2>&1
mtype -i "$here/build/qemu-test.img@@32256" ::BENCH.LOG
rm -f "$here/build/qemu-test.img"
