/* codegen.h - AST -> x86-64 assembly text (Intel syntax, GAS-style directives).
 *
 * The emitted dialect is the input language of Luma's own assembler
 * (src/asm.c). It is deliberately also valid GNU `as` input under
 * `.intel_syntax noprefix`, which lets the test suite cross-check our
 * encodings against an independent assembler. The build pipeline itself
 * never invokes an external assembler. */
#ifndef LUMA_CODEGEN_H
#define LUMA_CODEGEN_H

#include "ast.h"
#include "util.h"

/* Appends the assembly for prog to out. */
void codegen_program(const Program *prog, Buf *out);

#endif
