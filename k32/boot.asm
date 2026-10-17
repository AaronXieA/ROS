; ============================================
; ROS Command - 32-bit kernel entry
; Entered in real mode at 0x1000:0000, switches
; to protected mode and calls kmain().
;
; Linked as ELF at 0x10000, so 16-bit code must
; use (_start-relative) offsets inside the segment.
; ============================================
bits 16

section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    cld

    ; ---- enable A20 (fast gate) ----
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    ; ---- conventional memory size ----
    int 0x12
    movzx eax, ax
    mov di, boot_conv_kb - _start
    mov [di], eax

    ; ---- E820 memory map ----
    mov si, boot_e820_count - _start
    mov dword [si], 0
    mov di, boot_e820 - _start
    xor ebx, ebx
.e820:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [di + 20], 1
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    cmp ecx, 20
    jb .e820_done
    inc dword [si]
    add di, 24
    cmp dword [si], 32
    jae .e820_done
    test ebx, ebx
    jnz .e820
.e820_done:

    ; ---- GDT descriptor: base = CS*16 + gdt offset ----
    mov di, gdt_desc - _start
    xor eax, eax
    mov ax, cs
    shl eax, 4
    mov bx, gdt_start - _start
    movzx ebx, bx
    add eax, ebx
    mov [di + 2], eax

    lgdt [di]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ; far jump with a 32-bit offset (flat code base is 0, so EIP
    ; must be the absolute link address 0x10000+)
    db 0x66, 0xFF, 0x2E
    dw pm_far_ptr - _start

align 4
pm_far_ptr:
    dd pm_entry
    dw 0x08

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9F000

    ; ---- zero .bss ----
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- GDT ----
align 8
gdt_start:
    dq 0                        ; null
    dw 0xFFFF                   ; 0x08: 32-bit code, flat 4 GB
    dw 0x0000
    db 0
    db 0x9A
    db 0xCF
    db 0
    dw 0xFFFF                   ; 0x10: 32-bit data, flat 4 GB
    dw 0x0000
    db 0
    db 0x92
    db 0xCF
    db 0
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd 0

; ---- boot info (kept out of .bss, filled before zeroing) ----
section .data
global boot_e820
global boot_e820_count
global boot_conv_kb
align 4
boot_e820:       times 24*32 db 0
boot_e820_count: dd 0
boot_conv_kb:    dd 0
