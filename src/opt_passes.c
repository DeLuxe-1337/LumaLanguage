/* opt_passes.c - SSA optimization passes: SCCP, GVN/CSE, LICM, DCE.
 *
 * All passes run on SSA form (every vreg has exactly one definition, which
 * dominates its uses) and keep it. They use the static types from
 * ir_types (a value never changes type in SSA) to decide what can trap. */
#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"
#include "value.h"

static bool subty(IrTy t, IrTy of) { return (t & ~of) == 0; }

/* ======================================================================== */
/* SCCP: sparse conditional constant propagation (Wegman & Zadeck)          */

enum { L_TOP, L_CONST, L_BOT };

typedef struct {
    unsigned char st;  /* L_TOP / L_CONST / L_BOT */
    unsigned char op;  /* L_CONST: IR_CONST_INT / NIL / TRUE / FALSE / DATA */
    int64_t v;         /* IR_CONST_INT: value; IR_CONST_DATA: global index */
} Lat;

static const Lat TOP = {L_TOP, 0, 0}, BOT = {L_BOT, 0, 0};

static Lat lconst(IrOp op, int64_t v) { return (Lat){L_CONST, (unsigned char)op, v}; }
static Lat lbool(bool b) { return lconst(b ? IR_CONST_TRUE : IR_CONST_FALSE, 0); }
static bool same_const(Lat a, Lat b) { return a.op == b.op && a.v == b.v; }

static Lat meet(Lat a, Lat b) {
    if (a.st == L_TOP) return b;
    if (b.st == L_TOP) return a;
    if (a.st == L_BOT || b.st == L_BOT) return BOT;
    return same_const(a, b) ? a : BOT;
}

static bool lat_eq(Lat a, Lat b) { return a.st == b.st && (a.st != L_CONST || same_const(a, b)); }

static bool const_truthy(Lat c) { return c.op != IR_CONST_NIL && c.op != IR_CONST_FALSE; }

static IrTy const_ty(Lat c) {
    switch (c.op) {
    case IR_CONST_INT: return TY_INT;
    case IR_CONST_DATA: return TY_STR;
    case IR_CONST_NIL: return TY_NIL;
    default: return TY_BOOL;
    }
}

static bool fix(int64_t v) { return v >= LUMA_FIXNUM_MIN && v <= LUMA_FIXNUM_MAX; }

/* Folds an integer operation; returns BOT when it would raise an error. */
static Lat fold_int(IrOp op, int64_t x, int64_t y) {
    int64_t r;
    switch (op) {
    case IR_ADD: return __builtin_add_overflow(x, y, &r) || !fix(r) ? BOT : lconst(IR_CONST_INT, r);
    case IR_SUB: return __builtin_sub_overflow(x, y, &r) || !fix(r) ? BOT : lconst(IR_CONST_INT, r);
    case IR_MUL: return __builtin_mul_overflow(x, y, &r) || !fix(r) ? BOT : lconst(IR_CONST_INT, r);
    case IR_DIV:
        if (y == 0) return BOT;
        r = x / y;
        if ((x % y != 0) && ((x < 0) != (y < 0))) r--;
        return fix(r) ? lconst(IR_CONST_INT, r) : BOT;
    case IR_MOD:
        if (y == 0) return BOT;
        r = x % y;
        if (r != 0 && ((r < 0) != (y < 0))) r += y;
        return lconst(IR_CONST_INT, r);
    case IR_LT: return lbool(x < y);
    case IR_LE: return lbool(x <= y);
    case IR_GT: return lbool(x > y);
    case IR_GE: return lbool(x >= y);
    default: return BOT;
    }
}

static bool const_equal(const IrModule *m, Lat a, Lat b) {
    if (a.op != b.op) return false;
    if (a.op == IR_CONST_DATA) {
        const IrGlobal *x = &m->globals[a.v], *y = &m->globals[b.v];
        return x->data_len == y->data_len && memcmp(x->data, y->data, x->data_len) == 0;
    }
    return a.v == b.v;
}

/* Truthiness decided by the value or, failing that, by its type:
 * returns 1 (truthy), 0 (falsy) or -1 (unknown / not yet known). */
static int known_truth(Lat c, IrTy t) {
    if (c.st == L_CONST) return const_truthy(c);
    if (t && subty(t, TY_INT | TY_STR)) return 1;
    if (t && subty(t, TY_NIL)) return 0;
    return -1;
}

