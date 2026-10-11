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
 * Memory (see the "memory" section and docs/DESIGN.md): the compiler emits
 * luma_mm, the program's memory-management mode. Objects are reference
 * counted (count in header bits 32-63) unless their count is 0 ("not
 * counted"); the compiler inserts the retain/release operations. In
 * --mm=manual, struct instances that reach free() are not counted and are
 * freed by luma_free. LUMA_MEMCHECK=1 turns on leak reports and
 * use-after-free / double-free detection. */
#define _XOPEN_SOURCE 700 /* sigaction, sigaltstack, SA_ONSTACK, SA_SIGINFO (XSI) */
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
extern const int64_t luma_mm;           /* emitted into every program: LUMA_MM_* */
extern const uintptr_t luma_globals[];  /* emitted into every program: [count, &slot...] */

LumaValue luma_main(void);

static _Noreturn void luma_panic(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "luma: runtime error: %s\n", msg);
    exit(1);
}

static bool is_fixnum(LumaValue v) { return v & 1; }
static int64_t fixnum_val(LumaValue v) { return (int64_t)v >> 1; }

static bool is_object(LumaValue v) { return v != 0 && (v & 7) == 0; }

static uint64_t header_of(LumaValue v) { return *(const uint64_t *)(uintptr_t)v; }

__attribute__((noinline, cold, noreturn)) static void invalid_value(LumaValue v) {
    if (v == 0) luma_panic("invalid value (uninitialized slot)");
    luma_panic("Use after free."); /* a field of a freed object (LUMA_MEMCHECK), or the freed object itself */
}

/* Called on every operand of the generic operations. Only the error report
 * is out of line. */
static inline void check_valid(LumaValue v) {
    if (v & 1) return; /* fixnum */
    if (v == 0 || (v & 7) == 4 || (is_object(v) && (header_of(v) & 0xff) == LUMA_TYPE_FREED)) invalid_value(v);
}

static bool is_string(LumaValue v) {
    return is_object(v) && (((const LumaString *)(uintptr_t)v)->header & 0xff) == LUMA_TYPE_STRING;
}

static const LumaString *as_string(LumaValue v) { return (const LumaString *)(uintptr_t)v; }

static bool is_struct(LumaValue v) {
    return is_object(v) && (((const LumaString *)(uintptr_t)v)->header & 0xff) == LUMA_TYPE_STRUCT;
}

static LumaStruct *as_struct(LumaValue v) { return (LumaStruct *)(uintptr_t)v; }

/* ---- memory --------------------------------------------------------------
 * Every object is allocated by alloc_object and released by free_object.
 *
 *   counted objects    the count lives in header bits 32-63 (> 0 while the
 *                      object is alive). Compiled code retains and releases
 *                      them; at 0 the object is destroyed: its fields are
 *                      released (iteratively, so long lists cannot overflow
 *                      the C stack) and its memory is returned to malloc.
 *   uncounted objects  count 0: string literals (static), everything under
 *                      --mm=none (never freed), and in --mm=manual the struct
 *                      instances the program frees with free(). Their memory
 *                      comes from a per-size pool and is never returned to
 *                      malloc, so a stale reference still reads a header with
 *                      count 0 and retain/release on it stay no-ops.
 *
 * LUMA_MEMCHECK=1 (read at startup): freed memory is never reused; a freed
 * object's header says LUMA_TYPE_FREED and its fields read as LUMA_POISON, so
 * a later use is reported as "Use after free." At exit, objects still
 * allocated are listed as leaked or as reachable from global variables. */

static bool memcheck;
static uint64_t n_allocated, n_freed;

typedef struct {
    void **items;
    size_t n, cap;
} PtrVec;

static void vec_push(PtrVec *v, void *p) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        void **items = realloc(v->items, cap * sizeof *items);
        if (!items) luma_panic("Out of memory.");
        v->items = items;
        v->cap = cap;
    }
    v->items[v->n++] = p;
}

static PtrVec live; /* memcheck: every allocated object; its index is stored in the word before it */

static uint32_t *rc_word(const void *o) { return (uint32_t *)((char *)(uintptr_t)o + LUMA_RC_OFFSET); }

/* 8-aligned, never NULL. */
static void *alloc_object(size_t size) {
    n_allocated++;
    if (memcheck) {
        uint64_t *p = malloc(size + 8);
        if (!p) luma_panic("Out of memory.");
        p[0] = live.n;
        vec_push(&live, p + 1);
        return p + 1;
    }
    void *p = malloc(size ? size : 1);
    if (!p) luma_panic("Out of memory.");
    return p;
}

