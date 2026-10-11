#include "prelude.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "util.h"

/* generated from the std files by the Makefile (build/obj/prelude_src.c) */
extern const char *const luma_std_names[]; /* "prelude", "math", ... (prelude first); NULL-terminated */
extern const char *const luma_std_srcs[];

static const char *const DISPLAY[] = {"<prelude>"}; /* the core file's name in messages */
static char display_buf[64][64];

const char *prelude_path(const char *path, int line) {
    if (line < LUMA_PRELUDE_LINE) return path;
    int k = line / LUMA_PRELUDE_LINE - 1;
    if (k == 0) return DISPLAY[0];
    if (k < 64 && luma_std_names[k]) {
        snprintf(display_buf[k], sizeof display_buf[k], "<std/%s.luma>", luma_std_names[k]);
        return display_buf[k];
    }
    return "<std>";
}

int prelude_line(int line) { return line >= LUMA_PRELUDE_LINE ? line % LUMA_PRELUDE_LINE : line; }

bool prelude_is_core(int line) { return line >= LUMA_PRELUDE_LINE && line < 2 * LUMA_PRELUDE_LINE; }

static void shift_stmt(Stmt *s, int by);

static void shift_type(TypeRef *t, int by) {
    t->line += by;
    for (size_t i = 0; i < t->nargs; i++) shift_type(&t->args[i], by);
}

static void shift_expr(Expr *e, int by) {
    if (!e) return;
    e->line += by;
    shift_expr(e->left, by);
    shift_expr(e->right, by);
    for (size_t i = 0; i < e->nargs; i++) shift_expr(e->args[i], by);
    shift_stmt(e->body, by);
}

static void shift_stmt(Stmt *s, int by) {
    if (!s) return;
    s->line += by;
    shift_expr(s->expr, by);
    shift_type(&s->type, by);
    for (size_t i = 0; i < s->nparams; i++) {
        s->param_line[i] += by;
        if (s->param_types) shift_type(&s->param_types[i], by);
    }
    for (size_t i = 0; i < s->n; i++) shift_stmt(s->stmts[i], by);
    shift_stmt(s->then_branch, by);
    shift_stmt(s->else_branch, by);
    shift_stmt(s->body, by);
}

/* Parses one file, appending its statements to out. */
static bool load_one(const char *path, const char *src, size_t len, int index, Program *out) {
    TokenList toks = {0};
    Program p = {0};
    bool ok = lex(path, src, len, &toks) && parse(path, &toks, &p);
    token_list_free(&toks);
    if (!ok) {
        program_free(&p);
        return false;
    }
    for (size_t i = 0; i < p.len; i++) {
        shift_stmt(p.stmts[i], LUMA_PRELUDE_LINE * (index + 1));
        if (out->len == out->cap) {
            out->cap = out->cap ? 2 * out->cap : 64;
            out->stmts = xrealloc(out->stmts, out->cap * sizeof *out->stmts);
        }
        out->stmts[out->len++] = p.stmts[i];
    }
    free(p.stmts);
    return true;
}

bool prelude_load(Program *out) {
    const char *dir = getenv("LUMA_STD"), *pre = getenv("LUMA_PRELUDE");
    for (int k = 0; luma_std_names[k]; k++) {
        char path[4096];
        const char *file = NULL;
        if (k == 0 && pre && *pre) file = pre;
        else if (dir && *dir) {
            snprintf(path, sizeof path, "%s/%s.luma", dir, luma_std_names[k]);
            file = path;
        }
        bool ok;
        if (file) {
            size_t len;
            char *src = read_file(file, &len);
            ok = src && load_one(file, src, len, k, out);
            free(src);
        } else {
            char name[64];
            snprintf(name, sizeof name, k == 0 ? "<prelude>" : "<std/%s.luma>", luma_std_names[k]);
            ok = load_one(name, luma_std_srcs[k], strlen(luma_std_srcs[k]), k, out);
        }
        if (!ok) return false;
    }
    return true;
}
