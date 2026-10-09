/* parser.c - recursive-descent parser for the grammar in ast.h.
 * Stops at the first error. Nesting depth is bounded so malformed input
 * cannot overflow the C stack. */
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_DEPTH 1000

typedef struct {
    const char *path;
    TokenList *toks;
    size_t pos;
    int depth;
    bool failed;
} Parser;

static Token *cur(Parser *p) {
    /* The lexer guarantees a trailing TOK_EOF, so clamping is always valid. */
    return &p->toks->items[p->pos < p->toks->len ? p->pos : p->toks->len - 1];
}

static bool check(Parser *p, TokenKind k) { return cur(p)->kind == k; }

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}

static bool match(Parser *p, TokenKind k) {
    if (!check(p, k)) return false;
    advance(p);
    return true;
}

static void describe(const Token *t, char *buf, size_t n) {
    switch (t->kind) {
    case TOK_EOF: snprintf(buf, n, "end of file"); break;
    case TOK_STRING: snprintf(buf, n, "string literal"); break;
    case TOK_NUMBER: snprintf(buf, n, "number"); break;
    case TOK_IDENTIFIER: snprintf(buf, n, "identifier '%.*s'", (int)(t->lexeme_len > 40 ? 40 : t->lexeme_len), t->lexeme); break;
    default: snprintf(buf, n, "'%.*s'", (int)t->lexeme_len, t->lexeme); break;
    }
}

static void error_at(Parser *p, const Token *t, const char *msg) {
    if (p->failed) return;
    fprintf(stderr, "%s:%d:%d: error: %s\n", p->path, t->line, t->col, msg);
    p->failed = true;
}

static void expected(Parser *p, const char *what) {
    char found[80], msg[200];
    describe(cur(p), found, sizeof found);
    snprintf(msg, sizeof msg, "expected %s, found %s", what, found);
    error_at(p, cur(p), msg);
}

static bool consume(Parser *p, TokenKind k, const char *what) {
    if (match(p, k)) return true;
    expected(p, what);
    return false;
}

static bool enter(Parser *p) {
    if (++p->depth > MAX_DEPTH) {
        error_at(p, cur(p), "nesting too deep");
        return false;
    }
    return true;
}

static Expr *new_expr(ExprKind kind, const Token *at) {
    Expr *e = xcalloc(1, sizeof *e);
    e->kind = kind;
    e->line = at->line;
    e->col = at->col;
    return e;
}

static Stmt *new_stmt(StmtKind kind, const Token *at) {
    Stmt *s = xcalloc(1, sizeof *s);
    s->kind = kind;
    s->line = at->line;
    s->col = at->col;
    return s;
}

static void stmt_push(Stmt *block, Stmt *s) {
    if (block->n == block->cap) {
        block->cap = block->cap ? block->cap * 2 : 4;
        block->stmts = xrealloc(block->stmts, block->cap * sizeof *block->stmts);
    }
    block->stmts[block->n++] = s;
}

static const char *unsupported_keyword(TokenKind k) {
    switch (k) {
    case TOK_FUN: return "'fun' (functions) is not supported yet";
    case TOK_RETURN: return "'return' is not supported yet";
    case TOK_CLASS: return "'class' is not supported yet";
    case TOK_THIS: return "'this' is not supported yet";
    case TOK_SUPER: return "'super' is not supported yet";
    default: return NULL;
    }
}

/* ---- expressions ---- */

static Expr *expression(Parser *p);

