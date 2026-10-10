/* x86_isel.h - LIR -> x86-64 assembly text (docs/IR.md §8, naive v0.1).
 *
 * Every vreg lives in a stack slot ([rbp - 8(k+1)]); each LIR instruction
 * loads its operands into scratch registers, operates (or calls the
 * runtime), and stores the result. Output is the lasm dialect (also valid
 * GNU as input). The module must have passed ir_verify(). */
#ifndef LUMA_X86_ISEL_H
#define LUMA_X86_ISEL_H

#include "ir.h"
#include "util.h"

void x86_emit_module(const IrModule *m, Buf *out);

/* Shared with the optimizing backend (x86_gen.c): string objects, global
 * variable slots and their display names; and .ascii escaping. */
void x86_emit_data(const IrModule *m, Buf *out);
void x86_emit_ascii(Buf *out, const char *s, size_t n);

#endif
