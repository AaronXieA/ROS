#!/bin/sh
# Run ROS Command in QEMU
cd "$(dirname "$0")"
[ -f build/ros.img ] || ./build.sh
exec qemu-system-x86_64 -drive format=raw,file=build/ros.img
