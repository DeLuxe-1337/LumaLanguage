# Standalone assembler test: assembled by build/lasm, linked with answer_main.c.
# answer() returns 42 via a call to a local helper (internal label resolution)
# and add with an immediate. answer_msg() returns a pointer into .rodata
# (cross-section RIP-relative relocation against a section symbol).
    .intel_syntax noprefix
    .section .rodata
.Lpad:
    .ascii "xxxx"
.Lmsg:
    .asciz "from lasm"

    .text
    .globl answer
    .type answer, @function
answer:
    push rbp
    mov rbp, rsp
    call .Lforty
    add eax, 2
    pop rbp
    ret
    .size answer, .-answer

.Lforty:
    mov eax, 40
    ret

    .globl answer_msg
    .type answer_msg, @function
answer_msg:
    lea rax, [rip + .Lmsg]
    ret
    .size answer_msg, .-answer_msg

    .section .note.GNU-stack
