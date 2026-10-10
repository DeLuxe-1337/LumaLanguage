/* cfg.h - control-flow analyses over a LIR function, shared by the
 * optimizer (src/opt_*.c) and the register-allocating backend.
 *
 *   - successors / predecessors (only edges out of reachable blocks)
 *   - reverse postorder of the reachable blocks
 *   - dominator tree (Cooper, Harvey & Kennedy) and dominance frontiers
 *   - liveness of vregs (phi-aware) as bitsets
 *   - natural loops */
#ifndef LUMA_CFG_H
#define LUMA_CFG_H

#include <stdbool.h>
#include <stdint.h>

#include "ir.h"

/* ---- bitsets ---- */
typedef uint64_t Word;
#define BITS_WORDS(n) (((n) + 63) / 64)
static inline bool bit_get(const Word *s, int i) { return (s[i >> 6] >> (i & 63)) & 1; }
static inline void bit_set(Word *s, int i) { s[i >> 6] |= (Word)1 << (i & 63); }
static inline void bit_clear(Word *s, int i) { s[i >> 6] &= ~((Word)1 << (i & 63)); }

typedef struct {
    int nblocks;
    int (*succs)[2];  /* succs[b][0..nsucc[b]-1] */
    int *nsucc;
    int **preds;      /* preds[b][0..npred[b]-1], reachable predecessors only */
    int *npred;
    int *rpo;         /* reachable blocks in reverse postorder */
    int nrpo;
    int *rpo_index;   /* block -> index in rpo, or -1 if unreachable */
    int *idom;        /* immediate dominator, -1 for the entry and unreachable blocks */
    int **dchild;     /* dominator-tree children */
    int *ndchild;
    int *dom_pre, *dom_post; /* dominator-tree DFS numbering, for O(1) dominance queries */
} Cfg;

void cfg_build(const IrFunc *f, Cfg *c);
void cfg_free(Cfg *c);
bool cfg_reachable(const Cfg *c, int b);
/* Does block a dominate block b? (reflexive) */
bool cfg_dominates(const Cfg *c, int a, int b);
/* Index of `pred` in preds[b], or -1. */
int cfg_pred_index(const Cfg *c, int b, int pred);
/* Dominance frontiers: df[b] lists blocks, ndf[b] their count. Caller frees with cfg_free_lists. */
void cfg_frontiers(const Cfg *c, int ***df, int **ndf);
void cfg_free_lists(int **lists, int *counts, int n);

/* ---- liveness ----
 * live_in[b] / live_out[b] are bitsets of nwords words over vregs. A phi's
 * arguments are live out of the corresponding predecessor (not live into
 * the phi's block); a phi's destination is defined at the top of its block. */
typedef struct {
    int nblocks, nvregs, nwords;
    Word *in, *out; /* in + b * nwords */
} Liveness;

void liveness_compute(const IrFunc *f, const Cfg *c, Liveness *lv);
void liveness_free(Liveness *lv);
static inline Word *live_in(const Liveness *lv, int b) { return lv->in + (size_t)b * lv->nwords; }
static inline Word *live_out(const Liveness *lv, int b) { return lv->out + (size_t)b * lv->nwords; }

/* ---- natural loops ---- */
typedef struct {
    int header;
    Word *body;  /* bitset over blocks */
    int size;    /* number of blocks */
} Loop;

/* Loops sorted innermost (smallest) first; loops sharing a header are merged. */
void loops_find(const Cfg *c, Loop **out, int *n);
void loops_free(Loop *loops, int n);

#endif