static Expr *primary(Parser *p) {
    Token *t = cur(p);
    Expr *e;
    switch (t->kind) {
    case TOK_TRUE: advance(p); return new_expr(EXPR_TRUE, t);
    case TOK_FALSE: advance(p); return new_expr(EXPR_FALSE, t);
    case TOK_NIL: advance(p); return new_expr(EXPR_NIL, t);
    case TOK_NUMBER:
        advance(p);
        e = new_expr(EXPR_INT, t);
        e->ival = t->ival;
        return e;
    case TOK_STRING:
        advance(p);
        e = new_expr(EXPR_STRING, t);
        e->str = xstrndup(t->value, t->value_len);
        e->str_len = t->value_len;
        return e;
    case TOK_IDENTIFIER:
        advance(p);
        e = new_expr(EXPR_VAR, t);
        e->name = xstrndup(t->lexeme, t->lexeme_len);
        return e;
    case TOK_LEFT_PAREN: {
        advance(p);
        Expr *inner = expression(p);
        if (!inner) return NULL;
        if (!consume(p, TOK_RIGHT_PAREN, "')' after expression")) {
            expr_free(inner);
            return NULL;
        }
        /* A parenthesised expression is never an assignment target:
         * "(a) = 1" is rejected because the caller sees no EXPR_VAR token
         * directly before '='. We mark this by wrapping nothing; see assignment(). */
        return inner;
    }
    default: {
        const char *u = unsupported_keyword(t->kind);
        if (u) error_at(p, t, u);
        else expected(p, "expression");
        return NULL;
    }
    }
}

static Expr *unary(Parser *p) {
    if (check(p, TOK_BANG) || check(p, TOK_MINUS)) {
        if (!enter(p)) return NULL;
        Token *op = advance(p);
        Expr *right = unary(p);
        p->depth--;
        if (!right) return NULL;
        Expr *e = new_expr(EXPR_UNARY, op);
        e->op = op->kind;
        e->right = right;
        return e;
    }
    return primary(p);
}

typedef Expr *(*ParseFn)(Parser *);

static Expr *binary_level(Parser *p, ParseFn next, const TokenKind *ops, int nops, ExprKind kind) {
    Expr *left = next(p);
    if (!left) return NULL;
    int added = 0; /* each link of a left-deep chain deepens the tree that later passes recurse over */
    for (;;) {
        bool hit = false;
        for (int i = 0; i < nops; i++) hit |= check(p, ops[i]);
        if (!hit) {
            p->depth -= added;
            return left;
        }
        added++;
        if (!enter(p)) {
            p->depth -= added;
            expr_free(left);
            return NULL;
        }
        Token *op = advance(p);
        Expr *right = next(p);
        if (!right) {
            p->depth -= added;
            expr_free(left);
            return NULL;
        }
        Expr *e = new_expr(kind, op);
        e->op = op->kind;
        e->left = left;
        e->right = right;
        left = e;
    }
}

static Expr *factor(Parser *p) {
    static const TokenKind ops[] = {TOK_SLASH, TOK_STAR};
    return binary_level(p, unary, ops, 2, EXPR_BINARY);
}
static Expr *term(Parser *p) {
    static const TokenKind ops[] = {TOK_MINUS, TOK_PLUS};
    return binary_level(p, factor, ops, 2, EXPR_BINARY);
}
static Expr *comparison(Parser *p) {
    static const TokenKind ops[] = {TOK_GREATER, TOK_GREATER_EQUAL, TOK_LESS, TOK_LESS_EQUAL};
    return binary_level(p, term, ops, 4, EXPR_BINARY);
}
static Expr *equality(Parser *p) {
    static const TokenKind ops[] = {TOK_BANG_EQUAL, TOK_EQUAL_EQUAL};
    return binary_level(p, comparison, ops, 2, EXPR_BINARY);
}
static Expr *logic_and(Parser *p) {
    static const TokenKind ops[] = {TOK_AND};
    return binary_level(p, equality, ops, 1, EXPR_LOGICAL);
}
static Expr *logic_or(Parser *p) {
    static const TokenKind ops[] = {TOK_OR};
    return binary_level(p, logic_and, ops, 1, EXPR_LOGICAL);
}

