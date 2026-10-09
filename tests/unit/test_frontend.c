/* Unit tests for the lexer, parser and code generator. Expected diagnostics
 * are printed to stderr by the code under test; test results go to stdout. */
#include <stdlib.h>

#include "check.h"
#include "codegen.h"
#include "lexer.h"
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

static void test_lex_escapes(void) {
    TokenList t;
    CHECK(lex_str("print \"a\\\"b\\\\c\\nd\\te\";", &t));
    CHECK_EQ_INT(t.items[1].value_len, 9);
    CHECK(memcmp(t.items[1].value, "a\"b\\c\nd\te", 9) == 0);
    token_list_free(&t);
}

static void test_lex_whitespace(void) {
    TokenList t;
    CHECK(lex_str("  \r\n\t print\n\"x\"\n;  ", &t));
    CHECK_EQ_INT(t.len, 4);
    CHECK_EQ_INT(t.items[0].line, 2);
    CHECK_EQ_INT(t.items[1].line, 3);
    token_list_free(&t);
    CHECK(lex_str("", &t));
    CHECK_EQ_INT(t.len, 1);
    token_list_free(&t);
}

static void test_lex_errors(void) {
    TokenList t;
    fprintf(stderr, "[expected diagnostics follow]\n");
    CHECK(!lex_str("print \"Hello;", &t));      /* unterminated at EOF */
    CHECK(!lex_str("print \"Hello;\nx\";", &t)); /* unterminated at newline */
    CHECK(!lex_str("print \"abc\\", &t));       /* backslash at EOF */
    CHECK(!lex_str("print \"a\\q\";", &t));     /* unsupported escape */
    CHECK(!lex_str("print \"a\\0\";", &t));     /* \0 not supported */
    CHECK(!lex_str("printx \"a\";", &t));       /* unknown identifier */
    CHECK(!lex_str("print 'a';", &t));          /* single quotes */
    CHECK(!lex_str("print \"a\" @", &t));       /* stray char */
}

static bool parse_str(const char *src, Program *p, TokenList *t) {
    if (!lex_str(src, t)) return false;
    bool ok = parse("t.luma", t, p);
    token_list_free(t);
    return ok;
}

static void test_parse(void) {
    TokenList t;
    Program p;
    CHECK(parse_str("print \"a\"; print \"bc\";", &p, &t));
    CHECK_EQ_INT(p.len, 2);
    CHECK_EQ_INT(p.stmts[0].kind, STMT_PRINT);
    CHECK(strcmp(p.stmts[1].value.str, "bc") == 0);
    program_free(&p);

    CHECK(parse_str("", &p, &t));
    CHECK_EQ_INT(p.len, 0);
    program_free(&p);

    fprintf(stderr, "[expected diagnostics follow]\n");
    CHECK(!parse_str("print \"a\"", &p, &t));  /* missing ; */
    CHECK(!parse_str("print ;", &p, &t));      /* missing string */
    CHECK(!parse_str("print", &p, &t));        /* EOF after print */
    CHECK(!parse_str("\"a\";", &p, &t));       /* no keyword */
    CHECK(!parse_str(";", &p, &t));            /* empty statement */
    CHECK(!parse_str("print \"a\" \"b\";", &p, &t));
    CHECK(!parse_str("print print;", &p, &t));
}

static void test_codegen(void) {
    TokenList t;
    Program p;
    CHECK(parse_str("print \"q\\\"\\n\";", &p, &t));
    p.source_path = "t.luma";
    Buf b = {0};
    codegen_program(&p, &b);
    buf_byte(&b, 0);
    const char *s = (const char *)b.data;
    CHECK(strstr(s, ".asciz \"q\\\"\\012\"") != NULL);
    CHECK(strstr(s, "lea rdi, [rip + .Lstr0]") != NULL);
    CHECK(strstr(s, "call puts@PLT") != NULL);
    CHECK(strstr(s, ".globl main") != NULL);
    CHECK(strstr(s, "xor eax, eax") != NULL);
    buf_free(&b);
    program_free(&p);
}

int main(void) {
    test_lex_hello();
    test_lex_escapes();
    test_lex_whitespace();
    test_lex_errors();
    test_parse();
    test_codegen();
    return check_report("test_frontend");
}
