/* Unit tests for the lexer, parser and AST->LIR lowering. Expected
 * diagnostics are printed to stderr by the code under test; test results go
 * to stdout. */
#include <stdlib.h>

#include "check.h"
#include "ir.h"
#include "lexer.h"
#include "lower.h"
#include "parser.h"

static bool lex_str(const char *src, TokenList *t) { return lex("t.luma", src, strlen(src), t); }

static void test_lex_hello(void) {
    TokenList t;
    CHECK(lex_str("print \"Hello, world!\";\n", &t));
    CHECK_EQ_INT(t.len, 4);
    CHECK_EQ_INT(t.items[0].kind, TOK_PRINT);
    CHECK_EQ_INT(t.items[1].kind, TOK_STRING);
    CHECK(strcmp(t.items[1].value, "Hello, world!") == 0);
    CHECK_EQ_INT(t.items[1].col, 7);
    CHECK_EQ_INT(t.items[2].kind, TOK_SEMICOLON);
    CHECK_EQ_INT(t.items[3].kind, TOK_EOF);
    CHECK_EQ_INT(t.items[3].line, 2);
    token_list_free(&t);
}

static void test_lex_tokens(void) {
    TokenList t;
    CHECK(lex_str("(){},.-+;/* ! != = == > >= < <= // comment ! \"x\n"
                  "foo _bar b9 and class else false for fun if nil or print return super this true var while 0 42",
                  &t));
    static const TokenKind want[] = {
        TOK_LEFT_PAREN, TOK_RIGHT_PAREN, TOK_LEFT_BRACE, TOK_RIGHT_BRACE, TOK_COMMA, TOK_DOT, TOK_MINUS,
        TOK_PLUS, TOK_SEMICOLON, TOK_SLASH, TOK_STAR, TOK_BANG, TOK_BANG_EQUAL, TOK_EQUAL, TOK_EQUAL_EQUAL,
        TOK_GREATER, TOK_GREATER_EQUAL, TOK_LESS, TOK_LESS_EQUAL, TOK_IDENTIFIER, TOK_IDENTIFIER,
        TOK_IDENTIFIER, TOK_AND, TOK_CLASS, TOK_ELSE, TOK_FALSE, TOK_FOR, TOK_FUN, TOK_IF, TOK_NIL, TOK_OR,
        TOK_PRINT, TOK_RETURN, TOK_SUPER, TOK_THIS, TOK_TRUE, TOK_VAR, TOK_WHILE, TOK_NUMBER, TOK_NUMBER,
        TOK_EOF,
    };
    CHECK_EQ_INT(t.len, sizeof want / sizeof *want);
    for (size_t i = 0; i < t.len && i < sizeof want / sizeof *want; i++) {
        check_count++;
        if (t.items[i].kind != want[i]) {
            check_failures++;
            printf("FAIL token %zu: %s, expected %s\n", i, token_kind_name(t.items[i].kind), token_kind_name(want[i]));
        }
    }
    CHECK_EQ_INT(t.items[t.len - 2].ival, 42);
    CHECK_EQ_INT(t.items[19].line, 2); /* comment consumed the rest of line 1 */
    token_list_free(&t);

    CHECK(lex_str("4611686018427387903", &t)); /* 2^62 - 1: largest fixnum */
    CHECK_EQ_INT(t.items[0].ival, 4611686018427387903LL);
    token_list_free(&t);
}

static void test_lex_escapes(void) {
    TokenList t;
    CHECK(lex_str("print \"a\\\"b\\\\c\\nd\\te\";", &t));
    CHECK_EQ_INT(t.items[1].value_len, 9);
    CHECK(memcmp(t.items[1].value, "a\"b\\c\nd\te", 9) == 0);
    token_list_free(&t);
}

static void test_lex_errors(void) {
    TokenList t;
    fprintf(stderr, "[expected diagnostics follow]\n");
    CHECK(!lex_str("print \"Hello;", &t));      /* unterminated at EOF */
    CHECK(!lex_str("print \"Hello;\nx\";", &t)); /* unterminated at newline */
    CHECK(!lex_str("print \"abc\\", &t));       /* backslash at EOF */
    CHECK(!lex_str("print \"a\\q\";", &t));     /* unsupported escape */
    CHECK(!lex_str("print 'a';", &t));          /* single quotes */
    CHECK(!lex_str("print 1 % 2;", &t));        /* no modulo operator in Lox */
    CHECK(!lex_str("print 1.5;", &t));          /* floats not supported yet */
    CHECK(!lex_str("print 4611686018427387904;", &t)); /* 2^62: too large */
    CHECK(!lex_str("print 99999999999999999999999;", &t));
    CHECK(!lex_str("print 12abc;", &t));
}

