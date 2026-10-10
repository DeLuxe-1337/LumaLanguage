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
 * Memory: strings created at runtime (concatenation) and struct instances
 * are allocated through luma_alloc (malloc) and never freed: there is no
 * collector yet. Every allocation goes through luma_alloc, and every object
 * header names its layout, so a collector can be added behind them. */
#define _XOPEN_SOURCE 700 /* sigaction, sigaltstack, SA_ONSTACK (XSI) */
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
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

/* Struct descriptors are emitted by the compiler (src/x86_isel.c,
 * x86_emit_data) into .data; luma_structs lists them: [count, desc...]. */
typedef LumaValue (*LumaFn)(void); /* a compiled Luma function (called with its real arguments) */

typedef struct {
    const char *name;
    LumaFn fn;             /* called with `arity` LumaValue arguments */
    int64_t arity;         /* including self */
} LumaMethod;

typedef struct {
    int64_t id;            /* struct index + 1, as in instance headers */
    const char *name;
    int64_t nfields;
    const char *const *fields;
    const int64_t *ftys;   /* declared type mask of each field */
    int64_t nmethods;
    const LumaMethod *methods;
} LumaStructDesc;

typedef struct {
    uint64_t header;
    const LumaStructDesc *desc;
    LumaValue fields[];
} LumaStruct;

extern const uintptr_t luma_structs[]; /* emitted into every program: [count, desc...] */

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

static bool is_struct(LumaValue v) {
    return is_object(v) && (((const LumaString *)(uintptr_t)v)->header & 0xff) == LUMA_TYPE_STRUCT;
}

static LumaStruct *as_struct(LumaValue v) { return (LumaStruct *)(uintptr_t)v; }

/* Every heap allocation of the runtime: 8-aligned, never NULL. */
static void *luma_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) luma_panic("Out of memory.");
    return p;
}

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

/* ---- typed boundaries (gradual typing) ---------------------------------
 * Static type masks used by the compiler: int = 1, str = 2, bool = 4, nil = 8.
 * The compiler inserts luma_check_type(v, mask, context) wherever a value it
 * cannot type statically ("any") flows into an annotated variable, parameter
 * or return value. */

enum { MASK_INT = 1, MASK_STR = 2, MASK_BOOL = 4, MASK_NIL = 8, MASK_STRUCT = 16 };
#define MASK_SID_SHIFT 8 /* a mask's struct bits: 0 = any struct, k = struct with id k */

