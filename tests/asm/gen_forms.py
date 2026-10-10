#!/usr/bin/env python3
"""Generates an exhaustive-ish set of instruction forms (every base register
x displacement class, base+index*scale, index-only, RIP-relative with
trailing immediates, byte registers, setcc/movzx/test/imul/shifts/neg/idiv,
memory-immediate forms...). tests/run_e2e.sh assembles the output with both
lasm and GNU as and requires identical bytes and relocations."""
regs = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
r8 = ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"]
r32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"]


def disp(d):
    return "" if d == 0 else ("+%d" % d if d > 0 else "-%d" % -d)


print(".intel_syntax noprefix\n.data\nslot: .quad 0\n.text")
mems = []
for b in regs:
    for d in (0, 8, -8, 127, -128, 128, -129, 1024, -40000):
        mems.append("[%s%s]" % (b, disp(d)))
for b in ["rax", "rsp", "rbp", "r12", "r13", "r15"]:
    for i in ["rcx", "rbp", "r12", "r13", "r8"]:
        for s in (1, 2, 4, 8):
            for d in (0, 16, -300):
                mems.append("[%s + %s*%d%s]" % (b, i, s, disp(d)))
for i in ["rax", "r11", "rbp", "r13"]:
    for s in (1, 2, 8):
        mems.append("[%s*%d + 2]" % (i, s))
mems += ["[rip + slot]", "[rip + slot + 8]"]
for m in mems:
    print("    mov rax, %s" % m)
    print("    mov %s, r13" % m)
    print("    lea r9, %s" % m)
for m in mems[::7]:
    for op in ["add", "or", "and", "sub", "xor", "cmp"]:
        print("    %s rdx, %s" % (op, m))
        print("    %s %s, r10" % (op, m))
        print("    %s qword ptr %s, 5" % (op, m))
        print("    %s qword ptr %s, 1000" % (op, m))
    print("    test %s, rsi" % m)
    print("    test rsi, %s" % m)
    print("    test qword ptr %s, 1" % m)
    print("    test byte ptr %s, 7" % m)
    print("    mov qword ptr %s, -5" % m)
    print("    mov qword ptr %s, 123456" % m)
    print("    imul r12, %s" % m)
    print("    imul rax, %s, 3" % m)
    print("    imul rax, %s, 300" % m)
    print("    neg qword ptr %s" % m)
    print("    idiv qword ptr %s" % m)
    print("    sar qword ptr %s, 1" % m)
    print("    movzx eax, byte ptr %s" % m)
    print("    cmovl rax, %s" % m)
for x in regs:
    for y in regs:
        print("    imul %s, %s" % (x, y))
        print("    test %s, %s" % (x, y))
        print("    cmovge %s, %s" % (x, y))
    print("    imul %s, %s, 2" % (x, x))
    print("    imul %s, rbx, -1000" % x)
    for sh in ["shl", "shr", "sar"]:
        for k in (1, 2, 63):
            print("    %s %s, %d" % (sh, x, k))
    print("    neg %s" % x)
    print("    not %s" % x)
    print("    idiv %s" % x)
for i in range(16):
    for cc in ["e", "ne", "l", "le", "g", "ge", "b", "a", "o", "s"]:
        print("    set%s %s" % (cc, r8[i]))
    print("    movzx %s, %s" % (r32[i], r8[i]))
    print("    movzx %s, %s" % (regs[i], r8[(i + 3) % 16]))
    print("    test %s, 1" % r8[i])
    print("    test %s, 255" % r8[i])
    print("    test %s, 1" % r32[i])
    print("    test %s, 4096" % regs[i])
    print("    mov %s, 3" % r8[i])
    print("    mov %s, %s" % (r8[i], r8[(i + 5) % 16]))
    print("    mov %s, [rbp-16]" % r8[i])
    print("    mov [rax+1], %s" % r8[i])
print("    cqo")
print("    mov eax, [rbp-4]")
print("    mov [rsp+8], r9d")
print("    add eax, [rbx]")
print("    cmp dword ptr [rbp-4], 7")
print("    mov dword ptr [rax], 0xffffffff")
print("    mov byte ptr [rax+3], 200")
print("    mov qword ptr [rip + slot], 42")
print("    cmp qword ptr [rip + slot + 8], 1000")
print("    .section .note.GNU-stack")
