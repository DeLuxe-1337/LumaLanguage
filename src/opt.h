/* opt.h - the LIR optimizer.
 *
 *   level 0: nothing (the -O0 pipeline also uses the naive backend)
 *   level 1: nothing at the IR level (the register-allocating backend only)
 *   level 2: CFG simplification with loop rotation, SSA construction,
 *            SCCP (constant propagation + branch folding, type-aware),
 *            GVN/CSE (with copy propagation, check elimination and
 *            block-local load forwarding), LICM, DCE, out-of-SSA, cleanup.
 *
 * Every function is optimized on a copy; if a pass meets something it does
 * not handle, the original function is kept. The result is ordinary
 * (non-SSA) LIR that must pass ir_verify(); the driver checks that. */
#ifndef LUMA_OPT_H
#define LUMA_OPT_H

#include <stdbool.h>

#include "ir.h"

void opt_module(IrModule *m, int level);

#endif