static _Noreturn void panicf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static _Noreturn void panicf(const char *fmt, ...) {
    fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    fputs("luma: runtime error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static unsigned value_mask(LumaValue v) {
    if (is_fixnum(v)) return MASK_INT;
    if (v == LUMA_NIL) return MASK_NIL;
    if (v == LUMA_TRUE || v == LUMA_FALSE) return MASK_BOOL;
    if (is_string(v)) return MASK_STR;
    if (is_struct(v)) return MASK_STRUCT | (unsigned)(as_struct(v)->desc->id << MASK_SID_SHIFT);
    return 0;
}

/* Does a value of mask vm (from value_mask) belong to type mask m? */
static bool mask_accepts(unsigned m, unsigned vm) {
    if (vm & MASK_STRUCT) {
        unsigned want = m >> MASK_SID_SHIFT;
        return (m & MASK_STRUCT) && (want == 0 || want == vm >> MASK_SID_SHIFT);
    }
    return (vm & m & 15) != 0;
}

/* Name of the struct with id k (index + 1). */
static const char *struct_name(unsigned id) {
    if (id >= 1 && id <= luma_structs[0])
        return ((const LumaStructDesc *)luma_structs[id])->name;
    return "?struct";
}

static const char *value_type_name(LumaValue v) {
    unsigned vm = value_mask(v);
    if (vm & MASK_STRUCT) return as_struct(v)->desc->name;
    switch (vm) {
    case MASK_INT: return "int";
    case MASK_STR: return "str";
    case MASK_BOOL: return "bool";
    case MASK_NIL: return "nil";
    default: return "unknown";
    }
}

/* "int", "str?", "int or str", "Point?", ... (the same spelling the compiler uses) */
static void describe_mask(unsigned mask, char *buf, size_t n) {
    static const struct { unsigned bit; const char *name; } T[] = {
        {MASK_INT, "int"}, {MASK_STR, "str"}, {MASK_BOOL, "bool"}, {MASK_NIL, "nil"}};
    unsigned sid = mask >> MASK_SID_SHIFT;
    const char *sname = (mask & MASK_STRUCT) ? (sid ? struct_name(sid) : "struct") : NULL;
    if ((mask & 31) == 31 && sid == 0) { snprintf(buf, n, "any"); return; }
    unsigned rest = mask & (MASK_INT | MASK_STR | MASK_BOOL);
    int members = __builtin_popcount(rest) + (sname ? 1 : 0);
    if ((mask & MASK_NIL) && members == 1) { /* exactly one type plus nil: T? */
        const char *one = sname;
        for (int i = 0; i < 3 && !one; i++)
            if (T[i].bit == rest) one = T[i].name;
        snprintf(buf, n, "%s?", one);
        return;
    }
    buf[0] = '\0';
    for (int i = 0; i < 5; i++) {
        const char *name = i < 4 ? ((mask & T[i].bit) ? T[i].name : NULL) : sname;
        if (!name) continue;
        if (buf[0]) strncat(buf, " or ", n - strlen(buf) - 1);
        strncat(buf, name, n - strlen(buf) - 1);
    }
}

LumaValue luma_check_type(LumaValue v, LumaValue mask, LumaValue context) {
    check_valid(v);
    unsigned m = (unsigned)fixnum_val(mask);
    if (mask_accepts(m, value_mask(v))) return v; /* the optimizing backend uses the result */
    char want[300];
    describe_mask(m, want, sizeof want);
    const char *ctx = is_string(context) ? as_string(context)->bytes : "value";
    panicf("%s expects %s, got %s.", ctx, want, value_type_name(v));
}

/* ---- C FFI converters ----------------------------------------------------
 * For a call to an `extern fun`, the compiler converts each argument with
 * luma_ffi_arg_<ctype>(value, "argument N ('p') of f") and the result with
 * luma_ffi_ret_<ctype>(raw, "return value of f"). Values that do not fit the
 * C type are runtime errors, never silent truncation. */

static int64_t ffi_int(LumaValue v, const char *ctx, int64_t lo, int64_t hi, const char *cname) {
    check_valid(v);
    if (!is_fixnum(v)) panicf("%s expects int, got %s.", ctx, value_type_name(v));
    int64_t n = fixnum_val(v);
    if (n < lo || n > hi) panicf("%s: %" PRId64 " does not fit in %s.", ctx, n, cname);
    return n;
}

uint64_t luma_ffi_arg_i8(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, INT8_MIN, INT8_MAX, "i8"); }
uint64_t luma_ffi_arg_i16(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, INT16_MIN, INT16_MAX, "i16"); }
uint64_t luma_ffi_arg_i32(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, INT32_MIN, INT32_MAX, "i32"); }
uint64_t luma_ffi_arg_i64(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, INT64_MIN, INT64_MAX, "i64"); }
uint64_t luma_ffi_arg_u8(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, 0, UINT8_MAX, "u8"); }
uint64_t luma_ffi_arg_u16(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, 0, UINT16_MAX, "u16"); }
uint64_t luma_ffi_arg_u32(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, 0, UINT32_MAX, "u32"); }
uint64_t luma_ffi_arg_u64(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, 0, INT64_MAX, "u64"); }
uint64_t luma_ffi_arg_ptr(LumaValue v, const char *c) { return (uint64_t)ffi_int(v, c, 0, INT64_MAX, "ptr"); }

uint64_t luma_ffi_arg_bool(LumaValue v, const char *c) {
    check_valid(v);
    if (v == LUMA_TRUE) return 1;
    if (v == LUMA_FALSE) return 0;
    panicf("%s expects bool, got %s.", c, value_type_name(v));
}

uint64_t luma_ffi_arg_cstr(LumaValue v, const char *c) {
    check_valid(v);
    if (!is_string(v)) panicf("%s expects str, got %s.", c, value_type_name(v));
    return (uint64_t)(uintptr_t)as_string(v)->bytes; /* NUL-terminated; C must not modify it */
}

uint64_t luma_ffi_arg_cstr_opt(LumaValue v, const char *c) {
    check_valid(v);
    if (v == LUMA_NIL) return 0;
    if (!is_string(v)) panicf("%s expects str?, got %s.", c, value_type_name(v));
    return (uint64_t)(uintptr_t)as_string(v)->bytes;
}

