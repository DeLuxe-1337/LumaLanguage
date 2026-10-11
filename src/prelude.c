#include "prelude.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "util.h"

extern const char luma_prelude_src[]; /* generated from std/prelude.luma (Makefile) */

const char *prelude_path(const char *path, int line) { return line >= LUMA_PRELUDE_LINE ? "<prelude>" : path; }
int prelude_line(int line) { return line >= LUMA_PRELUDE_LINE ? line - LUMA_PRELUDE_LINE : line; }

static void shift_stmt(Stmt *s);

static void shift_type(TypeRef *t) {
    t->line += LUMA_PRELUDE_LINE;
    for (size_t i = 0; i < t->nargs; i++) shift_type(&t->args[i]);
}

static void shift_expr(Expr *e) {
    if (!e) return;
    e->line += LUMA_PRELUDE_LINE;
    shift_expr(e->left);
    shift_expr(e->right);
    for (size_t i = 0; i < e->nargs; i++) shift_expr(e->args[i]);
    shift_stmt(e->body);
}

static void shift_stmt(Stmt *s) {
    if (!s) return;
    s->line += LUMA_PRELUDE_LINE;
    shift_expr(s->expr);
    shift_type(&s->type);
    for (size_t i = 0; i < s->nparams; i++) {
        s->param_line[i] += LUMA_PRELUDE_LINE;
        if (s->param_types) shift_type(&s->param_types[i]);
    }
    for (size_t i = 0; i < s->n; i++) shift_stmt(s->stmts[i]);
    shift_stmt(s->then_branch);
    shift_stmt(s->else_branch);
    shift_stmt(s->body);
}

bool prelude_load(Program *out) {
    const char *path = getenv("LUMA_PRELUDE");
    char *owned = NULL;
    size_t len;
    const char *src = luma_prelude_src;
    if (path && *path) {
        if (!(owned = read_file(path, &len))) return false;
        src = owned;
    } else {
        path = "<prelude>";
        len = strlen(src);
    }
    TokenList toks = {0};
    bool ok = lex(path, src, len, &toks) && parse(path, &toks, out);
    token_list_free(&toks);
    free(owned);
    if (!ok) return false;
    for (size_t i = 0; i < out->len; i++) shift_stmt(out->stmts[i]);
    return true;
}
