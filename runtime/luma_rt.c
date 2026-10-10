/* luma_rt.c - the Luma runtime (v0.1), linked into every Luma program.
 *
 * Built by `make` into build/libluma_rt.a with the system C compiler. It is
 * part of the toolchain, like libc. Compiled Luma code never contains a
 * `main`: the runtime owns process entry and calls luma_main().
 *
 * All functions follow the System V ABI; every argument and result is a
 * tagged LumaValue (src/value.h). Errors print
 *   luma: runtime error: <message>
 * to stderr and exit with status 1. Messages follow Lox wording.
 *
 * Memory: strings created at runtime (concatenation) are allocated with
 * malloc and never freed. There is no GC in v0.1. */
#define _XOPEN_SOURCE 700 /* sigaction, sigaltstack, SA_ONSTACK (XSI) */
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/value.h"

typedef struct {
    uint64_t header; /* low byte: type id */
    uint64_t len;
    char bytes[];    /* len bytes + NUL */
} LumaString;

LumaValue luma_main(void);

static _Noreturn void luma_panic(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "luma: runtime error: %s\n", msg);
    exit(1);
}

static bool is_fixnum(LumaValue v) { return v & 1; }
static int64_t fixnum_val(LumaValue v) { return (int64_t)v >> 1; }

static void check_valid(LumaValue v) {
    if (v == 0) luma_panic("invalid value (uninitialized slot)");
}

static bool is_object(LumaValue v) { return v != 0 && (v & 7) == 0; }

static bool is_string(LumaValue v) {
    return is_object(v) && (((const LumaString *)(uintptr_t)v)->header & 0xff) == LUMA_TYPE_STRING;
}

static const LumaString *as_string(LumaValue v) { return (const LumaString *)(uintptr_t)v; }

static LumaValue make_fixnum_checked(int64_t n) {
    if (n < LUMA_FIXNUM_MIN || n > LUMA_FIXNUM_MAX) luma_panic("Integer overflow.");
    return luma_fixnum(n);
}

static LumaValue make_bool(bool b) { return b ? LUMA_TRUE : LUMA_FALSE; }

static void need_numbers(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    if (!is_fixnum(a) || !is_fixnum(b)) luma_panic("Operands must be numbers.");
}

/* ---- printing ----------------------------------------------------------
 * The builtin print(a, b, ...) lowers to
 *     luma_write(a); luma_write_space(); luma_write(b); ...; luma_write_newline();
 * luma_print(v) (= write + newline) is kept for hand-written LIR. */

LumaValue luma_write(LumaValue v) {
    check_valid(v);
    if (is_fixnum(v)) printf("%" PRId64, fixnum_val(v));
    else if (v == LUMA_NIL) fputs("nil", stdout);
    else if (v == LUMA_TRUE) fputs("true", stdout);
    else if (v == LUMA_FALSE) fputs("false", stdout);
    else if (is_string(v)) {
        const LumaString *s = as_string(v);
        fwrite(s->bytes, 1, s->len, stdout);
    } else luma_panic("cannot print value of unknown type");
    return LUMA_NIL;
}

LumaValue luma_write_space(void) {
    putchar(' ');
    return LUMA_NIL;
}

LumaValue luma_write_newline(void) {
    putchar('\n');
    return LUMA_NIL;
}

LumaValue luma_print(LumaValue v) {
    luma_write(v);
    return luma_write_newline();
}

/* Called by a checked `load` of a global slot that was never assigned. */
_Noreturn void luma_undefined_variable(const char *name) {
    fflush(stdout);
    fprintf(stderr, "luma: runtime error: Undefined variable '%s'.\n", name);
    exit(1);
}

/* ---- arithmetic -------------------------------------------------------- */

LumaValue luma_add(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    if (is_fixnum(a) && is_fixnum(b)) {
        int64_t r;
        if (__builtin_add_overflow(fixnum_val(a), fixnum_val(b), &r)) luma_panic("Integer overflow.");
        return make_fixnum_checked(r);
    }
    if (is_string(a) && is_string(b)) {
        const LumaString *x = as_string(a), *y = as_string(b);
        if (x->len > SIZE_MAX / 2 || y->len > SIZE_MAX / 2) luma_panic("String too long.");
        size_t n = x->len + y->len;
        LumaString *s = aligned_alloc(8, (sizeof *s + n + 1 + 7) & ~(size_t)7);
        if (!s) luma_panic("Out of memory.");
        s->header = LUMA_TYPE_STRING;
        s->len = n;
        memcpy(s->bytes, x->bytes, x->len);
        memcpy(s->bytes + x->len, y->bytes, y->len);
        s->bytes[n] = '\0';
        return (LumaValue)(uintptr_t)s;
    }
    luma_panic("Operands must be two numbers or two strings.");
}

