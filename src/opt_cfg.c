/* opt_cfg.c - CFG simplification on non-SSA LIR.
 *
 * Repeats until nothing changes:
 *   - `br %c, X, X`         -> `jmp X`
 *   - unreachable blocks are removed
 *   - jump threading: a block that is only `jmp C` is bypassed
 *   - block merging: `B: ... jmp C` where C's only predecessor is B
 *   - tail duplication (when enabled): `B: ... jmp H` where H is a small
 *     block ending in `br` gets a copy of H instead of the jump. Applied to a
 *     loop's `while.cond` block this is loop rotation: the test is copied
 *     into the loop entry (as a guard) and into the latch, so each iteration
 *     ends in one conditional branch and the old header disappears.
 * Copying instructions verbatim is valid because non-SSA vregs are mutable:
 * the copy recomputes exactly what H would have computed. */
#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"

#define TAIL_DUP_MAX 8 /* instructions, including the final br */

static IrInstr *term(IrFunc *f, int b) {
    IrBlock *bl = &f->blocks[b];
    return bl->n ? &bl->instrs[bl->n - 1] : NULL;
}

static int *pred_counts(IrFunc *f) {
    int *np = xcalloc((size_t)(f->nblocks ? f->nblocks : 1), sizeof *np);
    for (int b = 0; b < f->nblocks; b++) {
        IrInstr *t = term(f, b);
        if (!t) continue;
        if (t->op == IR_JMP) np[t->target[0]]++;
        else if (t->op == IR_BR) {
            np[t->target[0]]++;
            if (t->target[1] != t->target[0]) np[t->target[1]]++;
        }
    }
    return np;
}

static bool dup_candidate(IrFunc *f, int h) {
    IrBlock *bl = &f->blocks[h];
    if (bl->n == 0 || bl->n > TAIL_DUP_MAX || bl->instrs[bl->n - 1].op != IR_BR) return false;
    for (int k = 0; k < bl->n; k++) {
        IrOp op = bl->instrs[k].op;
        if (op == IR_CALL || op == IR_STORE || op == IR_PHI) return false; /* keep calls in one place */
    }
    return true;
}

bool opt_simplify_cfg(IrModule *m, IrFunc *f, bool tail_dup) {
    (void)m;
    bool any = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (int b = 0; b < f->nblocks; b++) {
            IrInstr *t = term(f, b);
            if (t && t->op == IR_BR && t->target[0] == t->target[1]) {
                t->op = IR_JMP;
                t->a = IR_NONE;
                t->target[1] = IR_NONE;
                changed = true;
            }
        }
        int before = f->nblocks;
        opt_remove_unreachable(f);
        if (f->nblocks != before) changed = true;

        /* jump threading */
        for (int b = 1; b < f->nblocks; b++) {
            IrBlock *bl = &f->blocks[b];
            if (bl->n != 1 || bl->instrs[0].op != IR_JMP) continue;
            int c = bl->instrs[0].target[0];
            if (c == b) continue;
            for (int x = 0; x < f->nblocks; x++) {
                IrInstr *t = term(f, x);
                if (!t || x == b) continue;
                for (int k = 0; k < 2; k++)
                    if ((t->op == IR_JMP || t->op == IR_BR) && t->target[k] == b) {
                        t->target[k] = c;
                        changed = true;
                    }
            }
        }

        /* block merging */
        int *np = pred_counts(f);
        for (int b = 0; b < f->nblocks; b++) {
            IrInstr *t = term(f, b);
            if (!t || t->op != IR_JMP) continue;
            int c = t->target[0];
            if (c == b || c == 0 || np[c] != 1 || f->blocks[c].n == 0) continue;
            f->blocks[b].n--; /* drop the jmp */
            for (int k = 0; k < f->blocks[c].n; k++) opt_append(f, b, f->blocks[c].instrs[k]);
            f->blocks[c].n = 0; /* moved; c is now unreachable and empty */
            /* c's successors now have b as predecessor instead: counts unchanged */
            np[c] = 0;
            changed = true;
        }
        free(np);
        if (changed) {
            any = true;
            continue;
        }

        if (tail_dup) {
            for (int b = 0; b < f->nblocks; b++) {
                IrInstr *t = term(f, b);
                if (!t || t->op != IR_JMP) continue;
                int h = t->target[0];
                if (h == b || !dup_candidate(f, h)) continue;
                f->blocks[b].n--; /* drop the jmp */
                IrBlock *hb = &f->blocks[h];
                for (int k = 0; k < hb->n; k++) {
                    IrInstr c = ir_instr_clone(&hb->instrs[k]);
                    opt_append(f, b, c);
                    hb = &f->blocks[h];
                }
                changed = true;
            }
        }
        if (changed) any = true;
    }
    return any;
}