static Expr *assignment(Parser *p) {
    if (!enter(p)) return NULL;
    /* Only a bare identifier immediately followed by '=' is a valid target. */
    bool bare_ident = check(p, TOK_IDENTIFIER) && p->pos + 1 < p->toks->len &&
                      p->toks->items[p->pos + 1].kind == TOK_EQUAL;
    Expr *e = logic_or(p);
    if (e && check(p, TOK_EQUAL)) {
        Token *eq = advance(p);
        if (!bare_ident || e->kind != EXPR_VAR) {
            error_at(p, eq, "invalid assignment target");
            expr_free(e);
            p->depth--;
            return NULL;
        }
        Expr *value = assignment(p);
        if (!value) {
            expr_free(e);
            p->depth--;
            return NULL;
        }
        Expr *a = new_expr(EXPR_ASSIGN, eq);
        a->line = e->line;
        a->col = e->col;
        a->name = e->name;
        e->name = NULL;
        expr_free(e);
        a->right = value;
        e = a;
    }
    p->depth--;
    return e;
}

static Expr *expression(Parser *p) { return assignment(p); }

/* ---- statements ---- */

static Stmt *declaration(Parser *p);
static Stmt *statement(Parser *p);

static Stmt *var_declaration(Parser *p, const Token *kw) {
    Token *name = cur(p);
    if (!consume(p, TOK_IDENTIFIER, "variable name after 'var'")) return NULL;
    Stmt *s = new_stmt(STMT_VAR, kw);
    s->name = xstrndup(name->lexeme, name->lexeme_len);
    s->line = name->line;
    s->col = name->col;
    if (match(p, TOK_EQUAL)) {
        s->expr = expression(p);
        if (!s->expr) { stmt_free(s); return NULL; }
    }
    if (!consume(p, TOK_SEMICOLON, "';' after variable declaration")) { stmt_free(s); return NULL; }
    return s;
}

static Stmt *expr_statement(Parser *p, StmtKind kind, const Token *at, const char *semi_what) {
    Expr *e = expression(p);
    if (!e) return NULL;
    if (!consume(p, TOK_SEMICOLON, semi_what)) {
        expr_free(e);
        return NULL;
    }
    Stmt *s = new_stmt(kind, at);
    s->expr = e;
    return s;
}

static Stmt *block_body(Parser *p, const Token *open) {
    Stmt *b = new_stmt(STMT_BLOCK, open);
    while (!check(p, TOK_RIGHT_BRACE) && !check(p, TOK_EOF)) {
        Stmt *s = declaration(p);
        if (!s) { stmt_free(b); return NULL; }
        stmt_push(b, s);
    }
    if (!consume(p, TOK_RIGHT_BRACE, "'}' after block")) { stmt_free(b); return NULL; }
    return b;
}

static Stmt *if_statement(Parser *p, const Token *kw) {
    if (!consume(p, TOK_LEFT_PAREN, "'(' after 'if'")) return NULL;
    Expr *cond = expression(p);
    if (!cond) return NULL;
    if (!consume(p, TOK_RIGHT_PAREN, "')' after if condition")) { expr_free(cond); return NULL; }
    Stmt *s = new_stmt(STMT_IF, kw);
    s->expr = cond;
    s->then_branch = statement(p);
    if (!s->then_branch) { stmt_free(s); return NULL; }
    if (match(p, TOK_ELSE)) {
        s->else_branch = statement(p);
        if (!s->else_branch) { stmt_free(s); return NULL; }
    }
    return s;
}

static Stmt *while_statement(Parser *p, const Token *kw) {
    if (!consume(p, TOK_LEFT_PAREN, "'(' after 'while'")) return NULL;
    Expr *cond = expression(p);
    if (!cond) return NULL;
    if (!consume(p, TOK_RIGHT_PAREN, "')' after while condition")) { expr_free(cond); return NULL; }
    Stmt *s = new_stmt(STMT_WHILE, kw);
    s->expr = cond;
    s->body = statement(p);
    if (!s->body) { stmt_free(s); return NULL; }
    return s;
}

