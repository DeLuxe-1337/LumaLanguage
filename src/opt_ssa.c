/* opt_ssa.c - conversion into and out of SSA form.
 *
 * Into SSA (Cytron et al., pruned by liveness): phis are placed on the
 * iterated dominance frontier of each variable's definitions, but only where
 * the variable is live; then a dominator-tree walk renames every definition
 * to a fresh vreg (the first definition keeps the original vreg, so names
 * stay readable) and every use to the reaching definition.
 *
 * Out of SSA: each phi `d = phi [a1, P1], [a2, P2]` gets a fresh temporary
 * t; every predecessor Pk ends with `t = mov ak` (before its terminator) and
 * the phi becomes `d = mov t`. Because every t is fresh, the copies never
 * interfere with each other, which avoids the classic lost-copy and swap
 * problems without splitting critical edges. The register allocator's move
 * hints remove most of these copies again. */
#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"

typedef struct {
    int *v;
    int n, cap;
} Stack;

static void spush(Stack *s, int v) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->v = xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = v;
}

bool opt_to_ssa(IrFunc *f) {
    int nb = f->nblocks, nv0 = f->nvregs;
    if (nb == 0) return true;
    Cfg c;
    cfg_build(f, &c);
    Liveness lv;
    liveness_compute(f, &c, &lv);
    int **df, *ndf;
    cfg_frontiers(&c, &df, &ndf);
    int nw = BITS_WORDS(nv0 ? nv0 : 1);
    Word *hasphi = xcalloc((size_t)nb * nw, sizeof(Word));
    Word *isdef = xcalloc((size_t)nb * nw, sizeof(Word));
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++)
            if (f->blocks[b].instrs[k].dst >= 0) bit_set(isdef + (size_t)b * nw, f->blocks[b].instrs[k].dst);
    for (int p = 0; p < f->nparams; p++) bit_set(isdef, p);

    /* ---- phi placement ---- */
    int *work = xmalloc((size_t)nb * sizeof *work);
    bool *inwork = xcalloc((size_t)nb, sizeof *inwork);
    for (int v = 0; v < nv0; v++) {
        int nwk = 0;
        memset(inwork, 0, (size_t)nb * sizeof *inwork);
        for (int b = 0; b < nb; b++)
            if (bit_get(isdef + (size_t)b * nw, v)) {
                work[nwk++] = b;
                inwork[b] = true;
            }
        while (nwk > 0) {
            int d = work[--nwk];
            for (int i = 0; i < ndf[d]; i++) {
                int y = df[d][i];
                if (bit_get(hasphi + (size_t)y * nw, v) || !bit_get(live_in(&lv, y), v)) continue;
                bit_set(hasphi + (size_t)y * nw, v);
                IrInstr phi;
                memset(&phi, 0, sizeof phi);
                phi.op = IR_PHI;
                phi.dst = v;
                phi.a = phi.b = IR_NONE;
                phi.global = IR_NONE;
                phi.target[0] = phi.target[1] = IR_NONE;
                phi.imm = v; /* the original variable, while renaming */
                phi.nargs = c.npred[y];
                phi.args = xmalloc((size_t)(phi.nargs ? phi.nargs : 1) * sizeof *phi.args);
                phi.phi_blocks = xmalloc((size_t)(phi.nargs ? phi.nargs : 1) * sizeof *phi.phi_blocks);
                for (int k = 0; k < phi.nargs; k++) {
                    phi.args[k] = IR_NONE;
                    phi.phi_blocks[k] = c.preds[y][k];
                }
                opt_insert(f, y, 0, phi);
                if (!inwork[y]) {
                    inwork[y] = true;
                    work[nwk++] = y;
                }
            }
        }
    }
    free(work);
    free(inwork);

    /* ---- renaming ---- */
    Stack *stacks = xcalloc((size_t)(nv0 ? nv0 : 1), sizeof *stacks);
    bool *kept = xcalloc((size_t)(nv0 ? nv0 : 1), sizeof *kept); /* original id already used by a def */
    for (int p = 0; p < f->nparams; p++) {
        spush(&stacks[p], p);
        kept[p] = true;
    }
    Stack log = {0}; /* originals pushed, in order, to undo on exit */
    int *frame_block = xmalloc((size_t)nb * 2 * sizeof *frame_block);
    int *frame_mark = xmalloc((size_t)nb * 2 * sizeof *frame_mark);
    int *frame_state = xmalloc((size_t)nb * 2 * sizeof *frame_state);
    int sp = 0;
    bool ok = true;
    frame_block[sp] = 0;
    frame_state[sp] = 0;
    sp++;
    int **ptrs = NULL;
    int cap_ptrs = 0;
    while (sp > 0 && ok) {
        int top = sp - 1, b = frame_block[top];
        if (frame_state[top] == 1) { /* exit: undo this block's definitions */
            while (log.n > frame_mark[top]) stacks[log.v[--log.n]].n--;
            sp--;
            continue;
        }
        frame_state[top] = 1;
        frame_mark[top] = log.n;
        IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            if (in->op != IR_PHI) {
                if (in->nargs + 2 > cap_ptrs) {
                    cap_ptrs = in->nargs + 2;
                    ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
                }
                int nu = ir_instr_uses(in, ptrs);
                for (int i = 0; i < nu; i++) {
                    int o = *ptrs[i];
                    if (o < 0 || o >= nv0 || stacks[o].n == 0) { ok = false; break; }
                    *ptrs[i] = stacks[o].v[stacks[o].n - 1];
                }
            }
            if (in->dst >= 0) {
                int o = in->op == IR_PHI ? (int)in->imm : in->dst;
                int nv = kept[o] ? opt_fresh_vreg(f, f->vregs[o]) : o;
                kept[o] = true;
                bl = &f->blocks[b];
                in = &bl->instrs[k];
                in->dst = nv;
                spush(&stacks[o], nv);
                spush(&log, o);
            }
        }
        if (!ok) break;
        for (int s = 0; s < c.nsucc[b]; s++) {
            IrBlock *sb = &f->blocks[c.succs[b][s]];
            for (int k = 0; k < sb->n && sb->instrs[k].op == IR_PHI; k++) {
                IrInstr *phi = &sb->instrs[k];
                int o = (int)phi->imm;
                for (int i = 0; i < phi->nargs; i++)
                    if (phi->phi_blocks[i] == b) phi->args[i] = stacks[o].n ? stacks[o].v[stacks[o].n - 1] : IR_NONE;
            }
        }
        for (int i = c.ndchild[b] - 1; i >= 0; i--) {
            frame_block[sp] = c.dchild[b][i];
            frame_state[sp] = 0;
            sp++;
        }
    }
    for (int b = 0; b < f->nblocks && ok; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (in->op != IR_PHI) break;
            in->imm = 0;
            for (int i = 0; i < in->nargs; i++)
                if (in->args[i] < 0) ok = false;
        }

    free(ptrs);
    for (int v = 0; v < nv0; v++) free(stacks[v].v);
    free(stacks);
    free(kept);
    free(log.v);
    free(frame_block);
    free(frame_mark);
    free(frame_state);
    free(hasphi);
    free(isdef);
    cfg_free_lists(df, ndf, nb);
    liveness_free(&lv);
    cfg_free(&c);
    return ok;
}

