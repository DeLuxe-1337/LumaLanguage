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

    /* milestone 2: rbp-relative memory */
    EXPECT("mov rax, [rbp - 8]", 0x48, 0x8b, 0x45, 0xf8);
    EXPECT("mov rax, [rbp-1024]", 0x48, 0x8b, 0x85, 0x00, 0xfc, 0xff, 0xff);
    EXPECT("mov rax, [rbp]", 0x48, 0x8b, 0x45, 0x00); /* mod=00 rm=101 would mean RIP */
    EXPECT("mov r9, [rbp + 16]", 0x4c, 0x8b, 0x4d, 0x10);
    EXPECT("mov [rbp - 16], rdi", 0x48, 0x89, 0x7d, 0xf0);
    EXPECT("mov [rbp - 16], r9", 0x4c, 0x89, 0x4d, 0xf0);
    EXPECT("lea rax, [rbp - 8]", 0x48, 0x8d, 0x45, 0xf8);
    /* ALU group incl. accumulator short forms (as chosen by GNU as) */
    EXPECT("cmp rax, 2", 0x48, 0x83, 0xf8, 0x02);
    EXPECT("cmp rax, 1000", 0x48, 0x3d, 0xe8, 0x03, 0x00, 0x00);
    EXPECT("cmp rcx, 1000", 0x48, 0x81, 0xf9, 0xe8, 0x03, 0x00, 0x00);
    EXPECT("cmp rax, rcx", 0x48, 0x39, 0xc8);
    EXPECT("and rax, -5", 0x48, 0x83, 0xe0, 0xfb);
    EXPECT("and rdx, rcx", 0x48, 0x21, 0xca);
    EXPECT("or rax, 1", 0x48, 0x83, 0xc8, 0x01);
    EXPECT("add rax, 0x1000", 0x48, 0x05, 0x00, 0x10, 0x00, 0x00);
    EXPECT("sub rax, 0x1000", 0x48, 0x2d, 0x00, 0x10, 0x00, 0x00);
    EXPECT("xor rax, 0x1000", 0x48, 0x35, 0x00, 0x10, 0x00, 0x00);
    EXPECT("test edx, 1", 0xf7, 0xc2, 0x01, 0x00, 0x00, 0x00);
    EXPECT("test eax, 1", 0xa9, 0x01, 0x00, 0x00, 0x00);
    EXPECT("test rax, rcx", 0x48, 0x85, 0xc8);

    /* milestone 5: general addressing (ModRM/SIB), byte registers, more instructions */
    EXPECT("mov rax, [rbx]", 0x48, 0x8b, 0x03);
    EXPECT("mov rax, [rsp]", 0x48, 0x8b, 0x04, 0x24);               /* rsp base needs SIB */
    EXPECT("mov rax, [r12 + 8]", 0x49, 0x8b, 0x44, 0x24, 0x08);
    EXPECT("mov rax, [r13]", 0x49, 0x8b, 0x45, 0x00);               /* r13 base needs disp8 */
    EXPECT("lea rax, [rbx + rcx*8 + 16]", 0x48, 0x8d, 0x44, 0xcb, 0x10);
    EXPECT("lea rax, [r11*8 + 2]", 0x4a, 0x8d, 0x04, 0xdd, 0x02, 0x00, 0x00, 0x00); /* no base */
    EXPECT("lea r9, [rax - 1]", 0x4c, 0x8d, 0x48, 0xff);
    EXPECT("mov qword ptr [rbp - 8], 5", 0x48, 0xc7, 0x45, 0xf8, 0x05, 0x00, 0x00, 0x00);
    EXPECT("cmp qword ptr [rbp - 8], 1000", 0x48, 0x81, 0x7d, 0xf8, 0xe8, 0x03, 0x00, 0x00);
    EXPECT("add rdx, [rbp - 16]", 0x48, 0x03, 0x55, 0xf0);
    EXPECT("sete al", 0x0f, 0x94, 0xc0);
    EXPECT("setl sil", 0x40, 0x0f, 0x9c, 0xc6);                     /* sil needs an empty REX */
    EXPECT("movzx eax, al", 0x0f, 0xb6, 0xc0);
    EXPECT("movzx r11d, r11b", 0x45, 0x0f, 0xb6, 0xdb);
    EXPECT("test al, 1", 0xa8, 0x01);
    EXPECT("test r11b, 1", 0x41, 0xf6, 0xc3, 0x01);
    EXPECT("sar rax, 1", 0x48, 0xd1, 0xf8);
    EXPECT("sar rax, 3", 0x48, 0xc1, 0xf8, 0x03);
    EXPECT("shl r10, 4", 0x49, 0xc1, 0xe2, 0x04);
    EXPECT("imul rax, rcx", 0x48, 0x0f, 0xaf, 0xc1);
    EXPECT("imul rax, rcx, 10", 0x48, 0x6b, 0xc1, 0x0a);
    EXPECT("imul rax, rax, 1000", 0x48, 0x69, 0xc0, 0xe8, 0x03, 0x00, 0x00);
    EXPECT("neg rax", 0x48, 0xf7, 0xd8);
    EXPECT("idiv r11", 0x49, 0xf7, 0xfb);
    EXPECT("cqo", 0x48, 0x99);
    EXPECT("cmovl rax, rcx", 0x48, 0x0f, 0x4c, 0xc1);
}

