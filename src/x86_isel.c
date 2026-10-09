#include "x86_isel.h"

#include <inttypes.h>
#include <string.h>

#include "value.h"

static const char *const ARG_REGS[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

/* Escapes bytes for an .ascii directive (printable ASCII literal, else octal). */
static void emit_ascii(Buf *out, const char *s, size_t n) {
    buf_printf(out, "    .ascii \"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') buf_printf(out, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f) buf_byte(out, c);
        else buf_printf(out, "\\%03o", c);
    }
    buf_printf(out, "\"\n");
}

/* Writes a short, comment-safe preview of a string ('#' is fine inside an
 * asm comment; newlines are not). */
static void emit_preview(Buf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n && i < 48; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') buf_printf(out, "\\n");
        else if (c == '\t') buf_printf(out, "\\t");
        else if (c >= 0x20 && c < 0x7f) buf_byte(out, c);
        else buf_byte(out, '?');
    }
    if (n > 48) buf_printf(out, "...");
}

static int64_t slot(int v) { return -8 * ((int64_t)v + 1); }

static void load(Buf *out, const char *reg, int v) { buf_printf(out, "    mov %s, [rbp - %" PRId64 "]\n", reg, -slot(v)); }
static void store(Buf *out, int v, const char *reg) { buf_printf(out, "    mov [rbp - %" PRId64 "], %s\n", -slot(v), reg); }

static void block_label(Buf *out, const IrModule *m, const IrFunc *f, int b) {
    buf_printf(out, ".L%s.%s", m->globals[f->global].name, f->blocks[b].label);
}

static void jump(Buf *out, const char *mn, const IrModule *m, const IrFunc *f, int b) {
    buf_printf(out, "    %s ", mn);
    block_label(out, m, f, b);
    buf_byte(out, '\n');
}

static void emit_instr(const IrModule *m, const IrFunc *f, int b, const IrInstr *in, Buf *out) {
    buf_printf(out, "    # ");
    ir_print_instr(m, f, in, out);
    buf_byte(out, '\n');
    switch (in->op) {
    case IR_CONST_INT:
        buf_printf(out, "    mov rax, %" PRId64 "\n", (int64_t)luma_fixnum(in->imm));
        store(out, in->dst, "rax");
        break;
    case IR_CONST_NIL:
    case IR_CONST_TRUE:
    case IR_CONST_FALSE: {
        LumaValue v = in->op == IR_CONST_NIL ? LUMA_NIL : in->op == IR_CONST_TRUE ? LUMA_TRUE : LUMA_FALSE;
        buf_printf(out, "    mov rax, %" PRIu64 "\n", (uint64_t)v);
        store(out, in->dst, "rax");
        break;
    }
    case IR_CONST_DATA:
        buf_printf(out, "    lea rax, [rip + .Ldata.%s]\n", m->globals[in->global].name);
        store(out, in->dst, "rax");
        break;
    case IR_MOV:
        load(out, "rax", in->a);
        store(out, in->dst, "rax");
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        load(out, "rdi", in->a);
        load(out, "rsi", in->b);
        buf_printf(out, "    call %s@PLT\n", ir_op_runtime(in->op));
        store(out, in->dst, "rax");
        break;
    case IR_NEG:
    case IR_NOT:
        load(out, "rdi", in->a);
        buf_printf(out, "    call %s@PLT\n", ir_op_runtime(in->op));
        store(out, in->dst, "rax");
        break;
    case IR_CALL: {
        const IrGlobal *g = &m->globals[in->global];
        for (int i = 0; i < in->nargs; i++) load(out, ARG_REGS[i], in->args[i]);
        buf_printf(out, "    call %s%s\n", g->name, g->kind == IRG_EXTERN ? "@PLT" : "");
        if (in->dst != IR_NONE) store(out, in->dst, "rax");
        break;
    }
    case IR_JMP:
        if (in->target[0] != b + 1) jump(out, "jmp", m, f, in->target[0]);
        break;
    case IR_BR: {
        /* falsy <=> (v & ~4) == 2  (nil = 6, false = 2) */
        int t = in->target[0], e = in->target[1];
        load(out, "rax", in->a);
        buf_printf(out, "    and rax, -5\n");
        buf_printf(out, "    cmp rax, 2\n");
        if (t == b + 1) {
            jump(out, "je", m, f, e);
        } else if (e == b + 1) {
            jump(out, "jne", m, f, t);
        } else {
            jump(out, "je", m, f, e);
            jump(out, "jmp", m, f, t);
        }
        break;
    }
    case IR_RET:
        load(out, "rax", in->a);
        buf_printf(out, "    mov rsp, rbp\n");
        buf_printf(out, "    pop rbp\n");
        buf_printf(out, "    ret\n");
        break;
    }
}

static void emit_func(const IrModule *m, const IrFunc *f, Buf *out) {
    const char *name = m->globals[f->global].name;
    int64_t frame = ((int64_t)f->nvregs * 8 + 15) / 16 * 16;
    buf_printf(out, "\n    .text\n");
    buf_printf(out, "    .globl %s\n", name);
    buf_printf(out, "    .type %s, @function\n", name);
    buf_printf(out, "%s:    # %d vreg slot%s, frame %" PRId64 " bytes\n", name, f->nvregs, f->nvregs == 1 ? "" : "s", frame);
    buf_printf(out, "    push rbp\n");
    buf_printf(out, "    mov rbp, rsp\n");
    if (frame) buf_printf(out, "    sub rsp, %" PRId64 "\n", frame);
    for (int i = 0; i < f->nparams; i++) store(out, i, ARG_REGS[i]);
    for (int b = 0; b < f->nblocks; b++) {
        block_label(out, m, f, b);
        buf_printf(out, ":\n");
        for (int k = 0; k < f->blocks[b].n; k++) emit_instr(m, f, b, &f->blocks[b].instrs[k], out);
    }
    buf_printf(out, "    .size %s, .-%s\n", name, name);
}

void x86_emit_module(const IrModule *m, Buf *out) {
    buf_printf(out, "# Generated by luma from %s -- do not edit.\n", m->source);
    buf_printf(out, "    .intel_syntax noprefix\n");
    bool any_data = false;
    for (int i = 0; i < m->nglobals; i++) {
        const IrGlobal *g = &m->globals[i];
        if (g->kind != IRG_DATA) continue;
        if (!any_data) buf_printf(out, "\n    .section .rodata\n");
        any_data = true;
        buf_printf(out, "    .p2align 3\n");
        buf_printf(out, ".Ldata.%s:    # \"", g->name);
        emit_preview(out, g->data, g->data_len);
        buf_printf(out, "\"\n");
        buf_printf(out, "    .quad %d    # header: string\n", LUMA_TYPE_STRING);
        buf_printf(out, "    .quad %zu    # length\n", g->data_len);
        if (g->data_len) emit_ascii(out, g->data, g->data_len);
        buf_printf(out, "    .byte 0\n");
    }
    for (int i = 0; i < m->nglobals; i++)
        if (m->globals[i].kind == IRG_FUNC) emit_func(m, &m->funcs[m->globals[i].func], out);
    buf_printf(out, "\n    .section .note.GNU-stack\n");
}
