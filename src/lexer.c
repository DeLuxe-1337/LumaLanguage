#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "value.h"

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

static int peek_at(const Lexer *lx, size_t k) {
    return lx->pos + k < lx->len ? (unsigned char)lx->src[lx->pos + k] : -1;
}
static int peek(const Lexer *lx) { return peek_at(lx, 0); }

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

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_char(int c) { return is_ident_start(c) || is_digit(c); }

static const struct {
    const char *word;
    TokenKind kind;
} KEYWORDS[] = {
    {"and", TOK_AND},       {"class", TOK_CLASS}, {"else", TOK_ELSE},     {"extern", TOK_EXTERN},
    {"false", TOK_FALSE},
    {"for", TOK_FOR},       {"fun", TOK_FUN},     {"if", TOK_IF},         {"impl", TOK_IMPL},    {"nil", TOK_NIL},
    {"or", TOK_OR},         {"return", TOK_RETURN}, {"struct", TOK_STRUCT}, {"super", TOK_SUPER},
    {"this", TOK_THIS},     {"true", TOK_TRUE},   {"var", TOK_VAR},       {"while", TOK_WHILE},
};

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

static bool lex_number(Lexer *lx, Token *tok) {
    size_t start = lx->pos;
    int64_t v = 0;
    bool too_big = false;
    while (is_digit(peek(lx))) {
        int d = peek(lx) - '0';
        if (v > (LUMA_FIXNUM_MAX - d) / 10) too_big = true;
        else v = v * 10 + d;
        advance(lx);
    }
    if (peek(lx) == '.' && is_digit(peek_at(lx, 1))) {
        lex_error(lx, tok->line, tok->col, "floating-point numbers are not supported yet%s", NULL);
        return false;
    }
    if (is_ident_start(peek(lx))) {
        lex_error(lx, tok->line, tok->col, "invalid number literal%s", NULL);
        return false;
    }
    if (too_big) {
        lex_error(lx, tok->line, tok->col, "integer literal too large (maximum is 4611686018427387903)%s", NULL);
        return false;
    }
    tok->kind = TOK_NUMBER;
    tok->ival = v;
    tok->lexeme_len = lx->pos - start;
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
        if (c == '/' && peek_at(&lx, 1) == '/') {
            while (peek(&lx) != -1 && peek(&lx) != '\n') advance(&lx);
            continue;
        }
        Token tok = {0};
        tok.line = lx.line;
        tok.col = lx.col;
        tok.lexeme = src + lx.pos;
        tok.lexeme_len = 1;
        if (c == -1) {
            tok.kind = TOK_EOF;
            tok.lexeme_len = 0;
            push(out, tok);
            return true;
        }
        if (c == '"') {
            if (!lex_string(&lx, &tok)) goto fail;
            push(out, tok);
            continue;
        }
        if (is_digit(c)) {
            if (!lex_number(&lx, &tok)) goto fail;
            push(out, tok);
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = lx.pos;
            while (is_ident_char(peek(&lx))) advance(&lx);
            tok.lexeme_len = lx.pos - start;
            tok.kind = TOK_IDENTIFIER;
            for (size_t k = 0; k < sizeof KEYWORDS / sizeof *KEYWORDS; k++)
                if (strlen(KEYWORDS[k].word) == tok.lexeme_len && memcmp(KEYWORDS[k].word, tok.lexeme, tok.lexeme_len) == 0)
                    tok.kind = KEYWORDS[k].kind;
            push(out, tok);
            continue;
        }
        int n = peek_at(&lx, 1);
        bool two = false;
        switch (c) {
        case '(': tok.kind = TOK_LEFT_PAREN; break;
        case ')': tok.kind = TOK_RIGHT_PAREN; break;
        case '{': tok.kind = TOK_LEFT_BRACE; break;
        case '}': tok.kind = TOK_RIGHT_BRACE; break;
        case ',': tok.kind = TOK_COMMA; break;
        case '.': tok.kind = TOK_DOT; break;
        case '-': tok.kind = TOK_MINUS; break;
        case '+': tok.kind = TOK_PLUS; break;
        case ';': tok.kind = TOK_SEMICOLON; break;
        case '/': tok.kind = TOK_SLASH; break;
        case '*': tok.kind = TOK_STAR; break;
        case ':': two = n == ':'; tok.kind = two ? TOK_COLON_COLON : TOK_COLON; break;
        case '?': tok.kind = TOK_QUESTION; break;
        case '!': two = n == '='; tok.kind = two ? TOK_BANG_EQUAL : TOK_BANG; break;
        case '=': two = n == '='; tok.kind = two ? TOK_EQUAL_EQUAL : TOK_EQUAL; break;
        case '<': two = n == '='; tok.kind = two ? TOK_LESS_EQUAL : TOK_LESS; break;
        case '>': two = n == '='; tok.kind = two ? TOK_GREATER_EQUAL : TOK_GREATER; break;
        default: {
            char shown[8];
            if (c >= 0x21 && c < 0x7f) snprintf(shown, sizeof shown, "%c", c);
            else snprintf(shown, sizeof shown, "\\x%02x", c);
            lex_error(&lx, tok.line, tok.col, "unexpected character '%s'", shown);
            goto fail;
        }
        }
        advance(&lx);
        if (two) {
            advance(&lx);
            tok.lexeme_len = 2;
        }
        push(out, tok);
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
    static const char *const names[] = {
        "LEFT_PAREN", "RIGHT_PAREN", "LEFT_BRACE", "RIGHT_BRACE", "COMMA", "DOT", "MINUS", "PLUS",
        "SEMICOLON", "SLASH", "STAR", "COLON", "QUESTION", "COLON_COLON", "BANG", "BANG_EQUAL", "EQUAL", "EQUAL_EQUAL", "GREATER",
        "GREATER_EQUAL", "LESS", "LESS_EQUAL", "IDENTIFIER", "STRING", "NUMBER", "AND", "CLASS",
        "ELSE", "EXTERN", "FALSE", "FOR", "FUN", "IF", "IMPL", "NIL", "OR", "RETURN", "STRUCT", "SUPER", "THIS", "TRUE",
        "VAR", "WHILE", "EOF",
    };
    return (unsigned)k < sizeof names / sizeof *names ? names[k] : "?";
}

void tokens_dump(const TokenList *t) {
    for (size_t i = 0; i < t->len; i++) {
        const Token *k = &t->items[i];
        printf("%d:%d\t%-14s\t%.*s\n", k->line, k->col, token_kind_name(k->kind), (int)k->lexeme_len, k->lexeme);
    }
}
