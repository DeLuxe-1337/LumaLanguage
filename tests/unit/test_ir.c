/* Unit tests for the LIR text form (parse/print round trip), the parser's
 * error handling, and the verifier. */
#include <stdlib.h>

#include "check.h"
#include "ir.h"

static bool parse_str(const char *src, IrModule *m) { return ir_parse("t.lir", src, strlen(src), m); }

/* Canonical text must survive parse -> print unchanged. */
static void expect_roundtrip(const char *src) {
    IrModule m;
    check_count++;
    if (!parse_str(src, &m)) {
        check_failures++;
        printf("FAIL: could not parse:\n%s\n", src);
        return;
    }
    Buf b = {0};
    ir_print_module(&m, &b);
    buf_byte(&b, 0);
    if (strcmp((char *)b.data, src) != 0) {
        check_failures++;
        printf("FAIL: round trip differs.\n--- input\n%s--- output\n%s", src, (char *)b.data);
    }
    check_count++;
    if (!ir_verify(&m, "t.lir")) {
        check_failures++;
        printf("FAIL: canonical module does not verify:\n%s\n", src);
    }
    buf_free(&b);
    ir_module_free(&m);
}

static const char *LOOP =
    "module \"loop.luma\"\n"
    "\n"
    "extern fn @luma_print(1)\n"
    "data @s0 = str \"q\\\"\\\\\\n\\t\\x01\\xff\"\n"
    "\n"
    "fn @luma_main() {\n"
    "entry:\n"
    "  %i = const 0\n"
    "  jmp cond\n"
    "cond:\n"
    "  %t0 = const 3\n"
    "  %t1 = lt %i, %t0\n"
    "  br %t1, body, exit\n"
    "body:\n"
    "  call @luma_print(%i)\n"
    "  %t2 = const 1\n"
    "  %i = add %i, %t2\n"
    "  jmp cond\n"
    "exit:\n"
    "  %s = const @s0\n"
    "  %r = call @twice(%s, %i)\n"
    "  %n = const -4611686018427387904\n"
    "  %b = const true\n"
    "  %c = const false\n"
    "  %d = not %b\n"
    "  %e = neg %n\n"
    "  %f = mov %e\n"
    "  %g = eq %c, %d\n"
    "  %r = const nil\n"
    "  ret %r\n"
    "}\n"
    "\n"
    "fn @twice(%x, %y) {\n"
    "entry:\n"
    "  %z = mul %x, %y\n"
    "  %w = sub %z, %y\n"
    "  %v = div %w, %x\n"
    "  %u = mod %v, %x\n"
    "  %a = ne %u, %v\n"
    "  %b = le %u, %v\n"
    "  %c = gt %u, %v\n"
    "  %d = ge %u, %v\n"
    "  ret %d\n"
    "}\n";

static void test_roundtrip(void) {
    expect_roundtrip(LOOP);
    expect_roundtrip("module \"\"\n");
    expect_roundtrip("module \"t\"\n\nglobal @g: int\nglobal @h: str|bool\ndata @c = str \"x\"\n\n"
                     "fn @f(%a: int, %b, %c: str?): int? {\nentry:\n  %d = check %b, int, @c\n  store @g, %d\n  ret %a\n}\n");
    expect_roundtrip("module \"c\"\n\nextern c fn @puts(s: cstr): i32\nextern c fn @f(a: i8, b: u64, c: bool, d: cstr?, e: ptr): void\n"
                     "extern c fn @g(): cstr?\n\nfn @luma_main() {\nentry:\n  %0 = call @g()\n  %1 = call @puts(%0)\n  ret %1\n}\n");
    expect_roundtrip("module \"g\"\n\nglobal @var.count\nextern fn @luma_print(1)\n\n"
                     "fn @luma_main() {\nentry:\n  %0 = const 1\n  store @var.count, %0\n"
                     "  %1 = load @var.count\n  call @luma_print(%1)\n  ret %1\n}\n");
    expect_roundtrip("module \"x\"\n\nfn @f() {\nb:\n  %0 = const 1\n  ret %0\n}\n");
}

