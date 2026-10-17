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
- **TCP + HTTP/1.1 client**: `pck search` / `pck install` download packages from
  GitHub Releases through a small host-side hub (`tools/pckhub.py`)
- **System update**: `update` downloads the newest release image and flashes it
  onto the disk in place, then asks you to reboot
- xsh shell: `help`, `dir`, `doc`, `mkdir`, `touch`, `goto`, `echo`, `time`, `date`,
  `mem`, `net`, `ping`, `pck`, `alias`, `run`, `ps`, `kill`, `vm` and more
- **Shell operators**: output redirection `>` / `>>`, input redirection `<`,
  and pipes `|` (up to 4 stages); built-in filters `grep`, `head`, `wc`
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

## Shell operators

```sh
echo hello > A.TXT       # write output to a file (overwrite)
echo world >> A.TXT      # append output to a file
doc < A.TXT              # feed a file to a command's stdin
dir | grep txt           # pipe output into another command
dir | grep txt | wc      # chain up to 4 stages
doc A.TXT | head 1
```

`grep`, `head` and `wc` read from stdin when used in a pipe or with `<`.
Pipes and redirection use two 32 KB kernel buffers, so a single stream is
limited to 32 KB.

## Packages

`.pck` packages are published as release assets:

<https://github.com/AaronXieA/ROS/releases/tag/pck-packages>

```sh
pck search            # list every package in the release
pck search demo       # filter by keyword
pck install demo      # use /PCK/DEMO.PCK, or download it from the release
pck remove demo       # delete the files that the package installed
```

`run.sh` starts `tools/pckhub.py` on the host (guest address `10.0.2.2:8000`).
The hub fetches the release asset list from the GitHub API and downloads
`.PCK` files on demand into `build/pckcache`, serving them to the kernel over
plain HTTP. GitHub itself only speaks HTTPS and the kernel has no TLS stack,
which is why this small bridge exists. Start QEMU with `PCKHUB=0 ./run.sh` to
skip the hub, or put a `.PCK` in the `/PCK` directory yourself.

## System update

```sh
update
```

The command asks the hub for the newest release that ships a `ros-*.img.gz`
(or `.img`) asset, compares its version with the running one and, if it is
newer, downloads the raw 32 MB image and writes it straight onto the disk
(the image is decompressed by the hub). The MBR signature is checked before
the first write, the final size must match, and `reboot` is required
afterwards.

For a local test without publishing a release, point the hub at an image:

```sh
ROS_IMAGE=build/ros.img ROS_VERSION=0.13.1-test ./run.sh
```

## Versioning

- Major features (new subsystems): bump the minor number, e.g. `0.12.0` → `0.13.0`
- Small features and fixes: bump the patch number, e.g. `0.12.0` → `0.12.1`
- Every version is published as a GitHub Release; pre-release builds use a
  `-pre` / `pre` suffix (e.g. `0.12.0pre`)
- Small releases are intentionally kept infrequent

## License

Personal educational project by XR Stu.
