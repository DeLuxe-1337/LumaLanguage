/* Unit tests for the LIR text form (parse/print round trip), the parser's
 * error handling, the verifier, and the memory pass (mem.c). */
#include <stdlib.h>

#include "check.h"
#include "ir.h"
#include "mem.h"

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

static const char *STRUCTS =
    "module \"s.luma\"\n"
    "\n"
    "struct Node {value: int, next: Node?} methods {sum = @m.Node.sum}\n"
    "struct Box {item}\n"
    "\n"
    "data @s0 = str \"ctx\"\n"
    "\n"
    "fn @m.Node.sum(%self: Node): int {\n"
    "entry:\n"
    "  %0 = getfield %self, Node.value\n"
    "  %1 = getfield %self, .next\n"
    "  %2 = new Box(%1)\n"
    "  setfield %2, Box.item, %0\n"
    "  setfield %2, .item, %0\n"
    "  %3 = callm %2, .size(%0, %1)\n"
    "  callm %2, .size()\n"
    "  %4 = check %3, int|nil|Node, @s0\n"
    "  %5 = check %4, struct, @s0\n"
    "  ret %0\n"
    "}\n";

static void test_roundtrip(void) {
    expect_roundtrip(STRUCTS);
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
    expect_roundtrip("module \"s\"\n\nshared struct T {x: int}\nstruct U {t: T?}\n");
    expect_roundtrip("module \"mv\"\n\nfn @f(move %a, %b: int, move %c) {\nentry:\n  ret %a\n}\n");
    /* C structs (v0.9): fields with C types, by-value eightbytes and results, symbol aliases */
    expect_roundtrip("module \"cs\"\n\ncstruct P {a: i64, b: i32, s: cstr?}\n\n"
                     "extern c fn @f(p: cs0, p: cs1, q: ptr): struct P symbol \"real_f\"\n");
    {
        IrModule m;
        const char *src = "module \"l\"\n\ncstruct M {r: u8, g: u8, b: u8, w: u16, p: ptr}\n";
        CHECK(ir_parse("t.lir", src, strlen(src), &m));
        const IrStruct *st = &m.structs[0];
        CHECK(st->cstruct && st->shared && st->ncf == 5 && st->cfoffs[3] == 4 && st->cfoffs[4] == 8 && st->csize == 16);
        CHECK(st->nfields == 2 && strcmp(st->fields[0], "__ptr") == 0);
        ir_module_free(&m);
    }
    {
        const char *bad = "module \"x\"\n\ncstruct Q {a: int}\n";
        IrModule m;
        CHECK(!ir_parse("t.lir", bad, strlen(bad), &m));
    }
    /* generic instances: names with type arguments, in types and field references */
    expect_roundtrip("module \"g\"\n\nstruct Box<int?> {value: int?}\nstruct Pair<Box<int?>,str> {first: Box<int?>, second: str}\n\ndata @ctx = str \"x\"\n\n"
                     "fn @f(%p: Pair<Box<int?>,str>?): int? {\nentry:\n  %p = check %p, Pair<Box<int?>,str>, @ctx\n"
                     "  %b = getfield %p, Pair<Box<int?>,str>.first\n  %v = getfield %b, Box<int?>.value\n  ret %v\n}\n");
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
    /* structs (v0.5) */
    expect_verify_fails("module \"m\"\nstruct P {x: int, y} methods {m = @pm}\nfn @pm(%self: P) {\ne:\n  ret %self\n}\n" "fn @f() {\ne:\n  %0 = const 1\n  %1 = new P(%0)\n  ret %1\n}\n"); /* too few fields */
    expect_verify_fails("module \"m\"\nstruct P {x: int, y} methods {m = @pm}\nfn @pm(%self: P) {\ne:\n  ret %self\n}\n" "fn @f(%a) {\ne:\n  %0 = getfield %a, P.x\n  ret %0\n}\n"); /* static access needs a P */
    expect_verify_fails("module \"m\"\nstruct P {x: int, y} methods {m = @pm}\nfn @pm(%self: P) {\ne:\n  ret %self\n}\n" "fn @f(%a: P) {\ne:\n  %0 = const nil\n  setfield %a, P.x, %0\n  ret %0\n}\n"); /* x: int */
    expect_verify_fails("module \"m\"\nstruct P {x: int, y} methods {m = @pm}\nfn @pm(%self: P) {\ne:\n  ret %self\n}\n" "fn @f() {\ne:\n  %0 = const 1\n  %1 = const nil\n  %2 = new P(%1, %0)\n  ret %2\n}\n"); /* x: int */
    expect_verify_fails("module \"m\"\nstruct P {} methods {m = @pm}\nfn @pm(%self: P, %a: int) {\ne:\n  ret %a\n}\n"); /* callm args are unchecked */
    expect_verify_fails("module \"m\"\nstruct P {} methods {m = @pm}\nstruct Q {}\nfn @pm(%self: Q) {\ne:\n  ret %self\n}\n"); /* self must accept P */
    expect_verify_fails("module \"m\"\nstruct P {} methods {m = @pm}\nfn @pm() {\ne:\n  %0 = const 1\n  ret %0\n}\n"); /* no self */
    expect_verify_fails("module \"m\"\nstruct P {x: int, y} methods {m = @pm}\nfn @pm(%self: P) {\ne:\n  ret %self\n}\n" "fn @f(%a) {\ne:\n  %0 = check %a, P, @f\n  ret %0\n}\n"); /* context must be data */

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

/* ---- memory pass (mem.c) ---- */

/* Parses src, runs mem_lower, verifies, and returns the printed body of
 * function @fname (from its "fn" line to "}"), or NULL. Caller frees. */
static char *mem_body(const char *src, const char *fname) {
    IrModule m;
    if (!parse_str(src, &m)) return NULL;
    char *res = NULL;
    if (ir_verify(&m, "t.lir")) {
        mem_lower(&m);
        if (ir_verify(&m, "t.lir")) {
            Buf b = {0};
            ir_print_module(&m, &b);
            buf_byte(&b, 0);
            char head[128];
            snprintf(head, sizeof head, "fn @%s(", fname);
            char *start = strstr((char *)b.data, head);
            char *end = start ? strstr(start, "\n}\n") : NULL;
            if (end) {
                size_t n = (size_t)(end - start) + 3;
                res = malloc(n + 1);
                memcpy(res, start, n);
                res[n] = '\0';
            }
            buf_free(&b);
        }
    }
    ir_module_free(&m);
    return res;
}

static void expect_mem(const char *src, const char *fname, const char *want) {
    char *got = mem_body(src, fname);
    check_count++;
    if (!got || strcmp(got, want) != 0) {
        check_failures++;
        printf("FAIL: memory pass on @%s\n--- want\n%s--- got\n%s\n", fname, want, got ? got : "(error)");
    }
    free(got);
}

static void test_memory(void) {
    /* the memory line round-trips; none is the default and is not printed */
    expect_roundtrip("module \"m\"\nmemory manual\n\nfn @luma_main() {\nentry:\n  %n = const nil\n  ret %n\n}\n");
    IrModule m;
    CHECK(!parse_str("module \"m\"\nmemory gc\n", &m));
    CHECK(!parse_str("module \"m\"\nmemory rc\n", &m)); /* removed: Rc::new instead */

    const char *HEAD = "module \"m\"\nmemory manual\n\nextern fn @luma_write(1)\nglobal @var.x\n\n";
    char src[2048];

    /* parameters are borrowed: no operations for them */
    snprintf(src, sizeof src, "%sfn @f(%%s) {\nentry:\n  call @luma_write(%%s)\n  %%r = add %%s, %%s\n  ret %%r\n}\n", HEAD);
    expect_mem(src, "f", "fn @f(%s) {\nentry:\n  call @luma_write(%s)\n  %r = add %s, %s\n  ret %r\n}\n");

    /* an owned temporary dies at a borrowing use: released right after it */
    snprintf(src, sizeof src, "%sfn @g(%%a) {\nentry:\n  %%t = add %%a, %%a\n  call @luma_write(%%t)\n  %%n = const nil\n  ret %%n\n}\n", HEAD);
    expect_mem(src, "g", "fn @g(%a) {\nentry:\n  %t = add %a, %a\n  call @luma_write(%t)\n  release %t\n  %n = const nil\n  ret %n\n}\n");

    /* consumed while still live (store): retained first; the last use (ret) moves it */
    snprintf(src, sizeof src, "%sfn @h(%%a) {\nentry:\n  %%t = add %%a, %%a\n  store @var.x, %%t\n  call @luma_write(%%t)\n  ret %%t\n}\n", HEAD);
    expect_mem(src, "h", "fn @h(%a) {\nentry:\n  %t = add %a, %a\n  retain %t\n  store @var.x, %t\n  call @luma_write(%t)\n  ret %t\n}\n");

    /* returning a borrowed parameter retains it; a load used right away by
     * something that cannot free anything is a borrowed read (no operations),
     * but one that lives across a Luma call is retained */
    snprintf(src, sizeof src, "%sfn @i(%%a) {\nentry:\n  %%g = load @var.x\n  call @luma_write(%%g)\n  ret %%a\n}\n", HEAD);
    expect_mem(src, "i", "fn @i(%a) {\nentry:\n  %g = load @var.x\n  call @luma_write(%g)\n  retain %a\n  ret %a\n}\n");
    snprintf(src, sizeof src, "%sfn @j(%%a) {\nentry:\n  %%g = load @var.x\n  %%r = call @i(%%a)\n  call @luma_write(%%g)\n  ret %%r\n}\n%s",
             HEAD, "fn @i(%a) {\nentry:\n  ret %a\n}\n");
    expect_mem(src, "j", "fn @j(%a) {\nentry:\n  %g = load @var.x\n  retain %g\n  %r = call @i(%a)\n  call @luma_write(%g)\n"
               "  release %g\n  ret %r\n}\n");

    /* dying on a critical edge: the edge gets its own block */
    snprintf(src, sizeof src, "%sfn @k(%%a, %%c) {\nentry:\n  %%t = add %%a, %%a\n  br %%c, use, join\nuse:\n"
             "  call @luma_write(%%t)\n  jmp join\njoin:\n  %%n = const nil\n  ret %%n\n}\n", HEAD);
    expect_mem(src, "k", "fn @k(%a, %c) {\nentry:\n  %t = add %a, %a\n  br %c, use, rel.1\nuse:\n  call @luma_write(%t)\n"
               "  release %t\n  jmp join\njoin:\n  %n = const nil\n  ret %n\nrel.1:\n  release %t\n  jmp join\n}\n");

    /* `%s = add %s, ...` with the old value dying: kept in a temporary */
    snprintf(src, sizeof src, "%sfn @l(%%a) {\nentry:\n  %%s = add %%a, %%a\n  %%s = add %%s, %%a\n  ret %%s\n}\n", HEAD);
    expect_mem(src, "l", "fn @l(%a) {\nentry:\n  %s = add %a, %a\n  %s.2 = mov %s\n  %s = add %s.2, %a\n  release %s.2\n  ret %s\n}\n");

    /* a reassigned parameter is retained on entry and owned from then on */
    snprintf(src, sizeof src, "%sfn @p(%%a) {\nentry:\n  call @luma_write(%%a)\n  %%a = add %%a, %%a\n  ret %%a\n}\n", HEAD);
    expect_mem(src, "p", "fn @p(%a) {\nentry:\n  retain %a\n  call @luma_write(%a)\n  %a.1 = mov %a\n  %a = add %a.1, %a.1\n"
               "  release %a.1\n  ret %a\n}\n");

    /* ints, booleans and literals need nothing */
    snprintf(src, sizeof src, "%sdata @s0 = str \"lit\"\nfn @q(%%n: int) {\nentry:\n  %%s = const @s0\n  call @luma_write(%%s)\n"
             "  %%m = add %%n, %%n\n  call @luma_write(%%m)\n  ret %%s\n}\n", HEAD);
    expect_mem(src, "q", "fn @q(%n: int) {\nentry:\n  %s = const @s0\n  call @luma_write(%s)\n  %m = add %n, %n\n"
               "  call @luma_write(%m)\n  ret %s\n}\n");

    /* escape analysis (manual): only the instance that stays local is counted */
    {
        const char *esc = "module \"m\"\nmemory manual\n\nstruct P {x}\n\nextern fn @luma_free(1)\nglobal @var.g\n\n"
                          "fn @keep(%o) {\nentry:\n  store @var.g, %o\n  ret %o\n}\n\n"
                          "fn @look(%o: P) {\nentry:\n  %x = getfield %o, P.x\n  ret %x\n}\n\n"
                          "fn @luma_main() {\nentry:\n  %n = const nil\n  %a = new P(%n)\n  %b = new P(%n)\n"
                          "  call @luma_free(%b)\n  %c = new P(%n)\n  %r = call @keep(%c)\n  %d = new P(%n)\n"
                          "  %e = call @look(%d)\n  ret %n\n}\n";
        IrModule em;
        CHECK(parse_str(esc, &em) && ir_verify(&em, "t.lir"));
        mem_lower(&em);
        CHECK(ir_verify(&em, "t.lir"));
        const IrFunc *mf = &em.funcs[em.globals[ir_find_global(&em, "luma_main")].func];
        int counted[4] = {-1, -1, -1, -1}, k = 0;
        for (int i = 0; i < mf->blocks[0].n && k < 4; i++)
            if (mf->blocks[0].instrs[i].op == IR_NEW) counted[k++] = (int)mf->blocks[0].instrs[i].imm;
        CHECK_EQ_INT(counted[0], 1); /* a: never leaves luma_main */
        CHECK_EQ_INT(counted[1], 0); /* b: passed to free() */
        CHECK_EQ_INT(counted[2], 0); /* c: stored into a global by @keep */
        CHECK_EQ_INT(counted[3], 1); /* d: @look only reads it */
        ir_module_free(&em);
    }

    /* compile-time free() checks */
    {
        const char *twice = "module \"m\"\n\nextern fn @luma_free(1)\n\nfn @luma_main() {\nentry:\n  %p = const nil\n"
                            "  call @luma_free(%p)\n  call @luma_free(%p)\n  ret %p\n}\n";
        const char *maybe = "module \"m\"\n\nextern fn @luma_free(1)\n\nfn @f(%p, %c) {\nentry:\n  br %c, a, b\na:\n"
                            "  call @luma_free(%p)\n  jmp b\nb:\n  ret %p\n}\n";
        const char *again = "module \"m\"\n\nextern fn @luma_free(1)\n\nfn @f(%p) {\nentry:\n  call @luma_free(%p)\n"
                            "  %p = const nil\n  ret %p\n}\n";
        IrModule fm;
        printf("[expected diagnostic follows]\n");
        CHECK(parse_str(twice, &fm) && !mem_check_frees(&fm, "t.lir"));
        ir_module_free(&fm);
        CHECK(parse_str(maybe, &fm) && mem_check_frees(&fm, "t.lir")); /* freed on one path only: allowed */
        ir_module_free(&fm);
        CHECK(parse_str(again, &fm) && mem_check_frees(&fm, "t.lir")); /* reassigned after free: fine */
        ir_module_free(&fm);
    }
}

int main(void) {
    test_roundtrip();
    test_parse_details();
    test_parse_errors();
    test_verifier();
    test_builder_verifier();
    test_memory();
    return check_report("test_ir");
}