static Lat sccp_eval(const IrModule *m, const IrInstr *in, const Lat *L, const IrTy *vty) {
    Lat a = in->a >= 0 ? L[in->a] : BOT, b = in->b >= 0 ? L[in->b] : BOT;
    switch (in->op) {
    case IR_CONST_INT: return lconst(IR_CONST_INT, in->imm);
    case IR_CONST_NIL:
    case IR_CONST_TRUE:
    case IR_CONST_FALSE: return lconst(in->op, 0);
    case IR_CONST_DATA: return lconst(IR_CONST_DATA, in->global);
    case IR_MOV: return a;
    case IR_CHECK:
        if (a.st == L_CONST) return (const_ty(a) & in->ty) ? a : BOT;
        return a.st == L_TOP ? TOP : BOT;
    case IR_NOT: {
        int t = known_truth(a, vty[in->a]);
        if (t >= 0) return lbool(!t);
        return a.st == L_TOP ? TOP : BOT;
    }
    case IR_NEG:
        if (a.st == L_TOP) return TOP;
        if (a.st == L_CONST && a.op == IR_CONST_INT && a.v != LUMA_FIXNUM_MIN) return lconst(IR_CONST_INT, -a.v);
        return BOT;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        if (a.st == L_TOP || b.st == L_TOP) return TOP;
        if (a.st == L_CONST && b.st == L_CONST && a.op == IR_CONST_INT && b.op == IR_CONST_INT)
            return fold_int(in->op, a.v, b.v);
        return BOT;
    case IR_EQ:
    case IR_NE: {
        if (a.st == L_CONST && b.st == L_CONST) return lbool(const_equal(m, a, b) == (in->op == IR_EQ));
        IrTy ta = vty[in->a], tb = vty[in->b];
        if (ta && tb && !(ta & tb)) return lbool(in->op == IR_NE); /* different types are never equal */
        if (a.st == L_TOP || b.st == L_TOP) return TOP;
        return BOT;
    }
    default: return BOT; /* calls, loads */
    }
}

static int succ_index(const Cfg *c, int b, int s) {
    for (int k = 0; k < c->nsucc[b]; k++)
        if (c->succs[b][k] == s) return k;
    return -1;
}

