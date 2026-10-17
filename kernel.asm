; ============================================
; ROS Command - Kernel
; By XR Stu - https://xrst.uk
;
; 16-bit real mode command-line kernel.
; Loaded at 0x1000:0000 by the FAT16 VBR.
; FAT16 driver (virtual hard disk) with read/write
; and subdirectories.
; ============================================

[BITS 16]
[ORG 0x0000]

%define KERNEL_SEG  0x1000
%define FILE_SEG    0x3000    ; file load buffer (doc)
%define BUF_SIZE    128
%define PATH_SIZE   96
%define VERSION     "0.7.0"
%define MAX_FAT_SEC 64        ; fat_buf holds up to 64 sectors (32KB)

; GDT selectors (descriptors are flat with base 0x10000,
; matching where this ORG 0 kernel is physically loaded)
%define PM32_CODE   0x08
%define PM32_DATA   0x10
%define PM16_CODE   0x18
%define PM16_DATA   0x20
%define HIGH_1MB    (0x100000 - 0x10000)   ; physical 1 MB
%define HIGH_16MB   (0x1000000 - 0x10000)  ; physical 16 MB

start:
    mov ax, KERNEL_SEG
    mov ds, ax
    mov es, ax
    mov [boot_drive], dl        ; drive passed from bootloader (after DS is set!)
    mov ax, 0x9000              ; stack segment
    mov ss, ax
    mov sp, 0xFFFE

    call init_fs                ; parse BPB + load FAT into fat_buf
    call detect_ram             ; E820 memory map + prepare GDT pointer

    call clear_screen
    mov si, banner
    call print

main_loop:
    mov si, txt_shell           ; "xsh:ROS"
    call print
    cmp byte [cur_path+1], 0    ; at root ("/") -> no path in prompt
    je .no_path
    mov al, ' '
    call print_char
    mov si, cur_path
    call print
.no_path:
    mov si, prompt
    call print
    call read_line
    mov si, newline
    call print

    ; skip leading spaces
    mov si, cmd_buf
.skip_spaces:
    lodsb
    cmp al, ' '
    je .skip_spaces
    dec si
    cmp byte [si], 0
    je main_loop                ; empty input -> new prompt

    call dispatch
    jmp main_loop

; --------------------------------------------
; Command dispatch
; --------------------------------------------
dispatch:
    mov si, name_help
    call match
    je .help
    mov si, name_about
    call match
    je .about
    mov si, name_ver
    call match
    je .ver
    mov si, name_clear
    call match
    je .clear
    mov si, name_cls            ; alias
    call match_a
    je .clear
    mov si, name_echo
    call match
    je .echo
    mov si, name_dir
    call match
    je .dir
    mov si, name_ls             ; alias
    call match_a
    je .dir
    mov si, name_doc
    call match
    je .doc
    mov si, name_cat            ; alias
    call match_a
    je .doc
    mov si, name_type           ; alias
    call match_a
    je .doc
    mov si, name_mkdir
    call match
    je .mkdir
    mov si, name_touch
    call match
    je .touch
    mov si, name_goto
    call match
    je .goto
    mov si, name_cd             ; alias
    call match_a
    je .goto
    mov si, name_alias
    call match
    je .alias
    mov si, name_where
    call match
    je .where
    mov si, name_pwd            ; alias
    call match_a
    je .where
    mov si, name_who
    call match
    je .who
    mov si, name_whoami         ; alias
    call match_a
    je .who
    mov si, name_time
    call match
    je .time
    mov si, name_date
    call match
    je .date
    mov si, name_mem
    call match
    je .mem
    mov si, name_pck
    call match
    je .pck
    mov si, name_pm
    call match
    je .pm
    mov si, name_reboot
    call match
    je .reboot
    mov si, name_halt
    call match
    je .halt

    ; no main command matched - check if it's a disabled alias
    mov si, name_cls
    call match
    je .alias_off
    mov si, name_ls
    call match
    je .alias_off
    mov si, name_type
    call match
    je .alias_off
    mov si, name_cat
    call match
    je .alias_off
    mov si, name_cd
    call match
    je .alias_off
    mov si, name_pwd
    call match
    je .alias_off
    mov si, name_whoami
    call match
    je .alias_off

    mov si, msg_unknown
    call print
    ret
.alias_off:
    mov si, txt_alias_off
    call print
    ret
.help:
    call cmd_help
    ret
.about:
    call cmd_about
    ret
.ver:
    call cmd_ver
    ret
.clear:
    call clear_screen
    ret
.echo:
    call cmd_echo
    ret
.dir:
    call cmd_dir
    ret
.doc:
    call cmd_doc
    ret
.mkdir:
    call cmd_mkdir
    ret
.touch:
    call cmd_touch
    ret
.goto:
    call cmd_goto
    ret
.alias:
    call cmd_alias
    ret
.where:
    call cmd_where
    ret
.who:
    call cmd_who
    ret
.time:
    call cmd_time
    ret
.date:
    call cmd_date
    ret
.mem:
    call cmd_mem
    ret
.pck:
    call cmd_pck
    ret
.pm:
    call cmd_pm
    ret
.reboot:
    call cmd_reboot
    ret
.halt:
    call cmd_halt
    ret

; --------------------------------------------
; match: compare null-terminated name at SI
; with first word of cmd_buf.
; Sets ZF=1 on match, ZF=0 otherwise.
; --------------------------------------------
match:
    push di
    mov di, cmd_buf
.next:
    lodsb                       ; AL = name char
    mov ah, [di]
    inc di
    cmp ah, ' '
    je .input_end
    cmp ah, 0
    je .input_end
    cmp al, ah
    jne .no_match
    test al, al
    jz .no_match
    jmp .next
.input_end:
    test al, al                 ; name must end too
    jnz .no_match
    pop di
    xor ax, ax                  ; ZF=1
    ret
.no_match:
    pop di
    or ax, 1                    ; ZF=0
    ret

; --------------------------------------------
; match_a: like match, but only when system
; aliases are enabled (alias -a true).
; --------------------------------------------
match_a:
    cmp byte [aliases_on], 1
    je match
    or ax, 1                    ; ZF=0
    ret

; --------------------------------------------
; match_ptr: compare null-terminated name at DI
; with word at SI (ends at space/null).
; Sets ZF=1 on match.
; --------------------------------------------
match_ptr:
    push si
.next:
    mov al, [di]
    inc di
    mov ah, [si]
    inc si
    cmp ah, ' '
    je .iend
    cmp ah, 0
    je .iend
    cmp al, ah
    jne .no
    test al, al
    jz .no
    jmp .next
.iend:
    test al, al
    jnz .no
    pop si
    xor ax, ax
    ret
.no:
    pop si
    or ax, 1
    ret

; --------------------------------------------
; get_arg: point SI at text after first space
; in cmd_buf (skipping extra spaces).
; ZF=1 if there is no argument.
; --------------------------------------------
get_arg:
    mov si, cmd_buf
.find_space:
    lodsb
    test al, al
    jz .none
    cmp al, ' '
    jne .find_space
.skip:
    lodsb
    cmp al, ' '
    je .skip
    dec si
    cmp byte [si], 0
    je .none
    or ax, 1                    ; ZF=0 (has arg)
    ret