static LumaValue ffi_ret_int(int64_t n, const char *ctx) {
    if (n < LUMA_FIXNUM_MIN || n > LUMA_FIXNUM_MAX)
        panicf("%s: %" PRId64 " does not fit in a Luma int (63 bits).", ctx, n);
    return luma_fixnum(n);
}

/* Narrow C return values: only the low bits of rax are defined (SysV), so
 * every converter truncates to the declared width first. */
LumaValue luma_ffi_ret_i8(uint64_t r, const char *c) { (void)c; return luma_fixnum((int8_t)r); }
LumaValue luma_ffi_ret_i16(uint64_t r, const char *c) { (void)c; return luma_fixnum((int16_t)r); }
LumaValue luma_ffi_ret_i32(uint64_t r, const char *c) { (void)c; return luma_fixnum((int32_t)r); }
LumaValue luma_ffi_ret_u8(uint64_t r, const char *c) { (void)c; return luma_fixnum((uint8_t)r); }
LumaValue luma_ffi_ret_u16(uint64_t r, const char *c) { (void)c; return luma_fixnum((uint16_t)r); }
LumaValue luma_ffi_ret_u32(uint64_t r, const char *c) { (void)c; return luma_fixnum((uint32_t)r); }
LumaValue luma_ffi_ret_i64(uint64_t r, const char *c) { return ffi_ret_int((int64_t)r, c); }

LumaValue luma_ffi_ret_u64(uint64_t r, const char *c) {
    if (r > (uint64_t)LUMA_FIXNUM_MAX) panicf("%s: %" PRIu64 " does not fit in a Luma int (63 bits).", c, r);
    return luma_fixnum((int64_t)r);
}

LumaValue luma_ffi_ret_ptr(uint64_t r, const char *c) {
    if (r > (uint64_t)LUMA_FIXNUM_MAX) panicf("%s: pointer 0x%" PRIx64 " does not fit in a Luma int.", c, r);
    return luma_fixnum((int64_t)r);
}

LumaValue luma_ffi_ret_bool(uint64_t r, const char *c) { (void)c; return make_bool((r & 0xff) != 0); }

static LumaValue string_from_c(const char *p) {
    size_t n = strlen(p);
    LumaString *s = luma_alloc((sizeof *s + n + 1 + 7) & ~(size_t)7);
    s->header = LUMA_TYPE_STRING;
    s->len = n;
    memcpy(s->bytes, p, n + 1);
    return (LumaValue)(uintptr_t)s;
}

LumaValue luma_ffi_ret_cstr(uint64_t r, const char *c) {
    if (!r) panicf("%s is NULL (declare the return type as cstr? to accept NULL).", c);
    return string_from_c((const char *)(uintptr_t)r); /* copied: C keeps ownership of its buffer */
}

LumaValue luma_ffi_ret_cstr_opt(uint64_t r, const char *c) {
    (void)c;
    return r ? string_from_c((const char *)(uintptr_t)r) : LUMA_NIL;
}

/* ---- printing ----------------------------------------------------------
 * The builtin print(a, b, ...) lowers to
 *     luma_write(a); luma_write_space(); luma_write(b); ...; luma_write_newline();
 * luma_print(v) (= write + newline) is kept for hand-written LIR. */

/* Structs print as  Point { x: 1, label: "hi" }  (strings inside quoted). An
 * instance that is already being printed (a cycle) prints as  Point { ... }. */
#define MAX_PRINT_DEPTH 64
static const LumaStruct *printing[MAX_PRINT_DEPTH];
static int nprinting;