bool opt_sccp(IrModule *m, IrFunc *f) {
    int nb = f->nblocks, nv = f->nvregs;
    if (nb == 0) return false;
    Cfg c;
    cfg_build(f, &c);
    IrTy *vty = opt_value_types(m, f);
    Lat *L = xmalloc((size_t)(nv ? nv : 1) * sizeof *L);
    for (int v = 0; v < nv; v++) L[v] = v < f->nparams ? BOT : TOP;
    bool *bexec = xcalloc((size_t)nb, sizeof *bexec);
    bool (*eexec)[2] = xcalloc((size_t)nb, sizeof *eexec);
    bexec[0] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < c.nrpo; i++) {
            int b = c.rpo[i];
            if (!bexec[b]) continue;
            IrBlock *bl = &f->blocks[b];
            for (int k = 0; k < bl->n; k++) {
                IrInstr *in = &bl->instrs[k];
                Lat nl;
                if (in->op == IR_PHI) {
                    nl = TOP;
                    for (int j = 0; j < in->nargs; j++) {
                        int p = in->phi_blocks[j], si = succ_index(&c, p, b);
                        if (si >= 0 && eexec[p][si]) nl = meet(nl, L[in->args[j]]);
                    }
                } else if (in->op == IR_JMP || in->op == IR_BR) {
                    int t = in->op == IR_JMP ? 1 : known_truth(L[in->a], vty[in->a]);
                    if (in->op == IR_BR && L[in->a].st == L_TOP && t < 0) continue; /* not known yet */
                    for (int s = 0; s < 2; s++) {
                        if (in->target[s] < 0) continue;
                        bool take = in->op == IR_JMP ? s == 0 : (t < 0 || (s == 0) == (t == 1));
                        if (!take) continue;
                        int si = succ_index(&c, b, in->target[s]);
                        if (si >= 0 && !eexec[b][si]) {
                            eexec[b][si] = true;
                            bexec[in->target[s]] = true;
                            changed = true;
                        }
                    }
                    continue;
                } else if (in->dst >= 0) {
                    nl = sccp_eval(m, in, L, vty);
                } else {
                    continue;
                }
                nl = meet(L[in->dst], nl);
                if (L[in->dst].st == L_CONST && nl.st == L_CONST && !same_const(L[in->dst], nl)) nl = BOT;
                if (!lat_eq(nl, L[in->dst])) {
                    L[in->dst] = nl;
                    changed = true;
                }
            }
        }
    }

    /* rewrite */
    bool any = false;
    for (int b = 0; b < nb; b++) {
        if (!bexec[b]) continue;
        IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            if (in->dst >= 0 && L[in->dst].st == L_CONST) {
                Lat x = L[in->dst];
                bool already = in->op == (IrOp)x.op &&
                               (x.op == IR_CONST_INT ? in->imm == x.v : x.op == IR_CONST_DATA ? in->global == x.v : true);
                if (!already) {
                    ir_instr_free(in);
                    in->op = (IrOp)x.op;
                    in->a = in->b = IR_NONE;
                    in->nargs = 0;
                    in->imm = x.op == IR_CONST_INT ? x.v : 0;
                    in->global = x.op == IR_CONST_DATA ? (int)x.v : IR_NONE;
                    any = true;
                }
            } else if (in->op == IR_BR) {
                int t = known_truth(L[in->a], vty[in->a]);
                if (t < 0) continue;
                int keep = in->target[t ? 0 : 1], drop = in->target[t ? 1 : 0];
                in->op = IR_JMP;
                in->a = IR_NONE;
                in->target[0] = keep;
                in->target[1] = IR_NONE;
                if (drop != keep) { /* this block no longer flows into `drop` */
                    IrBlock *db = &f->blocks[drop];
                    for (int q = 0; q < db->n && db->instrs[q].op == IR_PHI; q++) {
                        IrInstr *phi = &db->instrs[q];
                        int j = 0;
                        for (int i = 0; i < phi->nargs; i++)
                            if (phi->phi_blocks[i] != b) {
                                phi->args[j] = phi->args[i];
                                phi->phi_blocks[j] = phi->phi_blocks[i];
                                j++;
                            }
                        phi->nargs = j;
                    }
                }
                any = true;
            }
        }
    }
    /* phis that became constants are no longer phis: keep the remaining phis
     * at the top of their blocks (every pass relies on that layout) */
    for (int b = 0; b < nb; b++) {
        IrBlock *bl = &f->blocks[b];
        int w = 0;
        for (int k = 0; k < bl->n; k++)
            if (bl->instrs[k].op == IR_PHI) {
                IrInstr t = bl->instrs[k];
                memmove(&bl->instrs[w + 1], &bl->instrs[w], (size_t)(k - w) * sizeof *bl->instrs);
                bl->instrs[w++] = t;
            }
    }
    int before = f->nblocks;
    opt_remove_unreachable(f);
    if (f->nblocks != before) any = true;
    free(L);
    free(vty);
    free(bexec);
    free(eexec);
    cfg_free(&c);
    return any;
}

/* ======================================================================== */
/* GVN: dominator-scoped value numbering                                    */
/* (common subexpressions, copy propagation, trivial phis, redundant checks, */
/*  block-local load forwarding)                                            */

typedef struct {
    int op, a, b, global;
    int64_t imm;
    unsigned ty;
    int val;
    bool used;
} Ent;

typedef struct {
    Ent *t;
    size_t mask;
    size_t *log;
    size_t nlog, caplog;
} Tab;

static size_t ent_hash(const Ent *e) {
    uint64_t h = (uint64_t)e->op * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uint32_t)e->a + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
    h ^= (uint64_t)(uint32_t)e->b + 0x85EBCA77C2B2AE63ull + (h << 6) + (h >> 2);
    h ^= (uint64_t)e->imm + (h << 6) + (h >> 2);
    h ^= (uint64_t)(uint32_t)e->global * 31 + e->ty;
    return (size_t)(h ^ (h >> 29));
}

static bool ent_eq(const Ent *x, const Ent *y) {
    return x->op == y->op && x->a == y->a && x->b == y->b && x->imm == y->imm && x->global == y->global && x->ty == y->ty;
}

static int tab_find_or_add(Tab *t, const Ent *key, int val) {
    size_t i = ent_hash(key) & t->mask;
    while (t->t[i].used) {
        if (ent_eq(&t->t[i], key)) return t->t[i].val;
        i = (i + 1) & t->mask;
    }
    t->t[i] = *key;
    t->t[i].val = val;
    t->t[i].used = true;
    if (t->nlog == t->caplog) {
        t->caplog = t->caplog ? t->caplog * 2 : 64;
        t->log = xrealloc(t->log, t->caplog * sizeof *t->log);
    }
    t->log[t->nlog++] = i;
    return -1;
}

