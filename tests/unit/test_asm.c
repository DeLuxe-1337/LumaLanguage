/* Unit tests for the x86-64 assembler (independent of ELF output).
 * Expected encodings come from the Intel SDM opcode tables and were
 * cross-checked against GNU as (see tests/run_e2e.sh differential test). */
#include <stdlib.h>

#include "asm.h"
#include "check.h"

static bool assemble_str(const char *src, ObjFile *o) { return assemble("t.s", src, strlen(src), o); }

/* Assembles one line in .text and compares the bytes. */
static void expect_bytes(const char *insn, const unsigned char *want, size_t n) {
    char src[256];
    snprintf(src, sizeof src, ".text\n%s\n", insn);
    ObjFile o;
    if (!assemble_str(src, &o)) {
        check_count++;
        check_failures++;
        printf("FAIL: could not assemble '%s'\n", insn);
        return;
    }
    const Buf *b = &o.sections[0].data;
    check_count++;
    if (b->len != n || memcmp(b->data, want, n) != 0) {
        check_failures++;
        printf("FAIL: '%s' encoded as", insn);
        for (size_t i = 0; i < b->len; i++) printf(" %02x", b->data[i]);
        printf(", expected");
        for (size_t i = 0; i < n; i++) printf(" %02x", want[i]);
        printf("\n");
    }
    obj_free(&o);
}

#define EXPECT(insn, ...)                                         \
    do {                                                          \
        static const unsigned char w[] = {__VA_ARGS__};           \
        expect_bytes(insn, w, sizeof w);                          \
    } while (0)

static void test_encodings(void) {
    EXPECT("ret", 0xc3);
    EXPECT("nop", 0x90);
    EXPECT("push rbp", 0x55);
    EXPECT("push r12", 0x41, 0x54);
    EXPECT("pop rbp", 0x5d);
    EXPECT("pop r15", 0x41, 0x5f);
    EXPECT("mov rbp, rsp", 0x48, 0x89, 0xe5);
    EXPECT("mov r8, rax", 0x49, 0x89, 0xc0);
    EXPECT("mov rax, r9", 0x4c, 0x89, 0xc8);
    EXPECT("mov eax, ecx", 0x89, 0xc8);
    EXPECT("mov eax, 42", 0xb8, 0x2a, 0x00, 0x00, 0x00);
    EXPECT("mov r10d, 1", 0x41, 0xba, 0x01, 0x00, 0x00, 0x00);
    EXPECT("mov rax, -1", 0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff);
    EXPECT("mov rax, 0x123456789", 0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00);
    EXPECT("xor eax, eax", 0x31, 0xc0);
    EXPECT("xor r11, r11", 0x4d, 0x31, 0xdb);
    EXPECT("sub rsp, 8", 0x48, 0x83, 0xec, 0x08);
    EXPECT("add rsp, 8", 0x48, 0x83, 0xc4, 0x08);
    EXPECT("sub rsp, 256", 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00);
    EXPECT("add eax, 2", 0x83, 0xc0, 0x02);
    EXPECT("add rax, rcx", 0x48, 0x01, 0xc8);
    EXPECT("xor eax, 1", 0x83, 0xf0, 0x01);
    EXPECT("xor r8, 0x1000", 0x49, 0x81, 0xf0, 0x00, 0x10, 0x00, 0x00);
    EXPECT("sub rax, rcx", 0x48, 0x29, 0xc8);
    EXPECT("lea rdi, [rip + 16]", 0x48, 0x8d, 0x3d, 0x10, 0x00, 0x00, 0x00);
    EXPECT("lea r9, [rip - 4]", 0x4c, 0x8d, 0x0d, 0xfc, 0xff, 0xff, 0xff);
    EXPECT("RET", 0xc3); /* mnemonics are case-insensitive */
}