static void test_parse_details(void) {
    IrModule m;
    /* comments, blank lines and forward references are accepted */
    const char *src =
        "; leading comment\n"
        "module \"m\"   ; trailing\n"
        "\n\n"
        "fn @main() {\n"
        "entry:\n"
        "  %v = call @later()\n"
        "  jmp done ; forward label\n"
        "done:\n"
        "  ret %v\n"
        "}\n"
        "fn @later() {\n"
        "e:\n"
        "  %0 = const 7\n"
        "  ret %0\n"
        "}\n";
    CHECK(parse_str(src, &m));
    CHECK_EQ_INT(m.nfuncs, 2);
    CHECK_EQ_INT(m.funcs[0].blocks[0].instrs[1].target[0], 1);
    CHECK(strcmp(m.globals[m.funcs[0].blocks[0].instrs[0].global].name, "later") == 0);
    CHECK(ir_verify(&m, "t.lir"));
    ir_module_free(&m);

    /* vregs are numbered params first, then by first appearance */
    CHECK(parse_str("module \"m\"\nfn @f(%p, %q) {\ne:\n  %z = add %q, %p\n  %a = mov %z\n  ret %a\n}\n", &m));
    CHECK(strcmp(m.funcs[0].vregs[0], "p") == 0 && strcmp(m.funcs[0].vregs[2], "z") == 0 &&
          strcmp(m.funcs[0].vregs[3], "a") == 0);
    ir_module_free(&m);
}

static void test_parse_errors(void) {
    IrModule m;
    fprintf(stderr, "[expected diagnostics follow]\n");
    const char *bad[] = {
        "",                                                         /* no module line */
        "module m\n",                                               /* module name not a string */
        "module \"m\"\nbogus\n",
        "module \"m\"\nfn @f() {\n  %0 = const 1\n  ret %0\n}\n",   /* instruction before label */
        "module \"m\"\nfn @f() {\ne:\n  %0 = const 1\n}\n",          /* no terminator */
        "module \"m\"\nfn @f() {\ne:\n  %0 = const 1\nf:\n  ret %0\n}\n", /* label before terminator */
        "module \"m\"\nfn @f() {\ne:\n  jmp nowhere\n}\n",          /* unknown label */
        "module \"m\"\nfn @f() {\ne:\n  %0 = call @g()\n  ret %0\n}\n", /* undefined global */
        "module \"m\"\nfn @f() {\ne:\n  %0 = frob %1\n  ret %0\n}\n",  /* unknown operation */
        "module \"m\"\nfn @f() {\ne:\n  %0 = const\n  ret %0\n}\n",
        "module \"m\"\nfn @f() {\ne:\n  ret %0 %1\n}\n",            /* junk after instruction */
        "module \"m\"\nfn @f() {\ne:\n  ret %0\ne:\n  ret %0\n}\n",  /* duplicate label */
        "module \"m\"\nfn @f() {\n}\n",                             /* no blocks */
        "module \"m\"\nfn @f(%a, %a) {\ne:\n  ret %a\n}\n",         /* duplicate parameter */
        "module \"m\"\nextern fn @f(1)\nextern fn @f(1)\n",         /* duplicate global */
        "module \"m\"\ndata @s = str \"abc\n",                      /* unterminated string */
        "module \"m\"\ndata @s = str \"\\q\"\n",                    /* bad escape */
        "module \"m\"\ndata @s = str \"\\xZZ\"\n",
        "module \"m\"\nfn @f() {\ne:\n  %0 = const 99999999999999999999\n  ret %0\n}\n",
        "module \"m\"\nfn @f() {\ne:\n  %0 = const 1\n  ret %0\n",  /* EOF inside function */
        "module \"m\"\nextern fn @f(x)\n",
        "module \"m\"\nglobal g\n",                               /* missing @ */
        "module \"m\"\nglobal @g: integer\n",                      /* unknown type */
        "module \"m\"\nfn @f(%a: int?str) {\ne:\n  ret %a\n}\n",
        "module \"m\"\ndata @c = str \"c\"\nfn @f(%a) {\ne:\n  %b = check %a, @c\n  ret %b\n}\n", /* missing type */
        "module \"m\"\nextern c fn @f(a: int): void\n",            /* not a C type */
        "module \"m\"\nextern c fn @f(a: i32?): void\n",           /* only cstr? is nullable */
        "module \"m\"\nextern c fn @f(a): void\n",                 /* untyped parameter */
        "module \"m\"\nextern c fn @f(a: i32)\n",                  /* missing return type */
        "module \"m\"\nglobal @g\nglobal @g\n",                    /* duplicate */
        "module \"m\"\nfn @f() {\ne:\n  store %0\n  ret %0\n}\n",   /* store needs @global */
        "module \"m\"\nglobal @g\nfn @f() {\ne:\n  store @g %0\n  ret %0\n}\n", /* missing comma */
        "module \"m\"\nfn @f() {\ne:\n  %0 = load @missing\n  ret %0\n}\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        bool ok = parse_str(bad[i], &m);
        check_count++;
        if (ok) {
            check_failures++;
            printf("FAIL: expected parse error for:\n%s\n", bad[i]);
            ir_module_free(&m);
        }
    }
    char nul[] = "module \"m\"\n\0";
    CHECK(!ir_parse("t.lir", nul, sizeof nul - 1, &m));
}

