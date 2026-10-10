#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

/* ---- CFG, RPO, dominators ---- */

static void push(int **list, int *n, int *cap, int v) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *list = xrealloc(*list, (size_t)*cap * sizeof **list);
    }
    (*list)[(*n)++] = v;
}

void cfg_build(const IrFunc *f, Cfg *c) {
    int nb = f->nblocks;
    memset(c, 0, sizeof *c);
    c->nblocks = nb;
    size_t n = (size_t)(nb ? nb : 1);
    c->succs = xcalloc(n, sizeof *c->succs);
    c->nsucc = xcalloc(n, sizeof *c->nsucc);
    for (int b = 0; b < nb; b++) {
        const IrBlock *bl = &f->blocks[b];
        if (bl->n == 0) continue;
        const IrInstr *t = &bl->instrs[bl->n - 1];
        if (t->op == IR_JMP) {
            c->succs[b][0] = t->target[0];
            c->nsucc[b] = 1;
        } else if (t->op == IR_BR) {
            c->succs[b][0] = t->target[0];
            c->nsucc[b] = 1;
            if (t->target[1] != t->target[0]) c->succs[b][c->nsucc[b]++] = t->target[1];
        }
    }

    /* reverse postorder by iterative DFS */
    c->rpo = xmalloc(n * sizeof *c->rpo);
    c->rpo_index = xmalloc(n * sizeof *c->rpo_index);
    for (int b = 0; b < nb; b++) c->rpo_index[b] = -1;
    int *post = xmalloc(n * sizeof *post), npost = 0;
    int *stack = xmalloc(n * sizeof *stack), *edge = xcalloc(n, sizeof *edge), sp = 0;
    bool *visited = xcalloc(n, sizeof *visited);
    if (nb > 0) {
        stack[sp++] = 0;
        visited[0] = true;
    }
    while (sp > 0) {
        int b = stack[sp - 1];
        if (edge[b] < c->nsucc[b]) {
            int s = c->succs[b][edge[b]++];
            if (!visited[s]) {
                visited[s] = true;
                stack[sp++] = s;
            }
        } else {
            post[npost++] = b;
            sp--;
        }
    }
    c->nrpo = npost;
    for (int i = 0; i < npost; i++) {
        c->rpo[i] = post[npost - 1 - i];
        c->rpo_index[c->rpo[i]] = i;
    }

    /* predecessors (from reachable blocks only) */
    c->preds = xcalloc(n, sizeof *c->preds);
    c->npred = xcalloc(n, sizeof *c->npred);
    int *pcap = xcalloc(n, sizeof *pcap);
    for (int i = 0; i < c->nrpo; i++) {
        int b = c->rpo[i];
        for (int k = 0; k < c->nsucc[b]; k++) push(&c->preds[c->succs[b][k]], &c->npred[c->succs[b][k]], &pcap[c->succs[b][k]], b);
    }

    /* dominators: Cooper, Harvey & Kennedy, "A Simple, Fast Dominance Algorithm" */
    c->idom = xmalloc(n * sizeof *c->idom);
    for (int b = 0; b < nb; b++) c->idom[b] = -1;
    if (nb > 0) c->idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 1; i < c->nrpo; i++) {
            int b = c->rpo[i], nd = -1;
            for (int k = 0; k < c->npred[b]; k++) {
                int p = c->preds[b][k];
                if (c->idom[p] < 0) continue;
                if (nd < 0) { nd = p; continue; }
                int x = p, y = nd;
                while (x != y) {
                    while (c->rpo_index[x] > c->rpo_index[y]) x = c->idom[x];
                    while (c->rpo_index[y] > c->rpo_index[x]) y = c->idom[y];
                }
                nd = x;
            }
            if (nd >= 0 && c->idom[b] != nd) {
                c->idom[b] = nd;
                changed = true;
            }
        }
    }
    if (nb > 0) c->idom[0] = -1;

    /* dominator tree children and DFS numbering */
    c->dchild = xcalloc(n, sizeof *c->dchild);
    c->ndchild = xcalloc(n, sizeof *c->ndchild);
    int *ccap = xcalloc(n, sizeof *ccap);
    for (int i = 1; i < c->nrpo; i++) {
        int b = c->rpo[i];
        if (c->idom[b] >= 0) push(&c->dchild[c->idom[b]], &c->ndchild[c->idom[b]], &ccap[c->idom[b]], b);
    }
    c->dom_pre = xmalloc(n * sizeof *c->dom_pre);
    c->dom_post = xmalloc(n * sizeof *c->dom_post);
    for (int b = 0; b < nb; b++) c->dom_pre[b] = c->dom_post[b] = -1;
    int clock = 0;
    memset(edge, 0, n * sizeof *edge);
    sp = 0;
    if (nb > 0) {
        stack[sp++] = 0;
        c->dom_pre[0] = clock++;
    }
    while (sp > 0) {
        int b = stack[sp - 1];
        if (edge[b] < c->ndchild[b]) {
            int ch = c->dchild[b][edge[b]++];
            c->dom_pre[ch] = clock++;
            stack[sp++] = ch;
        } else {
            c->dom_post[b] = clock++;
            sp--;
        }
    }
    free(post);
    free(stack);
    free(edge);
    free(visited);
    free(pcap);
    free(ccap);
}

