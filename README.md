# ROS Command

A tiny command-line operating system written entirely in 16-bit x86 assembly (NASM).
It boots from a virtual hard disk (MBR + FAT16) and runs a built-in shell called **xsh**.

By **XR Stu** — <https://xrst.uk>

## Features

- Custom **MBR + FAT16 VBR** boot chain (no bootloader library)
- FAT16 filesystem with read/write and subdirectories (changes persist)
- xsh shell: `help`, `dir`, `doc`, `mkdir`, `touch`, `goto`, `echo`, `time`, `date`, `mem`, `net`, `ping` and more
- Command aliases: `cls`, `ls`, `cat`, `type`, `cd`, `pwd`, `whoami`
- **pck** package manager: `pck list` / `pck install <name>` / `pck remove <name>`
- **NE2000 (RTL8029) network driver**: PCI PIO, ARP + IPv4/ICMP, `ping 10.0.2.2`
- 32-bit protected mode demo (`pm`) that tests memory above 1 MB
- Runs in QEMU as a raw 32 MB hard disk image

## Requirements

- [NASM](https://www.nasm.us/), [mtools](https://www.gnu.org/software/mtools/), QEMU

```sh
# macOS
brew install nasm mtools qemu
```

## Build & Run

```sh
./build.sh    # assembles sources and creates build/ros.img
./run.sh      # boots the image in QEMU
```

## Packages

Packages (`.pck`) are published as release assets:

<https://github.com/AaronXieA/ROS/releases/tag/pck-packages>

Put a package in the `/PCK` directory of the disk image, then use `pck install` inside ROS.

## License

Personal educational project by XR Stu.