/* Uncounted struct instances of --mm=manual: free lists by field count. The
 * descriptor word of a freed instance links the list. */
static void **pool;
static int64_t npool;

static void *pool_get(int64_t nfields, size_t size) {
    if (nfields < npool && pool[nfields]) {
        LumaStruct *o = pool[nfields];
        pool[nfields] = (void *)(uintptr_t)o->desc;
        n_allocated++;
        return o;
    }
    return alloc_object(size);
}

static void pool_put(LumaStruct *o, int64_t nfields) {
    if (nfields >= npool) {
        int64_t n = nfields + 16;
        void **p = realloc(pool, (size_t)n * sizeof *p);
        if (!p) luma_panic("Out of memory.");
        for (int64_t i = npool; i < n; i++) p[i] = NULL;
        pool = p;
        npool = n;
    }
    o->desc = (const LumaStructDesc *)pool[nfields];
    pool[nfields] = o;
}

/* Marks o freed. Under memcheck the memory is kept (quarantined), poisoned,
 * and leaves the live set; otherwise it goes back to malloc or the pool. */
static void free_object(void *o, bool to_pool, int64_t nfields) {
    n_freed++;
    uint64_t *h = o;
    if (memcheck) {
        size_t i = (size_t)((uint64_t *)o)[-1];
        void *last = live.items[--live.n];
        if (last != o) {
            live.items[i] = last;
            ((uint64_t *)last)[-1] = i;
        }
        if ((*h & 0xff) == LUMA_TYPE_STRUCT)
            for (int64_t k = 0; k < nfields; k++) ((LumaStruct *)o)->fields[k] = LUMA_POISON;
        *h = LUMA_TYPE_FREED;
        return;
    }
    *h = LUMA_TYPE_FREED; /* count 0 */
    if (to_pool) pool_put(o, nfields);
    else free(o);
}

/* Objects whose count reached 0, waiting to be destroyed. */
static PtrVec dying;
static bool draining;

/* Drops one counted reference held by a field or slot that is going away. */
static void drop(LumaValue v) {
    if (!is_object(v)) return;
    uint32_t *c = rc_word((void *)(uintptr_t)v);
    if (*c != 0 && --*c == 0) vec_push(&dying, (void *)(uintptr_t)v);
}

/* Destroys every object in `dying`, and those their fields release in turn. */
static void drain(void) {
    if (draining) return;
    draining = true;
    while (dying.n > 0) {
        void *o = dying.items[--dying.n];
        if ((*(uint64_t *)o & 0xff) == LUMA_TYPE_STRUCT) {
            LumaStruct *s = o;
            int64_t n = s->desc->nfields;
            for (int64_t k = 0; k < n; k++) drop(s->fields[k]);
            free_object(o, false, n);
        } else {
            free_object(o, false, 0);
        }
    }
    draining = false;
}

/* Compiled code: the inline retain/release handle counted objects with
 * count > 1 themselves and call luma_rc_free for the last reference;
 * luma_retain / luma_release do the whole job (-O0 and hand-written LIR). */
void luma_retain(LumaValue v) {
    if (!is_object(v)) return;
    uint32_t *c = rc_word((void *)(uintptr_t)v);
    if (*c != 0) ++*c;
}

void luma_release(LumaValue v) {
    drop(v);
    drain();
}

/* The last reference to the counted object v is gone. */
void luma_rc_free(LumaValue v) {
    *rc_word((void *)(uintptr_t)v) = 0;
    vec_push(&dying, (void *)(uintptr_t)v);
    drain();
}

static LumaValue make_fixnum_checked(int64_t n) {
    if (n < LUMA_FIXNUM_MIN || n > LUMA_FIXNUM_MAX) luma_panic("Integer overflow.");
    return luma_fixnum(n);
}

static LumaValue make_bool(bool b) { return b ? LUMA_TRUE : LUMA_FALSE; }

__attribute__((noinline, cold, noreturn)) static void not_numbers(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    luma_panic("Operands must be numbers.");
}

