/* lower.h - AST -> LIR.
 *
 * The whole program becomes `fn @luma_main()`. Variables are resolved
 * statically with lexical block scoping (Lox rules): every declaration gets
 * its own vreg, shadowing is allowed in nested blocks, redeclaring in the
 * same block scope is an error (allowed at top level), and a block-local
 * variable cannot be read in its own initializer. Short-circuit `and`/`or`
 * become branches. Errors are reported as "path:line:col: error: ...". */
#ifndef LUMA_LOWER_H
#define LUMA_LOWER_H

#include <stdbool.h>

#include "ast.h"
#include "ir.h"

bool lower_program(const Program *prog, IrModule *out);

#endif
