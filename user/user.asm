; ============================================
; ROS Command - demo ring-3 user program
; Flat binary linked at USER_CODE_VA (0x40000000)
; Syscalls: eax=1 write(ebx,ecx), 2 exit, 3 getpid
; ============================================
bits 32
org 0x40000000

%define MSGLEN 33

_start:
    mov ebp, 40
.loop:
    mov eax, 3                  ; getpid
    int 0x80

    mov ebx, msg_a
    test eax, 1
    jz .print
    mov ebx, msg_b
.print:
    mov eax, 1                  ; write
    mov ecx, MSGLEN
    int 0x80

    mov edx, 150000000          ; busy delay, timer preempts us here
.delay:
    dec edx
    jnz .delay

    mov eax, 4                  ; yield to the scheduler
    int 0x80

    dec ebp
    jnz .loop

    mov eax, 2                  ; exit
    int 0x80
.hang:
    jmp .hang

msg_a: db "A: hello from user mode (ring 3)", 10
msg_b: db "B: hello from user mode (ring 3)", 10
