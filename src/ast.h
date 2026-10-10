/* ast.h - Luma abstract syntax tree (milestone 6).
 *
 *   program     → declaration* EOF
 *   declaration → funDecl | externDecl | structDecl | implDecl | varDecl | statement
 *   funDecl     → "fun" IDENTIFIER "(" parameters? ")" ( ":" type )? block   (top level only, for now)
 *   structDecl  → "struct" IDENTIFIER "{" ( field ( "," field )* ","? )? "}"   (top level)
 *   field       → IDENTIFIER ( ":" type )?
 *   implDecl    → "impl" IDENTIFIER "{" funDecl* "}"                      (top level; a function
 *                 whose first parameter is `self` is a method, any other is an associated function)
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
 *   assignment  → ( IDENTIFIER | call "." IDENTIFIER ) "=" assignment | logic_or
 *   logic_or    → logic_and ( "or" logic_and )*
 *   logic_and   → equality ( "and" equality )*
 *   equality    → comparison ( ( "!=" | "==" ) comparison )*
 *   comparison  → term ( ( ">" | ">=" | "<" | "<=" ) term )*
 *   term        → factor ( ( "-" | "+" ) factor )*
 *   factor      → unary ( ( "/" | "*" ) unary )*
 *   unary       → ( "!" | "-" ) unary | call
 *   call        → primary ( "(" arguments? ")" | "." IDENTIFIER ( "(" arguments? ")" )? )*
 *   arguments   → expression ( "," expression )*
 *   primary     → "true" | "false" | "nil" | NUMBER | STRING | "(" expression ")" | IDENTIFIER
 *               | IDENTIFIER "::" IDENTIFIER "(" arguments? ")"            (associated function call)
 *               | IDENTIFIER "{" ( init ( "," init )* ","? )? "}"         (struct literal)
 *   init        → IDENTIFIER ( ":" expression )?                          (`x` is short for `x: x`)
 *
 * Types are optional and gradual (see docs/DESIGN.md): Luma types are int, str,
 * bool, nil and any; `T?` means T or nil. Extern declarations use C types
 * (i8..i64, u8..u64, bool, cstr, cstr?, ptr; void as a return type).
 *
 * Functions are not first-class values yet: a call's callee must be the name
 * of a function (or the builtin `print`), and a function name may only appear
 * as a callee. `print(a, b, ...)` writes its arguments separated by spaces,
 * followed by a newline.
 *
 * Structs are heap objects with reference semantics: `Point { x: 1, y: 2 }`
 * creates one, `p.x` reads a field, `p.x = v` writes it, `p.m(args)` calls a
 * method from the struct's `impl` (with `self` = p) and `Point::f(args)` calls
 * any function of the impl. */
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
    EXPR_GET,     /* left.name */
    EXPR_SET,     /* left.name = right */
    EXPR_METHOD,  /* left.name(args...) */
    EXPR_ASSOC,   /* name::str(args...)   (name = the struct, str = the function) */
    EXPR_STRUCT,  /* name { fields[i]: args[i], ... } */
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
    Expr **args;    /* EXPR_CALL / EXPR_METHOD / EXPR_ASSOC / EXPR_STRUCT (owned) */
    size_t nargs;
    char **fields;  /* EXPR_STRUCT: field name per argument (owned) */
    int *field_line, *field_col;
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
    STMT_STRUCT,/* name, params (field names), param_types (field types) */
    STMT_IMPL,  /* name (the struct), stmts (STMT_FUN: methods and associated functions) */
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
    char **params;  /* STMT_FUN / STMT_EXTERN / STMT_STRUCT (owned) */
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