void cfg_free(Cfg *c) {
    for (int b = 0; b < c->nblocks; b++) {
        free(c->preds[b]);
        free(c->dchild[b]);
    }
    free(c->succs);
    free(c->nsucc);
    free(c->preds);
    free(c->npred);
    free(c->rpo);
    free(c->rpo_index);
    free(c->idom);
    free(c->dchild);
    free(c->ndchild);
    free(c->dom_pre);
    free(c->dom_post);
    memset(c, 0, sizeof *c);
}

bool cfg_reachable(const Cfg *c, int b) { return b >= 0 && b < c->nblocks && c->rpo_index[b] >= 0; }

bool cfg_dominates(const Cfg *c, int a, int b) {
    if (!cfg_reachable(c, a) || !cfg_reachable(c, b)) return false;
    return c->dom_pre[a] <= c->dom_pre[b] && c->dom_post[b] <= c->dom_post[a];
}

int cfg_pred_index(const Cfg *c, int b, int pred) {
    for (int k = 0; k < c->npred[b]; k++)
        if (c->preds[b][k] == pred) return k;
    return -1;
}

void cfg_frontiers(const Cfg *c, int ***df_out, int **ndf_out) {
    size_t n = (size_t)(c->nblocks ? c->nblocks : 1);
    int **df = xcalloc(n, sizeof *df);
    int *ndf = xcalloc(n, sizeof *ndf), *cap = xcalloc(n, sizeof *cap);
    for (int i = 0; i < c->nrpo; i++) {
        int b = c->rpo[i];
        if (c->npred[b] < 2) continue;
        for (int k = 0; k < c->npred[b]; k++) {
            for (int r = c->preds[b][k]; r >= 0 && r != c->idom[b]; r = c->idom[r]) {
                bool dup = false;
                for (int j = 0; j < ndf[r] && !dup; j++) dup = df[r][j] == b;
                if (!dup) push(&df[r], &ndf[r], &cap[r], b);
                if (r == 0) break; /* the entry has no idom */
            }
        }
    }
    free(cap);
    *df_out = df;
    *ndf_out = ndf;
}

void cfg_free_lists(int **lists, int *counts, int n) {
    for (int i = 0; i < n; i++) free(lists[i]);
    free(lists);
    free(counts);
}

/* ---- liveness ---- */

