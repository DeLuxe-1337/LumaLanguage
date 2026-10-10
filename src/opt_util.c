/* opt_util.c - instruction classes and CFG editing helpers for the optimizer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"

IrTy *opt_value_types(const IrModule *m, const IrFunc *f) {
    IrTypes t;
    ir_types_compute(m, f, &t);
    IrTy *vty = xcalloc((size_t)(f->nvregs ? f->nvregs : 1), sizeof *vty);
    for (int b = 0; b < f->nblocks; b++)
        for (int v = 0; v < f->nvregs; v++) vty[v] = ir_ty_union(vty[v], t.out[(size_t)b * f->nvregs + v]);
    for (int p = 0; p < f->nparams; p++) vty[p] = ir_ty_union(vty[p], ir_param_ty(&m->globals[f->global], p));
    ir_types_free(&t);
    return vty;
}

static bool sub(IrTy t, IrTy of) { return ir_ty_sub(t, of); }

bool opt_may_trap(const IrModule *m, const IrInstr *in, const IrTy *vty) {
    (void)m;
    IrTy ta = vty && in->a >= 0 ? vty[in->a] : TY_ANY, tb = vty && in->b >= 0 ? vty[in->b] : TY_ANY;
    switch (in->op) {
    case IR_CONST_INT: case IR_CONST_NIL: case IR_CONST_TRUE: case IR_CONST_FALSE: case IR_CONST_DATA:
    case IR_MOV: case IR_EQ: case IR_NE: case IR_NOT: case IR_PHI: case IR_NOP: case IR_STORE:
    case IR_JMP: case IR_BR: case IR_RET:
        return false;
    case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        return !(sub(ta, TY_INT) && sub(tb, TY_INT));
    case IR_ADD:
        return !(sub(ta, TY_STR) && sub(tb, TY_STR)); /* str + str never fails; ints may overflow */
    case IR_CHECK:
        return !sub(ta, in->ty);
    case IR_NEW:
        return false;
    case IR_GETFIELD:
    case IR_SETFIELD:
        return in->sid < 0; /* static accesses are proven; dynamic ones may not find the field */
    default: /* sub, mul, div, mod, neg (overflow / type / zero), load (unassigned), call, callm */
        return true;
    }
}

bool opt_has_effect(const IrModule *m, const IrInstr *in, const IrTy *vty) {
    switch (in->op) {
    case IR_CALL: case IR_STORE: case IR_JMP: case IR_BR: case IR_RET: return true;
    /* a new object's identity is observable; fields are mutable memory */
    case IR_NEW: case IR_GETFIELD: case IR_SETFIELD: case IR_CALLM: return true;
    default: return opt_may_trap(m, in, vty);
    }
}

IrInstr *opt_append(IrFunc *f, int b, IrInstr in) {
    IrBlock *bl = &f->blocks[b];
    if (bl->n == bl->cap) {
        bl->cap = bl->cap ? bl->cap * 2 : 4;
        bl->instrs = xrealloc(bl->instrs, (size_t)bl->cap * sizeof *bl->instrs);
    }
    bl->instrs[bl->n] = in;
    return &bl->instrs[bl->n++];
}

void opt_insert(IrFunc *f, int b, int pos, IrInstr in) {
    IrBlock *bl = &f->blocks[b];
    opt_append(f, b, in);
    memmove(&bl->instrs[pos + 1], &bl->instrs[pos], (size_t)(bl->n - 1 - pos) * sizeof *bl->instrs);
    bl->instrs[pos] = in;
}

/* Applies map (old block -> new block, -1 = removed) to every reference. */
static void remap_blocks(IrFunc *f, const int *map) {
    for (int b = 0; b < f->nblocks; b++) {
        IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            for (int t = 0; t < 2; t++)
                if (in->target[t] >= 0) in->target[t] = map[in->target[t]];
            if (in->op == IR_PHI) {
                int j = 0;
                for (int i = 0; i < in->nargs; i++) {
                    if (map[in->phi_blocks[i]] < 0) continue; /* edge from a removed block */
                    in->args[j] = in->args[i];
                    in->phi_blocks[j] = map[in->phi_blocks[i]];
                    j++;
                }
                in->nargs = j;
            }
        }
    }
}

