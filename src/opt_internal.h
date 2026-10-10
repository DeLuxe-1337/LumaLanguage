/* opt_internal.h - shared helpers of the optimizer passes (src/opt_*.c). */
#ifndef LUMA_OPT_INTERNAL_H
#define LUMA_OPT_INTERNAL_H

#include <stdbool.h>

#include "cfg.h"
#include "ir.h"

/* ---- instruction classes ---- */

/* Static type of every vreg (in SSA form a value has one type everywhere). */
IrTy *opt_value_types(const IrModule *m, const IrFunc *f);

/* Does executing `in` matter beyond defining its destination? True for
 * calls, stores, terminators and anything that may raise a runtime error
 * (given the operand types vty, which may be NULL = unknown). */
bool opt_has_effect(const IrModule *m, const IrInstr *in, const IrTy *vty);
/* May `in` raise a runtime error? */
bool opt_may_trap(const IrModule *m, const IrInstr *in, const IrTy *vty);

/* ---- CFG editing ---- */

/* Appends an instruction to block b (before nothing: callers manage order). */
IrInstr *opt_append(IrFunc *f, int b, IrInstr in);
/* Inserts `in` into block b at position pos. */
void opt_insert(IrFunc *f, int b, int pos, IrInstr in);
/* Removes blocks unreachable from the entry; drops phi arguments coming
 * from removed blocks and renumbers every block reference. */
void opt_remove_unreachable(IrFunc *f);
/* Inserts a new empty block at index `at` (later blocks shift up) and fixes
 * every block reference. Returns its index (== at). */
int opt_insert_block(IrFunc *f, int at, const char *label_base);
/* A fresh vreg named after `base`. */
int opt_fresh_vreg(IrFunc *f, const char *base);

/* ---- passes ---- */
bool opt_simplify_cfg(IrModule *m, IrFunc *f, bool tail_dup); /* non-SSA only */
bool opt_to_ssa(IrFunc *f);
void opt_from_ssa(IrFunc *f);
void opt_coalesce(IrFunc *f); /* non-SSA: merges non-interfering copy-related vregs */
bool opt_sccp(IrModule *m, IrFunc *f);
bool opt_gvn(IrModule *m, IrFunc *f);
bool opt_licm(IrModule *m, IrFunc *f);
bool opt_dce(IrModule *m, IrFunc *f);

#endif