LumaValue luma_sub(LumaValue a, LumaValue b) {
    need_numbers(a, b);
    int64_t r;
    if (__builtin_sub_overflow(fixnum_val(a), fixnum_val(b), &r)) luma_panic("Integer overflow.");
    return make_fixnum_checked(r);
}

LumaValue luma_mul(LumaValue a, LumaValue b) {
    need_numbers(a, b);
    int64_t r;
    if (__builtin_mul_overflow(fixnum_val(a), fixnum_val(b), &r)) luma_panic("Integer overflow.");
    return make_fixnum_checked(r);
}

/* Floor division: rounds toward negative infinity. */
LumaValue luma_div(LumaValue a, LumaValue b) {
    need_numbers(a, b);
    int64_t x = fixnum_val(a), y = fixnum_val(b);
    if (y == 0) luma_panic("Division by zero.");
    int64_t q = x / y; /* no overflow: |x| <= 2^62 */
    if ((x % y != 0) && ((x < 0) != (y < 0))) q--;
    return make_fixnum_checked(q);
}

/* Floor modulo: result has the sign of the divisor. */
LumaValue luma_mod(LumaValue a, LumaValue b) {
    need_numbers(a, b);
    int64_t x = fixnum_val(a), y = fixnum_val(b);
    if (y == 0) luma_panic("Division by zero.");
    int64_t r = x % y;
    if (r != 0 && ((r < 0) != (y < 0))) r += y;
    return luma_fixnum(r);
}

LumaValue luma_neg(LumaValue a) {
    check_valid(a);
    if (!is_fixnum(a)) luma_panic("Operand must be a number.");
    return make_fixnum_checked(-fixnum_val(a));
}

LumaValue luma_not(LumaValue a) {
    check_valid(a);
    return make_bool((a & LUMA_FALSY_MASK) == 2);
}

/* ---- comparison -------------------------------------------------------- */

static bool values_equal(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    if (a == b) return true;
    if (is_string(a) && is_string(b)) {
        const LumaString *x = as_string(a), *y = as_string(b);
        return x->len == y->len && memcmp(x->bytes, y->bytes, x->len) == 0;
    }
    return false;
}

LumaValue luma_eq(LumaValue a, LumaValue b) { return make_bool(values_equal(a, b)); }
LumaValue luma_ne(LumaValue a, LumaValue b) { return make_bool(!values_equal(a, b)); }

LumaValue luma_lt(LumaValue a, LumaValue b) { need_numbers(a, b); return make_bool(fixnum_val(a) < fixnum_val(b)); }
LumaValue luma_le(LumaValue a, LumaValue b) { need_numbers(a, b); return make_bool(fixnum_val(a) <= fixnum_val(b)); }
LumaValue luma_gt(LumaValue a, LumaValue b) { need_numbers(a, b); return make_bool(fixnum_val(a) > fixnum_val(b)); }
LumaValue luma_ge(LumaValue a, LumaValue b) { need_numbers(a, b); return make_bool(fixnum_val(a) >= fixnum_val(b)); }

/* ---- process entry ----------------------------------------------------- */

/* Unbounded recursion overflows the native stack. The fault is caught on an
 * alternate signal stack and reported as a Luma runtime error instead of a
 * bare "Segmentation fault". Any SIGSEGV in a Luma program is reported this
 * way; generated code never dereferences memory other than its own frame,
 * globals and runtime objects, so the stack is by far the likely cause. */
static char alt_stack[64 * 1024];

static void on_segv(int sig) {
    (void)sig;
    static const char msg[] = "luma: runtime error: Stack overflow.\n";
    fflush(stdout); /* best effort: keep output printed before the overflow */
    ssize_t r = write(2, msg, sizeof msg - 1);
    (void)r;
    _exit(1);
}

static void install_stack_guard(void) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    if (sigaltstack(&ss, NULL) != 0) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

int main(void) {
    install_stack_guard();
    luma_main();
    if (fflush(stdout) != 0) return 1; /* e.g. writing to a closed pipe/full disk */
    return 0;
}
