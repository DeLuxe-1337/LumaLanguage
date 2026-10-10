/* x86_gen.h - the optimizing x86-64 backend (-O1 and -O2).
 *
 * Unlike the naive backend (x86_isel.c, -O0), which keeps every vreg in a
 * stack slot, this backend
 *   - allocates registers with linear scan over liveness intervals (values
 *     live across calls go to callee-saved registers, others preferably to
 *     caller-saved ones; constants are rematerialized as immediates),
 *   - inlines tagged-fixnum fast paths for arithmetic and comparisons, with
 *     out-of-line slow paths that call the runtime and preserve exactly the
 *     live caller-saved registers, and uses static types to skip tag checks,
 *   - fuses compare + branch into cmp/jcc, emits tail calls as jumps, and
 *     builds minimal frames (none for leaf functions that need none),
 *   - runs a small peephole pass (fall-through jumps, inverted branches,
 *     self-moves).
 * The emitted dialect is the lasm subset of GNU as syntax. */
#ifndef LUMA_X86_GEN_H
#define LUMA_X86_GEN_H

#include "ir.h"
#include "util.h"

void x86_gen_module(const IrModule *m, Buf *out);

#endif