.none:
    xor ax, ax                  ; ZF=1 (no arg)
    ret

; --------------------------------------------
; Built-in commands
; --------------------------------------------
cmd_help:
    mov si, txt_help
    call print
    ret

cmd_about:
    mov si, txt_about
    call print
    ret

cmd_ver:
    mov si, txt_ver
    call print
    ret

cmd_echo:
    call get_arg
    jz .just_newline
    call print
.just_newline:
    mov si, newline
    call print
    ret

cmd_time:
    mov ah, 0x02                ; BIOS get time (BCD: CH=h CL=m DH=s)
    int 0x1A
    mov al, ch
    call print_bcd
    mov al, ':'
    call print_char
    mov al, cl
    call print_bcd
    mov al, ':'
    call print_char
    mov al, dh
    call print_bcd
    mov si, newline
    call print
    ret

cmd_date:
    mov ah, 0x04                ; BIOS get date (BCD: CH=cent CL=yr DH=mo DL=day)
    int 0x1A
    mov al, ch
    call print_bcd
    mov al, cl
    call print_bcd
    mov al, '-'
    call print_char
    mov al, dh
    call print_bcd
    mov al, '-'
    call print_char
    mov al, dl
    call print_bcd
    mov si, newline
    call print
    ret

cmd_mem:
    int 0x12                    ; AX = conventional memory in KB
    call print_dec
    mov si, txt_kb
    call print
    mov eax, [ram_mb]
    test eax, eax
    jz .no_ext
    call print_dec32
    mov si, txt_mb
    call print
.no_ext:
    ret

cmd_reboot:
    mov si, txt_reboot
    call print
    xor ax, ax
    int 0x16                    ; wait for a key
    int 0x19                    ; reload boot sector
    ret

cmd_halt:
    mov si, txt_halt
    call print
    cli
.loop:
    hlt
    jmp .loop

; --------------------------------------------
; pm: enter 32-bit protected mode, run a
; read/write test above 1 MB, return to real
; mode and report the result.
; --------------------------------------------
cmd_pm:
    call a20_enable
    jc .a20_fail
    mov si, txt_pm_enter
    call print
    mov [saved_sp], sp
    mov byte [pm_ok], 1
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword PM32_CODE:pm32_entry
.a20_fail:
    mov si, txt_pm_a20
    call print
    ret

; --- 32-bit protected mode code ---
[BITS 32]
pm32_entry:
    mov ax, PM32_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, pm_stack_top

    ; test physical 1 MB (flat descriptor base is 0x10000)
    mov edi, HIGH_1MB
    mov esi, [edi]              ; save original dword
    mov dword [edi], 0xDEADBEEF
    cmp dword [edi], 0xDEADBEEF
    jne .fail
    mov dword [edi], 0x55AA55AA
    cmp dword [edi], 0x55AA55AA
    jne .fail
    mov [edi], esi              ; restore

    ; test physical 16 MB
    mov edi, HIGH_16MB
    mov esi, [edi]
    mov dword [edi], 0xCAFEBABE
    cmp dword [edi], 0xCAFEBABE
    jne .fail
    mov dword [edi], 0x0F0F0F0F
    cmp dword [edi], 0x0F0F0F0F
    jne .fail
    mov [edi], esi

    ; switch back: 16-bit data segments, then 16-bit code
    mov ax, PM16_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp PM16_CODE:pm16_entry
.fail:
    mov byte [pm_ok], 0
    mov ax, PM16_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp PM16_CODE:pm16_entry

; --- back to 16-bit ---
[BITS 16]
pm16_entry:
    mov eax, cr0
    and al, 0xFE                ; clear PE (paging is off anyway)
    mov cr0, eax
    jmp KERNEL_SEG:pm_real
pm_real:
    mov ax, KERNEL_SEG
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov ax, 0x9000
    mov ss, ax
    mov sp, [saved_sp]
    sti
    cmp byte [pm_ok], 1
    jne .failed
    mov si, txt_pm_ok
    call print
    ret
.failed:
    mov si, txt_pm_fail
    call print
    ret

; --------------------------------------------
; A20 gate
; --------------------------------------------
a20_check:                      ; CF=0 enabled, CF=1 disabled
    pushf
    push ds
    push es
    push si
    push di
    push ax
    push bx
    cli
    xor ax, ax
    mov es, ax                  ; 0000:0500 = physical 0x000500
    mov ax, 0xFFFF
    mov ds, ax                  ; FFFF:0510 = physical 0x100500
    mov di, 0x0500
    mov si, 0x0510
    mov ax, [es:di]
    mov bx, [ds:si]
    push ax
    push bx
    mov word [es:di], 0x0000
    mov word [ds:si], 0xFFFF
    cmp word [es:di], 0xFFFF
    je .off
    pop bx
    pop ax
    mov [ds:si], bx
    mov [es:di], ax
    pop bx
    pop ax
    pop di
    pop si
    pop es
    pop ds
    popf
    clc
    ret
.off:
    pop bx
    pop ax
    mov [ds:si], bx
    mov [es:di], ax
    pop bx
    pop ax
    pop di
    pop si
    pop es
    pop ds
    popf
    stc
    ret

a20_wait_in:                    ; wait for input buffer empty
    in al, 0x64
    test al, 2
    jnz a20_wait_in
    ret

a20_wait_out:                   ; wait for output buffer full
    in al, 0x64
    test al, 1
    jz a20_wait_out
    ret

a20_kbd:                        ; enable A20 via keyboard controller
    call a20_wait_in
    mov al, 0xD0                ; read output port
    out 0x64, al
    call a20_wait_out
    in al, 0x60
    push ax
    call a20_wait_in
    mov al, 0xD1                ; write output port
    out 0x64, al
    call a20_wait_in
    pop ax
    or al, 2
    out 0x60, al
    call a20_wait_in
    ret

a20_enable:                     ; CF=0 on success, CF=1 if still off
    call a20_check
    jnc .ok
    in al, 0x92                 ; fast A20 (System Control Port)
    test al, 2
    jnz .check
    or al, 2
    and al, 0xFE                ; never pulse the reset bit
    out 0x92, al
.check:
    call a20_check
    jnc .ok
    call a20_kbd
    call a20_check
.ok:
    ret

; --------------------------------------------
; detect_ram: int 15h E820 memory map,
; total usable RAM (rounded up to MB),
; and physical address of the GDT.
; --------------------------------------------
detect_ram:
    pusha
    push es
    mov ax, KERNEL_SEG
    mov es, ax
    xor ebx, ebx
    mov word [e820_cnt], 0
    mov di, e820_buf
.next:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150         ; "SMAP"
    int 0x15
    jc .map_done
    cmp eax, 0x534D4150
    jne .map_done
    add di, 24
    inc word [e820_cnt]
    cmp bx, 0
    je .map_done
    cmp word [e820_cnt], 16
    jb .next
.map_done:
    ; GDT descriptor needs an absolute physical base
    mov eax, gdt_start
    add eax, 0x10000
    mov [gdt_ptr+2], eax
    ; sum usable (type 1) region lengths
    movzx si, word [e820_cnt]
    mov di, e820_buf
    xor ecx, ecx                ; total bytes (32-bit)
