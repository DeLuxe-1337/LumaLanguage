/* lexer.h - Luma tokenizer.
 *
 * Milestone 1 token set: the `print` keyword, double-quoted string literals,
 * `;`, and end of file. Whitespace (space, tab, CR, LF) separates tokens.
 *
 * String escapes supported: \"  \\  \n  \t
 * Any other escape (including \0, \xNN, \u{...}) is rejected with an error.
 * A raw newline inside a string literal is an error ("unterminated string"). */
#ifndef LUMA_LEXER_H
#define LUMA_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOK_PRINT,
    TOK_STRING,
    TOK_SEMICOLON,
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
