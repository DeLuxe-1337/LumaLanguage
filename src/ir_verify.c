/* ir_verify.c - structural and data-flow checks for LIR (docs/IR.md §6). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "value.h"

#define MAX_ARGS 6

typedef struct {
    const IrModule *m;
    const char *path;
    int errors;
} V;

static void verr(V *v, const IrFunc *f, int block, const IrInstr *in, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void verr(V *v, const IrFunc *f, int block, const IrInstr *in, const char *fmt, ...) {
    if (v->errors++ >= 20) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:", v->path);
    if (in && in->line) fprintf(stderr, "%d:", in->line);
    if (f) {
        fprintf(stderr, " @%s", v->m->globals[f->global].name);
        if (block >= 0 && block < f->nblocks) fprintf(stderr, "/%s", f->blocks[block].label);
        fputc(':', stderr);
    }
    fprintf(stderr, " error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static bool vreg_ok(const IrFunc *f, int r) { return r >= 0 && r < f->nvregs; }

/* Calls fn(ctx, vreg) for every vreg the instruction reads. */
static void for_each_use(const IrInstr *in, void (*fn)(void *, int), void *ctx) {
    switch (in->op) {
    case IR_MOV: case IR_NEG: case IR_NOT: case IR_BR: case IR_RET: case IR_STORE: case IR_CHECK:
        fn(ctx, in->a);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        fn(ctx, in->a);
        fn(ctx, in->b);
        break;
    case IR_CALL:
    case IR_PHI:
    case IR_NEW:
        for (int i = 0; i < in->nargs; i++) fn(ctx, in->args[i]);
        break;
    case IR_GETFIELD:
        fn(ctx, in->a);
        break;
    case IR_SETFIELD:
        fn(ctx, in->a);
        fn(ctx, in->b);
        break;
    case IR_CALLM:
        fn(ctx, in->a);
        for (int i = 0; i < in->nargs; i++) fn(ctx, in->args[i]);
        break;
    default: break;
    }
}

static bool op_has_dst(IrOp op) {
    return !ir_op_is_terminator(op) && op != IR_CALL && op != IR_STORE && op != IR_SETFIELD && op != IR_CALLM;
}

static bool ty_valid(const IrModule *m, IrTy t) {
    if (t == 0 || (t & 0xE0)) return false;
    if (!(t & TY_STRUCT)) return (t >> TY_SID_SHIFT) == 0;
    return ty_sid(t) < m->nstructs;
}

static bool sid_ok(const IrModule *m, int sid) { return sid >= 0 && sid < m->nstructs; }

/* ---- structural checks for one instruction ---- */
typedef struct { V *v; const IrFunc *f; int b; const IrInstr *in; } UseCtx;

static void check_use_index(void *ctx, int r) {
    UseCtx *c = ctx;
    if (!vreg_ok(c->f, r)) verr(c->v, c->f, c->b, c->in, "'%s' reads an invalid virtual register", ir_op_name(c->in->op));
}