.sum:
    test si, si
    jz .sum_done
    cmp dword [es:di+16], 1
    jne .skip
    mov eax, [es:di+12]
    test eax, eax               ; ignore regions extending past 4 GB
    jnz .skip
    mov eax, [es:di+8]
    add ecx, eax
.skip:
    add di, 24
    dec si
    jmp .sum
.sum_done:
    ; round up to whole megabytes
    add ecx, 0xFFFFF
    jc .cap
    shr ecx, 20
    jmp .store
.cap:
    mov ecx, 0xFFFFFFFF
.store:
    mov [ram_mb], ecx
    pop es
    popa
    ret

; --------------------------------------------
; dir / ls: list current directory entries
; --------------------------------------------
cmd_dir:
    call dir_walk_init
    mov word [file_cnt], 0
.sector:
    call dir_walk_next
    jc .done
    mov di, dir_buf
    mov dx, 16                  ; entries per sector
.entry:
    mov [entry_ptr], di
    cmp byte [di], 0x00
    je .done                    ; end of directory
    cmp byte [di], 0xE5
    je .skip                    ; deleted
    mov al, [di+11]
    cmp al, 0x0F
    je .skip                    ; LFN entry
    test al, 0x08
    jnz .skip                   ; volume label

    ; print "NAME     EXT  "
    mov di, [entry_ptr]
    mov cx, 8
    call print_field
    mov al, ' '
    call print_char
    mov di, [entry_ptr]
    add di, 8
    mov cx, 3
    call print_field
    mov al, ' '
    call print_char
    mov al, ' '
    call print_char

    mov di, [entry_ptr]
    test byte [di+11], 0x10
    jz .file
    mov si, txt_dir_tag
    call print
    jmp .nl
.file:
    mov ax, [di+28]             ; size (low word)
    call print_dec
.nl:
    mov si, newline
    call print
    inc word [file_cnt]
.skip:
    mov di, [entry_ptr]
    add di, 32
    dec dx
    jnz .entry
    jmp .sector
.done:
    mov ax, [file_cnt]
    call print_dec
    mov si, txt_files
    call print
    ret

; --------------------------------------------
; doc / cat / type: print file contents
; --------------------------------------------
cmd_doc:
    call get_arg
    jz .usage
    call make_fat_name
    call find_file
    jc .notfound
    test byte [found_attr], 0x10
    jnz .isdir
    mov [file_size], cx
    mov dx, FILE_SEG
    mov es, dx
    xor bx, bx
    call load_file              ; AX = first cluster (from find_file)

    ; print file content, converting LF -> CR+LF
    mov dx, FILE_SEG            ; load_file may have bumped ES
    mov es, dx
    mov cx, [file_size]
    test cx, cx
    jz .end
    xor si, si
.pr:
    mov al, [es:si]
    inc si
    cmp al, 10
    jne .out
    push ax
    mov al, 13
    call print_char
    pop ax
.out:
    call print_char
    loop .pr
.end:
    mov si, newline
    call print
    ret
.usage:
    mov si, txt_usage_doc
    call print
    ret
.notfound:
    mov si, txt_notfound
    call print
    ret
.isdir:
    mov si, txt_isdir
    call print
    ret

; --------------------------------------------
; mkdir: create a directory in current directory
; --------------------------------------------
cmd_mkdir:
    call get_arg
    jz .usage
    call make_fat_name
    cmp byte [fat_name], ' '
    je .invalid
    call find_file
    jnc .exists
    call find_free_entry
    jc .full
    call alloc_cluster
    test ax, ax
    jz .diskfull
    mov [new_cluster], ax
    mov dx, 0xFFFF              ; mark FAT[cluster] = EOC (FAT16)
    call set_fat

    ; fill directory entry in dir_buf (entry_ptr)
    mov di, [entry_ptr]
    mov si, fat_name
    mov cx, 11
.cp:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .cp
    mov byte [di], 0x10         ; attr = directory
    mov ax, [new_cluster]
    mov [di+15], ax             ; +26 first cluster
    xor ax, ax
    mov [di+17], ax             ; +28 size low = 0
    mov [di+19], ax             ; +30 size high = 0

    ; write back parent directory sector
    push ds
    pop es
    mov bx, dir_buf
    mov ax, [cur_lba]
    call write_lba

    ; build the new cluster: "." and ".." entries
    mov di, dir_buf
    mov cx, 512
    xor al, al
.zero:
    mov [di], al
    inc di
    loop .zero
    ; "." -> self
    mov byte [dir_buf], '.'
    mov di, dir_buf
    mov cx, 10
    mov al, ' '
.d1:
    inc di
    mov [di], al
    loop .d1
    mov byte [dir_buf+11], 0x10
    mov ax, [new_cluster]
    mov [dir_buf+26], ax
    ; ".." -> parent (0 = root)
    mov byte [dir_buf+32], '.'
    mov byte [dir_buf+33], '.'
    mov di, dir_buf+33
    mov cx, 9
    mov al, ' '
.d2:
    inc di
    mov [di], al
    loop .d2
    mov byte [dir_buf+32+11], 0x10
    mov ax, [cur_dir]
    mov [dir_buf+32+26], ax
    ; write new cluster to disk (all spc sectors; first holds . and ..)
    mov ax, [new_cluster]
    sub ax, 2
    mul word [sec_per_clus]
    add ax, [data_lba]
    push ds
    pop es
    mov bx, dir_buf
    call write_lba              ; first sector with . and ..
    mov cx, [sec_per_clus]
    dec cx
    jz .written
    push ax
    mov di, dir_buf             ; zero dir_buf for the remaining sectors
    push cx
    mov cx, 512
    xor al, al
.z:
    mov [di], al
    inc di
    loop .z
    pop cx
    pop ax
.wrloop:
    inc ax
    call write_lba              ; AX, CX, ES, BX preserved by write_lba
    loop .wrloop
.written:

    call write_fat              ; sync both FAT copies
    mov si, txt_mkdir_ok
    call print
    ret
.usage:
    mov si, txt_usage_mkdir
    call print
    ret
.exists:
    mov si, txt_exists
    call print
    ret
.full:
    mov si, txt_full
    call print
    ret
.diskfull:
    mov si, txt_diskfull
    call print
    ret
.invalid:
    mov si, txt_invalid
    call print
    ret

; --------------------------------------------
; touch: create an empty file in current directory
; --------------------------------------------
cmd_touch:
    call get_arg
    jz .usage
    call make_fat_name
    cmp byte [fat_name], ' '
    je .invalid
    call find_file
    jnc .exists
    call find_free_entry
    jc .full

    mov di, [entry_ptr]
    mov si, fat_name
    mov cx, 11
.cp:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .cp
    mov byte [di], 0x20         ; attr = archive
    ; cluster = 0, size = 0 (entry already zeroed)

    push ds
    pop es
    mov bx, dir_buf
    mov ax, [cur_lba]
    call write_lba
    mov si, txt_touch_ok
    call print
    ret
.usage:
    mov si, txt_usage_touch
    call print
    ret
.exists:
    mov si, txt_exists
    call print
    ret
.full:
    mov si, txt_full
    call print
    ret
.invalid:
    mov si, txt_invalid
    call print
    ret

