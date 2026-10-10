/* ir.c - LIR construction helpers and the canonical text printer. */
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ir_module_init(IrModule *m, const char *source) {
    memset(m, 0, sizeof *m);
    m->source = xstrdup(source ? source : "");
}

void ir_module_free(IrModule *m) {
    for (int i = 0; i < m->nfuncs; i++) {
        IrFunc *f = &m->funcs[i];
        for (int b = 0; b < f->nblocks; b++) {
            for (int k = 0; k < f->blocks[b].n; k++) free(f->blocks[b].instrs[k].args);
            free(f->blocks[b].instrs);
            free(f->blocks[b].label);
        }
        for (int v = 0; v < f->nvregs; v++) free(f->vregs[v]);
        free(f->vregs);
        free(f->blocks);
    }
    for (int i = 0; i < m->nglobals; i++) {
        free(m->globals[i].name);
        free(m->globals[i].data);
    }
    free(m->funcs);
    free(m->globals);
    free(m->source);
    memset(m, 0, sizeof *m);
}

int ir_find_global(const IrModule *m, const char *name) {
    for (int i = 0; i < m->nglobals; i++)
        if (strcmp(m->globals[i].name, name) == 0) return i;
    return -1;
}

static int add_global(IrModule *m, IrGlobalKind kind, const char *name) {
    m->globals = xrealloc(m->globals, (size_t)(m->nglobals + 1) * sizeof *m->globals);
    IrGlobal *g = &m->globals[m->nglobals];
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->name = xstrdup(name);
    g->func = -1;
    return m->nglobals++;
}

int ir_add_extern(IrModule *m, const char *name, int arity) {
    int g = add_global(m, IRG_EXTERN, name);
    m->globals[g].arity = arity;
    return g;
}

int ir_add_data(IrModule *m, const char *name, const char *bytes, size_t len) {
    int g = add_global(m, IRG_DATA, name);
    m->globals[g].data = xstrndup(bytes, len);
    m->globals[g].data_len = len;
    return g;
}

int ir_add_var(IrModule *m, const char *name) { return add_global(m, IRG_VAR, name); }

const char *ir_var_display_name(const char *name) {
    const char *dot = strchr(name, '.');
    return dot && dot[1] ? dot + 1 : name;
}

int ir_add_func(IrModule *m, const char *name, int nparams) {
    int g = add_global(m, IRG_FUNC, name);
    m->funcs = xrealloc(m->funcs, (size_t)(m->nfuncs + 1) * sizeof *m->funcs);
    IrFunc *f = &m->funcs[m->nfuncs];
    memset(f, 0, sizeof *f);
    f->global = g;
    f->nparams = nparams;
    m->globals[g].arity = nparams;
    m->globals[g].func = m->nfuncs;
    return m->nfuncs++;
}

int ir_func_find_vreg(const IrFunc *f, const char *name) {
    for (int i = 0; i < f->nvregs; i++)
        if (strcmp(f->vregs[i], name) == 0) return i;
    return -1;
}

int ir_func_vreg(IrFunc *f, const char *name) {
    int i = ir_func_find_vreg(f, name);
    if (i >= 0) return i;
    return ir_func_new_vreg(f, name);
}

static void canon_visit(int *map, int *next, int v) {
    if (v >= 0 && map[v] < 0) map[v] = (*next)++;
}

void ir_func_canonicalize(IrFunc *f) {
    int n = f->nvregs;
    if (n == 0) return;
    int *map = xmalloc((size_t)n * sizeof *map);
    for (int i = 0; i < n; i++) map[i] = -1;
    int next = 0;
    for (int p = 0; p < f->nparams; p++) canon_visit(map, &next, p);
    for (int b = 0; b < f->nblocks; b++) {
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            canon_visit(map, &next, in->dst);
            canon_visit(map, &next, in->a);
            canon_visit(map, &next, in->b);
            for (int j = 0; j < in->nargs; j++) canon_visit(map, &next, in->args[j]);
        }
    }
    /* Unreferenced vregs (e.g. left behind by pruned dead code) are dropped:
     * they would only waste stack slots, and the text form cannot express them. */
    char **names = xmalloc((size_t)n * sizeof *names);
    for (int i = 0; i < n; i++) {
        if (map[i] >= 0) names[map[i]] = f->vregs[i];
        else free(f->vregs[i]);
    }
    memcpy(f->vregs, names, (size_t)next * sizeof *names);
    f->nvregs = next;
    free(names);
    for (int b = 0; b < f->nblocks; b++) {
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (in->dst >= 0) in->dst = map[in->dst];
            if (in->a >= 0) in->a = map[in->a];
            if (in->b >= 0) in->b = map[in->b];
            for (int j = 0; j < in->nargs; j++) in->args[j] = map[in->args[j]];
        }
    }
    free(map);
}