static int find(int *repl, int v) {
    int r = v;
    while (repl[r] != r) r = repl[r];
    while (repl[v] != r) {
        int n = repl[v];
        repl[v] = r;
        v = n;
    }
    return r;
}

/* add is commutative only on integers: on strings it is concatenation */
static bool commutative(const IrInstr *in, const IrTy *vty) {
    if (in->op == IR_ADD) return subty(vty[in->a], TY_INT) && subty(vty[in->b], TY_INT);
    return in->op == IR_MUL || in->op == IR_EQ || in->op == IR_NE;
}

bool opt_gvn(IrModule *m, IrFunc *f) {
    int nb = f->nblocks, nv = f->nvregs;
    if (nb == 0) return false;
    Cfg c;
    cfg_build(f, &c);
    IrTy *vty = opt_value_types(m, f);
    int *repl = xmalloc((size_t)(nv ? nv : 1) * sizeof *repl);
    for (int v = 0; v < nv; v++) repl[v] = v;
    size_t ninstr = 0;
    for (int b = 0; b < nb; b++) ninstr += (size_t)f->blocks[b].n;
    size_t cap = 64;
    while (cap < ninstr * 2 + 16) cap *= 2;
    Tab tab = {xcalloc(cap, sizeof(Ent)), cap - 1, NULL, 0, 0};
    int *known = xmalloc((size_t)(m->nglobals ? m->nglobals : 1) * sizeof *known);
    int **ptrs = NULL, cap_ptrs = 0;
    bool any = false;

    int *stack = xmalloc((size_t)nb * sizeof *stack), *state = xmalloc((size_t)nb * sizeof *state);
    size_t *mark = xmalloc((size_t)nb * sizeof *mark);
    int sp = 0;
    stack[sp] = 0;
    state[sp] = 0;
    sp++;
    while (sp > 0) {
        int top = sp - 1, b = stack[top];
        if (state[top] == 1) {
            while (tab.nlog > mark[top]) tab.t[tab.log[--tab.nlog]].used = false;
            sp--;
            continue;
        }
        state[top] = 1;
        mark[top] = tab.nlog;
        for (int g = 0; g < m->nglobals; g++) known[g] = -1;
        IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            if (in->op == IR_NOP) continue;
            if (in->nargs + 2 > cap_ptrs) {
                cap_ptrs = in->nargs + 2;
                ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
            }
            int nu = ir_instr_uses(in, ptrs);
            for (int i = 0; i < nu; i++)
                if (*ptrs[i] >= 0) *ptrs[i] = find(repl, *ptrs[i]);
            Ent key;
            memset(&key, 0, sizeof key);
            key.op = in->op;
            key.a = in->a;
            key.b = in->b;
            key.global = in->global;
            switch (in->op) {
            case IR_PHI: {
                int same = -1;
                bool trivial = true;
                for (int i = 0; i < in->nargs && trivial; i++) {
                    int x = in->args[i]; /* (argument from a later block may not be final yet) */
                    x = find(repl, x);
                    if (x == in->dst) continue;
                    if (same < 0) same = x;
                    else if (x != same) trivial = false;
                }
                if (trivial && same >= 0) {
                    repl[in->dst] = same;
                    ir_instr_free(in);
                    in->op = IR_NOP;
                    any = true;
                }
                continue;
            }
            case IR_MOV:
                repl[in->dst] = in->a;
                in->op = IR_NOP;
                any = true;
                continue;
            case IR_CHECK:
                if (subty(vty[in->a], in->ty)) { /* proven: the check can never fail */
                    repl[in->dst] = in->a;
                    in->op = IR_NOP;
                    any = true;
                    continue;
                }
                key.ty = in->ty;
                key.global = 0; /* any earlier check of the same value and type subsumes this one */
                break;
            case IR_CONST_INT: key.imm = in->imm; break;
            case IR_CONST_NIL: case IR_CONST_TRUE: case IR_CONST_FALSE: case IR_CONST_DATA:
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD: case IR_NEG: case IR_NOT:
            case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
                if (commutative(in, vty) && key.a > key.b) {
                    int t = key.a;
                    key.a = key.b;
                    key.b = t;
                }
                break;
            case IR_LOAD:
                if (known[in->global] >= 0) {
                    repl[in->dst] = known[in->global];
                    in->op = IR_NOP;
                    any = true;
                } else {
                    known[in->global] = in->dst;
                }
                continue;
            case IR_STORE:
                known[in->global] = in->a;
                continue;
            case IR_CALL:
                if (m->globals[in->global].kind == IRG_FUNC) /* Luma functions may store to globals */
                    for (int g = 0; g < m->nglobals; g++) known[g] = -1;
                continue;
            default:
                continue;
            }
            int prev = tab_find_or_add(&tab, &key, in->dst);
            if (prev >= 0) {
                repl[in->dst] = prev;
                ir_instr_free(in);
                in->op = IR_NOP;
                any = true;
            }
        }
        for (int i = c.ndchild[b] - 1; i >= 0; i--) {
            stack[sp] = c.dchild[b][i];
            state[sp] = 0;
            sp++;
        }
    }
    /* final rewrite of every use (phi arguments from back edges included) */
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (in->nargs + 2 > cap_ptrs) {
                cap_ptrs = in->nargs + 2;
                ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
            }
            int nu = ir_instr_uses(in, ptrs);
            for (int i = 0; i < nu; i++)
                if (*ptrs[i] >= 0) *ptrs[i] = find(repl, *ptrs[i]);
        }
    ir_func_compact(f);
    free(ptrs);
    free(stack);
    free(state);
    free(mark);
    free(tab.t);
    free(tab.log);
    free(known);
    free(repl);
    free(vty);
    cfg_free(&c);
    return any;
}