static void check_instr(V *v, const IrFunc *f, int b, const IrInstr *in) {
    const IrModule *m = v->m;
    if (op_has_dst(in->op) && !vreg_ok(f, in->dst))
        verr(v, f, b, in, "'%s' needs a destination register", ir_op_name(in->op));
    if (in->op == IR_CALL && in->dst != IR_NONE && !vreg_ok(f, in->dst))
        verr(v, f, b, in, "call has an invalid destination register");
    if ((in->op == IR_CALL || in->op == IR_CALLM) && in->dst != IR_NONE && !vreg_ok(f, in->dst))
        verr(v, f, b, in, "'%s' has an invalid destination register", ir_op_name(in->op));
    if ((ir_op_is_terminator(in->op) || in->op == IR_STORE || in->op == IR_SETFIELD) && in->dst != IR_NONE)
        verr(v, f, b, in, "'%s' cannot have a destination", ir_op_name(in->op));
    UseCtx c = {v, f, b, in};
    for_each_use(in, check_use_index, &c);
    switch (in->op) {
    case IR_CONST_INT:
        if (in->imm < LUMA_FIXNUM_MIN || in->imm > LUMA_FIXNUM_MAX)
            verr(v, f, b, in, "integer constant %lld does not fit in a 63-bit fixnum", (long long)in->imm);
        break;
    case IR_CONST_DATA:
        if (in->global < 0 || in->global >= m->nglobals) verr(v, f, b, in, "const references an undefined global");
        else if (m->globals[in->global].kind != IRG_DATA)
            verr(v, f, b, in, "'const @%s' must reference a data global", m->globals[in->global].name);
        break;
    case IR_CHECK:
        if (in->global < 0 || in->global >= m->nglobals || m->globals[in->global].kind != IRG_DATA)
            verr(v, f, b, in, "'check' needs a data global as its error context");
        if (!ty_valid(m, in->ty)) verr(v, f, b, in, "'check' has an invalid type");
        break;
    case IR_NEW:
        if (!sid_ok(m, in->sid)) verr(v, f, b, in, "'new' of an unknown struct");
        else if (in->nargs != m->structs[in->sid].nfields)
            verr(v, f, b, in, "'new %s' gives %d field value%s, the struct has %d", m->structs[in->sid].name, in->nargs,
                 in->nargs == 1 ? "" : "s", m->structs[in->sid].nfields);
        break;
    case IR_GETFIELD:
    case IR_SETFIELD:
        if (in->sid >= 0) {
            if (!sid_ok(m, in->sid) || in->field < 0 || in->field >= m->structs[in->sid].nfields)
                verr(v, f, b, in, "'%s' of an unknown struct field", ir_op_name(in->op));
        } else if (!in->name || !*in->name) {
            verr(v, f, b, in, "'%s' needs a field name", ir_op_name(in->op));
        }
        break;
    case IR_CALLM:
        if (!in->name || !*in->name) verr(v, f, b, in, "'callm' needs a method name");
        if (in->nargs + 1 > MAX_ARGS) verr(v, f, b, in, "calls with more than %d arguments are not supported yet", MAX_ARGS);
        break;
    case IR_PHI:
    case IR_NOP:
        verr(v, f, b, in, "'%s' instructions only exist inside the optimizer", ir_op_name(in->op));
        break;
    case IR_LOAD: case IR_STORE:
        if (in->global < 0 || in->global >= m->nglobals) verr(v, f, b, in, "'%s' references an undefined global", ir_op_name(in->op));
        else if (m->globals[in->global].kind != IRG_VAR)
            verr(v, f, b, in, "'%s @%s' must reference a 'global' variable", ir_op_name(in->op), m->globals[in->global].name);
        break;
    case IR_CALL: {
        if (in->global < 0 || in->global >= m->nglobals) {
            verr(v, f, b, in, "call to an undefined global");
            break;
        }
        const IrGlobal *g = &m->globals[in->global];
        if (g->kind == IRG_DATA || g->kind == IRG_VAR) verr(v, f, b, in, "cannot call non-function global '@%s'", g->name);
        else if (in->nargs != g->arity)
            verr(v, f, b, in, "call to '@%s' passes %d argument%s, expected %d", g->name, in->nargs,
                 in->nargs == 1 ? "" : "s", g->arity);
        if (in->nargs > MAX_ARGS) verr(v, f, b, in, "calls with more than %d arguments are not supported yet", MAX_ARGS);
        break;
    }
    case IR_JMP: case IR_BR:
        for (int k = 0; k < (in->op == IR_BR ? 2 : 1); k++)
            if (in->target[k] < 0 || in->target[k] >= f->nblocks) verr(v, f, b, in, "branch to an unknown block");
        break;
    default: break;
    }
}

/* ---- definite assignment (forward "must" data-flow) ---- */

typedef struct { V *v; const IrFunc *f; int b; const IrInstr *in; unsigned char *defined; } DaCtx;

static void check_defined(void *ctx, int r) {
    DaCtx *c = ctx;
    if (vreg_ok(c->f, r) && !c->defined[r])
        verr(c->v, c->f, c->b, c->in, "'%%%s' may be read before it is assigned", c->f->vregs[r]);
}

