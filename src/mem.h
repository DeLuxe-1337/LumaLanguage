/* mem.h - memory management in the compiler (docs/DESIGN.md "Memory").
 *
 * Three static analyses over LIR:
 *
 *   mem_check_frees  on the lowered IR (every mode): a variable that is used,
 *                    or freed again, after free() -- or after Rc::new moved
 *                    it into a box -- on every path that reaches the use is
 *                    a compile error.
 *   mem_lower        after optimization, before the backend (--mm=manual):
 *                    1. escape analysis (interprocedural): a struct
 *                       allocation whose instances provably never leave the
 *                       function's local values (never stored, returned,
 *                       passed to free() or to a parameter that escapes) is
 *                       reference counted, i.e. freed automatically; Rc
 *                       boxes and arena handles always are;
 *                    2. ownership: inserts `retain` / `release` so that every
 *                       counted reference held by a vreg, a field or a global
 *                       is released exactly once, at the vreg's last use
 *                       (liveness), on the CFG edge where it dies, or when a
 *                       field or global is overwritten.
 *
 * After mem_lower the module has mem_lowered set and verifies again. */
#ifndef LUMA_MEM_H
#define LUMA_MEM_H

#include <stdbool.h>

#include "ir.h"

bool mem_check_frees(const IrModule *m, const char *path);
void mem_lower(IrModule *m);

#endif
