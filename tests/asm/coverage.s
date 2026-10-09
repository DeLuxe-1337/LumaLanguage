# Exercises every instruction form and directive the Luma assembler supports.
# Used by tests/run_e2e.sh for a differential check against GNU as (validation
# only: the Luma build pipeline never runs an external assembler).
    .intel_syntax noprefix

    .section .rodata
.Lpad:
    .byte 1, 2, 0xff, -1
.Lmsg:
    .asciz "quote\" back\\ nl\n tab\t oct\101"
    .ascii "raw"
    .string "s"

    .data
counter:
    .byte 0

    .text
    .globl entry
    .type entry, @function
entry:
    push rbp
    push r12
    mov rbp, rsp
    mov r8, rax
    mov rax, r9
    mov eax, ecx
    mov eax, 42
    mov r10d, 1
    mov rax, -1
    mov rax, 0x123456789
    xor eax, eax
    xor r11, r11
    sub rsp, 8
    add rsp, 8
    sub rsp, 256
    add eax, 2
    add rax, rcx
    sub rax, rcx
    lea rdi, [rip + .Lmsg]
    lea rsi, [rip + .Lmsg + 3]
    lea rdx, [rip + counter]
    lea r9, [rip - 4]
    call helper
    call entry
    call puts@PLT
    call external_fn
    nop
    pop r12
    pop rbp
    ret
    .size entry, .-entry
helper:
    ret

    .section .note.GNU-stack