/* ======================================================================== */
/* DCE: removes instructions whose results are unused and that have no      */
/* observable effect (including possible runtime errors).                   */

bool opt_dce(IrModule *m, IrFunc *f) {
    int nb = f->nblocks, nv = f->nvregs;
    IrTy *vty = opt_value_types(m, f);
    int *defb = xmalloc((size_t)(nv ? nv : 1) * sizeof *defb), *defk = xmalloc((size_t)(nv ? nv : 1) * sizeof *defk);
    for (int v = 0; v < nv; v++) defb[v] = -1;
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++)
            if (f->blocks[b].instrs[k].dst >= 0) {
                defb[f->blocks[b].instrs[k].dst] = b;
                defk[f->blocks[b].instrs[k].dst] = k;
            }
    bool *live = xcalloc((size_t)(nv ? nv : 1), sizeof *live);
    int *work = xmalloc((size_t)(nv ? nv : 1) * sizeof *work), nw = 0;
    int **ptrs = NULL, cap_ptrs = 0;
    /* roots: uses of every effectful instruction */
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (!opt_has_effect(m, in, vty)) continue;
            if (in->nargs + 2 > cap_ptrs) {
                cap_ptrs = in->nargs + 2;
                ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
            }
            int nu = ir_instr_uses(in, ptrs);
            for (int i = 0; i < nu; i++)
                if (*ptrs[i] >= 0 && !live[*ptrs[i]]) {
                    live[*ptrs[i]] = true;
                    work[nw++] = *ptrs[i];
                }
            if (in->dst >= 0 && !live[in->dst]) { /* an effectful definition stays even if unused */
                live[in->dst] = true;
                work[nw++] = in->dst;
            }
        }
    while (nw > 0) {
        int v = work[--nw];
        if (defb[v] < 0) continue; /* parameter */
        IrInstr *in = &f->blocks[defb[v]].instrs[defk[v]];
        if (in->nargs + 2 > cap_ptrs) {
            cap_ptrs = in->nargs + 2;
            ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
        }
        int nu = ir_instr_uses(in, ptrs);
        for (int i = 0; i < nu; i++)
            if (*ptrs[i] >= 0 && !live[*ptrs[i]]) {
                live[*ptrs[i]] = true;
                work[nw++] = *ptrs[i];
            }
    }
    bool any = false;
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (in->dst >= 0 && !live[in->dst] && !opt_has_effect(m, in, vty)) {
                ir_instr_free(in);
                in->op = IR_NOP;
                any = true;
            }
        }
    ir_func_compact(f);
    free(ptrs);
    free(defb);
    free(defk);
    free(live);
    free(work);
    free(vty);
    return any;
}