static void test_relocations(void) {
    ObjFile o;
    const char *src =
        "    .intel_syntax noprefix\n"
        "    .section .rodata\n"
        ".Lpad: .byte 1, 2, 3\n"
        ".Lmsg: .asciz \"hi\\n\"   # comment with \"quotes\"\n"
        "    .text\n"
        "    .globl main\n"
        "main:\n"
        "    lea rdi, [rip + .Lmsg]\n"
        "    lea rsi, [rip + .Lmsg + 2]\n"
        "    call puts@PLT\n"
        "    call helper\n"
        "    call main\n"
        "    ret\n"
        "helper: ret\n";
    CHECK(assemble_str(src, &o));
    int rodata = obj_find_section(&o, ".rodata"), text = obj_find_section(&o, ".text");
    CHECK(rodata >= 0 && text >= 0);
    CHECK_EQ_INT(o.sections[rodata].data.len, 3 + 4);
    CHECK(memcmp(o.sections[rodata].data.data + 3, "hi\n", 4) == 0);
    /* lea(7) lea(7) call(5) call(5) call(5) ret(1) helper:ret */
    CHECK_EQ_INT(o.sections[text].data.len, 7 + 7 + 5 + 5 + 5 + 1 + 1);

    /* 4 relocations: 2x lea -> .rodata (section symbol), puts (undef),
     * main (global, preemptible). helper is local => patched in place. */
    CHECK_EQ_INT(o.nrelocs, 4);
    ObjReloc *r = o.relocs;
    CHECK_EQ_INT(r[0].type, OBJ_R_X86_64_PC32);
    CHECK_EQ_INT(r[0].offset, 3);
    CHECK_EQ_INT(r[0].symbol, -1);
    CHECK_EQ_INT(r[0].target_section, rodata);
    CHECK_EQ_INT(r[0].addend, 3 - 4); /* .Lmsg is at .rodata+3 */
    CHECK_EQ_INT(r[1].offset, 10);
    CHECK_EQ_INT(r[1].addend, 3 + 2 - 4);
    CHECK_EQ_INT(r[2].type, OBJ_R_X86_64_PLT32);
    CHECK_EQ_INT(r[2].offset, 15);
    CHECK(strcmp(o.symbols[r[2].symbol].name, "puts") == 0);
    CHECK_EQ_INT(r[2].addend, -4);
    CHECK(o.symbols[r[2].symbol].global);
    CHECK(strcmp(o.symbols[r[3].symbol].name, "main") == 0);

    /* call helper at offset 19; field at 20; helper at 30 => 30 - 24 = 6 */
    const unsigned char *t = o.sections[text].data.data;
    CHECK_EQ_INT(t[19], 0xe8);
    CHECK_EQ_INT(t[20] | t[21] << 8 | t[22] << 16 | t[23] << 24, 6);
    int helper = obj_find_symbol(&o, "helper");
    CHECK(helper >= 0 && !o.symbols[helper].global && o.symbols[helper].value == 30);
    obj_free(&o);

    /* backward local call: field = target - next_insn */
    CHECK(assemble_str(".text\nloop: nop\ncall loop\n", &o));
    const unsigned char *u = o.sections[0].data.data;
    CHECK_EQ_INT((int32_t)(u[2] | u[3] << 8 | u[4] << 16 | (uint32_t)u[5] << 24), -6);
    CHECK_EQ_INT(o.nrelocs, 0);
    obj_free(&o);

    /* .size main, .-main */
    CHECK(assemble_str(".text\n.globl f\n.type f, @function\nf: push rbp\npop rbp\nret\n.size f, .-f\n", &o));
    int f = obj_find_symbol(&o, "f");
    CHECK(f >= 0 && o.symbols[f].size == 3 && o.symbols[f].type == OBJ_STT_FUNC);
    obj_free(&o);
}

static void test_errors(void) {
    ObjFile o;
    fprintf(stderr, "[expected diagnostics follow]\n");
    const char *bad[] = {
        "frobnicate rax\n",            /* unknown instruction */
        "mov rax, ebx\n",              /* size mismatch */
        "mov [rax], rbx\n",            /* non-RIP memory */
        "lea rax, [rbx + 8]\n",        /* non-RIP memory */
        "push eax\n",                  /* 32-bit push */
        "ret rax\n",                   /* operand count */
        "call .Lnowhere\n",            /* undefined local label */
        "a: nop\na: nop\n",            /* duplicate label */
        ".section .weird\n",           /* unknown section */
        ".bogus 1\n",                  /* unknown directive */
        ".asciz \"abc\n",              /* unterminated string */
        ".byte 300\n",                 /* byte out of range */
        ".section .rodata\nret\n",     /* code in data section */
        "call puts@GOT\n",             /* unsupported modifier */
        "mov eax, 0x100000000\n",      /* imm too big for r32 */
        ".intel_syntax prefix\n",      /* unsupported syntax */
        "lea rdi, [rip + a + b]\n",    /* two symbols */
        "mov rax, rbx, rcx, rdx\n",    /* too many operands */
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        bool ok = assemble_str(bad[i], &o);
        check_count++;
        if (ok) {
            check_failures++;
            printf("FAIL: expected assembly error for: %s", bad[i]);
            obj_free(&o);
        }
    }
    /* overlong line */
    char *big = malloc(10000);
    memset(big, 'a', 9999);
    big[9999] = '\0';
    CHECK(!assemble_str(big, &o));
    free(big);
}

int main(void) {
    test_encodings();
    test_relocations();
    test_errors();
    return check_report("test_asm");
}
