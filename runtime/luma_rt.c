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

typedef struct LumaStructDesc LumaStructDesc;
struct LumaStructDesc {
    int64_t id;            /* struct index + 1, as in instance headers */
    const char *name;      /* as shown in messages: "Point", "Rc<Point>", "Arena" */
    int64_t nfields;
    const char *const *fields;
    const int64_t *ftys;   /* declared type mask of each field */
    int64_t nmethods;
    const LumaMethod *methods;
    int64_t kind;          /* LUMA_STRUCT_USER / _RC (field 0: the value) / _ARENA (field 0: its state) */
    const LumaStructDesc *rc; /* user structs: the Rc<S> descriptor (NULL if the program uses no Rc) */
    LumaFn drop;           /* the struct's drop(self) method, called when an instance dies; or NULL */
    int64_t flags;         /* LUMA_STRUCT_SHARED */
};

typedef struct {
    uint64_t header;
    const LumaStructDesc *desc; /* bit 0 set: the instance lives in an arena */
    LumaValue fields[];
} LumaStruct;

static const LumaStructDesc *desc_of(const LumaStruct *o) {
    return (const LumaStructDesc *)((uintptr_t)o->desc & ~(uintptr_t)7);
}

static bool in_arena(const LumaStruct *o) { return (uintptr_t)o->desc & 1; }

extern const uintptr_t luma_structs[]; /* emitted into every program: [count, desc...] */
extern const int64_t luma_mm;           /* emitted into every program: LUMA_MM_* */
extern const uintptr_t luma_globals[];  /* emitted into every program: [count, &slot...] */

LumaValue luma_main(void);

static _Noreturn void panicf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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

/* ---- arenas ----
 * An Arena handle is a counted struct instance (kind LUMA_STRUCT_ARENA)
 * whose field 0 holds its state as (pointer | 1), or the fixnum 0 before
 * first use. While an arena is entered (`arena { ... }`), every struct
 * instance the program would otherwise free by hand is allocated in it,
 * marked by bit 0 of its descriptor word; free() on such an instance does
 * nothing. When the handle's last reference goes away, the values in every
 * instance's fields are released and the memory goes back in one piece. */

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used, cap;
    uint64_t data[];
} ArenaChunk;

typedef struct {
    ArenaChunk *chunks; /* every chunk, newest first */
    ArenaChunk *cur;    /* the chunk small objects are bumped from */
    PtrVec objs;        /* LUMA_MEMCHECK: the instances (allocated individually) */
    uint64_t nobjects;
    bool any_drop;      /* some instance has a drop method */
} ArenaState;

#define ARENA_CHUNK (64 * 1024)

static ArenaState *arena_state(LumaStruct *h) {
    LumaValue f = h->fields[0];
    if (f != luma_fixnum(0)) return (ArenaState *)(uintptr_t)(f & ~(LumaValue)1);
    ArenaState *a = calloc(1, sizeof *a);
    if (!a) luma_panic("Out of memory.");
    h->fields[0] = (LumaValue)(uintptr_t)a | 1;
    return a;
}

static void *arena_alloc(ArenaState *a, size_t size) {
    a->nobjects++;
    if (memcheck) {
        void *o = alloc_object(size);
        vec_push(&a->objs, o);
        return o;
    }
    n_allocated++;
    if (!a->cur || a->cur->cap - a->cur->used < size) {
        bool big = size > ARENA_CHUNK / 4;
        size_t cap = big ? size : ARENA_CHUNK;
        ArenaChunk *c = malloc(sizeof *c + cap);
        if (!c) luma_panic("Out of memory.");
        c->used = 0;
        c->cap = cap;
        c->next = a->chunks;
        a->chunks = c;
        if (!big) a->cur = c; /* a big object gets a chunk of its own */
        if (big) {
            c->used = size;
            return c->data;
        }
    }
    void *o = (char *)a->cur->data + a->cur->used;
    a->cur->used += size;
    return o;
}

static size_t instance_size(int64_t nfields) { return sizeof(LumaStruct) + (size_t)nfields * sizeof(LumaValue); }

/* ---- drop ----
 * A struct's drop(self) runs once per instance, right before its fields are
 * released and its memory freed: when a counted instance's count reaches 0,
 * when free()/dispose() frees one managed by hand, or when its arena is
 * destroyed. While it runs, a counted instance's count is held at 1, so
 * drop's own retains and releases of self balance; anything else means drop
 * kept a reference to an instance that is about to disappear. */