/* Returns the .text bytes of src, or NULL (caller frees via obj_free). */
static const Buf *text_of(const char *src, ObjFile *o) {
    if (!assemble_str(src, o)) return NULL;
    int t = obj_find_section(o, ".text");
    return t >= 0 ? &o->sections[t].data : NULL;
}

static char *nops(const char *head, int count, const char *tail) {
    size_t n = strlen(head) + strlen(tail) + (size_t)count * 4 + 1;
    char *s = malloc(n);
    strcpy(s, head);
    for (int i = 0; i < count; i++) strcat(s, "nop\n");
    strcat(s, tail);
    return s;
}

static void test_relaxation(void) {
    ObjFile o;
    const Buf *t;
    char *src;

    /* forward: displacement 127 fits rel8 */
    src = nops(".text\njmp L\n", 127, "L: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->len == 2 + 127 + 1 && t->data[0] == 0xeb && t->data[1] == 127);
    obj_free(&o); free(src);
    /* forward: displacement 128 needs rel32: E9 80 00 00 00 */
    src = nops(".text\njmp L\n", 128, "L: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->len == 5 + 128 + 1 && t->data[0] == 0xe9 && t->data[1] == 128 && t->data[2] == 0);
    obj_free(&o); free(src);
    /* backward: -128 fits (126 nops + 2-byte jump) */
    src = nops(".text\nL:\n", 126, "jmp L\n");
    t = text_of(src, &o);
    CHECK(t && t->len == 126 + 2 && t->data[126] == 0xeb && t->data[127] == 0x80);
    obj_free(&o); free(src);
    /* backward: -129 does not: long form, disp = -(127 + 5) */
    src = nops(".text\nL:\n", 127, "jmp L\n");
    t = text_of(src, &o);
    CHECK(t && t->len == 127 + 5 && t->data[127] == 0xe9);
    if (t) {
        int32_t d = (int32_t)(t->data[128] | t->data[129] << 8 | t->data[130] << 16 | (uint32_t)t->data[131] << 24);
        CHECK_EQ_INT(d, -132);
    }
    obj_free(&o); free(src);
    /* jcc short and long */
    src = nops(".text\nje L\n", 10, "L: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->data[0] == 0x74 && t->data[1] == 10);
    obj_free(&o); free(src);
    src = nops(".text\njne L\n", 200, "L: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->data[0] == 0x0f && t->data[1] == 0x85 && t->data[2] == 200 && t->data[3] == 0);
    obj_free(&o); free(src);
    /* mixed: OUTER disp = 2 + 126 + 1 = 129 -> long; INNER disp = 126 -> short */
    src = nops(".text\njmp OUTER\njmp INNER\n", 126, "INNER: nop\nOUTER: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->data[0] == 0xe9 && t->data[5] == 0xeb && t->data[6] == 126);
    obj_free(&o); free(src);
    /* cascade: pass 1 makes jmp B long (disp 134); that pushes A from disp
     * 126 to 129, so pass 2 makes jmp A long too; pass 3 is stable. */
    src = nops(".text\njmp A\njmp B\n", 124, "A: nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nB: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->data[0] == 0xe9 && t->data[5] == 0xe9 && t->len == 5 + 5 + 124 + 10 + 1);
    obj_free(&o); free(src);
    /* jump to an external symbol is always long with a PLT32 relocation */
    CHECK(assemble_str(".text\njmp ext\nje ext\n", &o));
    CHECK_EQ_INT(o.sections[0].data.len, 5 + 6);
    CHECK_EQ_INT(o.nrelocs, 2);
    CHECK_EQ_INT(o.relocs[0].type, OBJ_R_X86_64_PLT32);
    CHECK_EQ_INT(o.relocs[1].offset, 7);
    obj_free(&o);
    /* jumps to a GLOBAL symbol defined in the same section bind locally and
     * relax (as GNU as does: tail calls); calls keep their PLT32 relocation */
    CHECK(assemble_str(".text\n.globl f\nf: jmp g\ncall g\n.globl g\ng: ret\n", &o));
    {
        const Buf *tx = &o.sections[0].data;
        CHECK(tx->len == 2 + 5 + 1 && tx->data[0] == 0xeb && tx->data[1] == 5 && tx->data[2] == 0xe8);
    }
    CHECK_EQ_INT(o.nrelocs, 1);
    CHECK_EQ_INT(o.relocs[0].offset, 3);
    obj_free(&o);
    src = nops(".text\n.globl f\nf: jmp g\n", 200, ".globl g\ng: ret\n");
    t = text_of(src, &o);
    CHECK(t && t->data[0] == 0xe9 && t->data[1] == 200 && t->data[2] == 0);
    CHECK_EQ_INT(o.nrelocs, 0);
    obj_free(&o); free(src);
}

static void test_data_directives(void) {
    ObjFile o;
    CHECK(assemble_str(".section .rodata\n.byte 1\n.p2align 3\nq: .quad 1, -1\n", &o));
    const Buf *b = &o.sections[0].data;
    CHECK_EQ_INT(b->len, 8 + 16);
    CHECK_EQ_INT(o.sections[0].align, 8);
    CHECK(b->data[8] == 1 && b->data[16] == 0xff && b->data[23] == 0xff);
    CHECK_EQ_INT(o.symbols[obj_find_symbol(&o, "q")].value, 8);
    obj_free(&o);
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
        "mov [rax], [rbx]\n",          /* two memory operands */
        "lea rax, [rbx + rcx + rdx]\n", /* three registers */
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
        "mov rax, [rbx + rsp*2]\n",    /* rsp cannot be an index */
        "mov rax, [rbp + sym]\n",      /* symbols only with rip */
        "mov rax, [ebx]\n",            /* 32-bit address registers */
        "mov rax, [rbx*3]\n",          /* invalid scale */
        "mov [rbx], 5\n",              /* ambiguous size */
        "add qword ptr [rbx], rax, 1\n", /* too many operands for add */
        "mov rax, word ptr [rbx]\n",   /* unsupported size qualifier */
        "mov eax, rbx\n",              /* size mismatch */
        "sar rax, 64\n",               /* shift count */
        "sete rax\n",                  /* setcc needs a byte register */
        "movzx eax, ebx\n",            /* movzx needs an 8-bit source */
        "imul rax, rbx, 0x100000000\n",
        "mov al, 300\n",
        "jmp rax\n",                   /* indirect jumps unsupported */
        "jmp .Lnowhere\n",             /* undefined local label */
        ".text\n.p2align 4\n",         /* .p2align in code */
        ".section .rodata\n.p2align 99\n",
        ".quad 1x\n",
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
    test_relaxation();
    test_data_directives();
    test_errors();
    return check_report("test_asm");
}
