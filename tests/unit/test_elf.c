/* Unit tests for the ELF64 writer, independent of the assembler: an ObjFile
 * is built by hand, serialized, and parsed back with the host's <elf.h>. */
#include <elf.h>
#include <stdlib.h>

#include "check.h"
#include "elf_writer.h"

static const Elf64_Shdr *shdr(const Buf *b, int i) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b->data;
    return (const Elf64_Shdr *)(b->data + eh->e_shoff + (size_t)i * sizeof(Elf64_Shdr));
}

static const char *secname(const Buf *b, int i) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b->data;
    return (const char *)b->data + shdr(b, eh->e_shstrndx)->sh_offset + shdr(b, i)->sh_name;
}

static int find_sec(const Buf *b, const char *name) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b->data;
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(secname(b, i), name) == 0) return i;
    return -1;
}

static void build_sample(ObjFile *o) {
    memset(o, 0, sizeof *o);
    o->source_name = xstrdup("sample.luma");
    int text = obj_add_section(o, ".text", OBJ_SHT_PROGBITS, OBJ_SHF_ALLOC | OBJ_SHF_EXECINSTR, 16);
    int ro = obj_add_section(o, ".rodata", OBJ_SHT_PROGBITS, OBJ_SHF_ALLOC, 1);
    /* lea rdi,[rip+0]; call 0; ret */
    static const unsigned char code[] = {0x48, 0x8d, 0x3d, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0xc3};
    buf_append(&o->sections[text].data, code, sizeof code);
    buf_append(&o->sections[ro].data, "hey", 4);
    int lbl = obj_intern_symbol(o, "local_fn");
    o->symbols[lbl].section = text;
    o->symbols[lbl].value = 12;
    int m = obj_intern_symbol(o, "main");
    o->symbols[m].section = text;
    o->symbols[m].global = true;
    o->symbols[m].type = OBJ_STT_FUNC;
    o->symbols[m].size = 13;
    int puts_sym = obj_intern_symbol(o, "puts");
    obj_add_reloc(o, (ObjReloc){text, 3, OBJ_R_X86_64_PC32, -1, ro, -4});
    obj_add_reloc(o, (ObjReloc){text, 8, OBJ_R_X86_64_PLT32, puts_sym, -1, -4});
}