; --------------------------------------------
; goto / cd: change current directory
;   goto        -> root
;   goto /      -> root
;   goto .      -> stay
;   goto ..     -> parent
;   goto <dir>  -> enter subdirectory
; --------------------------------------------
cmd_goto:
    call get_arg
    jz .root                    ; no arg -> root
    cmp byte [si], '/'
    je .root
    cmp byte [si], '.'
    jne .named
    cmp byte [si+1], '.'
    je .parent
    ret                         ; "." -> stay
.root:
    mov word [cur_dir], 0
    mov byte [cur_path], '/'
    mov byte [cur_path+1], 0
    ret
.parent:
    mov ax, [cur_dir]
    test ax, ax
    jz .done                    ; already at root
    ; ".." entry is entry #1 of current dir's first cluster
    sub ax, 2
    mul word [sec_per_clus]
    add ax, [data_lba]
    push ds
    pop es
    mov bx, dir_buf
    call read_lba
    mov ax, [dir_buf+32+26]     ; parent cluster
    mov [cur_dir], ax
    call path_pop
.done:
    ret
.named:
    mov [arg_ptr], si
    call make_fat_name
    call find_file
    jc .notfound
    test byte [found_attr], 0x10
    jz .notdir
    mov [cur_dir], ax
    mov si, [arg_ptr]
    call path_push
    ret
.notfound:
    mov si, txt_notfound
    call print
    ret
.notdir:
    mov si, txt_notdir
    call print
    ret

; --------------------------------------------
; alias: manage system aliases
;   alias -a          -> show state
;   alias -a true     -> enable system aliases
;   alias -a false    -> disable system aliases
; --------------------------------------------
cmd_alias:
    call get_arg
    jz .usage
    cmp byte [si], '-'
    jne .usage
    cmp byte [si+1], 'a'
    jne .usage
    add si, 2
.sk:
    cmp byte [si], ' '
    jne .chk
    inc si
    jmp .sk
.chk:
    cmp byte [si], 0
    je .state
    mov di, txt_true
    call match_ptr
    je .enable
    mov di, txt_false
    call match_ptr
    je .disable
    jmp .usage
.enable:
    mov byte [aliases_on], 1
    jmp .state
.disable:
    mov byte [aliases_on], 0
.state:
    mov si, txt_alias_state
    call print
    mov si, txt_on
    cmp byte [aliases_on], 1
    je .pon
    mov si, txt_off
.pon:
    call print
    ret
.usage:
    mov si, txt_usage_alias
    call print
    ret

; --------------------------------------------
; where / pwd: show current path
; --------------------------------------------
cmd_where:
    mov si, cur_path
    call print
    mov si, newline
    call print
    ret

; --------------------------------------------
; who / whoami: show current user
; (single-user for now; multi-user comes later)
; --------------------------------------------
cmd_who:
    mov si, txt_who
    call print
    ret

; --------------------------------------------
; pck: package manager
; Packages live as .PCK files in the /PCK directory.
; PCK1 format: "PCK1" dw count, then per entry:
;   11-byte FAT name, dd size, then content bytes.
;   pck                  -> usage
;   pck list             -> list packages in /PCK
;   pck install <name>   -> extract NAME.PCK into current dir
;   pck remove <name>    -> delete files listed in NAME.PCK
; --------------------------------------------
cmd_pck:
    call get_arg
    jz .usage
    mov di, txt_list
    call match_ptr
    je .list
    mov di, txt_install
    call match_ptr
    je .install
    mov di, txt_remove
    call match_ptr
    je .remove
.usage:
    mov si, txt_pck_usage
    call print
    ret

; ---- pck list ----
.list:
    call open_pck_dir
    jc .nopck
    mov [pck_dir], ax
    mov bx, [cur_dir]
    mov [pck_save], bx
    mov [cur_dir], ax
    call dir_walk_init
    mov word [file_cnt], 0
.sector:
    call dir_walk_next
    jc .list_done
    mov di, dir_buf
    mov dx, 16
.entry:
    mov [entry_ptr], di
    cmp byte [di], 0x00
    je .list_done
    cmp byte [di], 0xE5
    je .skip
    mov al, [di+11]
    cmp al, 0x0F
    je .skip
    test al, 0x18               ; skip volume labels and subdirs
    jnz .skip
    cmp byte [di+8], 'P'        ; extension must be PCK
    jne .skip
    cmp byte [di+9], 'C'
    jne .skip
    cmp byte [di+10], 'K'
    jne .skip
    mov cx, 8
    call print_field
    mov si, newline
    call print
    inc word [file_cnt]
.skip:
    mov di, [entry_ptr]
    add di, 32
    dec dx
    jnz .entry
    jmp .sector
.list_done:
    mov ax, [pck_save]
    mov [cur_dir], ax
    mov ax, [file_cnt]
    call print_dec
    mov si, txt_pcks
    call print
    ret
.nopck:
    mov si, txt_nopck
    call print
    ret

; ---- pck install <name> ----
.install:
    call skip_word
    cmp byte [si], 0
    je .usage
    call make_pkg_name
    call find_pkg
    jc .notfound
    call load_pkg               ; package at FILE_SEG:0, header checked
    jc .bad
    mov word [pck_off], 6
.inst_next:
    cmp word [pck_count], 0
    je .inst_done
    mov dx, FILE_SEG
    mov es, dx
    call pck_read_entry         ; pkg_name / pck_size / pck_off at content
    call install_one
    mov ax, [pck_off]
    add ax, [pck_size]
    mov [pck_off], ax           ; skip content
    dec word [pck_count]
    jmp .inst_next
.inst_done:
    mov si, txt_pck_ok
    call print
    ret

; ---- pck remove <name> ----
.remove:
    call skip_word
    cmp byte [si], 0
    je .usage
    call make_pkg_name
    call find_pkg
    jc .notfound
    call load_pkg
    jc .bad
    mov word [pck_off], 6
.rm_next:
    cmp word [pck_count], 0
    je .rm_done
    mov dx, FILE_SEG
    mov es, dx
    call pck_read_entry
    call delete_one
    mov ax, [pck_off]
    add ax, [pck_size]
    mov [pck_off], ax
    dec word [pck_count]
    jmp .rm_next
.rm_done:
    mov si, txt_pck_rm
    call print
    ret

.notfound:
    mov si, txt_notfound
    call print
    ret
.bad:
    mov si, txt_badpck
    call print
    ret

; ---- pck helpers ----

; open_pck_dir: -> AX = cluster of /PCK, CF=1 if missing/not a dir
open_pck_dir:
    mov ax, [cur_dir]
    mov [pck_save], ax
    mov word [cur_dir], 0
    mov di, fat_name            ; fat_name = "PCK       "
    mov byte [di], 'P'
    mov byte [di+1], 'C'
    mov byte [di+2], 'K'
    mov cx, 8
    mov al, ' '
.fill:
    mov [di+3], al
    inc di
    loop .fill
    call find_file
    jc .fail
    test byte [found_attr], 0x10
    jz .fail
    mov cx, [pck_save]
    mov [cur_dir], cx
    clc
    ret
.fail:
    mov cx, [pck_save]
    mov [cur_dir], cx
    stc
    ret