typedef LumaValue (*LumaDropFn)(LumaValue self);
#define MAX_DROP_DEPTH 1024
static const LumaStruct *dropping[MAX_DROP_DEPTH]; /* instances whose drop is running */
static int ndropping;

static bool is_dropping(const LumaStruct *o) {
    for (int i = 0; i < ndropping; i++)
        if (dropping[i] == o) return true;
    return false;
}

static void drop(LumaValue v);

static void run_drop(LumaStruct *o, bool counted) {
    const LumaStructDesc *d = desc_of(o);
    if (!d->drop) return;
    if (ndropping == MAX_DROP_DEPTH) luma_panic("drop() calls nested too deeply.");
    dropping[ndropping++] = o;
    uint32_t *c = rc_word(o);
    if (counted) *c = 1;
    LumaValue r = ((LumaDropFn)(void (*)(void))d->drop)((LumaValue)(uintptr_t)o);
    ndropping--;
    if (counted) {
        if (*c != 1) panicf("%s.drop() kept a reference to self.", d->name);
        *c = 0;
    }
    drop(r); /* (a result is owned; drop normally returns nil) */
}

/* The handle's last reference is gone: release what the instances refer
 * to, then free them all. */
static void arena_destroy(LumaValue state) {
    if (state == luma_fixnum(0)) return; /* never used */
    ArenaState *a = (ArenaState *)(uintptr_t)(state & ~(LumaValue)1);
    if (a->any_drop) { /* the instances' drop methods first, newest first */
        PtrVec all = {0}; /* in allocation order (oldest first) */
        if (memcheck) {
            for (size_t i = 0; i < a->objs.n; i++) vec_push(&all, a->objs.items[i]);
        } else {
            PtrVec chunks = {0};
            for (ArenaChunk *c = a->chunks; c; c = c->next) vec_push(&chunks, c);
            for (size_t k = chunks.n; k-- > 0;) { /* the list is newest first */
                ArenaChunk *c = chunks.items[k];
                for (size_t off = 0; off < c->used;) {
                    LumaStruct *o = (LumaStruct *)((char *)c->data + off);
                    vec_push(&all, o);
                    off += instance_size(desc_of(o)->nfields);
                }
            }
            free(chunks.items);
        }
        for (size_t i = all.n; i-- > 0;) run_drop(all.items[i], false);
        free(all.items);
    }
    if (memcheck) {
        for (size_t i = 0; i < a->objs.n; i++) {
            LumaStruct *o = a->objs.items[i];
            int64_t n = desc_of(o)->nfields;
            for (int64_t k = 0; k < n; k++) drop(o->fields[k]);
            free_object(o, false, n);
        }
        free(a->objs.items);
    } else {
        for (ArenaChunk *c = a->chunks; c;) {
            for (size_t off = 0; off < c->used;) {
                LumaStruct *o = (LumaStruct *)((char *)c->data + off);
                int64_t n = desc_of(o)->nfields;
                for (int64_t k = 0; k < n; k++) drop(o->fields[k]);
                off += instance_size(n);
            }
            ArenaChunk *next = c->next;
            free(c);
            c = next;
        }
        n_freed += a->nobjects;
    }
    free(a);
}

/* The arenas entered, innermost last (each holds a reference to its handle). */
static LumaValue *arena_stack;
static size_t narena_stack, carena_stack;

static void destroy_owned(LumaValue v);

/* Destroys every object in `dying`, and those their fields release in turn. */
static void drain(void) {
    if (draining) return;
    draining = true;
    while (dying.n > 0) {
        void *o = dying.items[--dying.n];
        if ((*(uint64_t *)o & 0xff) == LUMA_TYPE_STRUCT) {
            LumaStruct *s = o;
            run_drop(s, true);
            const LumaStructDesc *d = desc_of(s);
            int64_t n = d->nfields;
            if (d->kind == LUMA_STRUCT_RC) destroy_owned(s->fields[0]); /* an Rc owns its value */
            else if (d->kind == LUMA_STRUCT_ARENA) arena_destroy(s->fields[0]);
            else
                for (int64_t k = 0; k < n; k++) drop(s->fields[k]);
            free_object(o, false, n);
        } else {
            free_object(o, false, 0);
        }
    }
    draining = false;
}