/* Both operands fixnums, or a runtime error (the check stays small inline). */
static inline void need_numbers(LumaValue a, LumaValue b) {
    if (!(a & b & 1)) not_numbers(a, b);
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

/* A new string of n bytes (plus NUL), counted unless --mm=none. */
static LumaString *new_string(size_t n) {
    LumaString *s = alloc_object((sizeof *s + n + 1 + 7) & ~(size_t)7);
    s->header = LUMA_TYPE_STRING | LUMA_STRING_HEAP | ((uint64_t)(luma_mm != LUMA_MM_NONE) << 32);
    s->len = n;
    s->bytes[n] = '\0';
    return s;
}

static LumaValue string_from_c(const char *p) {
    size_t n = strlen(p);
    LumaString *s = new_string(n);
    memcpy(s->bytes, p, n);
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

/* `counted` is decided by the compiler: always in --mm=rc, never in
 * --mm=none, and in --mm=manual for instances its escape analysis proves are
 * never freed by the program (the rest come from the manual pool). */
LumaValue luma_new_struct(const LumaStructDesc *d, int64_t counted) {
    size_t size = sizeof(LumaStruct) + (size_t)d->nfields * sizeof(LumaValue);
    LumaStruct *o = !counted && luma_mm == LUMA_MM_MANUAL && !memcheck ? pool_get(d->nfields, size) : alloc_object(size);
    o->header = LUMA_TYPE_STRUCT | ((uint64_t)d->id << LUMA_STRUCT_ID_SHIFT) | ((uint64_t)(counted != 0) << 32);
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
    LumaValue old = s->fields[k];
    s->fields[k] = v; /* the field takes over the caller's reference to v */
    luma_release(old);
    return LUMA_NIL;
}

/* free(v): in --mm=manual, frees a struct instance and releases the values
 * in its fields; in the other modes it only checks its argument (the object
 * is reclaimed automatically, or never). free(nil) does nothing. */
LumaValue luma_free(LumaValue v) {
    if (is_object(v) && (header_of(v) & 0xff) == LUMA_TYPE_FREED) luma_panic("Double free: this object was already freed.");
    check_valid(v);
    if (v == LUMA_NIL) return LUMA_NIL;
    if (is_string(v)) luma_panic("free() expects a struct instance, got str (strings are freed automatically).");
    if (!is_struct(v)) panicf("free() expects a struct instance, got %s.", value_type_name(v));
    if (luma_mm != LUMA_MM_MANUAL) return LUMA_NIL;
    LumaStruct *o = as_struct(v);
    if (*rc_word(o) != 0) /* the compiler only counts instances that never reach free() */
        panicf("internal error: free() of an automatically managed %s.", o->desc->name);
    int64_t n = o->desc->nfields;
    for (int64_t k = 0; k < n; k++) drop(o->fields[k]);
    free_object(o, true, n);
    drain();
    return LUMA_NIL;
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

static LumaValue add_slow(LumaValue a, LumaValue b);

LumaValue luma_add(LumaValue a, LumaValue b) {
    if (is_fixnum(a) && is_fixnum(b)) { /* the common case stays small: no registers to save */
        int64_t r;
        if (__builtin_add_overflow(fixnum_val(a), fixnum_val(b), &r)) luma_panic("Integer overflow.");
        return make_fixnum_checked(r);
    }
    return add_slow(a, b);
}

__attribute__((noinline)) static LumaValue add_slow(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    if (is_string(a) && is_string(b)) {
        const LumaString *x = as_string(a), *y = as_string(b);
        if (x->len > SIZE_MAX / 2 || y->len > SIZE_MAX / 2) luma_panic("String too long.");
        LumaString *s = new_string(x->len + y->len); /* a new object: the caller owns it */
        memcpy(s->bytes, x->bytes, x->len);
        memcpy(s->bytes + x->len, y->bytes, y->len);
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

__attribute__((noinline, cold, noreturn)) static void not_number(LumaValue a) {
    check_valid(a);
    luma_panic("Operand must be a number.");
}

LumaValue luma_neg(LumaValue a) {
    if (!is_fixnum(a)) not_number(a);
    return make_fixnum_checked(-fixnum_val(a));
}

LumaValue luma_not(LumaValue a) {
    check_valid(a);
    return make_bool((a & LUMA_FALSY_MASK) == 2);
}

/* ---- comparison -------------------------------------------------------- */

__attribute__((noinline)) static bool values_equal_slow(LumaValue a, LumaValue b) {
    check_valid(a);
    check_valid(b);
    if (a == b) return true;
    if (is_string(a) && is_string(b)) {
        const LumaString *x = as_string(a), *y = as_string(b);
        return x->len == y->len && memcmp(x->bytes, y->bytes, x->len) == 0;
    }
    return false;
}

static inline bool values_equal(LumaValue a, LumaValue b) {
    if (a & b & 1) return a == b; /* two fixnums */
    return values_equal_slow(a, b);
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

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    (void)ctx;
    static const char overflow[] = "luma: runtime error: Stack overflow.\n";
    static const char freed[] = "luma: runtime error: Use after free.\n";
    /* under LUMA_MEMCHECK, a field of a freed object holds LUMA_POISON; code
     * that trusts its static type and dereferences it faults near it */
    uintptr_t a = (uintptr_t)si->si_addr;
    bool poison = a >= (uintptr_t)LUMA_POISON - 4 && a < (uintptr_t)LUMA_POISON + 0x100000;
    fflush(stdout); /* best effort: keep output printed before the fault */
    ssize_t r = poison ? write(2, freed, sizeof freed - 1) : write(2, overflow, sizeof overflow - 1);
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
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

/* ---- LUMA_MEMCHECK report ----------------------------------------------- */

static const char *mm_name(void) {
    return luma_mm == LUMA_MM_RC ? "rc" : luma_mm == LUMA_MM_MANUAL ? "manual" : "none";
}

/* Live objects reachable from the global variables get marked[index] = 1. */
static void mark_from(LumaValue v, unsigned char *marked, PtrVec *stack) {
    if (!is_object(v)) return;
    vec_push(stack, (void *)(uintptr_t)v);
    while (stack->n > 0) {
        void *o = stack->items[--stack->n];
        if ((*(uint64_t *)o & 0xff) == LUMA_TYPE_FREED) continue;
        /* string literals are static: only allocated objects carry an index */
        uint64_t h = *(uint64_t *)o;
        if ((h & 0xff) == LUMA_TYPE_STRING && !(h & LUMA_STRING_HEAP)) continue;
        uint64_t i = ((uint64_t *)o)[-1];
        if (i >= live.n || live.items[i] != o || marked[i]) continue;
        marked[i] = 1;
        if ((h & 0xff) == LUMA_TYPE_STRUCT) {
            const LumaStruct *s = o;
            for (int64_t k = 0; k < s->desc->nfields; k++)
                if (is_object(s->fields[k])) vec_push(stack, (void *)(uintptr_t)s->fields[k]);
        }
    }
}

/* "2 Node, 1 str" for the live objects whose mark equals `which`. */
static void describe_objects(const unsigned char *marked, unsigned char which, char *buf, size_t n) {
    uint64_t nstructs = luma_structs[0];
    uint64_t *counts = calloc(nstructs + 2, sizeof *counts);
    if (!counts) luma_panic("Out of memory.");
    for (size_t i = 0; i < live.n; i++) {
        if (marked[i] != which) continue;
        const uint64_t *h = live.items[i];
        if ((*h & 0xff) == LUMA_TYPE_STRUCT) counts[((const LumaStruct *)h)->desc->id]++;
        else counts[nstructs + 1]++;
    }
    buf[0] = '\0';
    for (uint64_t id = 1; id <= nstructs + 1; id++) {
        if (!counts[id]) continue;
        size_t len = strlen(buf);
        snprintf(buf + len, n - len, "%s%" PRIu64 " %s", len ? ", " : "", counts[id], id <= nstructs ? struct_name((unsigned)id) : "str");
    }
    free(counts);
}

static void memcheck_report(void) {
    unsigned char *marked = calloc(live.n + 1, 1);
    PtrVec stack = {0};
    if (!marked) luma_panic("Out of memory.");
    for (uint64_t g = 1; g <= luma_globals[0]; g++) mark_from(*(const LumaValue *)luma_globals[g], marked, &stack);
    free(stack.items);
    size_t reachable = 0;
    for (size_t i = 0; i < live.n; i++) reachable += marked[i];
    size_t leaked = live.n - reachable;
    fprintf(stderr, "luma: memcheck: --mm=%s, %" PRIu64 " allocated, %" PRIu64 " freed, %zu leaked, %zu reachable from globals\n",
            mm_name(), n_allocated, n_freed, leaked, reachable);
    char buf[4096];
    if (leaked) {
        describe_objects(marked, 0, buf, sizeof buf);
        fprintf(stderr, "luma: memcheck: leaked: %s\n", buf);
    }
    if (reachable) {
        describe_objects(marked, 1, buf, sizeof buf);
        fprintf(stderr, "luma: memcheck: reachable from globals: %s\n", buf);
    }
    free(marked);
}

int main(void) {
    install_stack_guard();
    const char *mc = getenv("LUMA_MEMCHECK");
    memcheck = mc && *mc && strcmp(mc, "0") != 0;
    luma_main();
    if (fflush(stdout) != 0) return 1; /* e.g. writing to a closed pipe/full disk */
    if (memcheck) memcheck_report();
    return 0;
}