; find_pkg: fat_name = package file in /PCK
; -> AX = first cluster, CX = size; CF=1 if not found
find_pkg:
    mov si, fat_name            ; open_pck_dir clobbers fat_name,
    mov di, pkg_name            ; so stash the requested name first
    mov cx, 11
.sv:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .sv
    call open_pck_dir
    jc .fail
    mov [pck_dir], ax
    mov bx, [cur_dir]
    mov [pck_save], bx
    mov [cur_dir], ax
    mov si, pkg_name
    mov di, fat_name
    mov cx, 11
.rs:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .rs
    call find_file
    mov bx, [pck_save]
    mov [cur_dir], bx
    jc .fail
    clc
    ret
.fail:
    stc
    ret

; make_pkg_name: user text at SI -> fat_name, default extension PCK
make_pkg_name:
    call make_fat_name
    cmp byte [fat_name+8], ' '
    jne .ok
    mov byte [fat_name+8], 'P'
    mov byte [fat_name+9], 'C'
    mov byte [fat_name+10], 'K'
.ok:
    ret

; load_pkg: AX = first cluster, CX = size (from find_pkg).
; Loads package to FILE_SEG:0 and checks the PCK1 magic.
; -> pck_count = entry count; CF=1 if bad magic.
load_pkg:
    mov [file_size], cx
    mov dx, FILE_SEG
    mov es, dx
    xor bx, bx
    call load_file
    mov dx, FILE_SEG            ; load_file may bump ES
    mov es, dx
    cmp byte [es:0], 'P'
    jne .bad
    cmp byte [es:1], 'C'
    jne .bad
    cmp byte [es:2], 'K'
    jne .bad
    cmp byte [es:3], '1'
    jne .bad
    mov ax, [es:4]
    mov [pck_count], ax
    clc
    ret
.bad:
    stc
    ret

; pck_read_entry: read entry header at ES=pck_off (FILE_SEG).
; Fills pkg_name (11 bytes) and pck_size; pck_off ends at content.
pck_read_entry:
    mov bx, [pck_off]
    mov di, pkg_name
    mov cx, 11
.cp:
    mov al, [es:bx]
    inc bx
    mov [di], al
    inc di
    loop .cp
    mov ax, [es:bx]             ; size (low word; packages are small)
    mov [pck_size], ax
    add bx, 4
    mov [pck_off], bx
    ret

; install_one: create file pkg_name in the CURRENT directory,
; content at FILE_SEG:pck_off, length pck_size.
install_one:
    mov si, pkg_name            ; find_file works on fat_name
    mov di, fat_name
    mov cx, 11
.cp:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .cp
    call find_file
    jnc .exists
    call find_free_entry
    jc .full

    ; allocate cluster chain and write content
    mov word [pck_first], 0
    mov word [pck_prev], 0
    mov ax, [pck_size]
    test ax, ax
    jz .entry                   ; empty file: no data clusters
    mov bx, [sec_per_clus]
    shl bx, 9                   ; bytes per cluster
    xor dx, dx
    div bx                      ; AX = full clusters, DX = remainder
    test dx, dx
    jz .r1
    inc ax
.r1:
    mov [pck_clusters], ax
    mov ax, [pck_off]
    mov [pck_data], ax
.cl_loop:
    call alloc_cluster
    test ax, ax
    jz .diskfull
    mov cx, ax                  ; CX = new cluster
    cmp word [pck_prev], 0
    jne .link
    mov [pck_first], cx
    jmp .eoc
.link:
    mov ax, [pck_prev]
    mov dx, cx
    call set_fat                ; FAT[prev] = new
.eoc:
    mov ax, cx
    mov [pck_prev], ax
    mov dx, 0xFFFF
    call set_fat                ; FAT[new] = EOC
    ; write this cluster's sectors from FILE_SEG:pck_data
    sub ax, 2
    mul word [sec_per_clus]
    add ax, [data_lba]          ; AX = LBA of cluster
    mov cx, [sec_per_clus]
    mov bx, [pck_data]
    mov dx, FILE_SEG
    mov es, dx
.wr:
    call write_lba              ; preserves AX/BX/CX/ES
    inc ax
    add bx, 512
    loop .wr
    push ds
    pop es
    mov ax, [sec_per_clus]
    shl ax, 9
    add [pck_data], ax
    dec word [pck_clusters]
    jnz .cl_loop

.entry:
    mov di, [entry_ptr]         ; fill directory entry
    mov si, pkg_name
    mov cx, 11
.cp2:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .cp2
    mov byte [di], 0x20         ; attr = archive
    mov ax, [pck_first]
    mov [di+15], ax             ; +26 first cluster
    mov ax, [pck_size]
    mov [di+17], ax             ; +28 size low
    xor ax, ax
    mov [di+19], ax             ; +30 size high
    push ds
    pop es
    mov bx, dir_buf
    mov ax, [cur_lba]
    call write_lba
    call write_fat
    ret
.exists:
    mov si, txt_exists
    call print
    ret
.full:
    mov si, txt_full
    call print
    ret
.diskfull:
    mov si, txt_diskfull
    call print
    ret

; delete_one: delete file pkg_name from the CURRENT directory
delete_one:
    mov si, pkg_name
    mov di, fat_name
    mov cx, 11
.cp:
    mov al, [si]
    inc si
    mov [di], al
    inc di
    loop .cp
    call find_file
    jc .skip                    ; not there, nothing to do
    mov [pck_delcl], ax
    mov di, [entry_ptr]         ; mark entry deleted
    mov byte [di], 0xE5
    push ds
    pop es
    mov bx, dir_buf
    mov ax, [cur_lba]
    call write_lba
    mov ax, [pck_delcl]         ; free the cluster chain
.free:
    cmp ax, 2
    jb .done                    ; 0/1 = no chain
    cmp ax, 0xFFF8
    jae .done
    mov [pck_delcl], ax
    call fat_next
    mov [pck_next], ax
    mov ax, [pck_delcl]
    xor dx, dx
    call set_fat                ; FAT[cur] = 0
    mov ax, [pck_next]
    jmp .free
.done:
    call write_fat
.skip:
    ret

; skip_word: advance SI past the current word and following spaces
skip_word:
.w:
    lodsb
    cmp al, ' '
    je .sp
    test al, al
    jz .done
    jmp .w
.sp:
    lodsb
    cmp al, ' '
    je .sp
    dec si
.done:
    ret

; --------------------------------------------
; Path helpers
; --------------------------------------------

; path_push: append component (text at SI) to cur_path
path_push:
    push di
    mov di, cur_path
.findend:
    cmp byte [di], 0
    je .atend
    inc di
    jmp .findend
.atend:
    cmp di, cur_path+1          ; path is "/" -> no extra slash
    je .copy
    mov byte [di], '/'
    inc di
.copy:
    lodsb
    test al, al
    jz .fin
    cmp al, ' '
    je .fin
    cmp di, cur_path+PATH_SIZE-2
    jae .fin
    mov [di], al
    inc di
    jmp .copy
.fin:
    mov byte [di], 0
    pop di
    ret

; path_pop: remove last component from cur_path
path_pop:
    push di
    mov bx, cur_path            ; last slash position
    mov di, cur_path