void liveness_compute(const IrFunc *f, const Cfg *c, Liveness *lv) {
    int nb = f->nblocks, nv = f->nvregs, nw = BITS_WORDS(nv ? nv : 1);
    lv->nblocks = nb;
    lv->nvregs = nv;
    lv->nwords = nw;
    size_t cells = (size_t)(nb ? nb : 1) * (size_t)nw;
    lv->in = xcalloc(cells, sizeof(Word));
    lv->out = xcalloc(cells, sizeof(Word));
    Word *use = xcalloc(cells, sizeof(Word)), *def = xcalloc(cells, sizeof(Word));
    /* phiuse[p]: vregs read by phis of successors on edges out of p */
    Word *phiuse = xcalloc(cells, sizeof(Word));
    int *ptrs[300];
    int **big = NULL;
    int bigcap = 0;
    for (int b = 0; b < nb; b++) {
        Word *u = use + (size_t)b * nw, *d = def + (size_t)b * nw;
        const IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = (IrInstr *)&bl->instrs[k];
            if (in->op == IR_PHI) {
                for (int i = 0; i < in->nargs; i++)
                    if (in->args[i] >= 0 && in->phi_blocks[i] >= 0) bit_set(phiuse + (size_t)in->phi_blocks[i] * nw, in->args[i]);
                bit_set(d, in->dst);
                continue;
            }
            int **pp = ptrs;
            if (in->nargs + 2 > 300) {
                if (in->nargs + 2 > bigcap) {
                    bigcap = in->nargs + 2;
                    big = xrealloc(big, (size_t)bigcap * sizeof *big);
                }
                pp = big;
            }
            int nu = ir_instr_uses(in, pp);
            for (int i = 0; i < nu; i++)
                if (*pp[i] >= 0 && !bit_get(d, *pp[i])) bit_set(u, *pp[i]);
            if (in->dst >= 0) bit_set(d, in->dst);
        }
    }
    free(big);
    /* parameters are defined on entry */
    for (int p = 0; p < f->nparams && nb > 0; p++) bit_set(def, p);
    Word *tmp = xmalloc((size_t)nw * sizeof(Word));
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = c->nrpo - 1; i >= 0; i--) {
            int b = c->rpo[i];
            Word *out = lv->out + (size_t)b * nw, *in = lv->in + (size_t)b * nw;
            memcpy(tmp, phiuse + (size_t)b * nw, (size_t)nw * sizeof(Word));
            for (int k = 0; k < c->nsucc[b]; k++) {
                int s = c->succs[b][k];
                const Word *sin = lv->in + (size_t)s * nw;
                for (int w = 0; w < nw; w++) tmp[w] |= sin[w];
            }
            if (memcmp(tmp, out, (size_t)nw * sizeof(Word)) != 0) {
                memcpy(out, tmp, (size_t)nw * sizeof(Word));
                changed = true;
            }
            const Word *u = use + (size_t)b * nw, *d = def + (size_t)b * nw;
            for (int w = 0; w < nw; w++) {
                Word v = u[w] | (out[w] & ~d[w]);
                if (v != in[w]) {
                    in[w] = v;
                    changed = true;
                }
            }
        }
    }
    free(tmp);
    free(use);
    free(def);
    free(phiuse);
}

void liveness_free(Liveness *lv) {
    free(lv->in);
    free(lv->out);
    memset(lv, 0, sizeof *lv);
}

/* ---- natural loops ---- */

static int cmp_loop_size(const void *x, const void *y) {
    const Loop *a = x, *b = y;
    return a->size != b->size ? a->size - b->size : a->header - b->header;
}

void loops_find(const Cfg *c, Loop **out, int *nout) {
    int nb = c->nblocks, nw = BITS_WORDS(nb ? nb : 1);
    Loop *loops = NULL;
    int n = 0;
    int *stack = xmalloc((size_t)(nb ? nb : 1) * sizeof *stack);
    for (int i = 0; i < c->nrpo; i++) {
        int t = c->rpo[i];
        for (int k = 0; k < c->nsucc[t]; k++) {
            int h = c->succs[t][k];
            if (!cfg_dominates(c, h, t)) continue; /* not a back edge */
            int li = -1;
            for (int j = 0; j < n; j++)
                if (loops[j].header == h) li = j;
            if (li < 0) {
                loops = xrealloc(loops, (size_t)(n + 1) * sizeof *loops);
                loops[n].header = h;
                loops[n].body = xcalloc((size_t)nw, sizeof(Word));
                loops[n].size = 1;
                bit_set(loops[n].body, h);
                li = n++;
            }
            /* blocks that reach t without passing through h */
            int sp = 0;
            if (!bit_get(loops[li].body, t)) {
                bit_set(loops[li].body, t);
                loops[li].size++;
                stack[sp++] = t;
            }
            while (sp > 0) {
                int x = stack[--sp];
                for (int q = 0; q < c->npred[x]; q++) {
                    int p = c->preds[x][q];
                    if (!bit_get(loops[li].body, p)) {
                        bit_set(loops[li].body, p);
                        loops[li].size++;
                        stack[sp++] = p;
                    }
                }
            }
        }
    }
    free(stack);
    qsort(loops, (size_t)n, sizeof *loops, cmp_loop_size);
    *out = loops;
    *nout = n;
}

void loops_free(Loop *loops, int n) {
    for (int i = 0; i < n; i++) free(loops[i].body);
    free(loops);
}