void opt_remove_unreachable(IrFunc *f) {
    int nb = f->nblocks;
    if (nb == 0) return;
    bool *reach = xcalloc((size_t)nb, sizeof *reach);
    int *stack = xmalloc((size_t)nb * sizeof *stack), sp = 0;
    reach[0] = true;
    stack[sp++] = 0;
    while (sp > 0) {
        const IrBlock *bl = &f->blocks[stack[--sp]];
        if (bl->n == 0) continue;
        const IrInstr *t = &bl->instrs[bl->n - 1];
        int ns = t->op == IR_JMP ? 1 : t->op == IR_BR ? 2 : 0;
        for (int k = 0; k < ns; k++)
            if (!reach[t->target[k]]) {
                reach[t->target[k]] = true;
                stack[sp++] = t->target[k];
            }
    }
    int *map = xmalloc((size_t)nb * sizeof *map), next = 0;
    for (int b = 0; b < nb; b++) map[b] = reach[b] ? next++ : -1;
    if (next != nb) {
        for (int b = 0; b < nb; b++) {
            if (reach[b]) continue;
            for (int k = 0; k < f->blocks[b].n; k++) ir_instr_free(&f->blocks[b].instrs[k]);
            free(f->blocks[b].instrs);
            free(f->blocks[b].label);
        }
        int j = 0;
        for (int b = 0; b < nb; b++)
            if (reach[b]) f->blocks[j++] = f->blocks[b];
        f->nblocks = next;
        remap_blocks(f, map);
    }
    free(reach);
    free(stack);
    free(map);
}

int opt_insert_block(IrFunc *f, int at, const char *label_base) {
    char label[300];
    for (int k = 1;; k++) {
        snprintf(label, sizeof label, "%s.%d", label_base, k);
        if (ir_func_find_block(f, label) < 0) break;
    }
    int nb = f->nblocks;
    ir_func_block(f, label); /* appended at index nb */
    IrBlock nbk = f->blocks[nb];
    memmove(&f->blocks[at + 1], &f->blocks[at], (size_t)(nb - at) * sizeof *f->blocks);
    f->blocks[at] = nbk;
    int *map = xmalloc((size_t)(nb + 1) * sizeof *map);
    for (int b = 0; b < nb; b++) map[b] = b >= at ? b + 1 : b;
    map[nb] = at; /* (unused: nothing refers to the new block yet) */
    /* remap references in every block except the new (empty) one */
    for (int b = 0; b < nb + 1; b++) {
        if (b == at) continue;
        IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            for (int t = 0; t < 2; t++)
                if (in->target[t] >= 0) in->target[t] = map[in->target[t]];
            if (in->op == IR_PHI)
                for (int i = 0; i < in->nargs; i++) in->phi_blocks[i] = map[in->phi_blocks[i]];
        }
    }
    free(map);
    return at;
}

int opt_fresh_vreg(IrFunc *f, const char *base) {
    char name[300];
    /* strip an earlier version suffix so names stay readable: x.12 -> x */
    char root[256];
    snprintf(root, sizeof root, "%s", base);
    char *dot = strrchr(root, '.');
    if (dot && dot != root && dot[1] >= '0' && dot[1] <= '9' && strspn(dot + 1, "0123456789") == strlen(dot + 1)) *dot = '\0';
    for (int k = f->nvregs;; k++) {
        snprintf(name, sizeof name, "%s.%d", root, k);
        if (ir_func_find_vreg(f, name) < 0) break;
    }
    return ir_func_new_vreg(f, name);
}