static bool parse_str(const char *src, Program *p) {
    TokenList t;
    if (!lex_str(src, &t)) return false;
    bool ok = parse("t.luma", &t, p);
    token_list_free(&t);
    return ok;
}

static void test_parse(void) {
    Program p;
    CHECK(parse_str("print 1 + 2 * 3 - -4;", &p));
    CHECK_EQ_INT(p.len, 1);
    Expr *e = p.stmts[0]->expr; /* (- (+ 1 (* 2 3)) (- 4)) */
    CHECK(e->kind == EXPR_BINARY && e->op == TOK_MINUS);
    CHECK(e->left->kind == EXPR_BINARY && e->left->op == TOK_PLUS);
    CHECK(e->left->right->op == TOK_STAR);
    CHECK(e->right->kind == EXPR_UNARY && e->right->op == TOK_MINUS);
    program_free(&p);

    CHECK(parse_str("a = b = 3;", &p)); /* right associative */
    e = p.stmts[0]->expr;
    CHECK(e->kind == EXPR_ASSIGN && strcmp(e->name, "a") == 0 && e->right->kind == EXPR_ASSIGN);
    program_free(&p);

    CHECK(parse_str("print a or b and c;", &p)); /* and binds tighter */
    e = p.stmts[0]->expr;
    CHECK(e->kind == EXPR_LOGICAL && e->op == TOK_OR && e->right->op == TOK_AND);
    program_free(&p);

    CHECK(parse_str("for (var i = 0; i < 3; i = i + 1) print i;", &p)); /* desugared */
    Stmt *s = p.stmts[0];
    CHECK(s->kind == STMT_BLOCK && s->n == 2 && s->stmts[0]->kind == STMT_VAR && s->stmts[1]->kind == STMT_WHILE);
    CHECK(s->stmts[1]->body->kind == STMT_BLOCK && s->stmts[1]->body->n == 2);
    program_free(&p);

    CHECK(parse_str("for (;;) {}", &p)); /* empty clauses: condition is true */
    CHECK(p.stmts[0]->stmts[0]->kind == STMT_WHILE && p.stmts[0]->stmts[0]->expr->kind == EXPR_TRUE);
    program_free(&p);

    CHECK(parse_str("if (a) if (b) print 1; else print 2;", &p)); /* dangling else binds inner */
    CHECK(p.stmts[0]->else_branch == NULL && p.stmts[0]->then_branch->else_branch != NULL);
    program_free(&p);

    CHECK(parse_str("", &p));
    CHECK_EQ_INT(p.len, 0);
    program_free(&p);

    CHECK(parse_str("\"just a string\"; (1); nil;", &p)); /* expression statements */
    CHECK_EQ_INT(p.len, 3);
    program_free(&p);

    fprintf(stderr, "[expected diagnostics follow]\n");
    CHECK(!parse_str("print \"a\"", &p));     /* missing ; */
    CHECK(!parse_str("print ;", &p));         /* missing expression */
    CHECK(!parse_str("print", &p));
    CHECK(!parse_str("var = 1;", &p));
    CHECK(!parse_str("var x = ;", &p));
    CHECK(!parse_str("1 = 2;", &p));          /* invalid assignment target */
    CHECK(!parse_str("(a) = 2;", &p));
    CHECK(!parse_str("a + b = 2;", &p));
    CHECK(!parse_str("if a print 1;", &p));
    CHECK(!parse_str("while (true print 1;", &p));
    CHECK(!parse_str("{ print 1;", &p));      /* unterminated block */
    CHECK(!parse_str("}", &p));
    CHECK(!parse_str("fun f() {}", &p));      /* not supported yet */
    CHECK(!parse_str("return 1;", &p));
    CHECK(!parse_str("class A {}", &p));
    CHECK(!parse_str("print this;", &p));
    CHECK(!parse_str("print a.b;", &p));
    CHECK(!parse_str("for (var i = 0; i < 1) print i;", &p));

    /* nesting is bounded rather than overflowing the stack */
    size_t n = 5000;
    char *deep = malloc(n * 2 + 32);
    strcpy(deep, "print ");
    for (size_t i = 0; i < n; i++) strcat(deep, "(");
    strcat(deep, "1");
    for (size_t i = 0; i < n; i++) strcat(deep, ")");
    strcat(deep, ";");
    CHECK(!parse_str(deep, &p));
    free(deep);
    char *chain = malloc(n * 2 + 32);
    strcpy(chain, "print 1");
    for (size_t i = 0; i < n; i++) strcat(chain, "+1");
    strcat(chain, ";");
    CHECK(!parse_str(chain, &p));
    free(chain);
}

