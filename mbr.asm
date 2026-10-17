; ============================================
; ROS Command - MBR (virtual hard disk)
; By XR Stu - https://xrst.uk
;
; Loaded by BIOS at 0x7C00. Relocates itself to
; 0x0600, queries disk geometry (int13 AH=08h),
; stores a boot info block at physical 0x0500,
; finds the active partition, loads its VBR to
; 0x7C00 and jumps to it.
;
; Boot info block (physical 0x0500, ES=0):
;   +0  dw  partition base LBA (low word)
;   +2  dw  zero (high word, always 0)
;   +4  dw  sectors per track
;   +6  dw  number of heads
;   +8  db  boot drive
; ============================================

[BITS 16]
[ORG 0x7C00]

%define RELOC      0x0600
%define INFO_SEG   0x0000
%define INFO_OFF   0x0500

; absolute data refs after relocation need this macro
%define R(x) (x - 0x7C00 + RELOC)

mbr_start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; ---- relocate ourselves to 0x0600 ----
    mov si, 0x7C00
    mov di, RELOC
    mov cx, 256
    rep movsw
    jmp 0x0060:(main - 0x7C00)

main:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [R(boot_drive)], dl     ; store AFTER relocation (copy would overwrite)

    ; geometry is fixed by our crafted partition table
    ; (SeaBIOS derives 16 heads / 63 spt from the end CHS)
    mov word [R(geo_spt)], 63
    mov word [R(geo_heads)], 16

    ; ---- scan partition table for active entry ----
    mov si, RELOC + 0x01BE      ; partition table (relocated)
    mov cx, 4
.scan:
    cmp byte [si], 0x80
    je .found
    add si, 16
    loop .scan
    jmp no_partition

.found:
    ; partition base LBA (must fit in 16 bits)
    mov ax, [si+8]              ; low word of LBA start
    mov [R(part_base)], ax
    cmp word [si+10], 0         ; high word must be 0
    jne disk_error

    ; ---- write boot info block at 0x0500 ----
    mov di, INFO_OFF
    mov [es:di], ax             ; +0 part_base
    mov word [es:di+2], 0       ; +2 zero
    mov cx, [R(geo_spt)]
    mov [es:di+4], cx           ; +4 spt
    mov cx, [R(geo_heads)]
    mov [es:di+6], cx           ; +6 heads
    mov dl, [R(boot_drive)]
    mov [es:di+8], dl           ; +8 drive

    ; ---- load VBR (1 sector at partition base) to 0x7C00 ----
    ; LBA -> CHS using queried geometry
    xor dx, dx
    div word [R(geo_spt)]       ; AX = track, DX = sector-1
    inc dl
    mov cl, dl                  ; sector
    xor dx, dx
    div word [R(geo_heads)]     ; AX = cylinder, DX = head
    mov dh, dl                  ; head
    mov ch, al                  ; cylinder (low 8 bits)
    mov al, ah                  ; cylinder bits 8-9 -> CL bits 6-7
    and al, 0x03
    shl al, 6
    or cl, al
    mov ax, 0x0201              ; read 1 sector
    mov bx, 0x7C00
    mov dl, [R(boot_drive)]
    int 0x13
    jc disk_error

    mov dl, [R(boot_drive)]
    jmp 0x0000:0x7C00

disk_error:
    mov si, R(msg_disk)
    jmp print_hang
no_partition:
    mov si, R(msg_nopart)
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

boot_drive db 0
geo_spt    dw 0
geo_heads  dw 0
part_base  dw 0
msg_disk   db "MBR DISK ERR", 13, 10, 0
msg_nopart db "MBR NO PART", 13, 10, 0

; ---- partition table (hand-crafted) ----
; entry 0: bootable FAT16, LBA 2048, size 60480 sectors.
; end CHS head=15/sec=63/cyl=59 so SeaBIOS picks 16 heads, 63 spt.
times 446-($-$$) db 0
    db 0x80                     ; boot flag
    db 0xFE, 0xFF, 0xFF         ; start CHS (maxed, LBA used)
    db 0x06                     ; type: FAT16
    db 15, 63, 59               ; end CHS
    dd 2048                     ; start LBA
    dd 60480                    ; size in sectors
    times 16*3 db 0             ; entries 1-3 empty
    dw 0xAA55
