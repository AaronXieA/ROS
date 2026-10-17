; ============================================
; ROS Command - VBR (FAT16 partition boot sector)
; By XR Stu - https://xrst.uk
;
; Loaded by the MBR at 0x7C00 (partition LBA 2048).
; Parses its own FAT16 BPB, reads geometry and the
; partition base from the boot info block at 0x0500
; (written by the MBR), then finds KERNEL.BIN in the
; root directory and chain-loads it to 0x1000:0000.
; ============================================

[BITS 16]
[ORG 0x7C00]

jmp short vbr_start
nop

; ---- BIOS Parameter Block (FAT16) ----
OEMName       db "ROSCMD  "
BytesPerSec   dw 512
SecPerClus    db 4
ResvdSectors  dw 1
NumFATs       db 2
RootEntries   dw 512
TotalSectors  dw 0
MediaDesc     db 0xF8
SecPerFAT     dw 63             ; placeholder, fixed by mformat
SecPerTrack   dw 63
NumHeads      dw 16
HiddenSec     dd 2048
TotalSec32    dd 60480
DriveNumber   db 0x80
Reserved1     db 0
BootSignature db 0x29
VolumeID      dd 0x524F5332
VolumeLabel   db "ROS COMMAND"
FileSysType   db "FAT16   "

%define FAT_SEG    0x0800       ; FAT buffer (phys 0x8000, up to 32KB)
%define DIR_SEG    0x1000       ; root dir buffer (phys 0x10000, transient)
%define KERNEL_SEG 0x1000
%define INFO_OFF   0x0500       ; boot info block (from MBR)

vbr_start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    ; ---- read boot info block ----
    mov ax, [INFO_OFF]          ; partition base LBA
    mov [part_base], ax
    mov ax, [INFO_OFF+4]
    mov [geo_spt], ax
    mov ax, [INFO_OFF+6]
    mov [geo_heads], ax

    ; ---- parse BPB ----
    movzx ax, byte [SecPerClus]
    mov [sec_per_clus], ax
    mov ax, [ResvdSectors]
    add ax, [part_base]
    mov [fat_lba], ax           ; fat_lba = part_base + reserved
    mov ax, [RootEntries]
    shr ax, 4
    mov [root_secs], ax         ; root_secs = entries * 32 / 512
    mov ax, [SecPerFAT]
    mov [sec_per_fat], ax
    movzx bx, byte [NumFATs]
    mul bx                      ; ax = nfats * spf
    add ax, [fat_lba]
    mov [root_lba], ax          ; root_lba = fat_lba + nfats*spf
    add ax, [root_secs]
    mov [data_lba], ax          ; data_lba = root_lba + root_secs

    ; ---- load FAT: sec_per_fat sectors -> FAT_SEG ----
    mov ax, FAT_SEG
    mov es, ax
    xor bx, bx
    mov ax, [fat_lba]
    mov cx, [sec_per_fat]
    call read_sectors

    ; ---- load root directory -> DIR_SEG ----
    mov ax, DIR_SEG
    mov es, ax
    xor bx, bx
    mov ax, [root_lba]
    mov cx, [root_secs]
    call read_sectors

    ; ---- scan root dir for "KERNEL  BIN" ----
    xor bx, bx
.scan:
    cmp bx, [RootEntries]
    jae not_found
    mov di, bx
    shl di, 5
    cmp byte [es:di], 0
    je not_found
    mov si, kernel_name
    mov cx, 11
.cmp:
    mov al, [si]
    inc si
    cmp al, [es:di]
    jne .next
    inc di
    loop .cmp
    jmp found
.next:
    inc bx
    jmp .scan

found:
    mov di, bx
    shl di, 5
    mov ax, [es:di+26]          ; first cluster

    ; ---- follow FAT16 chain, load kernel ----
.load_loop:
    push ax
    sub ax, 2
    mul word [sec_per_clus]     ; (cluster-2)*spc, fits in AX
    add ax, [data_lba]
    mov dx, [dest_seg]
    mov es, dx
    mov bx, [dest_off]
    mov cx, [sec_per_clus]
    call read_sectors
    ; advance dest by spc*512 (kernel < 64KB, offset stays in range)
    mov ax, [sec_per_clus]
    shl ax, 8                   ; spc * 256 ... need *512
    shl ax, 1
    add [dest_off], ax

    pop ax                      ; current cluster
    mov bx, ax
    shl bx, 1                   ; FAT16: offset = cluster * 2
    mov dx, FAT_SEG
    mov es, dx
    mov bx, [es:bx]
    mov ax, bx
    cmp ax, 0xFFF8
    jb .load_loop

    mov dl, [boot_drive]
    jmp KERNEL_SEG:0x0000

; --------------------------------------------
; read_sectors: AX = start LBA, CX = count, ES:BX = dest
; One sector at a time via CHS, geometry from boot info.
; --------------------------------------------
read_sectors:
.rd:
    push cx
    push ax
    xor dx, dx
    div word [geo_spt]          ; AX = track, DX = LBA % spt
    inc dl
    mov cl, dl                  ; sector
    xor dx, dx
    div word [geo_heads]        ; AX = cylinder, DX = head
    mov dh, dl
    mov ch, al
    mov ax, 0x0201
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    pop ax
    pop cx
    add bx, 512
    inc ax
    loop .rd
    ret

disk_error:
    mov si, msg_disk
    jmp print_hang
not_found:
    mov si, msg_nokernel
print_hang:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_hang
.hang:
    cli
    hlt
    jmp .hang

boot_drive   db 0
geo_spt      dw 0
geo_heads    dw 0
part_base    dw 0
sec_per_clus dw 0
sec_per_fat  dw 0
root_secs    dw 0
fat_lba      dw 0
root_lba     dw 0
data_lba     dw 0
kernel_name  db "KERNEL  BIN"
dest_seg     dw KERNEL_SEG
dest_off     dw 0
msg_disk     db "VBR DISK ERR", 13, 10, 0
msg_nokernel db "VBR NO KERNEL", 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55