/* Lowers source to printed LIR (NULL on error). Caller frees. */
static char *lower_str(const char *src) {
    Program p;
    if (!parse_str(src, &p)) return NULL;
    p.source_path = "t.luma";
    IrModule m;
    bool ok = lower_program(&p, &m);
    program_free(&p);
    if (!ok) return NULL;
    Buf b = {0};
    ir_print_module(&m, &b);
    CHECK(ir_verify(&m, "t.luma"));
    ir_module_free(&m);
    buf_byte(&b, 0);
    return (char *)b.data;
}

static void expect_ir_contains(const char *src, const char *needle) {
    char *ir = lower_str(src);
    check_count++;
    if (!ir || !strstr(ir, needle)) {
        check_failures++;
        printf("FAIL: lowering of `%s` lacks `%s`; got:\n%s\n", src, needle, ir ? ir : "(error)");
    }
    free(ir);
}

static void test_lower(void) {
    expect_ir_contains("print \"hi\";", "data @s0 = str \"hi\"");
    expect_ir_contains("print \"hi\";", "%0 = const @s0\n  call @luma_print(%0)");
    expect_ir_contains("var x = 1; x = x + 1;", "%x = add %x, %");           /* hint: no extra mov */
    expect_ir_contains("var x = 1; var x = x + 1; print x;", "%x.1 = add %x, %"); /* top-level redeclare */
    expect_ir_contains("var x = 1; { var x = 2; print x; } print x;", "call @luma_print(%x.1)");
    expect_ir_contains("var x = 1; x = x + (x = 5);", "= mov %x\n");         /* left snapshot */
    expect_ir_contains("print 1 or 2;", "br %0, or.end.0, or.rhs.0");
    expect_ir_contains("print 1 and 2;", "br %0, and.rhs.0, and.end.0");
    expect_ir_contains("while (true) print 1;", "jmp while.cond.0");
    expect_ir_contains("if (1) print 1; else print 2;", "br %0, if.then.0, if.else.0");
    expect_ir_contains("print \"a\"; print \"a\";", "%1 = const @s0");         /* deduplicated data */
    expect_ir_contains("print -1;", "%1 = neg %0");
    expect_ir_contains("print !nil;", "%1 = not %0");
    /* blocks come out in source order: then before the outer else/end */
    expect_ir_contains("if (1) { if (2) print 3; } else print 4;",
                       "if.then.0:\n  %1 = const 2\n  br %1, if.then.1, if.end.1\nif.then.1:");

    fprintf(stderr, "[expected diagnostics follow]\n");
    char *ir;
    CHECK((ir = lower_str("print y;")) == NULL);                          /* undefined */
    CHECK((ir = lower_str("{ var a = 1; var a = 2; }")) == NULL);         /* redeclare in block */
    CHECK((ir = lower_str("{ var a = a; }")) == NULL);                    /* own initializer */
    CHECK((ir = lower_str("var a = 1; { var a = a; }")) == NULL);         /* even when shadowing */
    CHECK((ir = lower_str("var a = a;")) == NULL);                        /* top level, undefined */
    CHECK((ir = lower_str("{ var a = 1; } print a;")) == NULL);          /* out of scope */
    CHECK((ir = lower_str("x = 1;")) == NULL);                            /* assign undefined */
    ir = lower_str("var a = 1; var a = a + 1;");                          /* top level: allowed */
    CHECK(ir != NULL);
    free(ir);
}

int main(void) {
    test_lex_hello();
    test_lex_tokens();
    test_lex_escapes();
    test_lex_errors();
    test_parse();
    test_lower();
    return check_report("test_frontend");
}
