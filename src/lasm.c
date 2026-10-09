/* lasm.c - standalone front end for Luma's assembler + ELF writer.
 *
 *   lasm INPUT.s -o OUTPUT.o
 *
 * Lets the assembler be tested independently of the Luma compiler. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "elf_writer.h"
#include "util.h"

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "lasm: error: unknown option '%s'\n", argv[i]); return 2; }
        else if (!in) in = argv[i];
        else { fprintf(stderr, "lasm: error: multiple inputs\n"); return 2; }
    }
    if (!in || !out) {
        fprintf(stderr, "usage: lasm INPUT.s -o OUTPUT.o\n");
        return 2;
    }
    size_t len;
    char *src = read_file(in, &len);
    if (!src) return 1;
    ObjFile obj;
    int rc = 1;
    if (assemble(in, src, len, &obj)) {
        obj.source_name = xstrdup(in);
        if (elf_write_file(&obj, out)) rc = 0;
        obj_free(&obj);
    }
    free(src);
    return rc;
}
