/* main.c - the `luma` compiler driver.
 *
 *   luma INPUT.(luma|lir) [-o OUT] [--emit-ir | -S | -c]
 *        [--dump-tokens] [--dump-ast] [--dump-ir] [-v]
 *        [-l LIB] [-L DIR] [EXTRA.o | EXTRA.a | EXTRA.so ...]
 *
 * Stages (each leaves an inspectable file next to OUT):
 *   1. lex + parse INPUT.luma        -> AST          (--dump-tokens, --dump-ast)
 *   2. lower                         -> LIR          -> OUT.lir  (stop: --emit-ir)
 *      (or: parse INPUT.lir directly, skipping 1-2)
 *   3. verify LIR
 *   4. x86-64 instruction selection  -> OUT.s        (stop: -S)
 *   5. Luma assembler reads OUT.s    -> ObjFile
 *   6. Luma ELF writer               -> OUT.o        (stop: -c)
 *   7. system linker: cc -o OUT OUT.o [EXTRA...] libluma_rt.a [-LDIR...] [-lLIB...]
 *      ($LUMA_CC, $LUMA_RUNTIME). -l/-L and extra objects are how `extern fun`
 *      declarations are resolved against C libraries.
 *
 * No external assembler is ever invoked for Luma code: `cc` receives only
 * object files and an archive, so it runs only the linker. */
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asm.h"
#include "elf_writer.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "opt.h"
#include "parser.h"
#include "util.h"
#include "x86_gen.h"
#include "x86_isel.h"

extern char **environ;

static void usage(FILE *f) {
    fprintf(f,
            "usage: luma INPUT.luma|INPUT.lir [-o OUT] [--emit-ir | -S | -c] [options]\n"
            "  -o OUT          output executable (default: a.out);\n"
            "                  intermediates are OUT.lir, OUT.s and OUT.o\n"
            "  --emit-ir       stop after writing OUT.lir\n"
            "  -S              stop after writing OUT.s\n"
            "  -c              stop after writing OUT.o\n"
            "  --dump-tokens   print the token stream\n"
            "  --dump-ast      print the AST\n"
            "  --dump-ir       print the LIR module\n"
            "  --dump-opt-ir   print the optimized LIR module\n"
            "  -O0, -O1, -O2   optimization level (default -O2; see docs/DESIGN.md)\n"
            "  -v              print each stage and the link command\n"
            "  -l LIB, -L DIR  link with libLIB / search DIR (for extern fun)\n"
            "  EXTRA.o/.a/.so  additional objects or libraries passed to the linker\n"
            "environment:\n"
            "  LUMA_CC         linker driver to use (default: cc)\n"
            "  LUMA_RUNTIME    path to libluma_rt.a (default: next to the luma binary)\n");
}

static char *with_ext(const char *base, const char *ext) {
    size_t n = strlen(base), m = strlen(ext);
    char *s = xmalloc(n + m + 1);
    memcpy(s, base, n);
    memcpy(s + n, ext, m + 1);
    return s;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* $LUMA_RUNTIME, else libluma_rt.a in the directory containing this binary. */
static char *runtime_path(void) {
    const char *env = getenv("LUMA_RUNTIME");
    if (env && *env) return xstrdup(env);
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return xstrdup("libluma_rt.a");
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash) slash[1] = '\0';
    return with_ext(exe, "libluma_rt.a");
}

typedef struct {
    char **items;
    int n, cap;
} StrList;

static void strlist_push(StrList *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, (size_t)l->cap * sizeof *l->items);
    }
    l->items[l->n++] = s;
}

static void strlist_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
}