.scan:
    cmp byte [di], 0
    je .cut
    cmp byte [di], '/'
    jne .next
    mov bx, di
.next:
    inc di
    jmp .scan
.cut:
    cmp bx, cur_path            ; last slash is the root one
    jne .cutat
    inc bx                      ; keep "/"
.cutat:
    mov byte [bx], 0
    pop di
    ret

; --------------------------------------------
; FAT16 driver (virtual hard disk)
; --------------------------------------------

; init_fs: read the MBR's boot info block (phys 0x0500),
; read the partition's VBR sector, parse the FAT16 BPB
; into runtime variables, then load the FAT table.
init_fs:
    xor ax, ax
    mov es, ax                  ; info block lives at 0000:0500
    mov ax, [es:0x0500]
    mov [part_base], ax
    mov ax, [es:0x0504]
    mov [geo_spt], ax
    mov ax, [es:0x0506]
    mov [geo_heads], ax

    ; read VBR sector (at partition base) into dir_buf
    push ds
    pop es
    mov bx, dir_buf
    mov ax, [part_base]
    call read_lba

    ; parse BPB fields from the VBR copy in dir_buf
    movzx ax, byte [dir_buf+13] ; SecPerClus
    mov [sec_per_clus], ax
    mov ax, [dir_buf+14]        ; ResvdSectors
    add ax, [part_base]
    mov [fat_lba], ax
    mov ax, [dir_buf+17]        ; RootEntries
    shr ax, 4
    mov [root_secs], ax         ; entries * 32 / 512
    mov ax, [dir_buf+22]        ; SecPerFAT
    mov [sec_per_fat], ax
    movzx bx, byte [dir_buf+16] ; NumFATs
    mul bx                      ; AX = nfats * spf
    add ax, [fat_lba]
    mov [root_lba], ax
    add ax, [root_secs]
    mov [data_lba], ax

    ; total sectors (16-bit field, or low word of 32-bit field)
    mov ax, [dir_buf+19]
    test ax, ax
    jnz .have_tot
    mov ax, [dir_buf+32]
.have_tot:
    ; max_cluster = (totsec - (data_lba - part_base)) / spc + 1
    mov bx, [data_lba]
    sub bx, [part_base]
    sub ax, bx
    xor dx, dx
    div word [sec_per_clus]
    inc ax
    mov [max_cluster], ax

    jmp load_fat                ; tail: load FAT into fat_buf

; read_lba: AX = LBA, ES:BX = buffer. Reads 1 sector.
read_lba:
    pusha
    xor dx, dx
    div word [geo_spt]          ; AX = track, DX = LBA % spt
    inc dl                      ; sector = rem + 1
    mov cl, dl
    xor dx, dx
    div word [geo_heads]        ; AX = cylinder, DX = head
    mov dh, dl
    mov ch, al
    mov dl, [boot_drive]
    mov ax, 0x0201
    int 0x13
    jc .err
    popa
    ret
.err:
    mov si, msg_diskerr
    call print
    popa
    ret

; write_lba: AX = LBA, ES:BX = buffer. Writes 1 sector.
write_lba:
    pusha
    xor dx, dx
    div word [geo_spt]
    inc dl
    mov cl, dl
    xor dx, dx
    div word [geo_heads]
    mov dh, dl
    mov ch, al
    mov dl, [boot_drive]
    mov ax, 0x0301              ; write 1 sector
    int 0x13
    jc .err
    popa
    ret
.err:
    mov si, msg_diskerr
    call print
    popa
    ret

; load_fat: load the FAT table into fat_buf
load_fat:
    push ds
    pop es
    mov bx, fat_buf
    mov ax, [fat_lba]
    mov cx, [sec_per_fat]
.loop:
    call read_lba
    inc ax
    add bx, 512
    loop .loop
    ret

; write_fat: write fat_buf back to both FAT copies
write_fat:
    push ds
    pop es
    mov bx, fat_buf
    mov ax, [fat_lba]
    mov cx, [sec_per_fat]
.l1:
    call write_lba
    inc ax
    add bx, 512
    loop .l1
    mov bx, fat_buf
    mov ax, [fat_lba]
    add ax, [sec_per_fat]       ; second FAT copy
    mov cx, [sec_per_fat]
.l2:
    call write_lba
    inc ax
    add bx, 512
    loop .l2
    ret

; fat_next: AX = cluster -> AX = next cluster (FAT16)
fat_next:
    push bx
    mov bx, ax
    shl bx, 1                   ; offset = cluster * 2
    mov bx, [fat_buf+bx]
    mov ax, bx
    pop bx
    ret

; set_fat: AX = cluster, DX = 16-bit value to store (FAT16)
set_fat:
    push bx
    mov bx, ax
    shl bx, 1
    mov [fat_buf+bx], dx
    pop bx
    ret

; alloc_cluster: -> AX = free cluster, 0 if disk full
alloc_cluster:
    mov bx, 2
.scan:
    cmp bx, [max_cluster]
    jae .full
    mov ax, bx
    call fat_next               ; preserves BX
    test ax, ax
    jz .found
    inc bx
    jmp .scan
.found:
    mov ax, bx
    ret
.full:
    xor ax, ax
    ret

; dir_walk_init: start iterating current directory
dir_walk_init:
    mov ax, [cur_dir]
    mov [walk_cluster], ax      ; 0 = root, else first cluster
    test ax, ax
    jnz .sub
    mov ax, [root_lba]
    mov [walk_lba], ax
    mov ax, [root_secs]
    mov [walk_left], ax
    ret
.sub:
    mov word [walk_left], 0     ; force new cluster span on first call
    ret

; dir_walk_next: read next directory sector into dir_buf,
; cur_lba = its LBA. CF=1 when no more sectors.
; walk_left = sectors left in current span (root run or cluster).
dir_walk_next:
    cmp word [walk_left], 0
    jne .have_sector
    ; span exhausted - need a new one
    cmp word [walk_cluster], 0
    je .end                     ; root directory finished
    mov ax, [walk_cluster]
    cmp ax, 0xFFF8
    jae .end
    ; new cluster span: lba = data_lba + (cluster-2)*spc
    sub ax, 2
    mul word [sec_per_clus]
    add ax, [data_lba]
    mov [cur_lba], ax
    inc ax
    mov [walk_lba], ax
    mov ax, [sec_per_clus]
    dec ax
    mov [walk_left], ax
    mov ax, [walk_cluster]
    call fat_next
    mov [walk_cluster], ax
    jmp .read
.have_sector:
    mov ax, [walk_lba]
    mov [cur_lba], ax
    inc word [walk_lba]
    dec word [walk_left]
.read:
    push ds
    pop es
    mov bx, dir_buf
    mov ax, [cur_lba]
    call read_lba
    clc
    ret
.end:
    stc
    ret

; find_file: fat_name (11 bytes) in current directory
; -> AX = first cluster, CX = size, found_attr = attr.
; CF=1 if not found.
find_file:
    call dir_walk_init
.sector:
    call dir_walk_next
    jc .fail
    mov di, dir_buf
    mov dx, 16
