/* ast.h - Luma abstract syntax tree (Lox-style subset, milestone 2).
 *
 *   program     → declaration* EOF
 *   declaration → varDecl | statement
 *   varDecl     → "var" IDENTIFIER ( "=" expression )? ";"
 *   statement   → exprStmt | forStmt | ifStmt | printStmt | whileStmt | block
 *   forStmt     → "for" "(" ( varDecl | exprStmt | ";" ) expression? ";" expression? ")" statement
 *                 (desugared by the parser into a block + while loop)
 *   ifStmt      → "if" "(" expression ")" statement ( "else" statement )?
 *   printStmt   → "print" expression ";"
 *   whileStmt   → "while" "(" expression ")" statement
 *   block       → "{" declaration* "}"
 *   expression  → assignment
 *   assignment  → IDENTIFIER "=" assignment | logic_or
 *   logic_or    → logic_and ( "or" logic_and )*
 *   logic_and   → equality ( "and" equality )*
 *   equality    → comparison ( ( "!=" | "==" ) comparison )*
 *   comparison  → term ( ( ">" | ">=" | "<" | "<=" ) term )*
 *   term        → factor ( ( "-" | "+" ) factor )*
 *   factor      → unary ( ( "/" | "*" ) unary )*
 *   unary       → ( "!" | "-" ) unary | primary
 *   primary     → "true" | "false" | "nil" | NUMBER | STRING | "(" expression ")" | IDENTIFIER */
#ifndef LUMA_AST_H
#define LUMA_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lexer.h"

typedef enum {
    EXPR_INT,
    EXPR_STRING,
    EXPR_NIL,
    EXPR_TRUE,
    EXPR_FALSE,
    EXPR_VAR,     /* name */
    EXPR_ASSIGN,  /* name = right */
    EXPR_UNARY,   /* op right            (op: TOK_MINUS, TOK_BANG) */
    EXPR_BINARY,  /* left op right       (arithmetic, comparison, equality) */
    EXPR_LOGICAL, /* left op right       (op: TOK_AND, TOK_OR; short-circuit) */
} ExprKind;

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int line, col;
    TokenKind op;
    int64_t ival;   /* EXPR_INT */
    char *str;      /* EXPR_STRING: decoded bytes (owned) */
    size_t str_len;
    char *name;     /* EXPR_VAR / EXPR_ASSIGN (owned) */
    Expr *left, *right;
};

typedef enum {
    STMT_PRINT, /* expr */
    STMT_EXPR,  /* expr */
    STMT_VAR,   /* name, expr (initializer, may be NULL) */
    STMT_BLOCK, /* stmts */
    STMT_IF,    /* expr, then_branch, else_branch (may be NULL) */
    STMT_WHILE, /* expr, body */
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    int line, col;
    Expr *expr;
    char *name;
    Stmt **stmts;
    size_t n, cap;
    Stmt *then_branch, *else_branch, *body;
};

typedef struct {
    const char *source_path;
    Stmt **stmts;
    size_t len;
    size_t cap;
} Program;

void program_free(Program *p);
void expr_free(Expr *e);
void stmt_free(Stmt *s);
/* Prints an S-expression view, for `luma --dump-ast`. */
void program_dump(const Program *p);

#endif