/* ======================================================================== */
/* LICM: loop-invariant code motion                                         */
/*                                                                          */
/* An instruction whose operands are all defined outside the loop moves to  */
/* the loop's preheader when                                                */
/*   - it has no effect and cannot fail (constants, copies, ==, !, ...), or */
/*   - it may fail (arithmetic, unproven checks, loads) but sits in the     */
/*     loop header before anything with an effect: the header always runs  */
/*     right after the preheader, so the error (if any) happens at the same */
/*     point, with no observable effect in between.                        */
/* Loads additionally need the loop to contain no store to that global and  */
/* no call to a Luma function.                                              */

/* Gives `h` a preheader: a block outside the loop whose only successor is
 * h and through which every entry into the loop passes. Returns its index,
 * or -1 after inserting a new block (the caller must re-analyse). */
static int ensure_preheader(IrFunc *f, const Cfg *c, const Loop *lp) {
    int h = lp->header, nout = 0, out = -1;
    for (int k = 0; k < c->npred[h]; k++)
        if (!bit_get(lp->body, c->preds[h][k])) {
            nout++;
            out = c->preds[h][k];
        }
    if (nout == 1 && c->nsucc[out] == 1) return out;
    if (nout == 0) return -2; /* the entry is the header: no preheader possible */
    int *outs = xmalloc((size_t)nout * sizeof *outs), no = 0;
    for (int k = 0; k < c->npred[h]; k++)
        if (!bit_get(lp->body, c->preds[h][k])) outs[no++] = c->preds[h][k];
    int p = opt_insert_block(f, h, "pre"); /* h and every later block shift by one */
    int hh = h + 1;
    for (int i = 0; i < no; i++) {
        int o = outs[i] >= h ? outs[i] + 1 : outs[i];
        IrInstr *t = &f->blocks[o].instrs[f->blocks[o].n - 1];
        for (int s = 0; s < 2; s++)
            if (t->target[s] == hh) t->target[s] = p;
        outs[i] = o;
    }
    IrInstr j;
    memset(&j, 0, sizeof j);
    j.op = IR_JMP;
    j.dst = j.a = j.b = j.global = IR_NONE;
    j.target[0] = hh;
    j.target[1] = IR_NONE;
    /* phis of the header: arguments from outside predecessors now arrive via p */
    IrBlock *hb = &f->blocks[hh];
    for (int k = 0; k < hb->n && hb->instrs[k].op == IR_PHI; k++) {
        IrInstr *phi = &f->blocks[hh].instrs[k];
        int nin = 0, first = -1;
        for (int i = 0; i < phi->nargs; i++) {
            bool outside = false;
            for (int q = 0; q < no; q++) outside |= phi->phi_blocks[i] == outs[q];
            if (outside) {
                nin++;
                if (first < 0) first = i;
            }
        }
        if (nin == 0) continue;
        int incoming;
        if (nin == 1) {
            incoming = phi->args[first];
        } else { /* merge the outside arguments in a phi of the preheader */
            IrInstr np;
            memset(&np, 0, sizeof np);
            np.op = IR_PHI;
            np.a = np.b = np.global = IR_NONE;
            np.target[0] = np.target[1] = IR_NONE;
            np.args = xmalloc((size_t)nin * sizeof *np.args);
            np.phi_blocks = xmalloc((size_t)nin * sizeof *np.phi_blocks);
            for (int i = 0; i < phi->nargs; i++) {
                bool outside = false;
                for (int q = 0; q < no; q++) outside |= phi->phi_blocks[i] == outs[q];
                if (outside) {
                    np.args[np.nargs] = phi->args[i];
                    np.phi_blocks[np.nargs++] = phi->phi_blocks[i];
                }
            }
            np.dst = opt_fresh_vreg(f, f->vregs[f->blocks[hh].instrs[k].dst]);
            phi = &f->blocks[hh].instrs[k];
            np.line = phi->line;
            opt_append(f, p, np);
            phi = &f->blocks[hh].instrs[k];
            incoming = np.dst;
        }
        int w = 0;
        for (int i = 0; i < phi->nargs; i++) {
            bool outside = false;
            for (int q = 0; q < no; q++) outside |= phi->phi_blocks[i] == outs[q];
            if (outside) continue;
            phi->args[w] = phi->args[i];
            phi->phi_blocks[w++] = phi->phi_blocks[i];
        }
        phi->args[w] = incoming;
        phi->phi_blocks[w++] = p;
        phi->nargs = w;
    }
    opt_append(f, p, j);
    free(outs);
    return -1;
}