void opt_from_ssa(IrFunc *f) {
    for (int b = 0; b < f->nblocks; b++) {
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *phi = &f->blocks[b].instrs[k];
            if (phi->op != IR_PHI) continue;
            int t = opt_fresh_vreg(f, f->vregs[phi->dst]);
            phi = &f->blocks[b].instrs[k];
            for (int i = 0; i < phi->nargs; i++) {
                int p = phi->phi_blocks[i];
                IrInstr mv;
                memset(&mv, 0, sizeof mv);
                mv.op = IR_MOV;
                mv.dst = t;
                mv.a = phi->args[i];
                mv.b = IR_NONE;
                mv.global = IR_NONE;
                mv.target[0] = mv.target[1] = IR_NONE;
                mv.line = phi->line;
                opt_insert(f, p, f->blocks[p].n - 1, mv); /* before the terminator */
                phi = &f->blocks[b].instrs[k];
            }
            ir_instr_free(phi);
            phi->op = IR_MOV;
            phi->a = t;
            phi->nargs = 0;
        }
    }
}

/* ---- copy coalescing (after leaving SSA) ----
 * Chaitin-style aggressive coalescing: build the interference graph from
 * liveness, then merge the two sides of every `d = mov s` whose live ranges
 * do not interfere, and delete the copy. Parameters keep their identity. */

typedef struct {
    int nv, nw;
    Word *m; /* nv x nv bit matrix */
} Graph;

