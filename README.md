# ROS Command

A tiny command-line operating system: a **32-bit protected-mode kernel written in C**,
booted by a custom MBR + FAT16 VBR, with a built-in shell called **xsh**.

By **XR Stu** — <https://xrst.uk>

## Features

- Custom **MBR + FAT16 VBR** boot chain (no bootloader library)
- **32-bit protected-mode C kernel** (freestanding, no libc) with a small NASM entry stub
- **Paging and virtual memory**: 4 KB physical frame allocator, per-process page
  directories, kernel identity map, user mappings at `0x40000000`
- **Ring-3 user mode** with `int 0x80` system calls (`write`, `exit`, `getpid`, `yield`)
- **Preemptive round-robin scheduler** driven by the PIT, with independent kernel
  stacks, TSS-based ring transitions and per-process address spaces
- FAT16 filesystem with read/write and subdirectories (changes persist)
- ATA PIO disk driver, PS/2 keyboard, PIT timer, VGA text console
- RTC time/date (read and set), E820 memory report, CPU exception screen
- **NE2000 (RTL8029) network driver**: PCI PIO, ARP + IPv4/ICMP, `ping 10.0.2.2`
- xsh shell: `help`, `dir`, `doc`, `mkdir`, `touch`, `goto`, `echo`, `time`, `date`,
  `mem`, `net`, `ping`, `pck`, `alias`, `run`, `ps`, `kill`, `vm` and more
- **Multi-user accounts**: `cru <user>` to create, `cu <user>` to switch,
  `passwd <user>` to set a password (stored hashed in `/USER.DAT`)
- Command aliases: `cls`, `ls`, `cat`, `type`, `cd`, `pwd`, `whoami`
- **pck** package manager: `pck list` / `pck install <name>` / `pck remove <name>`
- Runs in QEMU as a raw 32 MB hard disk image

## Requirements

- [NASM](https://www.nasm.us/), [mtools](https://www.gnu.org/software/mtools/), QEMU
- clang (i386-elf target), LLVM `lld`, `llvm-objcopy`

```sh
# macOS
brew install nasm mtools qemu lld llvm
```

## Build & Run

```sh
./build.sh    # assembles the boot chain and builds the C kernel -> build/ros.img
./run.sh      # boots the image in QEMU (user-mode networking, NE2000 NIC)
```

The kernel is compiled freestanding with
`clang -target i386-elf -m32 -ffreestanding ...`, linked at physical `0x10000`
by `ld.lld`, and flattened with `llvm-objcopy`.

## Source layout

| Path | Contents |
| --- | --- |
| `mbr.asm`, `vbr.asm` | 16-bit boot chain (MBR + FAT16 VBR) |
| `k32/boot.asm` | real-mode entry: A20, E820, GDT, jump to protected mode |
| `k32/isr.asm` | interrupt stubs |
| `k32/kernel.c` | shell and command dispatch |
| `k32/console.c`, `keyboard.c`, `timer.c`, `idt.c` | VGA, PS/2, PIT, IDT/PIC |
| `k32/ata.c`, `fat.c` | ATA PIO driver and FAT16 |
| `k32/rtc.c`, `pck.c`, `net.c` | clock, packages, NE2000 + ARP/ICMP |
| `k32/paging.c`, `sched.c`, `syscall.c` | paging/VM, scheduler + TSS, `int 0x80` |
| `user/user.asm` | demo ring-3 program loaded as `/USER.BIN` |
| `legacy/kernel16.asm` | previous 16-bit assembly kernel (reference) |

## Packages

`.pck` packages are published as release assets:

<https://github.com/AaronXieA/ROS/releases/tag/pck-packages>

Put a package in the `/PCK` directory of the disk image, then `pck install <name>`.

## Versioning

- Major features (new subsystems): bump the minor number, e.g. `0.12.0` → `0.13.0`
- Small features and fixes: bump the patch number, e.g. `0.12.0` → `0.12.1`
- Every version is published as a GitHub Release; pre-release builds use a
  `-pre` / `pre` suffix (e.g. `0.12.0pre`)
- Small releases are intentionally kept infrequent

## License

Personal educational project by XR Stu.
