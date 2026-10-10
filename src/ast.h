/* ast.h - Luma abstract syntax tree (milestone 4).
 *
 *   program     → declaration* EOF
 *   declaration → funDecl | externDecl | varDecl | statement
 *   funDecl     → "fun" IDENTIFIER "(" parameters? ")" ( ":" type )? block   (top level only, for now)
 *   parameters  → param ( "," param )*
 *   param       → IDENTIFIER ( ":" type )?
 *   externDecl  → "extern" "fun" IDENTIFIER "(" cparams? ")" ":" type ";"     (top level; C function)
 *   cparams     → IDENTIFIER ":" type ( "," IDENTIFIER ":" type )*
 *   type        → ( IDENTIFIER | "nil" ) "?"?
 *   varDecl     → "var" IDENTIFIER ( ":" type )? ( "=" expression )? ";"
 *   statement   → exprStmt | forStmt | ifStmt | returnStmt | whileStmt | block
 *   returnStmt  → "return" expression? ";"                          (inside functions only)
 *   forStmt     → "for" "(" ( varDecl | exprStmt | ";" ) expression? ";" expression? ")" statement
 *                 (desugared by the parser into a block + while loop)
 *   ifStmt      → "if" "(" expression ")" statement ( "else" statement )?
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
 *   unary       → ( "!" | "-" ) unary | call
 *   call        → primary ( "(" arguments? ")" )*
 *   arguments   → expression ( "," expression )*
 *   primary     → "true" | "false" | "nil" | NUMBER | STRING | "(" expression ")" | IDENTIFIER
 *
 * Types are optional and gradual (see docs/DESIGN.md): Luma types are int, str,
 * bool, nil and any; `T?` means T or nil. Extern declarations use C types
 * (i8..i64, u8..u64, bool, cstr, cstr?, ptr; void as a return type).
 *
 * Functions are not first-class values yet: a call's callee must be the name
 * of a function (or the builtin `print`), and a function name may only appear
 * as a callee. `print(a, b, ...)` writes its arguments separated by spaces,
 * followed by a newline. */
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
    EXPR_CALL,    /* name(args...) */
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
    Expr **args;    /* EXPR_CALL (owned) */
    size_t nargs;
};

/* A type annotation as written. name == NULL means "no annotation". */
typedef struct {
    char *name;
    bool nullable; /* written with a trailing '?' */
    int line, col;
} TypeRef;

typedef enum {
    STMT_EXPR,  /* expr */
    STMT_VAR,   /* name, expr (initializer, may be NULL) */
    STMT_BLOCK, /* stmts */
    STMT_IF,    /* expr, then_branch, else_branch (may be NULL) */
    STMT_WHILE, /* expr, body */
    STMT_FUN,   /* name, params, stmts (the body) */
    STMT_RETURN,/* expr (may be NULL: returns nil) */
    STMT_EXTERN,/* name, params, param_types, type (the C return type) */
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
    char **params;  /* STMT_FUN / STMT_EXTERN (owned) */
    int *param_line, *param_col;
    TypeRef *param_types; /* STMT_FUN / STMT_EXTERN: one per param */
    size_t nparams;
    TypeRef type;   /* STMT_VAR: declared type; STMT_FUN / STMT_EXTERN: return type */
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
