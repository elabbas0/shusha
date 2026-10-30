bits 64
default rel

; Shusha userspace entry.  The kernel puts argc at [rsp] and argv at
; [rsp+8] (user_build_initial_stack in kernel/proc/user.c).

global _start
extern main

section .text
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    and rsp, -16
    call main
    mov rdi, rax
    mov eax, 4              ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