int ir_func_new_vreg(IrFunc *f, const char *name) {
    if (f->nvregs == f->cap_vregs) {
        f->cap_vregs = f->cap_vregs ? f->cap_vregs * 2 : 16;
        f->vregs = xrealloc(f->vregs, (size_t)f->cap_vregs * sizeof *f->vregs);
    }
    f->vregs[f->nvregs] = xstrdup(name);
    return f->nvregs++;
}

int ir_func_find_block(const IrFunc *f, const char *label) {
    for (int i = 0; i < f->nblocks; i++)
        if (strcmp(f->blocks[i].label, label) == 0) return i;
    return -1;
}

int ir_func_block(IrFunc *f, const char *label) {
    if (f->nblocks == f->cap_blocks) {
        f->cap_blocks = f->cap_blocks ? f->cap_blocks * 2 : 8;
        f->blocks = xrealloc(f->blocks, (size_t)f->cap_blocks * sizeof *f->blocks);
    }
    IrBlock *b = &f->blocks[f->nblocks];
    memset(b, 0, sizeof *b);
    b->label = xstrdup(label);
    return f->nblocks++;
}

IrInstr *ir_emit(IrFunc *f, int block, IrOp op) {
    IrBlock *b = &f->blocks[block];
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->instrs = xrealloc(b->instrs, (size_t)b->cap * sizeof *b->instrs);
    }
    IrInstr *in = &b->instrs[b->n++];
    memset(in, 0, sizeof *in);
    in->op = op;
    in->dst = in->a = in->b = IR_NONE;
    in->global = IR_NONE;
    in->target[0] = in->target[1] = IR_NONE;
    return in;
}

bool ir_op_is_terminator(IrOp op) { return op == IR_JMP || op == IR_BR || op == IR_RET; }

bool ir_block_terminated(const IrBlock *b) { return b->n > 0 && ir_op_is_terminator(b->instrs[b->n - 1].op); }

bool ir_op_is_binary(IrOp op) { return op >= IR_ADD && op <= IR_GE; }
bool ir_op_is_unary(IrOp op) { return op == IR_NEG || op == IR_NOT; }

const char *ir_op_name(IrOp op) {
    switch (op) {
    case IR_CONST_INT: case IR_CONST_NIL: case IR_CONST_TRUE: case IR_CONST_FALSE: case IR_CONST_DATA:
        return "const";
    case IR_MOV: return "mov";
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_DIV: return "div";
    case IR_MOD: return "mod";
    case IR_EQ: return "eq";
    case IR_NE: return "ne";
    case IR_LT: return "lt";
    case IR_LE: return "le";
    case IR_GT: return "gt";
    case IR_GE: return "ge";
    case IR_NEG: return "neg";
    case IR_NOT: return "not";
    case IR_CALL: return "call";
    case IR_LOAD: return "load";
    case IR_STORE: return "store";
    case IR_JMP: return "jmp";
    case IR_BR: return "br";
    case IR_RET: return "ret";
    }
    return "?";
}

const char *ir_op_runtime(IrOp op) {
    switch (op) {
    case IR_ADD: return "luma_add";
    case IR_SUB: return "luma_sub";
    case IR_MUL: return "luma_mul";
    case IR_DIV: return "luma_div";
    case IR_MOD: return "luma_mod";
    case IR_EQ: return "luma_eq";
    case IR_NE: return "luma_ne";
    case IR_LT: return "luma_lt";
    case IR_LE: return "luma_le";
    case IR_GT: return "luma_gt";
    case IR_GE: return "luma_ge";
    case IR_NEG: return "luma_neg";
    case IR_NOT: return "luma_not";
    default: return NULL;
    }
}

/* ---- printer ----------------------------------------------------------- */

