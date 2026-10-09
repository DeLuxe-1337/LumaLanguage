#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *path;
    TokenList *toks;
    size_t pos;
} Parser;

static Token *cur(Parser *p) {
    /* The lexer guarantees a trailing TOK_EOF, so clamping is always valid. */
    return &p->toks->items[p->pos < p->toks->len ? p->pos : p->toks->len - 1];
}

static void describe(const Token *t, char *buf, size_t n) {
    switch (t->kind) {
    case TOK_EOF: snprintf(buf, n, "end of file"); break;
    case TOK_STRING: snprintf(buf, n, "string literal"); break;
    case TOK_PRINT: snprintf(buf, n, "'print'"); break;
    case TOK_SEMICOLON: snprintf(buf, n, "';'"); break;
    }
}

static void parse_error(Parser *p, const Token *t, const char *expected) {
    char found[32];
    describe(t, found, sizeof found);
    fprintf(stderr, "%s:%d:%d: error: expected %s, found %s\n", p->path, t->line, t->col, expected, found);
}

static bool parse_print(Parser *p, Stmt *out) {
    Token *kw = cur(p);
    p->pos++;
    Token *s = cur(p);
    if (s->kind != TOK_STRING) {
        parse_error(p, s, "string literal after 'print'");
        return false;
    }
    p->pos++;
    Token *semi = cur(p);
    if (semi->kind != TOK_SEMICOLON) {
        parse_error(p, semi, "';' after print statement");
        return false;
    }
    p->pos++;
    out->kind = STMT_PRINT;
    out->line = kw->line;
    out->col = kw->col;
    out->value.kind = EXPR_STRING;
    out->value.line = s->line;
    out->value.col = s->col;
    out->value.str = s->value; /* take ownership */
    out->value.str_len = s->value_len;
    s->value = NULL;
    return true;
}

bool parse(const char *path, TokenList *tokens, Program *out) {
    memset(out, 0, sizeof *out);
    out->source_path = path;
    if (tokens->len == 0 || tokens->items[tokens->len - 1].kind != TOK_EOF) {
        fprintf(stderr, "%s: error: internal: token stream not terminated\n", path);
        return false;
    }
    Parser p = {path, tokens, 0};
    while (cur(&p)->kind != TOK_EOF) {
        Token *t = cur(&p);
        if (t->kind != TOK_PRINT) {
            parse_error(&p, t, "statement ('print')");
            program_free(out);
            return false;
        }
        Stmt st;
        if (!parse_print(&p, &st)) {
            program_free(out);
            return false;
        }
        if (out->len == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->stmts = xrealloc(out->stmts, out->cap * sizeof *out->stmts);
        }
        out->stmts[out->len++] = st;
    }
    return true;
}

void program_free(Program *p) {
    for (size_t i = 0; i < p->len; i++) free(p->stmts[i].value.str);
    free(p->stmts);
    p->stmts = NULL;
    p->len = p->cap = 0;
}

void program_dump(const Program *p) {
    printf("(program\n");
    for (size_t i = 0; i < p->len; i++) {
        const Stmt *s = &p->stmts[i];
        printf("  (print @%d:%d (string \"", s->line, s->col);
        for (size_t j = 0; j < s->value.str_len; j++) {
            unsigned char c = (unsigned char)s->value.str[j];
            if (c == '"' || c == '\\') printf("\\%c", c);
            else if (c == '\n') printf("\\n");
            else if (c == '\t') printf("\\t");
            else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
            else putchar(c);
        }
        printf("\"))\n");
    }
    printf(")\n");
}