bool opt_licm(IrModule *m, IrFunc *f) {
    bool any = false;
    char **done = NULL; /* headers already processed, by label (block indices shift) */
    int ndone = 0;
    for (int guard = 0; guard < 1000; guard++) {
        Cfg c;
        cfg_build(f, &c);
        Loop *loops;
        int nl;
        loops_find(&c, &loops, &nl);
        int li = -1;
        for (int i = 0; i < nl && li < 0; i++) {
            bool seen = false;
            for (int d = 0; d < ndone; d++) seen |= strcmp(done[d], f->blocks[loops[i].header].label) == 0;
            if (!seen) li = i;
        }
        if (li < 0) {
            loops_free(loops, nl);
            cfg_free(&c);
            break;
        }
        Loop *lp = &loops[li];
        int p = ensure_preheader(f, &c, lp);
        if (p == -1) { /* a block was inserted: start over with fresh analyses */
            loops_free(loops, nl);
            cfg_free(&c);
            any = true;
            continue;
        }
        done = xrealloc(done, (size_t)(ndone + 1) * sizeof *done);
        done[ndone++] = xstrdup(f->blocks[lp->header].label);
        if (p < 0) {
            loops_free(loops, nl);
            cfg_free(&c);
            continue;
        }

        int nv = f->nvregs, nb = f->nblocks;
        IrTy *vty = opt_value_types(m, f);
        int *defb = xmalloc((size_t)(nv ? nv : 1) * sizeof *defb);
        for (int v = 0; v < nv; v++) defb[v] = v < f->nparams ? 0 : -1;
        for (int b = 0; b < nb; b++)
            for (int k = 0; k < f->blocks[b].n; k++)
                if (f->blocks[b].instrs[k].dst >= 0) defb[f->blocks[b].instrs[k].dst] = b;
        bool *stored = xcalloc((size_t)(m->nglobals ? m->nglobals : 1), sizeof *stored);
        bool calls = false;
        for (int b = 0; b < nb; b++) {
            if (!bit_get(lp->body, b)) continue;
            for (int k = 0; k < f->blocks[b].n; k++) {
                const IrInstr *in = &f->blocks[b].instrs[k];
                if (in->op == IR_STORE) stored[in->global] = true;
                if (in->op == IR_CALL && m->globals[in->global].kind == IRG_FUNC) calls = true;
            }
        }
        int *ptrs_buf[16];
        int **ptrs = ptrs_buf;
        int **heap = NULL;
        for (int i = 0; i < c.nrpo; i++) {
            int b = c.rpo[i];
            if (!bit_get(lp->body, b)) continue;
            bool prefix = b == lp->header;
            IrBlock *bl = &f->blocks[b];
            for (int k = 0; k < bl->n; k++) {
                IrInstr *in = &f->blocks[b].instrs[k];
                if (in->op == IR_PHI || in->op == IR_NOP || ir_op_is_terminator(in->op)) continue;
                bool effect = opt_has_effect(m, in, vty);
                bool can = in->op != IR_CALL && in->op != IR_STORE && in->dst >= 0;
                if (can) {
                    if (in->nargs + 2 > 16) {
                        heap = xrealloc(heap, (size_t)(in->nargs + 2) * sizeof *heap);
                        ptrs = heap;
                    } else {
                        ptrs = ptrs_buf;
                    }
                    int nu = ir_instr_uses(in, ptrs);
                    for (int u = 0; u < nu && can; u++) {
                        int d = defb[*ptrs[u]];
                        can = d >= 0 && !bit_get(lp->body, d);
                    }
                }
                if (can && in->op == IR_LOAD) can = prefix && !stored[in->global] && !calls;
                else if (can && effect) can = prefix;
                if (can) {
                    IrInstr moved = *in;
                    in->op = IR_NOP;
                    in->args = NULL;
                    in->phi_blocks = NULL;
                    in->nargs = 0;
                    opt_insert(f, p, f->blocks[p].n - 1, moved);
                    defb[moved.dst] = p;
                    any = true;
                    continue;
                }
                if (effect) prefix = false;
            }
        }
        free(heap);
        ir_func_compact(f);
        free(vty);
        free(defb);
        free(stored);
        loops_free(loops, nl);
        cfg_free(&c);
    }
    for (int d = 0; d < ndone; d++) free(done[d]);
    free(done);
    return any;
}