/* Frees a struct instance the program manages by hand (count 0, not in an
 * arena): releases its fields, then its memory goes back to the pool. */
static void free_manual(LumaStruct *o) {
    run_drop(o, false);
    int64_t n = desc_of(o)->nfields;
    for (int64_t k = 0; k < n; k++) drop(o->fields[k]);
    free_object(o, true, n);
}

/* The value of an Rc whose last reference went away: an instance managed
 * by hand is freed (the Rc owned it), anything counted is released, an
 * instance in an arena stays (the arena owns it). */
static void destroy_owned(LumaValue v) {
    if (!is_object(v)) return;
    uint64_t h = header_of(v);
    if ((h & 0xff) == LUMA_TYPE_FREED) luma_panic("Double free: the value in an Rc was already freed.");
    LumaStruct *o = (LumaStruct *)(uintptr_t)v;
    if ((h & 0xff) == LUMA_TYPE_STRUCT && *rc_word(o) == 0) {
        if (!in_arena(o) && luma_mm == LUMA_MM_MANUAL) free_manual(o);
        return;
    }
    drop(v);
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
    if (is_struct(v)) return MASK_STRUCT | (unsigned)(desc_of(as_struct(v))->id << MASK_SID_SHIFT);
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
    if (vm & MASK_STRUCT) return desc_of(as_struct(v))->name;
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
    } else if (is_struct(v) && desc_of(as_struct(v))->kind == LUMA_STRUCT_RC) {
        fputs("Rc(", stdout);
        write_value(as_struct(v)->fields[0], true);
        putchar(')');
    } else if (is_struct(v) && desc_of(as_struct(v))->kind == LUMA_STRUCT_ARENA) {
        fputs("Arena", stdout);
    } else if (is_struct(v)) {
        const LumaStruct *o = as_struct(v);
        bool cycle = nprinting == MAX_PRINT_DEPTH;
        for (int i = 0; i < nprinting && !cycle; i++) cycle = printing[i] == o;
        if (cycle) {
            printf("%s { ... }", desc_of(o)->name);
            return;
        }
        if (desc_of(o)->nfields == 0) {
            printf("%s {}", desc_of(o)->name);
            return;
        }
        printing[nprinting++] = o;
        printf("%s { ", desc_of(o)->name);
        for (int64_t i = 0; i < desc_of(o)->nfields; i++) {
            printf("%s%s: ", i ? ", " : "", desc_of(o)->fields[i]);
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

/* `counted` is decided by the compiler: never in --mm=none; in
 * --mm=manual for Rc boxes, arena handles, and instances its escape analysis
 * proves are never freed by the program. The others are the program's to
 * free: they come from the current arena if one is entered, else from the
 * manual pool. */
LumaValue luma_new_struct(const LumaStructDesc *d, int64_t counted) {
    size_t size = instance_size(d->nfields);
    LumaStruct *o;
    bool arena = !counted && luma_mm == LUMA_MM_MANUAL && narena_stack > 0;
    if (arena) {
        ArenaState *a = arena_state(as_struct(arena_stack[narena_stack - 1]));
        a->any_drop |= d->drop != NULL;
        o = arena_alloc(a, size);
    }
    else if (!counted && luma_mm == LUMA_MM_MANUAL && !memcheck) o = pool_get(d->nfields, size);
    else o = alloc_object(size);
    o->header = LUMA_TYPE_STRUCT | ((uint64_t)d->id << LUMA_STRUCT_ID_SHIFT) | ((uint64_t)(counted != 0) << 32);
    o->desc = (const LumaStructDesc *)((uintptr_t)d | (arena ? 1 : 0));
    for (int64_t i = 0; i < d->nfields; i++) o->fields[i] = LUMA_NIL;
    return (LumaValue)(uintptr_t)o;
}

static LumaStruct *need_instance(LumaValue o, const char *what) {
    check_valid(o);
    if (!is_struct(o)) panicf("Only struct instances have %s, got %s.", what, value_type_name(o));
    return as_struct(o);
}

/* The instance a field access goes to: through any Rc to its value. */
static LumaStruct *deref_instance(LumaValue o, const char *what) {
    LumaStruct *s = need_instance(o, what);
    for (;;) {
        const LumaStructDesc *d = desc_of(s);
        if (d->kind == LUMA_STRUCT_RC) {
            s = need_instance(s->fields[0], what);
            continue;
        }
        if (d->kind == LUMA_STRUCT_ARENA) panicf("An Arena has no %s.", what);
        return s;
    }
}

static int64_t find_field(const LumaStruct *s, const char *name) {
    for (int64_t i = 0; i < desc_of(s)->nfields; i++)
        if (strcmp(desc_of(s)->fields[i], name) == 0) return i;
    return -1;
}

static const LumaMethod *find_method(const LumaStruct *s, const char *name) {
    for (int64_t i = 0; i < desc_of(s)->nmethods; i++)
        if (strcmp(desc_of(s)->methods[i].name, name) == 0) return &desc_of(s)->methods[i];
    return NULL;
}

/* deref (docs/DESIGN.md "Memory"): a member an instance's struct lacks is
 * looked up on what its deref(self) method returns. The results are kept in
 * Derefs (each is a reference the runtime owns) and released when the access
 * is done. */
#define MAX_DEREF 8
typedef struct {
    LumaValue v[MAX_DEREF];
    int n;
    const char *first; /* the struct the access started at */
} Derefs;

typedef LumaValue (*LumaFn1)(LumaValue);

/* The instance to access for member `name` (a field, or a method when
 * `method`), starting at s. */
static LumaStruct *deref_for(LumaStruct *s, const char *name, bool method, const char *what, Derefs *ds) {
    ds->n = 0;
    ds->first = desc_of(s)->name;
    for (;;) {
        if (method ? find_method(s, name) != NULL : find_field(s, name) >= 0) return s;
        const LumaMethod *d = find_method(s, "deref");
        if (!d || d->arity != 1) return s;
        if (ds->n == MAX_DEREF) panicf("deref chain of '%s' is longer than %d steps.", ds->first, MAX_DEREF);
        LumaValue t = ((LumaFn1)(void (*)(void))d->fn)((LumaValue)(uintptr_t)s);
        ds->v[ds->n++] = t;
        s = need_instance(t, what);
    }
}

static void derefs_done(Derefs *ds) {
    for (int i = ds->n; i-- > 0;) luma_release(ds->v[i]);
    ds->n = 0;
}

static _Noreturn void no_member(const LumaStruct *s, const char *kind, const char *name, const Derefs *ds) {
    if (ds->n > 0) panicf("Struct '%s' has no %s '%s' (nor does '%s', through deref).", ds->first, kind, name, desc_of(s)->name);
    panicf("Struct '%s' has no %s '%s'.", desc_of(s)->name, kind, name);
}

/* A dynamic getfield returns a reference of its own (it may come from a
 * temporary that deref returned). */
LumaValue luma_getfield(LumaValue o, const char *name) {
    LumaStruct *s = deref_instance(o, "fields");
    Derefs ds;
    s = deref_for(s, name, false, "fields", &ds);
    int64_t k = find_field(s, name);
    if (k < 0) no_member(s, "field", name, &ds);
    LumaValue v = s->fields[k];
    luma_retain(v);
    derefs_done(&ds);
    return v;
}

LumaValue luma_setfield(LumaValue o, const char *name, LumaValue v) {
    LumaStruct *s = deref_instance(o, "fields");
    Derefs ds;
    s = deref_for(s, name, false, "fields", &ds);
    int64_t k = find_field(s, name);
    if (k < 0) no_member(s, "field", name, &ds);
    check_valid(v);
    unsigned m = (unsigned)desc_of(s)->ftys[k];
    if (!mask_accepts(m, value_mask(v))) {
        char want[300];
        describe_mask(m, want, sizeof want);
        panicf("field '%s' of '%s' expects %s, got %s.", name, desc_of(s)->name, want, value_type_name(v));
    }
    LumaValue old = s->fields[k];
    s->fields[k] = v; /* the field takes over the caller's reference to v */
    luma_release(old);
    derefs_done(&ds);
    return LUMA_NIL;
}

/* free(v): in --mm=manual, frees a struct instance and releases the values
 * in its fields (an instance in an arena is left to the arena); in --mm=none
 * it only checks its argument. free(nil) does nothing. Rc boxes and arenas
 * free themselves when their last reference goes away. */
LumaValue luma_free(LumaValue v) {
    if (is_object(v) && (header_of(v) & 0xff) == LUMA_TYPE_FREED) luma_panic("Double free: this object was already freed.");
    check_valid(v);
    if (v == LUMA_NIL) return LUMA_NIL;
    if (is_string(v)) luma_panic("free() expects a struct instance, got str (strings are freed automatically).");
    if (!is_struct(v)) panicf("free() expects a struct instance, got %s.", value_type_name(v));
    LumaStruct *o = as_struct(v);
    const LumaStructDesc *d = desc_of(o);
    if (d->flags & LUMA_STRUCT_SHARED)
        panicf("'%s' is a shared struct; it is freed when its last reference goes away.", d->name);
    if (is_dropping(o)) panicf("free() of an instance of '%s' whose drop() is running.", d->name);
    if (d->kind == LUMA_STRUCT_RC)
        panicf("free() expects a struct instance, got %s (an Rc frees its value when its last reference goes away).", d->name);
    if (d->kind == LUMA_STRUCT_ARENA)
        luma_panic("free() expects a struct instance, got Arena (an arena frees everything in it when its last reference goes away).");
    if (luma_mm != LUMA_MM_MANUAL || in_arena(o)) return LUMA_NIL;
    if (*rc_word(o) != 0) /* the compiler only counts instances that never reach free() */
        panicf("internal error: free() of an automatically managed %s.", d->name);
    free_manual(o);
    drain();
    return LUMA_NIL;
}

/* dispose(v): frees v when it is an instance managed by hand (as free()
 * would); does nothing for anything else, whose lifetime is managed already:
 * values that are not structs, counted and shared instances, instances in an
 * arena. For generic code that owns a value of any type. */
LumaValue luma_dispose(LumaValue v) {
    if (is_object(v) && (header_of(v) & 0xff) == LUMA_TYPE_FREED) luma_panic("Double free: this object was already freed.");
    check_valid(v);
    if (!is_struct(v) || luma_mm != LUMA_MM_MANUAL) return LUMA_NIL;
    LumaStruct *o = as_struct(v);
    if (*rc_word(o) != 0 || in_arena(o) || desc_of(o)->kind != LUMA_STRUCT_USER || is_dropping(o)) return LUMA_NIL;
    free_manual(o);
    drain();
    return LUMA_NIL;
}

/* refcount(v): the number of references to a counted object; 0 for
 * anything else. */
LumaValue luma_refcount(LumaValue v) {
    check_valid(v);
    return luma_fixnum(is_object(v) ? *rc_word((void *)(uintptr_t)v) : 0);
}

/* ---- Rc ------------------------------------------------------------------
 * Rc::new(v) with a value whose struct is known at compile time is a plain
 * `new Rc.S(v)`; these serve the dynamic cases. Like every runtime function,
 * they borrow their arguments and return a reference the caller owns. */

static const LumaStructDesc *rc_any_desc(void) {
    static const LumaStructDesc *any;
    if (!any)
        for (uint64_t i = 1; i <= luma_structs[0] && !any; i++) {
            const LumaStructDesc *d = (const LumaStructDesc *)luma_structs[i];
            if (d->kind == LUMA_STRUCT_RC && strcmp(d->name, "Rc<any>") == 0) any = d;
        }
    if (!any) luma_panic("internal error: no Rc descriptor");
    return any;
}

LumaValue luma_rc_new(LumaValue v) {
    check_valid(v);
    const LumaStructDesc *d = is_struct(v) ? desc_of(as_struct(v))->rc : NULL;
    if (!d) d = rc_any_desc();
    LumaValue box = luma_new_struct(d, luma_mm != LUMA_MM_NONE);
    luma_retain(v); /* the box holds its own reference */
    as_struct(box)->fields[0] = v;
    return box;
}

static LumaStruct *need_rc(LumaValue r, const char *fn) {
    check_valid(r);
    if (!is_struct(r) || desc_of(as_struct(r))->kind != LUMA_STRUCT_RC) panicf("%s expects an Rc, got %s.", fn, value_type_name(r));
    return as_struct(r);
}

LumaValue luma_rc_get(LumaValue r) {
    LumaValue v = need_rc(r, "Rc::get")->fields[0];
    luma_retain(v);
    return v;
}

LumaValue luma_rc_count(LumaValue r) { return luma_fixnum(*rc_word(need_rc(r, "Rc::count"))); }

/* ---- entering arenas ----------------------------------------------------- */

LumaValue luma_arena_enter(LumaValue a) {
    check_valid(a);
    if (!is_struct(a) || desc_of(as_struct(a))->kind != LUMA_STRUCT_ARENA)
        panicf("arena NAME { ... } expects an Arena, got %s.", value_type_name(a));
    arena_state(as_struct(a));
    if (narena_stack == carena_stack) {
        carena_stack = carena_stack ? carena_stack * 2 : 8;
        LumaValue *n = realloc(arena_stack, carena_stack * sizeof *n);
        if (!n) luma_panic("Out of memory.");
        arena_stack = n;
    }
    luma_retain(a); /* entered, it stays alive at least until left */
    arena_stack[narena_stack++] = a;
    return LUMA_NIL;
}

LumaValue luma_arena_leave(void) {
    LumaValue a = arena_stack[--narena_stack];
    luma_release(a);
    return LUMA_NIL;
}

/* The code of method `name` of o's struct, for a call with nargs arguments
 * after self. */
/* A method call whose receiver lacks the method but has deref: luma_method
 * returns this function, which compiled code calls at once with the
 * receiver and the arguments (at most 5 after self; extra registers are
 * ignored by the callee under the SysV ABI). The method name comes from the
 * luma_method call just before. */
static const char *tramp_name;
static int64_t tramp_nargs;
typedef LumaValue (*LumaFn6)(LumaValue, LumaValue, LumaValue, LumaValue, LumaValue, LumaValue);

static LumaValue deref_trampoline(LumaValue self, LumaValue a1, LumaValue a2, LumaValue a3, LumaValue a4, LumaValue a5) {
    const char *name = tramp_name;
    int64_t nargs = tramp_nargs;
    Derefs ds;
    LumaStruct *s = deref_for(as_struct(self), name, true, "methods", &ds);
    const LumaMethod *mt = find_method(s, name);
    if (!mt) no_member(s, "method", name, &ds);
    if (mt->arity - 1 != nargs)
        panicf("'%s::%s' expects %" PRId64 " argument%s, got %" PRId64 ".", desc_of(s)->name, name, mt->arity - 1,
               mt->arity == 2 ? "" : "s", nargs);
    LumaValue r = ((LumaFn6)(void (*)(void))mt->fn)((LumaValue)(uintptr_t)s, a1, a2, a3, a4, a5);
    derefs_done(&ds);
    return r;
}

LumaFn luma_method(LumaValue o, const char *name, int64_t nargs) {
    LumaStruct *s = need_instance(o, "methods");
    if (strcmp(name, "drop") == 0)
        luma_panic("drop() runs by itself when an instance is freed; call free(x) or dispose(x) instead.");
    if (desc_of(s)->kind == LUMA_STRUCT_ARENA) luma_panic("An Arena has no methods.");
    /* an Rc<S>'s table forwards S's methods; anything else in an Rc has none */
    const LumaStruct *named = s;
    if (desc_of(s)->kind == LUMA_STRUCT_RC) named = deref_instance(o, "methods");
    for (int64_t i = 0; i < desc_of(s)->nmethods; i++) {
        const LumaMethod *mt = &desc_of(s)->methods[i];
        if (strcmp(mt->name, name) != 0) continue;
        if (mt->arity - 1 != nargs)
            panicf("'%s::%s' expects %" PRId64 " argument%s, got %" PRId64 ".", desc_of(named)->name, name, mt->arity - 1,
                   mt->arity == 2 ? "" : "s", nargs);
        return mt->fn;
    }
    const LumaMethod *d = find_method(s, "deref");
    if (d && d->arity == 1) { /* called right away with (o, args...): see deref_trampoline */
        tramp_name = name;
        tramp_nargs = nargs;
        return (LumaFn)(void (*)(void))deref_trampoline;
    }
    panicf("Struct '%s' has no method '%s'.", desc_of(named)->name, name);
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
    return luma_mm == LUMA_MM_MANUAL ? "manual" : "none";
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
            if (desc_of(s)->kind == LUMA_STRUCT_ARENA) { /* an arena reaches everything in it */
                if (s->fields[0] != luma_fixnum(0)) {
                    const ArenaState *a = (const ArenaState *)(uintptr_t)(s->fields[0] & ~(LumaValue)1);
                    for (size_t i = 0; i < a->objs.n; i++) vec_push(stack, a->objs.items[i]);
                }
                continue;
            }
            for (int64_t k = 0; k < desc_of(s)->nfields; k++)
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
        if ((*h & 0xff) == LUMA_TYPE_STRUCT) counts[desc_of((const LumaStruct *)h)->id]++;
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