static void expect_verify_fails(const char *src) {
    IrModule m;
    check_count++;
    if (!parse_str(src, &m)) {
        check_failures++;
        printf("FAIL: verifier test input did not parse:\n%s\n", src);
        return;
    }
    if (ir_verify(&m, "t.lir")) {
        check_failures++;
        printf("FAIL: expected verifier error for:\n%s\n", src);
    }
    ir_module_free(&m);
}

static void test_verifier(void) {
    fprintf(stderr, "[expected diagnostics follow]\n");
    /* read before write on one path */
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  %c = const true\n  br %c, a, b\na:\n  %x = const 1\n  jmp j\n"
                        "b:\n  jmp j\nj:\n  ret %x\n}\n");
    /* plain read before any write */
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  ret %x\n}\n");
    /* loop: value only defined later in the loop body */
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  jmp l\nl:\n  %y = mov %x\n  %x = const 1\n  jmp l\n}\n");
    /* arity */
    expect_verify_fails("module \"m\"\nextern fn @p(1)\nfn @f() {\ne:\n  %0 = const 1\n  call @p(%0, %0)\n  ret %0\n}\n");
    /* too many arguments */
    expect_verify_fails("module \"m\"\nextern fn @p(7)\nfn @f() {\ne:\n  %0 = const 1\n"
                        "  call @p(%0, %0, %0, %0, %0, %0, %0)\n  ret %0\n}\n");
    /* fixnum range (2^62) */
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  %0 = const 4611686018427387904\n  ret %0\n}\n");
    /* const must reference data */
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  %0 = const @f\n  ret %0\n}\n");
    /* declared types must be provable */
    expect_verify_fails("module \"m\"\nfn @f(%a: int): int {\ne:\n  ret %a\n}\nfn @g(%x) {\ne:\n  %r = call @f(%x)\n  ret %r\n}\n");
    expect_verify_fails("module \"m\"\nfn @f(%a): int {\ne:\n  ret %a\n}\n");
    expect_verify_fails("module \"m\"\nglobal @g: int\nfn @f() {\ne:\n  %a = const nil\n  store @g, %a\n  ret %a\n}\n");
    expect_verify_fails("module \"m\"\nfn @f(%a) {\ne:\n  %b = check %a, int, @f\n  ret %b\n}\n"); /* context must be data */
    /* ...and a check, or flow through int-only operations, proves them */
    {
        IrModule m;
        CHECK(parse_str("module \"m\"\ndata @c = str \"ctx\"\nfn @f(%a: int): int {\ne:\n  ret %a\n}\n"
                        "fn @g(%x, %y) {\ne:\n  %x = check %x, int, @c\n  %r = call @f(%x)\n  %s = sub %y, %x\n"
                        "  %t = call @f(%s)\n  br %y, a, b\na:\n  %u = const 1\n  jmp j\nb:\n  %u = const 2\n  jmp j\n"
                        "j:\n  %v = call @f(%u)\n  ret %v\n}\n", &m));
        CHECK(ir_verify(&m, "t.lir"));
        ir_module_free(&m);
    }
    /* load/store must name a 'global' variable */
    expect_verify_fails("module \"m\"\ndata @s = str \"x\"\nfn @f() {\ne:\n  %0 = load @s\n  ret %0\n}\n");
    expect_verify_fails("module \"m\"\nfn @f() {\ne:\n  %0 = const 1\n  store @f, %0\n  ret %0\n}\n");
    /* store reads its operand: it must be assigned */
    expect_verify_fails("module \"m\"\nglobal @g\nfn @f() {\ne:\n  store @g, %x\n  %0 = const 1\n  ret %0\n}\n");
    /* cannot call a global variable */
    expect_verify_fails("module \"m\"\nglobal @g\nfn @f() {\ne:\n  %0 = call @g()\n  ret %0\n}\n");
    /* C functions: void parameters and arity */
    expect_verify_fails("module \"m\"\nextern c fn @f(a: void): void\nfn @g() {\ne:\n  %0 = const 1\n  ret %0\n}\n");
    expect_verify_fails("module \"m\"\nextern c fn @f(a: i32): void\nfn @g() {\ne:\n  %0 = call @f()\n  ret %0\n}\n");
    /* cannot call data */
    expect_verify_fails("module \"m\"\ndata @s = str \"x\"\nfn @f() {\ne:\n  %0 = call @s()\n  ret %0\n}\n");

    /* accepted: defined on both paths; loop-carried after definition; params */
    IrModule m;
    CHECK(parse_str("module \"m\"\nfn @f(%p) {\ne:\n  br %p, a, b\na:\n  %x = const 1\n  jmp j\nb:\n  %x = const 2\n"
                    "  jmp j\nj:\n  ret %x\n}\n", &m));
    CHECK(ir_verify(&m, "t.lir"));
    ir_module_free(&m);
    CHECK(parse_str("module \"m\"\nfn @f() {\ne:\n  %x = const 0\n  jmp l\nl:\n  %x = add %x, %x\n  br %x, l, d\n"
                    "d:\n  ret %x\n}\n", &m));
    CHECK(ir_verify(&m, "t.lir"));
    ir_module_free(&m);
    /* unreachable blocks are not checked for definite assignment */
    CHECK(parse_str("module \"m\"\nfn @f() {\ne:\n  %x = const 0\n  ret %x\ndead:\n  ret %y\n}\n", &m));
    CHECK(ir_verify(&m, "t.lir"));
    ir_module_free(&m);
}