static int run_linker(const char *obj, const char *out, const StrList *extra, const StrList *libs, bool verbose) {
    const char *cc = getenv("LUMA_CC");
    if (!cc || !*cc) cc = "cc";
    char *rt = runtime_path();
    if (access(rt, R_OK) != 0) {
        fprintf(stderr, "luma: error: runtime library '%s' not found (build it with `make`, or set LUMA_RUNTIME)\n", rt);
        free(rt);
        return 1;
    }
    /* cc -o OUT OUT.o EXTRA... RUNTIME -L.../-l...  (libraries after the objects that use them) */
    int n = 0;
    char **argv = xmalloc((size_t)(6 + extra->n + libs->n) * sizeof *argv);
    argv[n++] = (char *)cc;
    argv[n++] = "-o";
    argv[n++] = (char *)out;
    argv[n++] = (char *)obj;
    for (int i = 0; i < extra->n; i++) argv[n++] = extra->items[i];
    argv[n++] = rt;
    for (int i = 0; i < libs->n; i++) argv[n++] = libs->items[i];
    argv[n] = NULL;
    if (verbose) {
        fprintf(stderr, "luma: link:");
        for (int i = 0; i < n; i++) fprintf(stderr, " %s", argv[i]);
        fputc('\n', stderr);
    }
    pid_t pid;
    int err = posix_spawnp(&pid, cc, NULL, NULL, argv, environ);
    free(argv);
    free(rt);
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
    bool stop_ir = false, stop_s = false, stop_c = false;
    bool dump_tokens = false, dump_ast = false, dump_ir = false, dump_opt_ir = false, verbose = false;
    int opt_level = 2;
    StrList extra = {0}, libs = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '-' && (a[1] == 'l' || a[1] == 'L'))) {
            const char *v = a[2] ? a + 2 : NULL;
            if (!v) {
                if (++i >= argc) { fprintf(stderr, "luma: error: %s requires an argument\n", a); return 2; }
                v = argv[i];
            }
            if (!*v || v[0] == '-') { fprintf(stderr, "luma: error: invalid argument for %.2s: '%s'\n", a, v); return 2; }
            char *opt = xmalloc(strlen(v) + 3);
            opt[0] = '-';
            opt[1] = a[1];
            strcpy(opt + 2, v);
            strlist_push(&libs, opt);
        } else if (a[0] != '-' && (ends_with(a, ".o") || ends_with(a, ".a") || ends_with(a, ".so"))) {
            strlist_push(&extra, xstrdup(a));
        } else if (strcmp(a, "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "luma: error: -o requires an argument\n"); return 2; }
            out = argv[i];
        } else if (strcmp(a, "--emit-ir") == 0) stop_ir = true;
        else if (strcmp(a, "-S") == 0) stop_s = true;
        else if (strcmp(a, "-c") == 0) stop_c = true;
        else if (strcmp(a, "--dump-tokens") == 0) dump_tokens = true;
        else if (strcmp(a, "--dump-ast") == 0) dump_ast = true;
        else if (strcmp(a, "--dump-ir") == 0) dump_ir = true;
        else if (strcmp(a, "--dump-opt-ir") == 0) dump_opt_ir = true;
        else if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '2' && !a[3]) opt_level = a[2] - '0';
        else if (strcmp(a, "-v") == 0) verbose = true;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "luma: error: unknown option '%s'\n", a); usage(stderr); return 2; }
        else if (input) { fprintf(stderr, "luma: error: multiple input files\n"); return 2; }
        else input = a;
    }
    if (!input) { usage(stderr); return 2; }
    if (!*out) { fprintf(stderr, "luma: error: empty output name\n"); return 2; }
    bool from_ir = ends_with(input, ".lir");

    int rc = 1;
    char *ir_path = with_ext(out, ".lir");
    char *opt_path = with_ext(out, ".opt.lir");
    Buf opt_buf = {0};
    char *asm_path = with_ext(out, ".s");
    char *obj_path = with_ext(out, ".o");
    size_t src_len = 0, asm_len = 0;
    char *src = NULL, *asm_text = NULL;
    TokenList toks = {0};
    Program prog = {0};
    IrModule ir = {0};
    bool have_ir = false;
    Buf ir_buf = {0}, asm_buf = {0};
    ObjFile obj = {0};

    if (!(src = read_file(input, &src_len))) goto done;
    if (memchr(src, '\0', src_len)) { fprintf(stderr, "%s: error: source contains NUL bytes\n", input); goto done; }

    if (from_ir) {
        /* 1-2'. hand-written (or previously emitted) IR */
        if (!ir_parse(input, src, src_len, &ir)) goto done;
        have_ir = true;
    } else {
        /* 1. front end */
        if (!lex(input, src, src_len, &toks)) goto done;
        if (dump_tokens) tokens_dump(&toks);
        if (!parse(input, &toks, &prog)) goto done;
        if (dump_ast) program_dump(&prog);
        if (verbose) fprintf(stderr, "luma: parsed %zu top-level statement(s) from %s\n", prog.len, input);
        /* 2. lowering */
        if (!lower_program(&prog, &ir)) goto done;
        have_ir = true;
    }
    ir_print_module(&ir, &ir_buf);
    if (dump_ir) fwrite(ir_buf.data, 1, ir_buf.len, stdout);
    if (!from_ir) {
        if (!write_file(ir_path, ir_buf.data, ir_buf.len)) goto done;
        if (verbose) fprintf(stderr, "luma: wrote IR %s\n", ir_path);
    }
    /* 3. verify (always, including IR we produced ourselves) */
    /* (line numbers refer to INPUT: .lir lines, or .luma lines for lowered IR) */
    if (!ir_verify(&ir, input)) goto done;

    /* 3b. optimize (-O2): the result is written to OUT.opt.lir and must verify */
    if (opt_level >= 2) {
        opt_module(&ir, opt_level);
        ir_print_module(&ir, &opt_buf);
        if (dump_opt_ir) fwrite(opt_buf.data, 1, opt_buf.len, stdout);
        if (!write_file(opt_path, opt_buf.data, opt_buf.len)) goto done;
        if (verbose) fprintf(stderr, "luma: wrote optimized IR %s\n", opt_path);
        if (!ir_verify(&ir, opt_path)) {
            fprintf(stderr, "luma: internal error: the optimizer produced invalid IR (%s); "
                            "-O1 or -O0 avoids the optimizer\n", opt_path);
            goto done;
        }
    }
    if (stop_ir) {
        if (from_ir) { fprintf(stderr, "luma: error: --emit-ir needs a .luma input\n"); goto done; }
        rc = 0;
        goto done;
    }

    /* 4. instruction selection */
    if (opt_level >= 1) x86_gen_module(&ir, &asm_buf);
    else x86_emit_module(&ir, &asm_buf);
    if (!write_file(asm_path, asm_buf.data, asm_buf.len)) goto done;
    if (verbose) fprintf(stderr, "luma: wrote assembly %s (%zu bytes)\n", asm_path, asm_buf.len);
    if (stop_s) { rc = 0; goto done; }

    /* 5. assemble: read the .s file back from disk so the object is built
     *    from exactly the inspectable artifact. */
    if (!(asm_text = read_file(asm_path, &asm_len))) goto done;
    if (!assemble(asm_path, asm_text, asm_len, &obj)) goto done;
    obj.source_name = xstrdup(input);

    /* 6. ELF object */
    if (!elf_write_file(&obj, obj_path)) goto done;
    if (verbose) fprintf(stderr, "luma: wrote ELF64 object %s\n", obj_path);
    if (stop_c) { rc = 0; goto done; }

    /* 7. link */
    rc = run_linker(obj_path, out, &extra, &libs, verbose);
    if (rc == 0 && verbose) fprintf(stderr, "luma: wrote executable %s\n", out);

done:
    free(src);
    free(asm_text);
    token_list_free(&toks);
    program_free(&prog);
    if (have_ir) ir_module_free(&ir);
    buf_free(&ir_buf);
    buf_free(&asm_buf);
    obj_free(&obj);
    free(ir_path);
    free(opt_path);
    buf_free(&opt_buf);
    free(asm_path);
    free(obj_path);
    strlist_free(&extra);
    strlist_free(&libs);
    return rc;
}