static void write_value(LumaValue v, bool quoted) {
    check_valid(v);
    if (is_fixnum(v)) printf("%" PRId64, fixnum_val(v));
    else if (v == LUMA_NIL) fputs("nil", stdout);
    else if (v == LUMA_TRUE) fputs("true", stdout);
    else if (v == LUMA_FALSE) fputs("false", stdout);
    else if (is_string(v)) {
        const LumaString *s = as_string(v);
        if (!quoted) {
            fwrite(s->bytes, 1, s->len, stdout);
            return;
        }
        putchar('"');
        for (uint64_t i = 0; i < s->len; i++) {
            char c = s->bytes[i];
            if (c == '"' || c == '\\') printf("\\%c", c);
            else if (c == '\n') fputs("\\n", stdout);
            else if (c == '\t') fputs("\\t", stdout);
            else putchar(c);
        }
        putchar('"');
    } else if (is_struct(v)) {
        const LumaStruct *o = as_struct(v);
        bool cycle = nprinting == MAX_PRINT_DEPTH;
        for (int i = 0; i < nprinting && !cycle; i++) cycle = printing[i] == o;
        if (cycle) {
            printf("%s { ... }", o->desc->name);
            return;
        }
        if (o->desc->nfields == 0) {
            printf("%s {}", o->desc->name);
            return;
        }
        printing[nprinting++] = o;
        printf("%s { ", o->desc->name);
        for (int64_t i = 0; i < o->desc->nfields; i++) {
            printf("%s%s: ", i ? ", " : "", o->desc->fields[i]);
            write_value(o->fields[i], true);
        }
        fputs(" }", stdout);
        nprinting--;
    } else luma_panic("cannot print value of unknown type");
}

LumaValue luma_write(LumaValue v) {
    write_value(v, false);
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

/* Error exits of the optimizing backend's inline fast paths (noreturn;
 * called with the stack aligned, from out-of-line stubs). */
_Noreturn void luma_int_overflow(void) { luma_panic("Integer overflow."); }
_Noreturn void luma_div_zero(void) { luma_panic("Division by zero."); }

/* Called by a checked `load` of a global slot that was never assigned. */
_Noreturn void luma_undefined_variable(const char *name) {
    fflush(stdout);
    fprintf(stderr, "luma: runtime error: Undefined variable '%s'.\n", name);
    exit(1);
}

/* ---- structs -----------------------------------------------------------
 * `new` allocates with luma_new_struct and the compiler stores the fields;
 * accesses whose struct is known statically are plain loads and stores. The
 * functions below serve values whose struct is not known at compile time. */

LumaValue luma_new_struct(const LumaStructDesc *d) {
    LumaStruct *o = luma_alloc(sizeof *o + (size_t)d->nfields * sizeof(LumaValue));
    o->header = LUMA_TYPE_STRUCT | ((uint64_t)d->id << LUMA_STRUCT_ID_SHIFT);
    o->desc = d;
    for (int64_t i = 0; i < d->nfields; i++) o->fields[i] = LUMA_NIL;
    return (LumaValue)(uintptr_t)o;
}

static LumaStruct *need_instance(LumaValue o, const char *what) {
    check_valid(o);
    if (!is_struct(o)) panicf("Only struct instances have %s, got %s.", what, value_type_name(o));
    return as_struct(o);
}

static int64_t field_index(const LumaStruct *s, const char *name) {
    for (int64_t i = 0; i < s->desc->nfields; i++)
        if (strcmp(s->desc->fields[i], name) == 0) return i;
    panicf("Struct '%s' has no field '%s'.", s->desc->name, name);
}

LumaValue luma_getfield(LumaValue o, const char *name) {
    LumaStruct *s = need_instance(o, "fields");
    return s->fields[field_index(s, name)];
}

LumaValue luma_setfield(LumaValue o, const char *name, LumaValue v) {
    LumaStruct *s = need_instance(o, "fields");
    int64_t k = field_index(s, name);
    check_valid(v);
    unsigned m = (unsigned)s->desc->ftys[k];
    if (!mask_accepts(m, value_mask(v))) {
        char want[300];
        describe_mask(m, want, sizeof want);
        panicf("field '%s' of '%s' expects %s, got %s.", name, s->desc->name, want, value_type_name(v));
    }
    s->fields[k] = v;
    return v;
}

/* The code of method `name` of o's struct, for a call with nargs arguments
 * after self. */
LumaFn luma_method(LumaValue o, const char *name, int64_t nargs) {
    LumaStruct *s = need_instance(o, "methods");
    for (int64_t i = 0; i < s->desc->nmethods; i++) {
        const LumaMethod *mt = &s->desc->methods[i];
        if (strcmp(mt->name, name) != 0) continue;
        if (mt->arity - 1 != nargs)
            panicf("'%s::%s' expects %" PRId64 " argument%s, got %" PRId64 ".", s->desc->name, name, mt->arity - 1,
                   mt->arity == 2 ? "" : "s", nargs);
        return mt->fn;
    }
    panicf("Struct '%s' has no method '%s'.", s->desc->name, name);
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
        LumaString *s = luma_alloc((sizeof *s + n + 1 + 7) & ~(size_t)7);
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