static void test_builder_verifier(void) {
    /* Malformed modules can only be built in memory; the verifier must catch them. */
    fprintf(stderr, "[expected diagnostics follow]\n");
    IrModule m;
    ir_module_init(&m, "x");
    int fi = ir_add_func(&m, "f", 0);
    IrFunc *f = &m.funcs[fi];
    int b = ir_func_block(f, "e");
    int v = ir_func_vreg(f, "0");
    IrInstr *in = ir_emit(f, b, IR_CONST_INT);
    in->dst = v;
    in->imm = 1;
    CHECK(!ir_verify(&m, "mem")); /* no terminator */
    in = ir_emit(f, b, IR_RET);
    in->a = v;
    CHECK(ir_verify(&m, "mem"));
    in = ir_emit(f, b, IR_RET); /* instruction after terminator */
    in->a = v;
    CHECK(!ir_verify(&m, "mem"));
    f->blocks[b].n--;
    in = &f->blocks[b].instrs[1];
    in->a = 42; /* invalid vreg */
    CHECK(!ir_verify(&m, "mem"));
    in->a = v;
    in->op = IR_JMP;
    in->target[0] = 9; /* unknown block */
    CHECK(!ir_verify(&m, "mem"));
    ir_module_free(&m);
}

int main(void) {
    test_roundtrip();
    test_parse_details();
    test_parse_errors();
    test_verifier();
    test_builder_verifier();
    return check_report("test_ir");
}