static void test_layout(void) {
    ObjFile o;
    build_sample(&o);
    Buf b = {0};
    CHECK(elf_write_object(&o, &b));
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b.data;
    CHECK(memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0);
    CHECK_EQ_INT(eh->e_ident[EI_CLASS], ELFCLASS64);
    CHECK_EQ_INT(eh->e_ident[EI_DATA], ELFDATA2LSB);
    CHECK_EQ_INT(eh->e_type, ET_REL);
    CHECK_EQ_INT(eh->e_machine, EM_X86_64);
    CHECK_EQ_INT(eh->e_ehsize, sizeof(Elf64_Ehdr));
    CHECK_EQ_INT(eh->e_shentsize, sizeof(Elf64_Shdr));
    CHECK_EQ_INT(eh->e_phnum, 0);
    CHECK_EQ_INT(eh->e_shoff % 8, 0);
    CHECK(eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) == b.len);
    /* null, .text, .rodata, .rela.text, .symtab, .strtab, .shstrtab */
    CHECK_EQ_INT(eh->e_shnum, 7);
    CHECK_EQ_INT(shdr(&b, 0)->sh_type, SHT_NULL);

    int text = find_sec(&b, ".text"), ro = find_sec(&b, ".rodata"), rela = find_sec(&b, ".rela.text");
    int symtab = find_sec(&b, ".symtab"), strtab = find_sec(&b, ".strtab");
    CHECK(text == 1 && ro == 2 && rela > 0 && symtab > 0 && strtab > 0);
    CHECK_EQ_INT(eh->e_shstrndx, find_sec(&b, ".shstrtab"));
    CHECK_EQ_INT(shdr(&b, text)->sh_flags, SHF_ALLOC | SHF_EXECINSTR);
    CHECK_EQ_INT(shdr(&b, text)->sh_offset % 16, 0);
    CHECK_EQ_INT(shdr(&b, text)->sh_size, 13);
    CHECK(memcmp(b.data + shdr(&b, ro)->sh_offset, "hey", 4) == 0);

    const Elf64_Shdr *rs = shdr(&b, rela);
    CHECK_EQ_INT(rs->sh_type, SHT_RELA);
    CHECK_EQ_INT(rs->sh_link, symtab);
    CHECK_EQ_INT(rs->sh_info, text);
    CHECK_EQ_INT(rs->sh_entsize, sizeof(Elf64_Rela));
    CHECK_EQ_INT(rs->sh_size, 2 * sizeof(Elf64_Rela));

    const Elf64_Shdr *ss = shdr(&b, symtab);
    CHECK_EQ_INT(ss->sh_link, strtab);
    CHECK_EQ_INT(ss->sh_entsize, sizeof(Elf64_Sym));
    const Elf64_Sym *syms = (const Elf64_Sym *)(b.data + ss->sh_offset);
    int nsyms = (int)(ss->sh_size / sizeof(Elf64_Sym));
    const char *names = (const char *)b.data + shdr(&b, strtab)->sh_offset;
    /* null, FILE, SECTION x2, local_fn | main, puts */
    CHECK_EQ_INT(nsyms, 7);
    CHECK_EQ_INT(ss->sh_info, 5);
    for (int i = 0; i < nsyms; i++) {
        int bind = ELF64_ST_BIND(syms[i].st_info);
        CHECK(i < (int)ss->sh_info ? bind == STB_LOCAL : bind == STB_GLOBAL);
    }
    CHECK_EQ_INT(ELF64_ST_TYPE(syms[1].st_info), STT_FILE);
    CHECK(strcmp(names + syms[1].st_name, "sample.luma") == 0);
    CHECK_EQ_INT(ELF64_ST_TYPE(syms[2].st_info), STT_SECTION);
    CHECK_EQ_INT(syms[2].st_shndx, text);
    CHECK_EQ_INT(syms[3].st_shndx, ro);
    CHECK(strcmp(names + syms[4].st_name, "local_fn") == 0);
    CHECK_EQ_INT(syms[4].st_value, 12);
    CHECK(strcmp(names + syms[5].st_name, "main") == 0);
    CHECK_EQ_INT(ELF64_ST_TYPE(syms[5].st_info), STT_FUNC);
    CHECK_EQ_INT(syms[5].st_size, 13);
    CHECK(strcmp(names + syms[6].st_name, "puts") == 0);
    CHECK_EQ_INT(syms[6].st_shndx, SHN_UNDEF);

    const Elf64_Rela *rel = (const Elf64_Rela *)(b.data + rs->sh_offset);
    CHECK_EQ_INT(rel[0].r_offset, 3);
    CHECK_EQ_INT(ELF64_R_TYPE(rel[0].r_info), R_X86_64_PC32);
    CHECK_EQ_INT(ELF64_R_SYM(rel[0].r_info), 3); /* .rodata section symbol */
    CHECK_EQ_INT(rel[0].r_addend, -4);
    CHECK_EQ_INT(rel[1].r_offset, 8);
    CHECK_EQ_INT(ELF64_R_TYPE(rel[1].r_info), R_X86_64_PLT32);
    CHECK_EQ_INT(ELF64_R_SYM(rel[1].r_info), 6); /* puts */
    CHECK_EQ_INT(rel[1].r_addend, -4);

    buf_free(&b);
    obj_free(&o);
}

static void test_validation(void) {
    ObjFile o;
    Buf b = {0};
    fprintf(stderr, "[expected diagnostics follow]\n");
    build_sample(&o);
    o.relocs[0].offset = 11; /* 4-byte field would overrun 13-byte .text */
    CHECK(!elf_write_object(&o, &b));
    o.relocs[0].offset = 3;
    o.relocs[0].type = 99;
    CHECK(!elf_write_object(&o, &b));
    o.relocs[0].type = OBJ_R_X86_64_PC32;
    o.relocs[0].target_section = 7;
    CHECK(!elf_write_object(&o, &b));
    o.relocs[0].target_section = 1;
    o.relocs[1].symbol = 42;
    CHECK(!elf_write_object(&o, &b));
    o.relocs[1].symbol = 2;
    o.symbols[0].value = 1000; /* outside .text */
    CHECK(!elf_write_object(&o, &b));
    obj_free(&o);
    buf_free(&b);

    /* Empty object is still a valid ELF file. */
    memset(&o, 0, sizeof o);
    CHECK(elf_write_object(&o, &b));
    CHECK_EQ_INT(((Elf64_Ehdr *)b.data)->e_shnum, 4);
    buf_free(&b);
}

int main(void) {
    test_layout();
    test_validation();
    return check_report("test_elf");
}