static void verify_def_before_use(V *v, const IrFunc *f) {
    int nb = f->nblocks, nv = f->nvregs;
    if (nv == 0) return;
    /* in[b] = vregs definitely assigned on entry to b. Start at "everything"
     * (top) except the entry block, then iterate to the greatest fixed point. */
    unsigned char *in = xmalloc((size_t)nb * (size_t)nv);
    unsigned char *cur = xmalloc((size_t)nv);
    memset(in, 1, (size_t)nb * (size_t)nv);
    memset(in, 0, (size_t)nv);
    for (int p = 0; p < f->nparams; p++) in[p] = 1;
    /* reachability: unreachable blocks are not checked */
    unsigned char *reach = xcalloc((size_t)nb, 1);
    reach[0] = 1;
    for (bool changed = true; changed;) {
        changed = false;
        for (int b = 0; b < nb; b++) {
            if (!reach[b]) continue;
            memcpy(cur, in + (size_t)b * nv, (size_t)nv);
            const IrBlock *bl = &f->blocks[b];
            for (int k = 0; k < bl->n; k++)
                if (vreg_ok(f, bl->instrs[k].dst)) cur[bl->instrs[k].dst] = 1;
            if (bl->n == 0) continue;
            const IrInstr *t = &bl->instrs[bl->n - 1];
            int ns = t->op == IR_JMP ? 1 : t->op == IR_BR ? 2 : 0;
            for (int s = 0; s < ns; s++) {
                int succ = t->target[s];
                if (succ < 0 || succ >= nb) continue;
                unsigned char *si = in + (size_t)succ * nv;
                if (!reach[succ]) {
                    reach[succ] = 1;
                    if (succ != 0) memcpy(si, cur, (size_t)nv); /* first visit: take our facts */
                    else for (int r = 0; r < nv; r++) si[r] &= cur[r];
                    changed = true;
                    continue;
                }
                for (int r = 0; r < nv; r++)
                    if (si[r] && !cur[r]) { si[r] = 0; changed = true; }
            }
        }
    }
    for (int b = 0; b < nb; b++) {
        if (!reach[b]) continue;
        memcpy(cur, in + (size_t)b * nv, (size_t)nv);
        const IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            DaCtx c = {v, f, b, &bl->instrs[k], cur};
            for_each_use(&bl->instrs[k], check_defined, &c);
            if (vreg_ok(f, bl->instrs[k].dst)) cur[bl->instrs[k].dst] = 1;
        }
    }
    free(in);
    free(cur);
    free(reach);
}

/* ---- declared types (proved with ir_types) ---- */

static void type_error(V *v, const IrFunc *f, int b, const IrInstr *in, const char *what, IrTy got, IrTy want) {
    char g[300], w[300];
    ir_ty_name(v->m, got, g, sizeof g);
    ir_ty_name(v->m, want, w, sizeof w);
    verr(v, f, b, in, "%s may be %s, but it is declared %s (add a 'check')", what, g, w);
}

static void verify_types(V *v, const IrFunc *f) {
    const IrModule *m = v->m;
    IrTypes t;
    ir_types_compute(m, f, &t);
    IrTy *st = xmalloc((size_t)(f->nvregs ? f->nvregs : 1) * sizeof *st);
    const IrGlobal *self = &m->globals[f->global];
    char what[300];
    for (int b = 0; b < f->nblocks; b++) {
        memcpy(st, &t.in[(size_t)b * f->nvregs], (size_t)f->nvregs * sizeof *st);
        const IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            const IrInstr *in = &bl->instrs[k];
            if (in->op == IR_CALL && m->globals[in->global].kind == IRG_FUNC) {
                const IrGlobal *g = &m->globals[in->global];
                for (int i = 0; i < in->nargs && i < g->arity; i++) {
                    IrTy want = ir_param_ty(g, i);
                    if (!ir_ty_sub(st[in->args[i]], want)) {
                        snprintf(what, sizeof what, "argument %d of '@%s'", i + 1, g->name);
                        type_error(v, f, b, in, what, st[in->args[i]], want);
                    }
                }
            } else if (in->op == IR_RET && !ir_ty_sub(st[in->a], self->ty)) {
                snprintf(what, sizeof what, "the return value of '@%s'", self->name);
                type_error(v, f, b, in, what, st[in->a], self->ty);
            } else if (in->op == IR_STORE && !ir_ty_sub(st[in->a], m->globals[in->global].ty)) {
                snprintf(what, sizeof what, "the value stored to '@%s'", m->globals[in->global].name);
                type_error(v, f, b, in, what, st[in->a], m->globals[in->global].ty);
            } else if (in->op == IR_NEW) {
                const IrStruct *sd = &m->structs[in->sid];
                for (int i = 0; i < in->nargs; i++)
                    if (!ir_ty_sub(st[in->args[i]], sd->ftys[i])) {
                        snprintf(what, sizeof what, "field '%s' of a new %s", sd->fields[i], sd->name);
                        type_error(v, f, b, in, what, st[in->args[i]], sd->ftys[i]);
                    }
            } else if ((in->op == IR_GETFIELD || in->op == IR_SETFIELD) && in->sid >= 0) {
                const IrStruct *sd = &m->structs[in->sid];
                if (!ir_ty_sub(st[in->a], ty_struct_of(in->sid))) {
                    snprintf(what, sizeof what, "the object of '%s %s.%s'", ir_op_name(in->op), sd->name, sd->fields[in->field]);
                    type_error(v, f, b, in, what, st[in->a], ty_struct_of(in->sid));
                } else if (in->op == IR_SETFIELD && !ir_ty_sub(st[in->b], sd->ftys[in->field])) {
                    snprintf(what, sizeof what, "the value stored to %s.%s", sd->name, sd->fields[in->field]);
                    type_error(v, f, b, in, what, st[in->b], sd->ftys[in->field]);
                }
            }
            ir_types_step(m, in, st);
        }
    }
    free(st);
    ir_types_free(&t);
}

