#include "elf_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ELF64 constants (System V gABI + x86-64 psABI). Defined locally so the
 * writer does not depend on the host's <elf.h>. */
enum {
    ELFCLASS64 = 2,
    ELFDATA2LSB = 1,
    EV_CURRENT = 1,
    ELFOSABI_SYSV = 0,
    ET_REL = 1,
    EM_X86_64 = 62,
    EHDR_SIZE = 64,
    SHDR_SIZE = 64,
    SYM_SIZE = 24,
    RELA_SIZE = 24,
    SHT_NULL = 0,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHF_INFO_LINK = 0x40,
    SHN_UNDEF = 0,
    SHN_ABS = 0xfff1,
    STB_LOCAL = 0,
    STB_GLOBAL = 1,
    STT_SECTION = 3,
    STT_FILE = 4,
};

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Shdr;

/* String table builder: offset 0 is always the empty string. */
static uint32_t strtab_add(Buf *t, const char *s) {
    if (t->len == 0) buf_byte(t, 0);
    if (!*s) return 0;
    uint32_t off = (uint32_t)t->len;
    buf_append(t, s, strlen(s) + 1);
    return off;
}

static void put_sym(Buf *b, uint32_t name, uint8_t bind, uint8_t type, uint16_t shndx, uint64_t value,
                    uint64_t size) {
    buf_u32(b, name);
    buf_byte(b, (uint8_t)((bind << 4) | (type & 0xf)));
    buf_byte(b, 0); /* st_other: STV_DEFAULT */
    buf_u16(b, shndx);
    buf_u64(b, value);
    buf_u64(b, size);
}

static uint32_t reloc_width(uint32_t type) {
    switch (type) {
    case OBJ_R_X86_64_64: return 8;
    case OBJ_R_X86_64_PC32:
    case OBJ_R_X86_64_PLT32: return 4;
    }
    return 0;
}

static bool validate(const ObjFile *o) {
    if (o->nsections > 0xff00 / 2 - 8) {
        fprintf(stderr, "elf: error: too many sections\n");
        return false;
    }
    for (int i = 0; i < o->nsymbols; i++) {
        const ObjSymbol *s = &o->symbols[i];
        if (s->section != OBJ_UNDEF && (s->section < 0 || s->section >= o->nsections)) {
            fprintf(stderr, "elf: error: symbol '%s' has invalid section %d\n", s->name, s->section);
            return false;
        }
        if (s->section != OBJ_UNDEF && s->value > o->sections[s->section].data.len) {
            fprintf(stderr, "elf: error: symbol '%s' lies outside its section\n", s->name);
            return false;
        }
    }
    for (int i = 0; i < o->nrelocs; i++) {
        const ObjReloc *r = &o->relocs[i];
        uint32_t w = reloc_width(r->type);
        if (!w) {
            fprintf(stderr, "elf: error: unsupported relocation type %u\n", r->type);
            return false;
        }
        if (r->section < 0 || r->section >= o->nsections) {
            fprintf(stderr, "elf: error: relocation %d targets invalid section %d\n", i, r->section);
            return false;
        }
        size_t len = o->sections[r->section].data.len;
        if (r->offset > len || len - r->offset < w) {
            fprintf(stderr, "elf: error: relocation %d at offset %llu overruns section '%s'\n", i,
                    (unsigned long long)r->offset, o->sections[r->section].name);
            return false;
        }
        if (r->symbol >= 0) {
            if (r->symbol >= o->nsymbols) {
                fprintf(stderr, "elf: error: relocation %d references invalid symbol %d\n", i, r->symbol);
                return false;
            }
            if (o->symbols[r->symbol].local_label) {
                fprintf(stderr, "elf: error: relocation against local label '%s' (use a section symbol)\n",
                        o->symbols[r->symbol].name);
                return false;
            }
        } else if (r->target_section < 0 || r->target_section >= o->nsections) {
            fprintf(stderr, "elf: error: relocation %d has no valid symbol or section\n", i);
            return false;
        }
    }
    return true;
}