static void print_string(Buf *out, const char *s, size_t n) {
    buf_byte(out, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') buf_printf(out, "\\%c", c);
        else if (c == '\n') buf_printf(out, "\\n");
        else if (c == '\t') buf_printf(out, "\\t");
        else if (c >= 0x20 && c < 0x7f) buf_byte(out, c);
        else buf_printf(out, "\\x%02x", c);
    }
    buf_byte(out, '"');
}

static const char *vname(const IrFunc *f, int v) { return v >= 0 && v < f->nvregs ? f->vregs[v] : "?"; }
static const char *gname(const IrModule *m, int g) { return g >= 0 && g < m->nglobals ? m->globals[g].name : "?"; }
static const char *bname(const IrFunc *f, int b) { return b >= 0 && b < f->nblocks ? f->blocks[b].label : "?"; }

void ir_print_instr(const IrModule *m, const IrFunc *f, const IrInstr *in, Buf *out) {
    if (in->dst != IR_NONE) buf_printf(out, "%%%s = ", vname(f, in->dst));
    switch (in->op) {
    case IR_CONST_INT: buf_printf(out, "const %lld", (long long)in->imm); break;
    case IR_CONST_NIL: buf_printf(out, "const nil"); break;
    case IR_CONST_TRUE: buf_printf(out, "const true"); break;
    case IR_CONST_FALSE: buf_printf(out, "const false"); break;
    case IR_CONST_DATA: buf_printf(out, "const @%s", gname(m, in->global)); break;
    case IR_MOV: case IR_NEG: case IR_NOT:
        buf_printf(out, "%s %%%s", ir_op_name(in->op), vname(f, in->a));
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        buf_printf(out, "%s %%%s, %%%s", ir_op_name(in->op), vname(f, in->a), vname(f, in->b));
        break;
    case IR_CALL:
        buf_printf(out, "call @%s(", gname(m, in->global));
        for (int i = 0; i < in->nargs; i++) buf_printf(out, "%s%%%s", i ? ", " : "", vname(f, in->args[i]));
        buf_printf(out, ")");
        break;
    case IR_LOAD: buf_printf(out, "load @%s", gname(m, in->global)); break;
    case IR_STORE: buf_printf(out, "store @%s, %%%s", gname(m, in->global), vname(f, in->a)); break;
    case IR_JMP: buf_printf(out, "jmp %s", bname(f, in->target[0])); break;
    case IR_BR:
        buf_printf(out, "br %%%s, %s, %s", vname(f, in->a), bname(f, in->target[0]), bname(f, in->target[1]));
        break;
    case IR_RET: buf_printf(out, "ret %%%s", vname(f, in->a)); break;
    }
}

static void print_func(const IrModule *m, const IrFunc *f, Buf *out) {
    buf_printf(out, "fn @%s(", m->globals[f->global].name);
    for (int i = 0; i < f->nparams; i++) buf_printf(out, "%s%%%s", i ? ", " : "", f->vregs[i]);
    buf_printf(out, ") {\n");
    for (int b = 0; b < f->nblocks; b++) {
        buf_printf(out, "%s:\n", f->blocks[b].label);
        for (int k = 0; k < f->blocks[b].n; k++) {
            buf_printf(out, "  ");
            ir_print_instr(m, f, &f->blocks[b].instrs[k], out);
            buf_byte(out, '\n');
        }
    }
    buf_printf(out, "}\n");
}

void ir_print_module(const IrModule *m, Buf *out) {
    buf_printf(out, "module ");
    print_string(out, m->source, strlen(m->source));
    buf_byte(out, '\n');
    /* Canonical order: externs and data (in declaration order), then functions. */
    bool first = true;
    for (int i = 0; i < m->nglobals; i++) {
        const IrGlobal *g = &m->globals[i];
        if (g->kind == IRG_FUNC) continue;
        if (first) buf_byte(out, '\n');
        first = false;
        if (g->kind == IRG_EXTERN) {
            buf_printf(out, "extern fn @%s(%d)\n", g->name, g->arity);
        } else if (g->kind == IRG_VAR) {
            buf_printf(out, "global @%s\n", g->name);
        } else {
            buf_printf(out, "data @%s = str ", g->name);
            print_string(out, g->data, g->data_len);
            buf_byte(out, '\n');
        }
    }
    for (int i = 0; i < m->nglobals; i++) {
        const IrGlobal *g = &m->globals[i];
        if (g->kind != IRG_FUNC) continue;
        buf_byte(out, '\n');
        print_func(m, &m->funcs[g->func], out);
    }
}
