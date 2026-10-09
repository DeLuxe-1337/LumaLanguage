/* parser.h - recursive-descent parser: tokens -> AST. Independent of codegen. */
#ifndef LUMA_PARSER_H
#define LUMA_PARSER_H

#include <stdbool.h>

#include "ast.h"
#include "lexer.h"

/* Parses tokens into *out. String values are moved out of the token list.
 * On error prints "path:line:col: error: ..." and returns false. */
bool parse(const char *path, TokenList *tokens, Program *out);

#endif
