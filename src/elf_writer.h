/* elf_writer.h - ELF64 relocatable object writer (x86-64, little endian).
 *
 * Serializes an ObjFile into an ET_REL image. Layout (see docs/DESIGN.md):
 *
 *   ELF header | user section data... | .rela.<sec>... | .symtab | .strtab
 *   | .shstrtab | section header table
 *
 * Symbol table order: null, STT_FILE, one STT_SECTION per user section,
 * named local symbols, then globals (defined and undefined). sh_info of
 * .symtab is the index of the first global, as the ELF spec requires. */
#ifndef LUMA_ELF_WRITER_H
#define LUMA_ELF_WRITER_H

#include <stdbool.h>

#include "obj.h"
#include "util.h"

/* Appends the ELF image to out. Validates the object model first (relocation
 * targets in bounds, symbol references valid); on failure prints an error
 * and returns false. */
bool elf_write_object(const ObjFile *o, Buf *out);
/* Convenience: serialize and write to path. */
bool elf_write_file(const ObjFile *o, const char *path);

#endif
