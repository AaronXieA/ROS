#!/bin/sh
# Run ROS Command in QEMU, with the local package / update hub on 10.0.2.2:8000.
cd "$(dirname "$0")"
[ -f build/ros.img ] || ./build.sh

hub=
if [ "${PCKHUB:-1}" = "1" ] && command -v python3 >/dev/null 2>&1; then
    python3 tools/pckhub.py &
    hub=$!
fi
trap '[ -n "$hub" ] && kill "$hub" 2>/dev/null' EXIT INT TERM

qemu-system-x86_64 -rtc base=localtime -drive format=raw,file=build/ros.img \
    -netdev user,id=n0 -device ne2k_pci,netdev=n0
