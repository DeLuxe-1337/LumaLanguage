/* ast.h - minimal Luma abstract syntax tree.
 *
 *   Program   := Stmt*
 *   Stmt      := PrintStmt
 *   PrintStmt := 'print' StringLit ';'
 *   Expr      := StringLit            (the only expression kind for now) */
#ifndef LUMA_AST_H
#define LUMA_AST_H

#include <stddef.h>

typedef enum { EXPR_STRING } ExprKind;

typedef struct {
    ExprKind kind;
    int line, col;
    char *str;      /* EXPR_STRING: decoded bytes (owned), NUL-terminated */
    size_t str_len; /* byte length, excluding the terminator */
} Expr;

typedef enum { STMT_PRINT } StmtKind;

typedef struct {
    StmtKind kind;
    int line, col;
    Expr value; /* STMT_PRINT: the value printed */
} Stmt;

typedef struct {
    const char *source_path;
    Stmt *stmts;
    size_t len;
    size_t cap;
} Program;

void program_free(Program *p);
/* Prints an S-expression view, for `luma --dump-ast`. */
void program_dump(const Program *p);

#endif