/* for (init; cond; incr) body   ==>   { init; while (cond) { body; incr; } } */
static Stmt *for_statement(Parser *p, const Token *kw) {
    if (!consume(p, TOK_LEFT_PAREN, "'(' after 'for'")) return NULL;
    Stmt *init = NULL;
    Expr *cond = NULL, *incr = NULL;
    Stmt *body = NULL;
    if (match(p, TOK_SEMICOLON)) {
        /* no initializer */
    } else if (check(p, TOK_VAR)) {
        Token *v = advance(p);
        if (!(init = var_declaration(p, v))) goto fail;
    } else {
        if (!(init = expr_statement(p, STMT_EXPR, cur(p), "';' after loop initializer"))) goto fail;
    }
    if (!check(p, TOK_SEMICOLON) && !(cond = expression(p))) goto fail;
    if (!consume(p, TOK_SEMICOLON, "';' after loop condition")) goto fail;
    if (!check(p, TOK_RIGHT_PAREN) && !(incr = expression(p))) goto fail;
    if (!consume(p, TOK_RIGHT_PAREN, "')' after for clauses")) goto fail;
    if (!(body = statement(p))) goto fail;

    if (incr) {
        Stmt *b = new_stmt(STMT_BLOCK, kw);
        stmt_push(b, body);
        Stmt *is = new_stmt(STMT_EXPR, kw);
        is->line = incr->line;
        is->col = incr->col;
        is->expr = incr;
        incr = NULL;
        stmt_push(b, is);
        body = b;
    }
    if (!cond) cond = new_expr(EXPR_TRUE, kw);
    Stmt *loop = new_stmt(STMT_WHILE, kw);
    loop->expr = cond;
    loop->body = body;
    Stmt *outer = new_stmt(STMT_BLOCK, kw);
    if (init) stmt_push(outer, init);
    stmt_push(outer, loop);
    return outer;
fail:
    stmt_free(init);
    expr_free(cond);
    expr_free(incr);
    stmt_free(body);
    return NULL;
}

static Stmt *statement(Parser *p) {
    if (!enter(p)) return NULL;
    Token *t = cur(p);
    Stmt *s;
    switch (t->kind) {
    case TOK_PRINT: advance(p); s = expr_statement(p, STMT_PRINT, t, "';' after value"); break;
    case TOK_IF: advance(p); s = if_statement(p, t); break;
    case TOK_WHILE: advance(p); s = while_statement(p, t); break;
    case TOK_FOR: advance(p); s = for_statement(p, t); break;
    case TOK_LEFT_BRACE: advance(p); s = block_body(p, t); break;
    default: {
        const char *u = unsupported_keyword(t->kind);
        if (u) { error_at(p, t, u); s = NULL; break; }
        s = expr_statement(p, STMT_EXPR, t, "';' after expression");
        break;
    }
    }
    p->depth--;
    return s;
}

static Stmt *declaration(Parser *p) {
    if (check(p, TOK_VAR)) {
        Token *kw = advance(p);
        return var_declaration(p, kw);
    }
    return statement(p);
}

bool parse(const char *path, TokenList *tokens, Program *out) {
    memset(out, 0, sizeof *out);
    out->source_path = path;
    if (tokens->len == 0 || tokens->items[tokens->len - 1].kind != TOK_EOF) {
        fprintf(stderr, "%s: error: internal: token stream not terminated\n", path);
        return false;
    }
    Parser p = {path, tokens, 0, 0, false};
    while (!check(&p, TOK_EOF)) {
        Stmt *s = declaration(&p);
        if (!s) {
            if (!p.failed) fprintf(stderr, "%s: error: parse failed\n", path);
            program_free(out);
            return false;
        }
        if (out->len == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->stmts = xrealloc(out->stmts, out->cap * sizeof *out->stmts);
        }
        out->stmts[out->len++] = s;
    }
    return true;
}

