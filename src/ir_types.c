/* ir_types.c - flow-sensitive type inference for LIR.
 *
 * A forward dataflow analysis computes, for every block, the set of
 * runtime types each vreg may hold on entry (the union over predecessors).
 * Parameters start at their declared types; every other vreg starts at 0
 * ("not assigned yet"). Instruction result types follow the runtime's
 * semantics: an operation that would fail for some operand types simply
 * produces no value for them, e.g. `add` of int and any can only produce an
 * int (a str operand would be a runtime error). */
#include <stdlib.h>
#include <string.h>

#include "ir.h"

static IrTy ty_of(const IrTy *state, int v) { return v >= 0 ? state[v] : 0; }

IrTy ir_types_result(const IrModule *m, const IrInstr *in, const IrTy *st) {
    IrTy a = ty_of(st, in->a), b = ty_of(st, in->b);
    switch (in->op) {
    case IR_CONST_INT: return TY_INT;
    case IR_CONST_NIL: return TY_NIL;
    case IR_CONST_TRUE:
    case IR_CONST_FALSE: return TY_BOOL;
    case IR_CONST_DATA: return TY_STR;
    case IR_MOV: return a;
    case IR_ADD: return ((a & TY_INT) && (b & TY_INT) ? TY_INT : 0) | ((a & TY_STR) && (b & TY_STR) ? TY_STR : 0);
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
    case IR_MOD: return (a & TY_INT) && (b & TY_INT) ? TY_INT : 0;
    case IR_NEG: return (a & TY_INT) ? TY_INT : 0;
    case IR_LT:
    case IR_LE:
    case IR_GT:
    case IR_GE: return (a & TY_INT) && (b & TY_INT) ? TY_BOOL : 0;
    case IR_EQ:
    case IR_NE:
    case IR_NOT: return TY_BOOL;
    case IR_CALL: {
        const IrGlobal *g = &m->globals[in->global];
        if (g->kind == IRG_FUNC) return g->ty;
        if (g->kind == IRG_CEXTERN) return ir_ctype_ty(g->cret);
        return TY_ANY;
    }
    case IR_LOAD: return m->globals[in->global].ty;
    case IR_CHECK: return ir_ty_inter(a, in->ty);
    case IR_NEW: return ty_struct_of(in->sid);
    case IR_GETFIELD:
        if (in->sid >= 0 && in->sid < m->nstructs && in->field >= 0 && in->field < m->structs[in->sid].nfields)
            return m->structs[in->sid].ftys[in->field];
        return TY_ANY;
    case IR_CALLM: return TY_ANY;
    case IR_PHI: {
        IrTy t = 0;
        for (int i = 0; i < in->nargs; i++) t = ir_ty_union(t, ty_of(st, in->args[i]));
        return t;
    }
    default: return 0;
    }
}

void ir_types_step(const IrModule *m, const IrInstr *in, IrTy *state) {
    if (in->dst >= 0) state[in->dst] = ir_types_result(m, in, state);
}

/* After an instruction that raises a runtime error unless an operand is an
 * int, that operand is an int (if it may be one at all). Operands the
 * instruction redefines keep the result type. */
void ir_types_refine(const IrInstr *in, IrTy *state) {
    switch (in->op) {
    case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        if (in->b >= 0 && in->b != in->dst && (state[in->b] & TY_INT)) state[in->b] = TY_INT;
        /* fallthrough */
    case IR_NEG:
        if (in->a >= 0 && in->a != in->dst && (state[in->a] & TY_INT)) state[in->a] = TY_INT;
        break;
    case IR_CHECK:
        if (in->a >= 0 && in->a != in->dst && ir_ty_inter(state[in->a], in->ty)) state[in->a] = ir_ty_inter(state[in->a], in->ty);
        break;
    default: break;
    }
}

static bool refining; /* ir_types_compute_refined in progress */

void ir_types_free(IrTypes *t) {
    free(t->in);
    free(t->out);
    memset(t, 0, sizeof *t);
}

void ir_types_compute(const IrModule *m, const IrFunc *f, IrTypes *t) {
    int nb = f->nblocks, nv = f->nvregs;
    t->nblocks = nb;
    t->nvregs = nv;
    size_t cells = (size_t)(nb ? nb : 1) * (size_t)(nv ? nv : 1);
    t->in = xcalloc(cells, sizeof *t->in);
    t->out = xcalloc(cells, sizeof *t->out);
    if (nb == 0) return;
    const IrGlobal *g = &m->globals[f->global];
    for (int p = 0; p < f->nparams; p++) t->in[p] = ir_param_ty(g, p);

    IrTy *st = xmalloc((size_t)(nv ? nv : 1) * sizeof *st);
    bool *queued = xcalloc((size_t)nb, sizeof *queued);
    int *work = xmalloc((size_t)nb * sizeof *work);
    int nw = 0;
    work[nw++] = 0;
    queued[0] = true;
    bool *seen = xcalloc((size_t)nb, sizeof *seen); /* processed at least once */
    while (nw > 0) {
        int bi = work[--nw];
        queued[bi] = false;
        const IrBlock *blk = &f->blocks[bi];
        memcpy(st, &t->in[(size_t)bi * nv], (size_t)nv * sizeof *st);
        /* phis read their arguments in the predecessors' out-states */
        for (int k = 0; k < blk->n && blk->instrs[k].op == IR_PHI; k++) {
            const IrInstr *in = &blk->instrs[k];
            IrTy ty = 0;
            for (int i = 0; i < in->nargs; i++)
                if (in->args[i] >= 0) ty = ir_ty_union(ty, t->out[(size_t)in->phi_blocks[i] * nv + in->args[i]]);
            st[in->dst] = ty;
        }
        for (int k = 0; k < blk->n; k++)
            if (blk->instrs[k].op != IR_PHI) {
                ir_types_step(m, &blk->instrs[k], st);
                if (refining) ir_types_refine(&blk->instrs[k], st);
            }
        bool changed = !seen[bi] || memcmp(st, &t->out[(size_t)bi * nv], (size_t)nv * sizeof *st) != 0;
        seen[bi] = true;
        if (!changed) continue;
        memcpy(&t->out[(size_t)bi * nv], st, (size_t)nv * sizeof *st);
        if (blk->n == 0) continue;
        const IrInstr *term = &blk->instrs[blk->n - 1];
        int ns = term->op == IR_JMP ? 1 : term->op == IR_BR ? 2 : 0;
        for (int s = 0; s < ns; s++) {
            int succ = term->target[s];
            if (succ < 0 || succ >= nb) continue;
            IrTy *sin = &t->in[(size_t)succ * nv];
            bool grew = !seen[succ];
            for (int v = 0; v < nv; v++) {
                IrTy u = ir_ty_union(sin[v], st[v]);
                if (u != sin[v]) {
                    sin[v] = u;
                    grew = true;
                }
            }
            /* a successor that has phis must be revisited whenever an out-state changes */
            if ((grew || (f->blocks[succ].n && f->blocks[succ].instrs[0].op == IR_PHI)) && !queued[succ]) {
                queued[succ] = true;
                work[nw++] = succ;
            }
        }
    }
    free(st);
    free(queued);
    free(work);
    free(seen);
}

void ir_types_compute_refined(const IrModule *m, const IrFunc *f, IrTypes *t) {
    refining = true;
    ir_types_compute(m, f, t);
    refining = false;
}
