/* lexer.h - Luma tokenizer (Lox-style token set).
 *
 * Numbers: decimal integer literals only, 0 .. 2^62-1 (the fixnum range;
 * negative values come from unary minus). A literal with a fractional part
 * is rejected ("floating-point numbers are not supported yet").
 *
 * Strings: double-quoted, single line. Escapes: \"  \\  \n  \t. Any other
 * escape, a raw newline, or EOF inside a string is an error. (Unlike
 * reference Lox, which has no escapes and allows multi-line strings.)
 *
 * Comments: // to end of line. */
#ifndef LUMA_LEXER_H
#define LUMA_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    /* single-character */
    TOK_LEFT_PAREN, TOK_RIGHT_PAREN, TOK_LEFT_BRACE, TOK_RIGHT_BRACE,
    TOK_COMMA, TOK_DOT, TOK_MINUS, TOK_PLUS, TOK_SEMICOLON, TOK_SLASH, TOK_STAR,
    /* one or two characters */
    TOK_BANG, TOK_BANG_EQUAL, TOK_EQUAL, TOK_EQUAL_EQUAL,
    TOK_GREATER, TOK_GREATER_EQUAL, TOK_LESS, TOK_LESS_EQUAL,
    /* literals */
    TOK_IDENTIFIER, TOK_STRING, TOK_NUMBER,
    /* keywords */
    TOK_AND, TOK_CLASS, TOK_ELSE, TOK_FALSE, TOK_FOR, TOK_FUN, TOK_IF, TOK_NIL,
    TOK_OR, TOK_PRINT, TOK_RETURN, TOK_SUPER, TOK_THIS, TOK_TRUE, TOK_VAR, TOK_WHILE,
    TOK_EOF,
} TokenKind;

typedef struct {
    TokenKind kind;
    int line;           /* 1-based */
    int col;            /* 1-based, in bytes */
    const char *lexeme; /* points into the source buffer */
    size_t lexeme_len;
    char *value;        /* TOK_STRING: decoded bytes, NUL-terminated (owned) */
    size_t value_len;
    int64_t ival;       /* TOK_NUMBER */
} Token;

typedef struct {
    Token *items;
    size_t len;
    size_t cap;
} TokenList;

/* Tokenizes src (len bytes). On error prints "path:line:col: error: ..." to
 * stderr and returns false. The list always ends with TOK_EOF on success. */
bool lex(const char *path, const char *src, size_t len, TokenList *out);
void token_list_free(TokenList *t);
const char *token_kind_name(TokenKind k);
/* Prints one token per line, for `luma --dump-tokens`. */
void tokens_dump(const TokenList *t);

#endif