/* ---- freeing and dumping ---- */

void expr_free(Expr *e) {
    if (!e) return;
    expr_free(e->left);
    expr_free(e->right);
    free(e->str);
    free(e->name);
    free(e);
}

void stmt_free(Stmt *s) {
    if (!s) return;
    expr_free(s->expr);
    free(s->name);
    for (size_t i = 0; i < s->n; i++) stmt_free(s->stmts[i]);
    free(s->stmts);
    stmt_free(s->then_branch);
    stmt_free(s->else_branch);
    stmt_free(s->body);
    free(s);
}

void program_free(Program *p) {
    for (size_t i = 0; i < p->len; i++) stmt_free(p->stmts[i]);
    free(p->stmts);
    p->stmts = NULL;
    p->len = p->cap = 0;
}

static const char *op_text(TokenKind k) {
    switch (k) {
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_BANG: return "!";
    case TOK_BANG_EQUAL: return "!=";
    case TOK_EQUAL_EQUAL: return "==";
    case TOK_LESS: return "<";
    case TOK_LESS_EQUAL: return "<=";
    case TOK_GREATER: return ">";
    case TOK_GREATER_EQUAL: return ">=";
    case TOK_AND: return "and";
    case TOK_OR: return "or";
    default: return "?";
    }
}

static void dump_string(const char *s, size_t n) {
    putchar('"');
    for (size_t j = 0; j < n; j++) {
        unsigned char c = (unsigned char)s[j];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
    putchar('"');
}

static void dump_expr(const Expr *e) {
    switch (e->kind) {
    case EXPR_INT: printf("%lld", (long long)e->ival); break;
    case EXPR_STRING: dump_string(e->str, e->str_len); break;
    case EXPR_NIL: printf("nil"); break;
    case EXPR_TRUE: printf("true"); break;
    case EXPR_FALSE: printf("false"); break;
    case EXPR_VAR: printf("%s", e->name); break;
    case EXPR_ASSIGN: printf("(= %s ", e->name); dump_expr(e->right); putchar(')'); break;
    case EXPR_UNARY: printf("(%s ", op_text(e->op)); dump_expr(e->right); putchar(')'); break;
    case EXPR_BINARY:
    case EXPR_LOGICAL:
        printf("(%s ", op_text(e->op));
        dump_expr(e->left);
        putchar(' ');
        dump_expr(e->right);
        putchar(')');
        break;
    }
}

static void dump_stmt(const Stmt *s, int indent) {
    printf("%*s", indent * 2, "");
    switch (s->kind) {
    case STMT_PRINT: printf("(print @%d:%d ", s->line, s->col); dump_expr(s->expr); printf(")\n"); break;
    case STMT_EXPR: printf("(expr "); dump_expr(s->expr); printf(")\n"); break;
    case STMT_VAR:
        printf("(var %s", s->name);
        if (s->expr) { putchar(' '); dump_expr(s->expr); }
        printf(")\n");
        break;
    case STMT_BLOCK:
        printf("(block\n");
        for (size_t i = 0; i < s->n; i++) dump_stmt(s->stmts[i], indent + 1);
        printf("%*s)\n", indent * 2, "");
        break;
    case STMT_IF:
        printf("(if ");
        dump_expr(s->expr);
        printf("\n");
        dump_stmt(s->then_branch, indent + 1);
        if (s->else_branch) dump_stmt(s->else_branch, indent + 1);
        printf("%*s)\n", indent * 2, "");
        break;
    case STMT_WHILE:
        printf("(while ");
        dump_expr(s->expr);
        printf("\n");
        dump_stmt(s->body, indent + 1);
        printf("%*s)\n", indent * 2, "");
        break;
    }
}

void program_dump(const Program *p) {
    printf("(program\n");
    for (size_t i = 0; i < p->len; i++) dump_stmt(p->stmts[i], 1);
    printf(")\n");
}
