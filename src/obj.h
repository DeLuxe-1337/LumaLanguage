/* obj.h - in-memory relocatable object model.
 *
 * This is the contract between the assembler (producer) and the ELF writer
 * (consumer). Neither depends on the other: tests can build an ObjFile by
 * hand and hand it to the ELF writer, or inspect what the assembler
 * produced without writing ELF at all. */
#ifndef LUMA_OBJ_H
#define LUMA_OBJ_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

/* ELF constants used by the object model (values from the ELF64/x86-64 ABI). */
enum {
    OBJ_SHT_PROGBITS = 1,
    OBJ_SHT_NOBITS = 8,
    OBJ_SHF_WRITE = 0x1,
    OBJ_SHF_ALLOC = 0x2,
    OBJ_SHF_EXECINSTR = 0x4,
    OBJ_STT_NOTYPE = 0,
    OBJ_STT_OBJECT = 1,
    OBJ_STT_FUNC = 2,
    /* x86-64 relocation types */
    OBJ_R_X86_64_64 = 1,     /* S + A          absolute 64-bit */
    OBJ_R_X86_64_PC32 = 2,   /* S + A - P      32-bit PC-relative */
    OBJ_R_X86_64_PLT32 = 4,  /* L + A - P      32-bit PC-relative via PLT */
};

typedef struct {
    char *name;
    uint32_t type;  /* OBJ_SHT_* */
    uint64_t flags; /* OBJ_SHF_* */
    uint64_t align;
    Buf data;
} ObjSection;

#define OBJ_UNDEF (-1)

typedef struct {
    char *name;
    int section;    /* index into ObjFile.sections, or OBJ_UNDEF */
    uint64_t value; /* offset within section */
    uint64_t size;
    uint8_t type;   /* OBJ_STT_* */
    bool global;
    bool local_label; /* ".L" names: never written to the symbol table */
} ObjSymbol;

typedef struct {
    int section;     /* section whose bytes are patched */
    uint64_t offset; /* offset of the field within that section */
    uint32_t type;   /* OBJ_R_X86_64_* */
    /* Exactly one of these is used: symbol >= 0 references a named symbol;
     * otherwise target_section >= 0 references that section's section symbol. */
    int symbol;
    int target_section;
    int64_t addend;
} ObjReloc;

typedef struct {
    char *source_name; /* recorded as an STT_FILE symbol; may be NULL */
    ObjSection *sections;
    int nsections;
    ObjSymbol *symbols;
    int nsymbols;
    ObjReloc *relocs;
    int nrelocs;
} ObjFile;

int obj_add_section(ObjFile *o, const char *name, uint32_t type, uint64_t flags, uint64_t align);
int obj_find_section(const ObjFile *o, const char *name);
int obj_find_symbol(const ObjFile *o, const char *name);
/* Returns the existing symbol with this name or creates an undefined one. */
int obj_intern_symbol(ObjFile *o, const char *name);
void obj_add_reloc(ObjFile *o, ObjReloc r);
void obj_free(ObjFile *o);

#endif
