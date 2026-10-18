#!/bin/sh
# Run ROS Command in QEMU, with the local package / update hub on 10.0.2.2:8000.
#
# Persistent data disk: build/disk.img is copied from build/ros.img on first
# run and reused afterwards, so rebuilding the kernel does not wipe guest
# files. Use `FRESH=1 ./run.sh` to rebase the data disk from build/ros.img.
cd "$(dirname "$0")"
[ -f build/ros.img ] || ./build.sh

disk=build/disk.img
if [ "${FRESH:-0}" = "1" ]; then
    rm -f "$disk"
fi
if [ ! -f "$disk" ]; then
    cp build/ros.img "$disk"
    echo "ROS: data disk $disk created (FRESH=1 ./run.sh resets it)"
elif [ build/ros.img -nt "$disk" ]; then
    echo "ROS: ============================================================"
    echo "ROS: WARNING: $disk still contains an older system image."
    echo "ROS:          You are NOT testing the freshly built kernel."
    echo "ROS: Run 'FRESH=1 ./run.sh' to replace it (guest files are lost),"
    echo "ROS: or delete $disk before booting."
    echo "ROS: ============================================================"
fi

hub=
if [ "${PCKHUB:-1}" = "1" ] && command -v python3 >/dev/null 2>&1; then
    python3 tools/pckhub.py &
    hub=$!
fi
trap '[ -n "$hub" ] && kill "$hub" 2>/dev/null' EXIT INT TERM

qemu-system-x86_64 -rtc base=localtime -drive format=raw,file="$disk" \
    -netdev user,id=n0 -device ne2k_pci,netdev=n0
