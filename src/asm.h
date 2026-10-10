/* asm.h - Luma's x86-64 assembler: assembly text -> ObjFile.
 *
 * Input dialect: Intel syntax (`.intel_syntax noprefix`) with GAS-style
 * directives. See docs/LASM.md for the full instruction/directive list,
 * encoding decisions, and how symbols and relocations are resolved.
 *
 * The assembler produces machine code bytes and relocation records into an
 * ObjFile; it knows nothing about ELF. Use elf_write_object() (elf_writer.h) to serialize. */
#ifndef LUMA_ASM_H
#define LUMA_ASM_H

#include <stdbool.h>
#include <stddef.h>

#include "obj.h"

/* Assembles src (len bytes). path is used in diagnostics. On error prints
 * "path:line: error: ..." to stderr, frees partial output, and returns false. */
bool assemble(const char *path, const char *src, size_t len, ObjFile *out);

#endif
