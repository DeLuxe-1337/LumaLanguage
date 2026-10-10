/* lower.h - AST -> LIR.
 *
 * Module layout produced:
 *   fn @luma_main()      the top-level statements, in order
 *   fn @fn.NAME(...)     one per top-level `fun NAME`
 *   global @var.NAME     one per top-level `var NAME` (module globals)
 *   data @sN             string literals (deduplicated)
 *   extern fn @luma_write/@luma_write_space/@luma_write_newline   (when print is used)
 *
 * Name resolution is static (Lox rules, checked at compile time):
 *  - block-local variables (including parameters) shadow globals; redeclaring
 *    in the same block, or reading a local in its own initializer, is an error;
 *  - top-level variables are globals. Top-level code sees those declared
 *    earlier; function bodies see all of them (reading one before its
 *    declaration has executed is a runtime "Undefined variable" error);
 *  - functions are hoisted: any function may call any other, in any order;
 *  - `print` is a builtin; functions are not first-class values yet.
 * Errors are reported as "path:line:col: error: ...". */
#ifndef LUMA_LOWER_H
#define LUMA_LOWER_H

#include <stdbool.h>

#include "ast.h"
#include "ir.h"

bool lower_program(const Program *prog, IrModule *out);

#endif
