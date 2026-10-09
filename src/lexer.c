#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *path;
    const char *src;
    size_t len;
    size_t pos;
    int line;
    int col;
} Lexer;

static void lex_error(const Lexer *lx, int line, int col, const char *fmt, const char *arg) {
    fprintf(stderr, "%s:%d:%d: error: ", lx->path, line, col);
    fprintf(stderr, fmt, arg ? arg : "");
    fputc('\n', stderr);
}

static int peek(const Lexer *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }

static void advance(Lexer *lx) {
    if (lx->pos >= lx->len) return;
    if (lx->src[lx->pos] == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    lx->pos++;
}

static void push(TokenList *t, Token tok) {
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->items = xrealloc(t->items, t->cap * sizeof *t->items);
    }
    t->items[t->len++] = tok;
}

static bool is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static bool lex_string(Lexer *lx, Token *tok) {
    int sline = lx->line, scol = lx->col;
    size_t start = lx->pos;
    advance(lx); /* opening quote */
    Buf val = {0};
    for (;;) {
        int c = peek(lx);
        if (c == -1 || c == '\n' || c == '\r') {
            lex_error(lx, sline, scol, "unterminated string literal%s",
                      c == -1 ? " (reached end of file)" : " (strings may not span lines; use \\n)");
            buf_free(&val);
            return false;
        }
        if (c == '"') {
            advance(lx);
            break;
        }
        if (c == '\\') {
            int eline = lx->line, ecol = lx->col;
            advance(lx);
            int e = peek(lx);
            char out;
            switch (e) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case 'n': out = '\n'; break;
            case 't': out = '\t'; break;
            case -1:
                lex_error(lx, sline, scol, "unterminated string literal (reached end of file)", NULL);
                buf_free(&val);
                return false;
            default: {
                char esc[3] = {'\\', (char)e, 0};
                if (e < 0x20 || e >= 0x7f) strcpy(esc, "\\?");
                lex_error(lx, eline, ecol,
                          "unsupported escape sequence '%s' (supported: \\\" \\\\ \\n \\t)", esc);
                buf_free(&val);
                return false;
            }
            }
            advance(lx);
            buf_byte(&val, (uint8_t)out);
            continue;
        }
        if (c == 0) {
            lex_error(lx, lx->line, lx->col, "NUL byte inside string literal", NULL);
            buf_free(&val);
            return false;
        }
        buf_byte(&val, (uint8_t)c);
        advance(lx);
    }
    tok->kind = TOK_STRING;
    tok->line = sline;
    tok->col = scol;
    tok->lexeme = lx->src + start;
    tok->lexeme_len = lx->pos - start;
    tok->value_len = val.len;
    buf_byte(&val, 0);
    tok->value = (char *)val.data;
    return true;
}

bool lex(const char *path, const char *src, size_t len, TokenList *out) {
    Lexer lx = {path, src, len, 0, 1, 1};
    memset(out, 0, sizeof *out);
    for (;;) {
        int c = peek(&lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(&lx);
            continue;
        }
        Token tok = {0};
        tok.line = lx.line;
        tok.col = lx.col;
        tok.lexeme = src + lx.pos;
        if (c == -1) {
            tok.kind = TOK_EOF;
            tok.lexeme_len = 0;
            push(out, tok);
            return true;
        }
        if (c == ';') {
            tok.kind = TOK_SEMICOLON;
            tok.lexeme_len = 1;
            advance(&lx);
            push(out, tok);
            continue;
        }
        if (c == '"') {
            if (!lex_string(&lx, &tok)) goto fail;
            push(out, tok);
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = lx.pos;
            while (is_ident_char(peek(&lx))) advance(&lx);
            size_t n = lx.pos - start;
            if (n == 5 && memcmp(src + start, "print", 5) == 0) {
                tok.kind = TOK_PRINT;
                tok.lexeme_len = n;
                push(out, tok);
                continue;
            }
            char *word = xstrndup(src + start, n > 64 ? 64 : n);
            lex_error(&lx, tok.line, tok.col, "unknown identifier '%s' (only 'print' is supported)", word);
            free(word);
            goto fail;
        }
        {
            char shown[8];
            if (c >= 0x21 && c < 0x7f) snprintf(shown, sizeof shown, "%c", c);
            else snprintf(shown, sizeof shown, "\\x%02x", c);
            lex_error(&lx, tok.line, tok.col, "unexpected character '%s'", shown);
        }
        goto fail;
    }
fail:
    token_list_free(out);
    return false;
}

void token_list_free(TokenList *t) {
    for (size_t i = 0; i < t->len; i++) free(t->items[i].value);
    free(t->items);
    memset(t, 0, sizeof *t);
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
    case TOK_PRINT: return "PRINT";
    case TOK_STRING: return "STRING";
    case TOK_SEMICOLON: return "SEMICOLON";
    case TOK_EOF: return "EOF";
    }
    return "?";
}

void tokens_dump(const TokenList *t) {
    for (size_t i = 0; i < t->len; i++) {
        const Token *k = &t->items[i];
        printf("%d:%d\t%-10s\t%.*s\n", k->line, k->col, token_kind_name(k->kind), (int)k->lexeme_len,
               k->lexeme);
    }
}
