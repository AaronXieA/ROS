# ROS Command

A tiny command-line operating system: a **32-bit protected-mode kernel written in C**,
booted by a custom MBR + FAT16 VBR, with a built-in shell called **xsh**.

By **XR Stu** — <https://xrst.uk>

> **A note on history:** the first releases (v0.5–v0.9) were a pure-assembly 16-bit
> real-mode kernel. Since v0.10.0 the kernel has been rewritten in 32-bit C and the
> system has grown paging, ring-3 user mode, preemptive scheduling, multi-user
> accounts, TCP/IP and a package manager. The old kernel is kept for reference in
> `legacy/kernel16.asm`.

## Contents

[Features](#features) · [How it boots](#how-it-boots) · [Requirements](#requirements) ·
[Build & Run](#build--run) · [Source layout](#source-layout) ·
[Shell operators](#shell-operators) · [Packages](#packages) · [DocEditor](#doceditor) ·
[System update](#system-update) · [Versioning](#versioning) · [License](#license)

## Features

- Custom **MBR + FAT16 VBR** boot chain (no bootloader library)
- **32-bit protected-mode C kernel** (freestanding, no libc) with a small NASM entry stub
- **Paging and virtual memory**: 4 KB physical frame allocator, per-process page
  directories, kernel identity map, user mappings at `0x40000000`
- **Ring-3 user mode** with `int 0x80` system calls (`write`, `exit`, `getpid`,
  `yield`, `getchar`, `clear`, `putat`, `setcursor`, `readfile`, `writefile`,
  `args`, `brk`)
- **User-space heap**: per-process `sbrk`/`malloc`/`free`, multi-megabyte
  allocations backed by on-demand page mapping (`user/rt.h`)
- **Simplified JVM**: `jvm <file.cls>` runs a Java 8 class file — constant
  pool / field / method parsing, ~60 bytecodes (const, load/store, iaload,
  iadd, if_icmp*, invoke{static,virtual,special}, new, newarray, getstatic),
  native stubs for `System.out.println`, and a conservative mark-sweep GC
- **DocEditor (`doce`)**: a full-screen ring-3 text editor with arrow keys,
  line editing, load/save and an unsaved-changes guard
- **Droppable commands**: a bare command name runs `<NAME>.BIN` from the current
  directory, so `pck` packages can add programs (`pck install doce` → `doce A.TXT`)
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
- xsh shell: `help`, `ver`, `about`, `dir`, `doc`, `mkdir`, `touch`, `goto`, `echo`,
  `time`, `date`, `mem`, `net`, `ping`, `pck`, `alias`, `run`, `jvm`, `doce`, `ps`,
  `kill`, `vm`, `update`, `reboot`, `halt` and more
- **Shell operators**: output redirection `>` / `>>`, input redirection `<`,
  and pipes `|` (up to 4 stages); built-in filters `grep`, `head`, `wc`
- **Paged output**: `help` and `doc` pause every 20 lines (any key: next,
  `q` / ESC: quit); pipes and redirection are never paged
- **Multi-user accounts**: `cru <user>` to create, `cu <user>` to switch,
  `passwd <user>` to set a password (stored hashed in `/USER.DAT`)
- Command aliases: `cls`, `ls`, `cat`, `type`, `cd`, `pwd`, `whoami`
- **pck** package manager: `pck list` / `pck install <name>` / `pck remove <name>`
- Runs in QEMU as a raw 32 MB hard disk image

## How it boots

No bootloader library is used — every stage is hand-written:

```
BIOS
 └─ MBR (sector 0)   mbr.asm       locates the FAT16 partition, loads its VBR
     └─ VBR (LBA 2048)  vbr.asm    finds KERNEL.BIN in the root directory and
                                   loads it to 0x1000:0000
         └─ k32/boot.asm           16-bit entry: A20, E820 memory map, GDT,
                                   then jumps to 32-bit protected mode
             └─ k32/kernel.c       C kernel linked at physical 0x10000: sets up
                                   the IDT/PIC, paging, scheduler, then starts xsh
```

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

`run.sh` keeps your guest files on a persistent data disk: on first run it
copies `build/ros.img` to `build/disk.img` and boots from that, so rebuilding
the kernel never wipes data created inside the guest. Use `FRESH=1 ./run.sh`
to rebase the data disk from a freshly built image.

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
| `user/doce.c`, `user/user.ld` | DocEditor source and its `0x40000000` linker script |
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

Installed files land in the current directory, and a bare command name runs
`<NAME>.BIN` from there. That is how packages add commands:

```sh
pck install doce     # installs DOCE.BIN
doce NOTES.TXT       # open (or create) NOTES.TXT in DocEditor
```

## DocEditor

`doce` is a full-screen text editor written in C and built as a ring-3
program (`user/doce.c`). It talks to the kernel only through `int 0x80`, so
it lives in a `DOCE.BIN` that `pck install` can drop on any v0.13.2+ system.

| Key | Action |
| --- | --- |
| Arrows, Home, End, PgUp, PgDn | move the cursor |
| Enter / Backspace / Delete | edit the document |
| Tab | insert spaces to the next 4-column stop |
| Ctrl+S | save |
| Ctrl+Q | quit (press twice if there are unsaved changes) |

Files use 8.3 names and can hold up to 100 lines × 78 columns (about 8 KB).
`doce` without a file name starts with `UNTITLED.TXT`.

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

If GitHub is temporarily unreachable, the hub falls back to the last image it
cached in `build/pckcache`.

> **Updating from 0.13.3 or older:** those builds have a broken `update`
> command (their 32 KB download buffer overlapped the kernel stack and crashed
> with `#UD` at `CS:EIP = 0x8:0x3`). Flash the Alpha v0.13.4 image manually
> once; `update` works from 0.13.4 onwards.

For a local test without publishing a release, point the hub at an image:

```sh
ROS_IMAGE=build/ros.img ROS_VERSION=0.13.4-test ./run.sh
```

## Versioning

- Major features (new subsystems): bump the minor number, e.g. `0.12.0` → `0.13.0`
- Small features and fixes: bump the patch number, e.g. `0.12.0` → `0.12.1`
- Every version is published as a GitHub Release; pre-release builds use a
  `-pre` / `pre` suffix (e.g. `0.12.0pre`)
- Small releases are intentionally kept infrequent

## License

Personal educational project by XR Stu.
