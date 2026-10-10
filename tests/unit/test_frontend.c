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
    CHECK(lex_str("print(\"Hello, world!\");\n", &t));
    CHECK_EQ_INT(t.len, 6);
    CHECK_EQ_INT(t.items[0].kind, TOK_IDENTIFIER); /* print is a builtin function, not a keyword */
    CHECK_EQ_INT(t.items[1].kind, TOK_LEFT_PAREN);
    CHECK_EQ_INT(t.items[2].kind, TOK_STRING);
    CHECK(strcmp(t.items[2].value, "Hello, world!") == 0);
    CHECK_EQ_INT(t.items[2].col, 7);
    CHECK_EQ_INT(t.items[3].kind, TOK_RIGHT_PAREN);
    CHECK_EQ_INT(t.items[4].kind, TOK_SEMICOLON);
    CHECK_EQ_INT(t.items[5].kind, TOK_EOF);
    CHECK_EQ_INT(t.items[5].line, 2);
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
        TOK_IDENTIFIER, TOK_RETURN, TOK_SUPER, TOK_THIS, TOK_TRUE, TOK_VAR, TOK_WHILE, TOK_NUMBER, TOK_NUMBER,
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
    CHECK(lex_str("\"a\\\"b\\\\c\\nd\\te\";", &t));
    CHECK_EQ_INT(t.items[0].value_len, 9);
    CHECK(memcmp(t.items[0].value, "a\"b\\c\nd\te", 9) == 0);
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
    CHECK(parse_str("1 + 2 * 3 - -4;", &p));
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

    CHECK(parse_str("a or b and c;", &p)); /* and binds tighter */
    e = p.stmts[0]->expr;
    CHECK(e->kind == EXPR_LOGICAL && e->op == TOK_OR && e->right->op == TOK_AND);
    program_free(&p);

    /* structs and impls */
    CHECK(parse_str("struct P { x: int, y, } impl P { fun new(x) { return P { x, y: 1 }; } fun m(self, d) {} }", &p));
    CHECK_EQ_INT(p.len, 2);
    CHECK(p.stmts[0]->kind == STMT_STRUCT && p.stmts[0]->nparams == 2 && strcmp(p.stmts[0]->param_types[0].name, "int") == 0 &&
          p.stmts[0]->param_types[1].name == NULL);
    CHECK(p.stmts[1]->kind == STMT_IMPL && p.stmts[1]->n == 2 && p.stmts[1]->stmts[1]->nparams == 2);
    e = p.stmts[1]->stmts[0]->stmts[0]->expr; /* P { x, y: 1 }: x is short for x: x */
    CHECK(e->kind == EXPR_STRUCT && e->nargs == 2 && strcmp(e->fields[0], "x") == 0 && e->args[0]->kind == EXPR_VAR &&
          strcmp(e->fields[1], "y") == 0 && e->args[1]->kind == EXPR_INT);
    program_free(&p);
    CHECK(parse_str("a.b.c = P::new(1).m(2).d;", &p));
    e = p.stmts[0]->expr; /* (set (get a b) c (get (callm (call P::new 1) m 2) d)) */
    CHECK(e->kind == EXPR_SET && strcmp(e->name, "c") == 0 && e->left->kind == EXPR_GET);
    CHECK(e->right->kind == EXPR_GET && e->right->left->kind == EXPR_METHOD && e->right->left->left->kind == EXPR_ASSOC &&
          strcmp(e->right->left->left->str, "new") == 0);
    program_free(&p);
    CHECK(parse_str("if (a) { b; }", &p)); /* NAME '{' is a block here, not a struct literal */
    CHECK(p.stmts[0]->kind == STMT_IF && p.stmts[0]->then_branch->kind == STMT_BLOCK);
    program_free(&p);

    CHECK(parse_str("for (var i = 0; i < 3; i = i + 1) print(i);", &p)); /* desugared */
    Stmt *s = p.stmts[0];
    CHECK(s->kind == STMT_BLOCK && s->n == 2 && s->stmts[0]->kind == STMT_VAR && s->stmts[1]->kind == STMT_WHILE);
    CHECK(s->stmts[1]->body->kind == STMT_BLOCK && s->stmts[1]->body->n == 2);
    program_free(&p);

    CHECK(parse_str("for (;;) {}", &p)); /* empty clauses: condition is true */
    CHECK(p.stmts[0]->stmts[0]->kind == STMT_WHILE && p.stmts[0]->stmts[0]->expr->kind == EXPR_TRUE);
    program_free(&p);

    CHECK(parse_str("if (a) if (b) print(1); else print(2);", &p)); /* dangling else binds inner */
    CHECK(p.stmts[0]->else_branch == NULL && p.stmts[0]->then_branch->else_branch != NULL);
    program_free(&p);

    CHECK(parse_str("", &p));
    CHECK_EQ_INT(p.len, 0);
    program_free(&p);

    CHECK(parse_str("\"just a string\"; (1); nil;", &p)); /* expression statements */
    CHECK_EQ_INT(p.len, 3);
    program_free(&p);

    CHECK(parse_str("print(1, \"a\", f(2, g()));", &p)); /* calls, nested */
    e = p.stmts[0]->expr;
    CHECK(e->kind == EXPR_CALL && strcmp(e->name, "print") == 0 && e->nargs == 3);
    CHECK(e->args[2]->kind == EXPR_CALL && e->args[2]->nargs == 2 && e->args[2]->args[1]->nargs == 0);
    program_free(&p);

    CHECK(parse_str("fun add(a, b) { var c = a + b; return c; } fun none() { return; }", &p));
    s = p.stmts[0];
    CHECK(s->kind == STMT_FUN && strcmp(s->name, "add") == 0 && s->nparams == 2 && s->n == 2);
    CHECK(strcmp(s->params[1], "b") == 0 && s->stmts[1]->kind == STMT_RETURN && s->stmts[1]->expr);
    CHECK(p.stmts[1]->stmts[0]->kind == STMT_RETURN && p.stmts[1]->stmts[0]->expr == NULL);
    program_free(&p);

    CHECK(parse_str("print;", &p)); /* parses; rejected by lowering (print must be called) */
    program_free(&p);
    CHECK(parse_str("(f)(1);", &p)); /* a parenthesised name is still that name */
    CHECK(p.stmts[0]->expr->kind == EXPR_CALL && strcmp(p.stmts[0]->expr->name, "f") == 0);
    program_free(&p);

    fprintf(stderr, "[expected diagnostics follow]\n");
    CHECK(!parse_str("print(\"a\")", &p));   /* missing ; */
    CHECK(!parse_str("print(;", &p));         /* missing expression */
    CHECK(!parse_str("print", &p));
    CHECK(!parse_str("print \"old\";", &p));  /* old statement syntax */
    CHECK(!parse_str("print 1;", &p));
    CHECK(!parse_str("f(1,);", &p));
    CHECK(!parse_str("f(1;", &p));
    CHECK(!parse_str("f()();", &p));         /* not first-class yet */
    CHECK(!parse_str("\"s\"();", &p));
    CHECK(!parse_str("fun f(a,) {}", &p));
    CHECK(!parse_str("fun (a) {}", &p));
    CHECK(!parse_str("fun f(1) {}", &p));
    CHECK(!parse_str("fun f() print(1);", &p));
    CHECK(!parse_str("{ fun f() {} }", &p));  /* only at top level */
    CHECK(!parse_str("fun f() { fun g() {} }", &p));
    CHECK(!parse_str("{ return; }", &p));     /* return outside a function */
    CHECK(!parse_str("fun f() { return 1 }", &p));
    CHECK(!parse_str("var = 1;", &p));
    CHECK(!parse_str("var x = ;", &p));
    CHECK(!parse_str("1 = 2;", &p));          /* invalid assignment target */
    CHECK(!parse_str("(a) = 2;", &p));
    CHECK(!parse_str("a + b = 2;", &p));
    CHECK(!parse_str("if a print 1;", &p));
    CHECK(!parse_str("while (true print 1;", &p));
    CHECK(!parse_str("{ print 1;", &p));      /* unterminated block */
    CHECK(!parse_str("}", &p));
    CHECK(!parse_str("return 1;", &p));
    CHECK(!parse_str("class A {}", &p));
    CHECK(!parse_str("this;", &p));
    CHECK(!parse_str("a.;", &p));
    CHECK(!parse_str("struct P { x: }", &p));
    CHECK(!parse_str("struct P { x y }", &p));
    CHECK(!parse_str("var p = P { x: 1;", &p));
    CHECK(!parse_str("a.b() = 1;", &p));      /* a call result is not a target */
    CHECK(!parse_str("(a.b) = 1;", &p));
    CHECK(!parse_str("impl P { var x; }", &p));
    CHECK(!parse_str("{ struct P {} }", &p));  /* only at top level */
    CHECK(!parse_str("fun f() { impl P {} }", &p));
    CHECK(!parse_str("P::;", &p));
    CHECK(!parse_str("P::f;", &p));            /* associated functions must be called */
    CHECK(!parse_str("p.f()();", &p));
    CHECK(!parse_str("for (var i = 0; i < 1) print(i);", &p));

    /* nesting is bounded rather than overflowing the stack */
    size_t n = 5000;
    char *deep = malloc(n * 2 + 32);
    strcpy(deep, "var x = ");
    for (size_t i = 0; i < n; i++) strcat(deep, "(");
    strcat(deep, "1");
    for (size_t i = 0; i < n; i++) strcat(deep, ")");
    strcat(deep, ";");
    CHECK(!parse_str(deep, &p));
    free(deep);
    char *chain = malloc(n * 2 + 32);
    strcpy(chain, "var x = 1");
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
    expect_ir_contains("print(\"hi\");", "data @s0 = str \"hi\"");
    expect_ir_contains("print(\"hi\");", "%0 = const @s0\n  call @luma_write(%0)\n  call @luma_write_newline()\n");
    expect_ir_contains("print(1, 2);", "call @luma_write(%0)\n  call @luma_write_space()\n  call @luma_write(%1)\n");
    expect_ir_contains("print();", "entry:\n  call @luma_write_newline()\n");
    expect_ir_contains("var v = print();", "%0 = const nil\n  store @var.v, %0"); /* print returns nil */
    expect_ir_contains("{ var x = 1; x = x + 1; }", "%x = add %x, %");          /* hint: no extra mov */
    expect_ir_contains("{ var x = 1; { var x = 2; print(x); } print(x); }", "call @luma_write(%x.1)");
    expect_ir_contains("{ var x = 1; x = x + (x = 5); }", "= mov %x\n");        /* left snapshot */
    expect_ir_contains("print(1 or 2);", "br %0, or.end.0, or.rhs.0");
    expect_ir_contains("print(1 and 2);", "br %0, and.rhs.0, and.end.0");
    expect_ir_contains("while (true) print(1);", "jmp while.cond.0");
    expect_ir_contains("if (1) print(1); else print(2);", "br %0, if.then.0, if.else.0");
    expect_ir_contains("print(\"a\"); print(\"a\");", "%1 = const @s0");         /* deduplicated data */
    expect_ir_contains("print(-1);", "%1 = neg %0");
    expect_ir_contains("print(!nil);", "%1 = not %0");
    /* blocks come out in source order: then before the outer else/end */
    expect_ir_contains("if (1) { if (2) print(3); } else print(4);",
                       "if.then.0:\n  %1 = const 2\n  br %1, if.then.1, if.end.1\nif.then.1:");
    /* top-level variables are module globals */
    expect_ir_contains("var g = 1; print(g);", "global @var.g\n");
    expect_ir_contains("var g = 1; print(g);", "%0 = const 1\n  store @var.g, %0\n  %1 = load @var.g\n");
    expect_ir_contains("var g = 1; var g = g + 1;", "load @var.g"); /* redeclaration reuses the slot */
    /* functions */
    expect_ir_contains("fun id(a) { return a; }", "fn @fn.id(%a) {\nentry:\n  ret %a\n}");
    expect_ir_contains("fun f() {}", "fn @fn.f() {\nentry:\n  %0 = const nil\n  ret %0\n}");
    expect_ir_contains("fun f(a) { return a; } print(f(1));", "%1 = call @fn.f(%0)");
    expect_ir_contains("fun f() {} f();", "  call @fn.f()\n");                      /* result discarded */
    expect_ir_contains("print(g()); fun g() { return 7; }", "call @fn.g()");        /* hoisted */
    expect_ir_contains("var n = 0; fun inc() { n = n + 1; }", "fn @fn.inc() {\nentry:\n  %0 = load @var.n");
    expect_ir_contains("fun f(a) { var a2 = a; { var a = 1; return a; } }", "%a.1 = const 1");

    /* gradual typing */
    expect_ir_contains("fun f(a: int): int { return a; } print(f(1));", "%1 = call @fn.f(%0)"); /* fits: no guard */
    expect_ir_contains("fun u(x) { return x; } fun f(a: int) {} f(u(1));",
                       "%1 = check %1, int, @s0\n  call @fn.f(%1)");                 /* any -> int: guarded */
    expect_ir_contains("fun u(x) { return x; } fun f(a: int) {} f(u(1));", "data @s0 = str \"argument 'a' of 'f'\"");
    expect_ir_contains("fun u(x) { return x; } var n: int = u(1);",
                       "%1 = check %1, int, @s0\n  store @var.n, %1");
    expect_ir_contains("fun u(x) { return x; } var n: int = u(1);", "global @var.n: int\n");
    expect_ir_contains("fun f(a: int, b, c: str?): bool { return true; }", "fn @fn.f(%a: int, %b, %c: str?): bool {");
    expect_ir_contains("fun u(x) { return x; } var n: int = u(1) + 1;", "%3 = add %1, %2\n  store @var.n, %3"); /* '+' with int -> int: no guard */
    expect_ir_contains("fun f(): int { for (;;) { return 1; } }", "fn @fn.f(): int {\nentry:\n  jmp while.cond.0\n");
    char *g = lower_str("var n: int = 1; n = 2;");
    CHECK(g && strstr(g, "luma_check_type") == NULL); /* fully typed: no guards at all */
    free(g);
    g = lower_str("var s: str? = nil; var b: bool = 1 < 2; var a: any = s;");
    CHECK(g && strstr(g, "luma_check_type") == NULL);
    free(g);
    /* extern fun */
    expect_ir_contains("extern fun puts(s: cstr): i32; puts(\"x\");", "extern c fn @puts(s: cstr): i32\n");
    expect_ir_contains("extern fun puts(s: cstr): i32; puts(\"x\");", "  call @puts(%0)\n");
    expect_ir_contains("extern fun getenv(n: cstr): cstr?; var h: str? = getenv(\"H\");", "store @var.h, %1");

    fprintf(stderr, "[expected diagnostics follow]\n");
    char *ir;
    CHECK((ir = lower_str("print(y);")) == NULL);                         /* undefined */
    CHECK((ir = lower_str("var n: int = \"x\";")) == NULL);
    CHECK((ir = lower_str("var n: int;")) == NULL);
    CHECK((ir = lower_str("fun f(a: str) {} f(1);")) == NULL);
    CHECK((ir = lower_str("fun f(): int {}")) == NULL);
    CHECK((ir = lower_str("fun f(): int { return nil; }")) == NULL);
    CHECK((ir = lower_str("fun f(x): str { if (x) return \"a\"; }")) == NULL);
    CHECK((ir = lower_str("var x: u8 = 1;")) == NULL);
    CHECK((ir = lower_str("var x: number = 1;")) == NULL);
    CHECK((ir = lower_str("print(1 + \"a\");")) == NULL);
    CHECK((ir = lower_str("print(\"a\" - \"b\");")) == NULL);
    CHECK((ir = lower_str("print(-true);")) == NULL);
    CHECK((ir = lower_str("print(nil < 1);")) == NULL);
    CHECK((ir = lower_str("var b: bool = true; var i: int = b;")) == NULL);
    CHECK((ir = lower_str("var a: int = 1; var a = 2;")) == NULL);      /* redeclared with another type */
    CHECK((ir = lower_str("extern fun f(x: int): void;")) == NULL);
    CHECK((ir = lower_str("extern fun f(x: f64): void;")) == NULL);
    CHECK((ir = lower_str("extern fun f(): i64; f(1);")) == NULL);      /* arity */
    CHECK((ir = lower_str("extern fun f(s: cstr): void; f(1);")) == NULL);
    CHECK((ir = lower_str("extern fun f(): void; extern fun f(): void;")) == NULL);
    CHECK((ir = lower_str("extern fun print(): void;")) == NULL);
    CHECK((ir = lower_str("extern fun luma_x(): void;")) == NULL);
    CHECK((ir = lower_str("extern fun f(a: i8, b: i8, c: i8, d: i8, e: i8, g: i8, h: i8): void;")) == NULL);
    ir = lower_str("print(1 + 2, \"a\" + \"b\");");                  /* well-typed literals are fine */
    CHECK(ir != NULL);
    free(ir);
    CHECK((ir = lower_str("print;")) == NULL);                            /* builtin not called */
    CHECK((ir = lower_str("fun f(a) {} f();")) == NULL);                  /* arity */
    CHECK((ir = lower_str("fun f() {} var x = f;")) == NULL);             /* not first-class */
    CHECK((ir = lower_str("var f = 1; f();")) == NULL);                   /* not a function */
    CHECK((ir = lower_str("g();")) == NULL);                              /* undefined function */
    CHECK((ir = lower_str("fun f() {} fun f() {}")) == NULL);
    CHECK((ir = lower_str("fun print() {}")) == NULL);
    CHECK((ir = lower_str("var print = 1;")) == NULL);
    CHECK((ir = lower_str("fun f(a, a) {}")) == NULL);
    CHECK((ir = lower_str("fun f(a) { var a = 1; }")) == NULL);           /* params share the body scope */
    CHECK((ir = lower_str("fun f(a, b, c, d, e, g, h) {}")) == NULL);     /* > 6 params */
    CHECK((ir = lower_str("fun f() {} var f;")) == NULL);
    CHECK((ir = lower_str("fun f() {} f = 1;")) == NULL);
    CHECK((ir = lower_str("print(later); var later = 1;")) == NULL);      /* top level: not yet declared */
    ir = lower_str("fun f() { return later; } var later = 1;");           /* function bodies: fine */
    CHECK(ir != NULL);
    free(ir);
    ir = lower_str("fun f() { return 1; print(2); }");                    /* dead code is pruned */
    CHECK(ir != NULL && strstr(ir, "dead") == NULL && strstr(ir, "call @luma_write") == NULL);
    free(ir);
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
