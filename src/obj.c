#include "obj.h"

#include <stdlib.h>
#include <string.h>

int obj_add_section(ObjFile *o, const char *name, uint32_t type, uint64_t flags, uint64_t align) {
    o->sections = xrealloc(o->sections, (size_t)(o->nsections + 1) * sizeof *o->sections);
    ObjSection *s = &o->sections[o->nsections];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    s->type = type;
    s->flags = flags;
    s->align = align ? align : 1;
    return o->nsections++;
}

int obj_find_section(const ObjFile *o, const char *name) {
    for (int i = 0; i < o->nsections; i++)
        if (strcmp(o->sections[i].name, name) == 0) return i;
    return -1;
}

int obj_find_symbol(const ObjFile *o, const char *name) {
    for (int i = 0; i < o->nsymbols; i++)
        if (strcmp(o->symbols[i].name, name) == 0) return i;
    return -1;
}

int obj_intern_symbol(ObjFile *o, const char *name) {
    int i = obj_find_symbol(o, name);
    if (i >= 0) return i;
    o->symbols = xrealloc(o->symbols, (size_t)(o->nsymbols + 1) * sizeof *o->symbols);
    ObjSymbol *s = &o->symbols[o->nsymbols];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    s->section = OBJ_UNDEF;
    s->local_label = strncmp(name, ".L", 2) == 0;
    return o->nsymbols++;
}

void obj_add_reloc(ObjFile *o, ObjReloc r) {
    o->relocs = xrealloc(o->relocs, (size_t)(o->nrelocs + 1) * sizeof *o->relocs);
    o->relocs[o->nrelocs++] = r;
}

void obj_free(ObjFile *o) {
    for (int i = 0; i < o->nsections; i++) {
        free(o->sections[i].name);
        buf_free(&o->sections[i].data);
    }
    for (int i = 0; i < o->nsymbols; i++) free(o->symbols[i].name);
    free(o->sections);
    free(o->symbols);
    free(o->relocs);
    free(o->source_name);
    memset(o, 0, sizeof *o);
}
