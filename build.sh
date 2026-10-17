#!/bin/sh
# ROS Command build script
# Builds a ~30MB virtual hard disk image:
#   sector 0    = mbr.asm (MBR)
#   partition   = FAT16 (LBA 2048) with VBR
#   KERNEL.BIN  = kernel
#   files/*     = extra files copied into the partition
set -e
cd "$(dirname "$0")"

export MTOOLS_SKIP_CHECK=1

mkdir -p build

nasm -f bin mbr.asm  -o build/mbr.bin
nasm -f bin vbr.asm  -o build/vbr.bin
nasm -f bin kernel.asm -o build/kernel.bin

for f in build/mbr.bin build/vbr.bin; do
    SZ=$(stat -f%z "$f")
    if [ "$SZ" -ne 512 ]; then
        echo "error: $f is $SZ bytes, must be 512" >&2
        exit 1
    fi
done

# blank ~30MB raw image (62528 sectors = 32,014,336 bytes)
dd if=/dev/zero of=build/ros.img bs=512 count=62528 2>/dev/null

# install MBR at offset 0
dd if=build/mbr.bin of=build/ros.img bs=512 count=1 conv=notrunc 2>/dev/null

# format partition starting at LBA 2048 (1048576 bytes offset) as FAT16
# (-r is root dir size in SECTORS: 32 sectors = 512 entries)
PARTOFF=$((2048 * 512))
mformat -i build/ros.img@@${PARTOFF} -h 16 -s 63 -c 4 -r 32 -B build/vbr.bin ::

# copy kernel and sample files into the partition
mcopy -i build/ros.img@@${PARTOFF} -o build/kernel.bin ::KERNEL.BIN

for f in files/*; do
    if [ -d "$f" ]; then
        d=$(basename "$f" | tr 'a-z' 'A-Z')
        mmd -i build/ros.img@@${PARTOFF} "::$d" 2>/dev/null || true
        for g in "$f"/*; do
            [ -f "$g" ] || continue
            upper=$(basename "$g" | tr 'a-z' 'A-Z')
            mcopy -i build/ros.img@@${PARTOFF} -o "$g" "::$d/$upper"
        done
    elif [ -f "$f" ]; then
        upper=$(basename "$f" | tr 'a-z' 'A-Z')
        mcopy -i build/ros.img@@${PARTOFF} -o "$f" "::$upper"
    fi
done

# pck packages: each pkgsrc/<name>/ directory becomes /PCK/<NAME>.PCK
if [ -d pkgsrc ]; then
    mmd -i build/ros.img@@${PARTOFF} "::PCK" 2>/dev/null || true
    for d in pkgsrc/*; do
        [ -d "$d" ] || continue
        name=$(basename "$d" | tr 'a-z' 'A-Z')
        sh tools/mkpck.sh "build/$name.PCK" "$d"/* >/dev/null
        mcopy -i build/ros.img@@${PARTOFF} -o "build/$name.PCK" "::PCK/$name.PCK"
    done
fi

echo "OK: build/ros.img (kernel $(stat -f%z build/kernel.bin) bytes)"