.entry:
    mov [entry_ptr], di
    cmp byte [di], 0x00
    je .fail
    cmp byte [di], 0xE5
    je .skip
    mov al, [di+11]
    cmp al, 0x0F
    je .skip
    test al, 0x08
    jnz .skip
    ; compare 11-byte name
    mov si, fat_name
    mov cx, 11
.cmp:
    mov al, [si]
    inc si
    cmp al, [di]
    jne .skip
    inc di
    loop .cmp
    ; found
    mov di, [entry_ptr]
    mov al, [di+11]
    mov [found_attr], al
    mov ax, [di+26]             ; first cluster
    mov cx, [di+28]             ; size (low word)
    clc
    ret
.skip:
    mov di, [entry_ptr]
    add di, 32
    dec dx
    jnz .entry
    jmp .sector
.fail:
    stc
    ret

; find_free_entry: find a free slot in current directory.
; dir_buf holds the sector, entry_ptr points at the (zeroed) entry,
; cur_lba = sector LBA. CF=1 if directory is full.
find_free_entry:
    call dir_walk_init
.sector:
    call dir_walk_next
    jc .fail
    mov di, dir_buf
    mov dx, 16
.entry:
    mov al, [di]
    cmp al, 0x00
    je .found
    cmp al, 0xE5
    je .found
    add di, 32
    dec dx
    jnz .entry
    jmp .sector
.found:
    mov [entry_ptr], di
    mov cx, 32                  ; clear the entry
    xor al, al
.clr:
    mov [di], al
    inc di
    loop .clr
    clc
    ret
.fail:
    stc
    ret

; load_file: AX = first cluster, ES:BX = dest. Follows FAT chain.
; Reads all sec_per_clus sectors of each cluster; bumps ES when
; BX wraps so files larger than 64KB keep loading.
load_file:
.next_cluster:
    cmp ax, 0xFFF8
    jae .done
    mov [load_cluster], ax      ; save current cluster
    sub ax, 2
    mul word [sec_per_clus]
    add ax, [data_lba]          ; AX = LBA of cluster start
    mov cx, [sec_per_clus]
.sector_loop:
    call read_lba               ; preserves AX/CX/ES/BX (pusha)
    inc ax
    add bx, 512
    jnz .no_bump
    push ax
    mov ax, es
    add ax, 0x1000
    mov es, ax
    pop ax
.no_bump:
    loop .sector_loop
    mov ax, [load_cluster]
    call fat_next
    jmp .next_cluster
.done:
    ret

; make_fat_name: convert user text at SI (e.g. "readme.txt")
; into 11-byte padded uppercase FAT name in fat_name.
make_fat_name:
    push di
    mov di, fat_name
    mov cx, 11
    mov al, ' '
.fill:
    mov [di], al
    inc di
    loop .fill
    mov di, fat_name
.name:
    lodsb
    test al, al
    jz .done
    cmp al, ' '
    je .done
    cmp al, '.'
    je .ext
    cmp di, fat_name+8
    jae .name                   ; skip extra name chars
    call to_upper
    mov [di], al
    inc di
    jmp .name
.ext:
    mov di, fat_name+8
.extloop:
    lodsb
    test al, al
    jz .done
    cmp al, ' '
    je .done
    cmp di, fat_name+11
    jae .done
    call to_upper
    mov [di], al
    inc di
    jmp .extloop
.done:
    pop di
    ret

to_upper:                       ; AL -> uppercase
    cmp al, 'a'
    jb .ok
    cmp al, 'z'
    ja .ok
    sub al, 0x20
.ok:
    ret

; --------------------------------------------
; read_line: read a line into cmd_buf (max BUF_SIZE-1)
; Handles Enter and Backspace.
; --------------------------------------------
read_line:
    xor di, di                  ; length / index
    mov byte [cmd_buf], 0
.loop:
    xor ah, ah
    int 0x16                    ; blocking key read -> AL = ASCII
    cmp al, 13                  ; Enter
    je .enter
    cmp al, 8                   ; Backspace
    je .back
    cmp al, 32                  ; ignore other control chars
    jb .loop
    cmp di, BUF_SIZE-1
    jae .loop                   ; buffer full, ignore
    mov [cmd_buf+di], al
    inc di
    mov byte [cmd_buf+di], 0
    mov ah, 0x0E                ; echo char
    mov bx, 0x0007
    int 0x10
    jmp .loop
.back:
    test di, di
    jz .loop
    dec di
    mov byte [cmd_buf+di], 0
    mov bx, 0x0007
    mov ah, 0x0E
    mov al, 8                   ; backspace
    int 0x10
    mov al, ' '                 ; erase char
    int 0x10
    mov al, 8
    int 0x10
    jmp .loop
.enter:
    ret

; --------------------------------------------
; Video / print helpers (BIOS teletype, page 0)
; --------------------------------------------
clear_screen:
    mov ax, 0x0003              ; 80x25 text mode, clears screen
    int 0x10
    ret

print:                          ; SI -> null-terminated string (DS)
    pusha
    mov bx, 0x0007
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .loop
.done:
    popa
    ret

print_char:                     ; AL = character
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    ret

print_field:                    ; print CX chars from [DI] (DS)
    push cx
    push di
.loop:
    mov al, [di]
    call print_char
    inc di
    loop .loop
    pop di
    pop cx
    ret

print_bcd:                      ; AL = packed BCD byte -> print two digits
    push ax
    mov cl, al
    shr al, 4
    add al, '0'
    call print_char
    mov al, cl
    and al, 0x0F
    add al, '0'
    call print_char
    pop ax
    ret

print_dec:                      ; AX = unsigned 16-bit -> print decimal
    pusha
    mov bx, 10
    xor cx, cx
.divide:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .divide
.output:
    pop ax
    add al, '0'
    call print_char
    loop .output
    popa
    ret

print_dec32:                    ; EAX = unsigned 32-bit -> print decimal
    pusha
    mov ebx, 10
    xor ecx, ecx
.divide:
    xor edx, edx
    div ebx
    push dx
    inc cx
    test eax, eax
    jnz .divide
.output:
    pop dx
    mov al, dl
    add al, '0'
    call print_char
    loop .output
    popa
    ret

; --------------------------------------------
; Data
; --------------------------------------------
name_help   db "help", 0
name_about  db "about", 0
name_ver    db "ver", 0
name_clear  db "clear", 0
name_cls    db "cls", 0
name_echo   db "echo", 0
name_dir    db "dir", 0
name_ls     db "ls", 0
name_doc    db "doc", 0
name_cat    db "cat", 0
name_type   db "type", 0
name_mkdir  db "mkdir", 0
name_touch  db "touch", 0
name_goto   db "goto", 0
name_cd     db "cd", 0
name_alias  db "alias", 0
name_where  db "where", 0
name_pwd    db "pwd", 0
name_who    db "who", 0
name_whoami db "whoami", 0
name_time   db "time", 0
name_date   db "date", 0
name_mem    db "mem", 0
name_pck    db "pck", 0
name_pm     db "pm", 0
name_reboot db "reboot", 0
name_halt   db "halt", 0

banner      db "============================================", 13, 10
            db " ROS Command v", VERSION, 13, 10
            db " Lightweight kernel by XR Stu - xrst.uk", 13, 10
            db " Shell: xsh (X Shell)", 13, 10
            db "============================================", 13, 10
            db "Type 'help' to list commands.", 13, 10, 13, 10, 0