bool ir_verify(const IrModule *m, const char *path) {
    V v = {m, path, 0};
    for (int i = 0; i < m->nglobals; i++)
        for (int j = i + 1; j < m->nglobals; j++)
            if (strcmp(m->globals[i].name, m->globals[j].name) == 0)
                verr(&v, NULL, -1, NULL, "duplicate global '@%s'", m->globals[i].name);
    for (int i = 0; i < m->nglobals; i++) {
        const IrGlobal *g = &m->globals[i];
        if (g->kind != IRG_CEXTERN) continue;
        if (g->arity > MAX_ARGS)
            verr(&v, NULL, -1, NULL, "C function '@%s' has more than %d parameters (not supported yet)", g->name, MAX_ARGS);
        for (int k = 0; k < g->arity; k++)
            if (g->cparams[k] == CT_VOID) verr(&v, NULL, -1, NULL, "C function '@%s': parameter '%s' cannot be void", g->name, g->cnames[k]);
    }
    for (int si = 0; si < m->nstructs; si++) {
        const IrStruct *sd = &m->structs[si];
        for (int k = 0; k < sd->nfields; k++) {
            if (!ty_valid(m, sd->ftys[k])) verr(&v, NULL, -1, NULL, "struct '%s': field '%s' has an invalid type", sd->name, sd->fields[k]);
            for (int j = k + 1; j < sd->nfields; j++)
                if (strcmp(sd->fields[k], sd->fields[j]) == 0) verr(&v, NULL, -1, NULL, "struct '%s': duplicate field '%s'", sd->name, sd->fields[k]);
        }
        for (int k = 0; k < sd->nmethods; k++) {
            /* callm passes (self, args...) unchecked: self is an instance of
             * this struct (the runtime found the method on it), the other
             * arguments may be anything */
            const IrGlobal *g = sd->mglobals[k] >= 0 && sd->mglobals[k] < m->nglobals ? &m->globals[sd->mglobals[k]] : NULL;
            if (!g || g->kind != IRG_FUNC || g->arity < 1) {
                verr(&v, NULL, -1, NULL, "struct '%s': method '%s' must be a function taking self", sd->name, sd->mnames[k]);
                continue;
            }
            if (!ir_ty_sub(ty_struct_of(si), ir_param_ty(g, 0)))
                verr(&v, NULL, -1, NULL, "struct '%s': method '%s' (@%s) must accept a %s as its first parameter", sd->name,
                     sd->mnames[k], g->name, sd->name);
            for (int i = 1; i < g->arity; i++)
                if (ir_param_ty(g, i) != TY_ANY)
                    verr(&v, NULL, -1, NULL, "struct '%s': method '%s' (@%s) is called dynamically, so its parameter %d must be untyped",
                         sd->name, sd->mnames[k], g->name, i + 1);
            for (int j = k + 1; j < sd->nmethods; j++)
                if (strcmp(sd->mnames[k], sd->mnames[j]) == 0) verr(&v, NULL, -1, NULL, "struct '%s': duplicate method '%s'", sd->name, sd->mnames[k]);
        }
    }
    for (int fi = 0; fi < m->nfuncs; fi++) {
        const IrFunc *f = &m->funcs[fi];
        if (f->nblocks == 0) {
            verr(&v, f, -1, NULL, "function has no blocks");
            continue;
        }
        if (f->nparams > MAX_ARGS) verr(&v, f, -1, NULL, "functions with more than %d parameters are not supported yet", MAX_ARGS);
        for (int b = 0; b < f->nblocks; b++) {
            const IrBlock *bl = &f->blocks[b];
            for (int c = b + 1; c < f->nblocks; c++)
                if (strcmp(bl->label, f->blocks[c].label) == 0) verr(&v, f, b, NULL, "duplicate block label");
            if (!ir_block_terminated(bl)) verr(&v, f, b, NULL, "block does not end with a terminator (jmp, br or ret)");
            for (int k = 0; k < bl->n; k++) {
                if (k < bl->n - 1 && ir_op_is_terminator(bl->instrs[k].op))
                    verr(&v, f, b, &bl->instrs[k], "instruction after terminator '%s'", ir_op_name(bl->instrs[k].op));
                check_instr(&v, f, b, &bl->instrs[k]);
            }
        }
        if (v.errors == 0) verify_def_before_use(&v, f);
        if (v.errors == 0) verify_types(&v, f);
    }
    if (v.errors > 20) fprintf(stderr, "%s: ... %d more error(s)\n", path, v.errors - 20);
    return v.errors == 0;
}