static bool is_global(const ObjSymbol *s) { return s->global || s->section == OBJ_UNDEF; }

bool elf_write_object(const ObjFile *o, Buf *out) {
    if (out->len != 0) {
        fprintf(stderr, "elf: error: output buffer must be empty\n");
        return false;
    }
    if (!validate(o)) return false;

    const int nsec = o->nsections;
    /* Section header indices. */
    int *rela_of = xcalloc((size_t)nsec, sizeof(int)); /* ELF index of .rela.<sec>, or 0 */
    int next = 1 + nsec;
    for (int i = 0; i < nsec; i++) {
        for (int r = 0; r < o->nrelocs; r++)
            if (o->relocs[r].section == i) { rela_of[i] = next++; break; }
    }
    const int symtab_idx = next++;
    const int strtab_idx = next++;
    const int shstrtab_idx = next++;
    const int nshdr = next;
    Shdr *sh = xcalloc((size_t)nshdr, sizeof *sh);

    Buf shstr = {0}, str = {0}, symtab = {0};
    strtab_add(&shstr, "");
    strtab_add(&str, "");

    /* ---- symbol table ---- */
    int *symidx = xcalloc((size_t)(o->nsymbols ? o->nsymbols : 1), sizeof(int));
    int *secsym = xcalloc((size_t)(nsec ? nsec : 1), sizeof(int));
    int nsyms = 0;
    put_sym(&symtab, 0, 0, 0, SHN_UNDEF, 0, 0), nsyms++;
    if (o->source_name)
        put_sym(&symtab, strtab_add(&str, o->source_name), STB_LOCAL, STT_FILE, SHN_ABS, 0, 0), nsyms++;
    for (int i = 0; i < nsec; i++) {
        secsym[i] = nsyms++;
        put_sym(&symtab, 0, STB_LOCAL, STT_SECTION, (uint16_t)(i + 1), 0, 0);
    }
    for (int i = 0; i < o->nsymbols; i++) {
        const ObjSymbol *s = &o->symbols[i];
        if (s->local_label || is_global(s)) continue;
        symidx[i] = nsyms++;
        put_sym(&symtab, strtab_add(&str, s->name), STB_LOCAL, s->type, (uint16_t)(s->section + 1), s->value,
                s->size);
    }
    const int first_global = nsyms;
    for (int i = 0; i < o->nsymbols; i++) {
        const ObjSymbol *s = &o->symbols[i];
        if (s->local_label || !is_global(s)) continue;
        symidx[i] = nsyms++;
        uint16_t shndx = s->section == OBJ_UNDEF ? SHN_UNDEF : (uint16_t)(s->section + 1);
        put_sym(&symtab, strtab_add(&str, s->name), STB_GLOBAL, s->type, shndx,
                s->section == OBJ_UNDEF ? 0 : s->value, s->size);
    }

    /* ---- file image ---- */
    buf_zeros(out, EHDR_SIZE); /* header filled in at the end */
    size_t base = out->len - EHDR_SIZE;
#define FOFF() ((uint64_t)(out->len - base))

    for (int i = 0; i < nsec; i++) {
        const ObjSection *s = &o->sections[i];
        Shdr *h = &sh[i + 1];
        h->name = strtab_add(&shstr, s->name);
        h->type = s->type;
        h->flags = s->flags;
        h->addralign = s->align;
        buf_align(out, s->align > 1 ? s->align : 1);
        h->offset = FOFF();
        h->size = s->data.len;
        if (s->type != OBJ_SHT_NOBITS) buf_append(out, s->data.data, s->data.len);
    }
    for (int i = 0; i < nsec; i++) {
        if (!rela_of[i]) continue;
        Shdr *h = &sh[rela_of[i]];
        char name[300];
        snprintf(name, sizeof name, ".rela%s", o->sections[i].name);
        h->name = strtab_add(&shstr, name);
        h->type = SHT_RELA;
        h->flags = SHF_INFO_LINK;
        h->link = (uint32_t)symtab_idx;
        h->info = (uint32_t)(i + 1);
        h->addralign = 8;
        h->entsize = RELA_SIZE;
        buf_align(out, 8);
        h->offset = FOFF();
        for (int r = 0; r < o->nrelocs; r++) {
            const ObjReloc *rel = &o->relocs[r];
            if (rel->section != i) continue;
            uint64_t sym = rel->symbol >= 0 ? (uint64_t)symidx[rel->symbol] : (uint64_t)secsym[rel->target_section];
            buf_u64(out, rel->offset);
            buf_u64(out, (sym << 32) | rel->type);
            buf_u64(out, (uint64_t)rel->addend);
        }
        h->size = FOFF() - h->offset;
    }

    Shdr *h = &sh[symtab_idx];
    h->name = strtab_add(&shstr, ".symtab");
    h->type = SHT_SYMTAB;
    h->link = (uint32_t)strtab_idx;
    h->info = (uint32_t)first_global;
    h->addralign = 8;
    h->entsize = SYM_SIZE;
    buf_align(out, 8);
    h->offset = FOFF();
    h->size = symtab.len;
    buf_append(out, symtab.data, symtab.len);

    h = &sh[strtab_idx];
    h->name = strtab_add(&shstr, ".strtab");
    h->type = SHT_STRTAB;
    h->addralign = 1;
    h->offset = FOFF();
    h->size = str.len;
    buf_append(out, str.data, str.len);

    h = &sh[shstrtab_idx];
    h->name = strtab_add(&shstr, ".shstrtab"); /* add own name before measuring */
    h->type = SHT_STRTAB;
    h->addralign = 1;
    h->offset = FOFF();
    h->size = shstr.len;
    buf_append(out, shstr.data, shstr.len);

    buf_align(out, 8);
    uint64_t shoff = FOFF();
    for (int i = 0; i < nshdr; i++) {
        buf_u32(out, sh[i].name);
        buf_u32(out, sh[i].type);
        buf_u64(out, sh[i].flags);
        buf_u64(out, sh[i].addr);
        buf_u64(out, sh[i].offset);
        buf_u64(out, sh[i].size);
        buf_u32(out, sh[i].link);
        buf_u32(out, sh[i].info);
        buf_u64(out, sh[i].addralign);
        buf_u64(out, sh[i].entsize);
    }

    /* ---- ELF header ---- */
    Buf eh = {0};
    const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_SYSV};
    buf_append(&eh, ident, 16);
    buf_u16(&eh, ET_REL);
    buf_u16(&eh, EM_X86_64);
    buf_u32(&eh, EV_CURRENT);
    buf_u64(&eh, 0); /* e_entry */
    buf_u64(&eh, 0); /* e_phoff */
    buf_u64(&eh, shoff);
    buf_u32(&eh, 0); /* e_flags */
    buf_u16(&eh, EHDR_SIZE);
    buf_u16(&eh, 0); /* e_phentsize */
    buf_u16(&eh, 0); /* e_phnum */
    buf_u16(&eh, SHDR_SIZE);
    buf_u16(&eh, (uint16_t)nshdr);
    buf_u16(&eh, (uint16_t)shstrtab_idx);
    memcpy(out->data + base, eh.data, EHDR_SIZE);
#undef FOFF

    buf_free(&eh);
    buf_free(&shstr);
    buf_free(&str);
    buf_free(&symtab);
    free(sh);
    free(symidx);
    free(secsym);
    free(rela_of);
    return true;
}

bool elf_write_file(const ObjFile *o, const char *path) {
    Buf b = {0};
    bool ok = elf_write_object(o, &b) && write_file(path, b.data, b.len);
    buf_free(&b);
    return ok;
}