txt_shell   db "xsh:ROS", 0
prompt      db "> ", 0
newline     db 13, 10, 0

msg_unknown db "Unknown command. Type 'help'.", 13, 10, 0
msg_diskerr db "Disk error!", 13, 10, 0
txt_kb      db " KB conventional memory", 13, 10, 0
txt_mb      db " MB extended memory", 13, 10, 0
txt_reboot  db "Press any key to reboot...", 13, 10, 0
txt_halt    db "System halted. It is now safe to power off.", 13, 10, 0
txt_ver     db "ROS Command version ", VERSION, 13, 10, 0
txt_dir_tag db "<DIR>", 0
txt_files   db " file(s)", 13, 10, 0
txt_usage_doc   db "Usage: doc <filename>", 13, 10, 0
txt_usage_mkdir db "Usage: mkdir <name>", 13, 10, 0
txt_usage_touch db "Usage: touch <name>", 13, 10, 0
txt_usage_alias db "Usage: alias -a [true|false]", 13, 10, 0
txt_notfound db "File not found.", 13, 10, 0
txt_isdir   db "Is a directory.", 13, 10, 0
txt_notdir  db "Not a directory.", 13, 10, 0
txt_exists  db "Already exists.", 13, 10, 0
txt_full    db "Directory is full.", 13, 10, 0
txt_diskfull db "Disk is full.", 13, 10, 0
txt_invalid db "Invalid name.", 13, 10, 0
txt_mkdir_ok db "Directory created.", 13, 10, 0
txt_touch_ok db "File created.", 13, 10, 0
txt_alias_state db "System aliases: ", 0
txt_on      db "on", 13, 10, 0
txt_off     db "off", 13, 10, 0
txt_true    db "true", 0
txt_false   db "false", 0
txt_who     db "root", 13, 10, 0
txt_pck_usage db "Usage: pck list | pck install <name> | pck remove <name>", 13, 10, 0
txt_pcks    db " package(s)", 13, 10, 0
txt_nopck   db "No PCK directory.", 13, 10, 0
txt_badpck  db "Invalid package file.", 13, 10, 0
txt_pck_ok  db "Package installed.", 13, 10, 0
txt_pck_rm  db "Package removed.", 13, 10, 0
txt_list    db "list", 0
txt_install db "install", 0
txt_remove  db "remove", 0
txt_pm_enter db "Entering 32-bit protected mode...", 13, 10, 0
txt_pm_ok   db "Back in real mode. R/W test at 1 MB and 16 MB: OK.", 13, 10
            db "32-bit PM works - the full 4 GB address space is reachable.", 13, 10, 0
txt_pm_fail db "Protected mode high-memory test failed.", 13, 10, 0
txt_pm_a20  db "Failed to enable A20 gate.", 13, 10, 0
txt_alias_off db "Unknown command. Type 'help'. This command is not a standard ROS command. Use 'alias -a true' to enable system alias.", 13, 10, 0

txt_about   db "ROS Command - the command-line edition of ROS.", 13, 10
            db "A tiny kernel written in pure assembly.", 13, 10
            db "Developer : XR Stu", 13, 10
            db "Website   : https://xrst.uk", 13, 10
            db "Shell     : xsh (X Shell)", 13, 10, 0

txt_help    db "Available commands:", 13, 10
            db "  help    - show this help", 13, 10
            db "  about   - about ROS Command", 13, 10
            db "  ver     - show version", 13, 10
            db "  clear   - clear screen (alias: cls)", 13, 10
            db "  echo    - print text, e.g. echo hello", 13, 10
            db "  dir     - list files (alias: ls)", 13, 10
            db "  doc     - show file contents (aliases: cat, type)", 13, 10
            db "  mkdir   - create a directory", 13, 10
            db "  touch   - create an empty file", 13, 10
            db "  goto    - change directory (alias: cd)", 13, 10
            db "  where   - show current path (alias: pwd)", 13, 10
            db "  who     - show current user (alias: whoami)", 13, 10
            db "  alias   - alias -a [true|false]", 13, 10
            db "  pck     - packages: pck list / install / remove", 13, 10
            db "  pm      - enter 32-bit protected mode / test high memory", 13, 10
            db "  time    - show current time", 13, 10
            db "  date    - show current date", 13, 10
            db "  mem     - show conventional memory", 13, 10
            db "  reboot  - reboot the system", 13, 10
            db "  halt    - halt the system", 13, 10, 0

; --------------------------------------------
; Global Descriptor Table (base 0x10000)
; --------------------------------------------
gdt_start:
    dq 0                        ; null descriptor
gdt_code32:                     ; 0x08 32-bit code, flat, 4 GB
    dw 0xFFFF
    dw 0x0000
    db 0x01
    db 0x9A
    db 0xCF
    db 0x00
gdt_data32:                     ; 0x10 32-bit data, flat, 4 GB
    dw 0xFFFF
    dw 0x0000
    db 0x01
    db 0x92
    db 0xCF
    db 0x00
gdt_code16:                     ; 0x18 16-bit code, 64 KB
    dw 0xFFFF
    dw 0x0000
    db 0x01
    db 0x9A
    db 0x00
    db 0x00
gdt_data16:                     ; 0x20 16-bit data, 64 KB
    dw 0xFFFF
    dw 0x0000
    db 0x01
    db 0x92
    db 0x00
    db 0x00
gdt_end:
gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd 0                        ; physical base, filled by detect_ram

boot_drive  db 0
aliases_on  db 1
found_attr  db 0
cur_dir     dw 0                ; 0 = root, else first cluster
cur_lba     dw 0
entry_ptr   dw 0
arg_ptr     dw 0
file_cnt    dw 0
file_size   dw 0
new_cluster dw 0
walk_lba    dw 0
walk_left   dw 0
walk_cluster dw 0
load_cluster dw 0

; pck package manager state
pck_save    dw 0
pck_dir     dw 0
pck_off     dw 0
pck_size    dw 0
pck_count   dw 0
pck_first   dw 0
pck_prev    dw 0
pck_clusters dw 0
pck_data    dw 0
pck_delcl   dw 0
pck_next    dw 0
pkg_name    times 11 db 0

; protected mode / memory detection state
saved_sp    dw 0
pm_ok       db 0
e820_cnt    dw 0
ram_mb      dd 0
pm_stack    times 256 db 0
pm_stack_top equ $
e820_buf    times 24*16 db 0

; FAT16 runtime layout (parsed from the VBR's BPB by init_fs)
part_base   dw 0                ; partition base LBA
geo_spt     dw 0                ; sectors per track (from BIOS)
geo_heads   dw 0                ; number of heads (from BIOS)
sec_per_clus dw 0
sec_per_fat dw 0
root_secs   dw 0
fat_lba     dw 0
root_lba    dw 0
data_lba    dw 0
max_cluster dw 0                ; first invalid cluster id

cur_path    db "/", 0
            times PATH_SIZE-2 db 0
fat_name    times 11 db 0

cmd_buf     times BUF_SIZE db 0
dir_buf     times 512 db 0
fat_buf     times 512*MAX_FAT_SEC db 0