static void edge(Graph *g, int a, int b) {
    if (a == b) return;
    bit_set(g->m + (size_t)a * g->nw, b);
    bit_set(g->m + (size_t)b * g->nw, a);
}

static int root(int *uf, int v) {
    while (uf[v] != v) {
        uf[v] = uf[uf[v]];
        v = uf[v];
    }
    return v;
}

void opt_coalesce(IrFunc *f) {
    int nv = f->nvregs, nb = f->nblocks;
    if (nv == 0 || nb == 0) return;
    Cfg c;
    cfg_build(f, &c);
    Liveness lv;
    liveness_compute(f, &c, &lv);
    Graph g = {nv, BITS_WORDS(nv), NULL};
    g.m = xcalloc((size_t)nv * g.nw, sizeof(Word));
    Word *live = xmalloc((size_t)g.nw * sizeof(Word));
    int **ptrs = NULL, cap_ptrs = 0;
    for (int b = 0; b < nb; b++) {
        if (!cfg_reachable(&c, b)) continue;
        memcpy(live, live_out(&lv, b), (size_t)g.nw * sizeof(Word));
        IrBlock *bl = &f->blocks[b];
        for (int k = bl->n - 1; k >= 0; k--) {
            IrInstr *in = &bl->instrs[k];
            if (in->dst >= 0) {
                for (int w = 0; w < g.nw; w++) {
                    Word x = live[w];
                    while (x) {
                        int v = w * 64 + __builtin_ctzll(x);
                        x &= x - 1;
                        if (!(in->op == IR_MOV && v == in->a)) edge(&g, in->dst, v);
                    }
                }
                bit_clear(live, in->dst);
            }
            if (in->nargs + 2 > cap_ptrs) {
                cap_ptrs = in->nargs + 2;
                ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
            }
            int nu = ir_instr_uses(in, ptrs);
            for (int i = 0; i < nu; i++)
                if (*ptrs[i] >= 0) bit_set(live, *ptrs[i]);
        }
    }
    /* parameters are all defined on entry: they interfere with each other
     * and with anything live into the entry block */
    for (int p = 0; p < f->nparams; p++) {
        for (int q = 0; q < f->nparams; q++) edge(&g, p, q);
        for (int v = 0; v < nv; v++)
            if (bit_get(live_in(&lv, 0), v)) edge(&g, p, v);
    }
    int *uf = xmalloc((size_t)nv * sizeof *uf);
    for (int v = 0; v < nv; v++) uf[v] = v;
    bool merged = false;
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            if (in->op != IR_MOV) continue;
            int x = root(uf, in->dst), y = root(uf, in->a);
            if (x == y) continue;
            if (bit_get(g.m + (size_t)x * g.nw, y)) continue; /* interfere */
            if (x < f->nparams && y < f->nparams) continue;
            if (x < f->nparams || (y >= f->nparams && y < x)) { /* keep the parameter (or the older vreg) */
                int t = x;
                x = y;
                y = t;
            }
            /* merge y into x... unless y is a parameter, in which case x merges into y */
            int keep = y < f->nparams ? y : x, gone = keep == x ? y : x;
            uf[gone] = keep;
            Word *rk = g.m + (size_t)keep * g.nw, *rg = g.m + (size_t)gone * g.nw;
            for (int w = 0; w < g.nw; w++) rk[w] |= rg[w];
            for (int v = 0; v < nv; v++)
                if (bit_get(rg, v)) bit_set(g.m + (size_t)v * g.nw, keep);
            merged = true;
        }
    if (merged) {
        for (int b = 0; b < nb; b++)
            for (int k = 0; k < f->blocks[b].n; k++) {
                IrInstr *in = &f->blocks[b].instrs[k];
                if (in->nargs + 2 > cap_ptrs) {
                    cap_ptrs = in->nargs + 2;
                    ptrs = xrealloc(ptrs, (size_t)cap_ptrs * sizeof *ptrs);
                }
                int nu = ir_instr_uses(in, ptrs);
                for (int i = 0; i < nu; i++)
                    if (*ptrs[i] >= 0) *ptrs[i] = root(uf, *ptrs[i]);
                if (in->dst >= 0) in->dst = root(uf, in->dst);
                if (in->op == IR_MOV && in->dst == in->a) in->op = IR_NOP;
            }
        ir_func_compact(f);
    }
    free(uf);
    free(ptrs);
    free(live);
    free(g.m);
    liveness_free(&lv);
    cfg_free(&c);
}
