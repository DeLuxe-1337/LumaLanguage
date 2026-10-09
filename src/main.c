/* main.c - the `luma` compiler driver.
 *
 *   luma INPUT.luma [-o OUT] [-S | -c] [--dump-tokens] [--dump-ast] [-v]
 *
 * Stages (each leaves an inspectable file next to OUT):
 *   1. lex + parse INPUT            -> AST (in memory; --dump-ast prints it)
 *   2. codegen                      -> OUT.s   (stop here with -S)
 *   3. Luma assembler reads OUT.s   -> ObjFile
 *   4. Luma ELF writer              -> OUT.o   (stop here with -c)
 *   5. system linker: cc -o OUT OUT.o   (override with $LUMA_CC)
 *
 * No external assembler is ever invoked: `cc` receives only an object file,
 * so it runs only the linker (collect2/ld) with the C runtime start files. */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "asm.h"
#include "codegen.h"
#include "elf_writer.h"
#include "lexer.h"
#include "parser.h"
#include "util.h"

extern char **environ;

static void usage(FILE *f) {
    fprintf(f,
            "usage: luma INPUT.luma [-o OUT] [-S | -c] [--dump-tokens] [--dump-ast] [-v]\n"
            "  -o OUT          output executable (default: a.out); intermediates are OUT.s and OUT.o\n"
            "  -S              stop after writing OUT.s\n"
            "  -c              stop after writing OUT.o\n"
            "  --dump-tokens   print the token stream\n"
            "  --dump-ast      print the AST\n"
            "  -v              print each stage and the link command\n"
            "environment: LUMA_CC  linker driver to use (default: cc)\n");
}

static char *with_ext(const char *base, const char *ext) {
    size_t n = strlen(base), m = strlen(ext);
    char *s = xmalloc(n + m + 1);
    memcpy(s, base, n);
    memcpy(s + n, ext, m + 1);
    return s;
}

static int run_linker(const char *obj, const char *out, bool verbose) {
    const char *cc = getenv("LUMA_CC");
    if (!cc || !*cc) cc = "cc";
    char *argv[] = {(char *)cc, "-o", (char *)out, (char *)obj, NULL};
    if (verbose) fprintf(stderr, "luma: link: %s -o %s %s\n", cc, out, obj);
    pid_t pid;
    int err = posix_spawnp(&pid, cc, NULL, NULL, argv, environ);
    if (err != 0) {
        fprintf(stderr, "luma: error: cannot run linker '%s': %s\n", cc, strerror(err));
        return 1;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "luma: error: waitpid: %s\n", strerror(errno));
            return 1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "luma: error: linker '%s' failed\n", cc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *input = NULL, *out = "a.out";
    bool stop_s = false, stop_c = false, dump_tokens = false, dump_ast = false, verbose = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "luma: error: -o requires an argument\n"); return 2; }
            out = argv[i];
        } else if (strcmp(a, "-S") == 0) stop_s = true;
        else if (strcmp(a, "-c") == 0) stop_c = true;
        else if (strcmp(a, "--dump-tokens") == 0) dump_tokens = true;
        else if (strcmp(a, "--dump-ast") == 0) dump_ast = true;
        else if (strcmp(a, "-v") == 0) verbose = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "luma: error: unknown option '%s'\n", a); usage(stderr); return 2; }
        else if (input) { fprintf(stderr, "luma: error: multiple input files\n"); return 2; }
        else input = a;
    }
    if (!input) { usage(stderr); return 2; }
    if (!*out) { fprintf(stderr, "luma: error: empty output name\n"); return 2; }

    int rc = 1;
    char *asm_path = with_ext(out, ".s");
    char *obj_path = with_ext(out, ".o");
    size_t src_len = 0, asm_len = 0;
    char *src = NULL, *asm_text = NULL;
    TokenList toks = {0};
    Program prog = {0};
    Buf asm_buf = {0};
    ObjFile obj = {0};

    /* 1. front end */
    if (!(src = read_file(input, &src_len))) goto done;
    if (memchr(src, '\0', src_len)) { fprintf(stderr, "%s: error: source contains NUL bytes\n", input); goto done; }
    if (!lex(input, src, src_len, &toks)) goto done;
    if (dump_tokens) tokens_dump(&toks);
    if (!parse(input, &toks, &prog)) goto done;
    if (dump_ast) program_dump(&prog);
    if (verbose) fprintf(stderr, "luma: parsed %zu statement(s) from %s\n", prog.len, input);

    /* 2. code generation */
    codegen_program(&prog, &asm_buf);
    if (!write_file(asm_path, asm_buf.data, asm_buf.len)) goto done;
    if (verbose) fprintf(stderr, "luma: wrote assembly %s (%zu bytes)\n", asm_path, asm_buf.len);
    if (stop_s) { rc = 0; goto done; }

    /* 3. assemble: read the .s file back from disk so the object is built
     *    from exactly the inspectable artifact. */
    if (!(asm_text = read_file(asm_path, &asm_len))) goto done;
    if (!assemble(asm_path, asm_text, asm_len, &obj)) goto done;
    obj.source_name = xstrdup(input);

    /* 4. ELF object */
    if (!elf_write_file(&obj, obj_path)) goto done;
    if (verbose) fprintf(stderr, "luma: wrote ELF64 object %s\n", obj_path);
    if (stop_c) { rc = 0; goto done; }

    /* 5. link */
    rc = run_linker(obj_path, out, verbose);
    if (rc == 0 && verbose) fprintf(stderr, "luma: wrote executable %s\n", out);

done:
    free(src);
    free(asm_text);
    token_list_free(&toks);
    program_free(&prog);
    buf_free(&asm_buf);
    obj_free(&obj);
    free(asm_path);
    free(obj_path);
    return rc;
}
