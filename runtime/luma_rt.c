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
    const char *name;      /* as shown in messages: "Point", "Rc<Point>" */
    int64_t nfields;
    const char *const *fields;
    const int64_t *ftys;   /* declared type mask of each field */
    int64_t nmethods;
    const LumaMethod *methods;
    LumaFn drop;           /* the struct's drop(self) method, called when an instance dies; or NULL */
    LumaFn show;           /* its show(self): str method, how print shows an instance; or NULL */
    int64_t flags;         /* LUMA_STRUCT_SHARED, LUMA_STRUCT_C */
    const struct LumaCLayout *c; /* a C struct's fields (LUMA_STRUCT_C), or NULL */
};

/* A C struct's layout (System V), emitted by the compiler. */
typedef struct LumaCLayout {
    int64_t size;
    int64_t nfields;
    const char *const *names;
    const int64_t *ctypes;  /* LUMA_CT_* */
    const int64_t *offsets;
} LumaCLayout;

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

/* An array (the std library's List storage, reached only through prelude
 * intrinsics): counted like a string, its items are references. */
typedef struct {
    uint64_t header;
    uint64_t len;
    LumaValue items[];
} LumaArray;

static bool is_array(LumaValue v) {
    return is_object(v) && (((const LumaString *)(uintptr_t)v)->header & 0xff) == LUMA_TYPE_ARRAY;
}

static LumaArray *as_array(LumaValue v) { return (LumaArray *)(uintptr_t)v; }

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
 * The prelude's Arena (std/prelude.luma) is a shared struct whose field 0
 * holds its state as (pointer | 1), made by luma_arena_create. While an
 * arena is entered (its enter() pushes it with luma_arena_enter), every
 * struct instance the program would otherwise free by hand is allocated in
 * it, marked by bit 0 of its descriptor word; free() on such an instance
 * does nothing. Its drop() calls luma_arena_destroy: the values in every
 * instance's fields are released and the memory goes back in one piece. */

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used, cap;
    uint64_t data[];
} ArenaChunk;

typedef struct ArenaState {
    ArenaChunk *chunks; /* every chunk, newest first */
    ArenaChunk *cur;    /* the chunk small objects are bumped from */
    PtrVec objs;        /* LUMA_MEMCHECK: the instances (allocated individually) */
    uint64_t nobjects;
    bool any_drop;      /* some instance has a drop method */
    const void *handle; /* the Arena instance (set when entered; for memcheck's reachability) */
    struct ArenaState *prev, *next; /* every live arena */
} ArenaState;

static ArenaState *live_arenas;

#define ARENA_CHUNK (64 * 1024)

/* The state of an Arena handle (field 0; only the prelude makes them). */
static ArenaState *arena_state(const LumaStruct *h) { return (ArenaState *)(uintptr_t)(h->fields[0] & ~(LumaValue)1); }

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
    ArenaState *a = (ArenaState *)(uintptr_t)(state & ~(LumaValue)1);
    if (a->prev) a->prev->next = a->next;
    else live_arenas = a->next;
    if (a->next) a->next->prev = a->prev;
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

/* Destroys every object in `dying`, and those their fields release in turn. */
static void drain(void) {
    if (draining) return;
    draining = true;
    while (dying.n > 0) {
        void *o = dying.items[--dying.n];
        if ((*(uint64_t *)o & 0xff) == LUMA_TYPE_STRUCT) {
            LumaStruct *s = o;
            run_drop(s, true);
            int64_t n = desc_of(s)->nfields;
            if ((desc_of(s)->flags & LUMA_STRUCT_C) && s->fields[1] == luma_fixnum(1)) /* owned C memory */
                free((void *)(uintptr_t)fixnum_val(s->fields[0]));
            for (int64_t k = 0; k < n; k++) drop(s->fields[k]);
            free_object(o, false, n);
        } else if ((*(uint64_t *)o & 0xff) == LUMA_TYPE_ARRAY) {
            LumaArray *a = o;
            for (uint64_t k = 0; k < a->len; k++) drop(a->items[k]);
            free_object(o, false, 0);
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
    if (is_array(v)) return "array";
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

/* ---- C structs (extern struct) -------------------------------------------
 * A C struct value is a counted struct instance whose fields 0 and 1 hold the
 * address of its C memory and whether it owns it (both as Luma ints); the
 * descriptor's layout says where each C field is. Owned memory is calloc'd
 * (rounded up to 16 bytes, so by-value calls can read whole eightbytes) and
 * freed with the instance; views of memory C owns (*S results) are not. */

LumaValue luma_new_struct(const LumaStructDesc *d, int64_t counted);

static char *cdata(const LumaStruct *o) { return (char *)(uintptr_t)fixnum_val(o->fields[0]); }

static LumaValue cfield_get(const LumaStruct *o, int64_t k) {
    const LumaCLayout *c = desc_of(o)->c;
    const char *p = cdata(o) + c->offsets[k];
    switch (c->ctypes[k]) {
    case LUMA_CT_I8: return luma_fixnum(*(const int8_t *)p);
    case LUMA_CT_I16: { int16_t v; memcpy(&v, p, 2); return luma_fixnum(v); }
    case LUMA_CT_I32: { int32_t v; memcpy(&v, p, 4); return luma_fixnum(v); }
    case LUMA_CT_U8: return luma_fixnum(*(const uint8_t *)p);
    case LUMA_CT_U16: { uint16_t v; memcpy(&v, p, 2); return luma_fixnum(v); }
    case LUMA_CT_U32: { uint32_t v; memcpy(&v, p, 4); return luma_fixnum(v); }
    case LUMA_CT_BOOL: return make_bool(*p != 0);
    default: break;
    }
    uint64_t w;
    memcpy(&w, p, 8);
    char ctx[300];
    snprintf(ctx, sizeof ctx, "field '%s' of %s", c->names[k], desc_of(o)->name);
    switch (c->ctypes[k]) {
    case LUMA_CT_I64: return luma_ffi_ret_i64(w, ctx);
    case LUMA_CT_U64: return luma_ffi_ret_u64(w, ctx);
    case LUMA_CT_PTR: return luma_ffi_ret_ptr(w, ctx);
    case LUMA_CT_CSTR: return luma_ffi_ret_cstr(w, ctx);
    default: return luma_ffi_ret_cstr_opt(w, ctx);
    }
}

static void cfield_set(LumaStruct *o, int64_t k, LumaValue v) {
    const LumaCLayout *c = desc_of(o)->c;
    char *p = cdata(o) + c->offsets[k];
    char ctx[300];
    snprintf(ctx, sizeof ctx, "field '%s' of %s", c->names[k], desc_of(o)->name);
    uint64_t w;
    switch (c->ctypes[k]) {
    case LUMA_CT_I8: w = luma_ffi_arg_i8(v, ctx); break;
    case LUMA_CT_I16: w = luma_ffi_arg_i16(v, ctx); break;
    case LUMA_CT_I32: w = luma_ffi_arg_i32(v, ctx); break;
    case LUMA_CT_I64: w = luma_ffi_arg_i64(v, ctx); break;
    case LUMA_CT_U8: w = luma_ffi_arg_u8(v, ctx); break;
    case LUMA_CT_U16: w = luma_ffi_arg_u16(v, ctx); break;
    case LUMA_CT_U32: w = luma_ffi_arg_u32(v, ctx); break;
    case LUMA_CT_U64: w = luma_ffi_arg_u64(v, ctx); break;
    case LUMA_CT_BOOL: w = luma_ffi_arg_bool(v, ctx); break;
    case LUMA_CT_PTR: w = luma_ffi_arg_ptr(v, ctx); break;
    default: panicf("%s is a C string: it can be read, not written (its memory belongs to C).", ctx);
    }
    int size = c->ctypes[k] == LUMA_CT_I8 || c->ctypes[k] == LUMA_CT_U8 || c->ctypes[k] == LUMA_CT_BOOL ? 1
             : c->ctypes[k] == LUMA_CT_I16 || c->ctypes[k] == LUMA_CT_U16 ? 2
             : c->ctypes[k] == LUMA_CT_I32 || c->ctypes[k] == LUMA_CT_U32 ? 4 : 8;
    memcpy(p, &w, (size_t)size); /* little-endian: the low bytes */
}

static int64_t cfield_index(const LumaStruct *o, const char *name) {
    const LumaCLayout *c = desc_of(o)->c;
    for (int64_t i = 0; i < c->nfields; i++)
        if (strcmp(c->names[i], name) == 0) return i;
    return -1;
}

static LumaStruct *need_cstruct(LumaValue v) {
    check_valid(v);
    if (!is_struct(v) || !(desc_of(as_struct(v))->flags & LUMA_STRUCT_C))
        panicf("internal error: C struct expected, got %s.", value_type_name(v));
    return as_struct(v);
}

/* `S { ... }` of a C struct: new zeroed C memory, owned. */
LumaValue luma_cs_init(LumaValue v) {
    LumaStruct *o = need_cstruct(v);
    size_t size = ((size_t)desc_of(o)->c->size + 15) / 16 * 16;
    void *p = calloc(1, size ? size : 16);
    if (!p) luma_panic("Out of memory.");
    o->fields[0] = luma_fixnum((int64_t)(uintptr_t)p);
    o->fields[1] = luma_fixnum(1);
    return LUMA_NIL;
}

/* s.f / s.f = v with the field's index k known at compile time. */
LumaValue luma_cs_get(LumaValue v, LumaValue k) { return cfield_get(need_cstruct(v), fixnum_val(k)); }

LumaValue luma_cs_set(LumaValue v, LumaValue k, LumaValue x) {
    cfield_set(need_cstruct(v), fixnum_val(k), x);
    return LUMA_NIL;
}

/* A *S argument: the address of s's C memory, 0 for nil. */
LumaValue luma_cs_addr(LumaValue v) {
    if (v == LUMA_NIL) return luma_fixnum(0);
    return need_cstruct(v)->fields[0];
}

/* A *S result, or S::at(p): v was made as a view of address p (fields
 * p, 0). NULL is nil when allowed (nullable), else a runtime error. */
LumaValue luma_cs_view(LumaValue v, LumaValue nullable, LumaValue what) {
    LumaStruct *o = need_cstruct(v);
    if (o->fields[0] == luma_fixnum(0)) {
        if (nullable == LUMA_TRUE) return LUMA_NIL;
        panicf("%s is a NULL pointer to %s.", is_string(what) ? as_string(what)->bytes : "a C struct", desc_of(o)->name);
    }
    luma_retain(v);
    return v;
}

/* A C struct argument passed by value (at most 16 bytes): its eightbytes. */
static uint64_t cs_word(LumaValue v, const char *ctx, int i) {
    check_valid(v);
    if (!is_struct(v) || !(desc_of(as_struct(v))->flags & LUMA_STRUCT_C)) panicf("%s expects a C struct, got %s.", ctx, value_type_name(v));
    const LumaStruct *o = as_struct(v);
    int64_t size = desc_of(o)->c->size;
    uint64_t w = 0;
    if (size > 8 * i) memcpy(&w, cdata(o) + 8 * i, (size_t)(size - 8 * i < 8 ? size - 8 * i : 8));
    return w;
}

uint64_t luma_ffi_arg_cs0(LumaValue v, const char *c) { return cs_word(v, c, 0); }
uint64_t luma_ffi_arg_cs1(LumaValue v, const char *c) { return cs_word(v, c, 1); }

/* A C struct returned by value: rax and rdx hold its eightbytes. */
LumaValue luma_ffi_ret_cs(uint64_t lo, uint64_t hi, const char *c, const LumaStructDesc *d) {
    (void)c;
    LumaValue v = luma_new_struct(d, luma_mm != LUMA_MM_NONE);
    luma_cs_init(v);
    uint64_t w[2] = {lo, hi};
    memcpy(cdata(as_struct(v)), w, (size_t)(d->c->size < 16 ? d->c->size : 16));
    return v;
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

/* A struct with a show(self) method prints as the string it returns. */
static void write_value(FILE *out, LumaValue v, bool quoted) {
    check_valid(v);
    if (is_fixnum(v)) fprintf(out, "%" PRId64, fixnum_val(v));
    else if (v == LUMA_NIL) fputs("nil", out);
    else if (v == LUMA_TRUE) fputs("true", out);
    else if (v == LUMA_FALSE) fputs("false", out);
    else if (is_string(v)) {
        const LumaString *s = as_string(v);
        if (!quoted) {
            fwrite(s->bytes, 1, s->len, out);
            return;
        }
        fputc('"', out);
        for (uint64_t i = 0; i < s->len; i++) {
            char c = s->bytes[i];
            if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
            else if (c == '\n') fputs("\\n", out);
            else if (c == '\t') fputs("\\t", out);
            else fputc(c, out);
        }
        fputc('"', out);
    } else if (is_array(v)) {
        const LumaArray *a = as_array(v);
        fputc('[', out);
        for (uint64_t i = 0; i < a->len; i++) {
            if (i) fputs(", ", out);
            write_value(out, a->items[i], true);
        }
        fputc(']', out);
    } else if (is_struct(v)) {
        const LumaStruct *o = as_struct(v);
        bool cycle = nprinting == MAX_PRINT_DEPTH;
        for (int i = 0; i < nprinting && !cycle; i++) cycle = printing[i] == o;
        if (cycle) {
            fprintf(out, "%s { ... }", desc_of(o)->name);
            return;
        }
        if ((desc_of(o)->flags & LUMA_STRUCT_C) && !desc_of(o)->show) {
            const LumaCLayout *c = desc_of(o)->c;
            fprintf(out, "%s {", desc_of(o)->name);
            for (int64_t i = 0; i < c->nfields; i++) {
                fprintf(out, "%s %s: ", i ? "," : "", c->names[i]);
                LumaValue f = cfield_get(o, i);
                write_value(out, f, true);
                luma_release(f);
            }
            fputs(c->nfields ? " }" : "}", out);
            return;
        }
        if (desc_of(o)->show) {
            printing[nprinting++] = o;
            LumaValue r = ((LumaValue(*)(LumaValue))(void (*)(void))desc_of(o)->show)(v);
            nprinting--;
            if (!is_string(r)) panicf("%s.show() must return a str, got %s.", desc_of(o)->name, value_type_name(r));
            fwrite(as_string(r)->bytes, 1, as_string(r)->len, out);
            luma_release(r);
            return;
        }
        if (desc_of(o)->nfields == 0) {
            fprintf(out, "%s {}", desc_of(o)->name);
            return;
        }
        printing[nprinting++] = o;
        fprintf(out, "%s { ", desc_of(o)->name);
        for (int64_t i = 0; i < desc_of(o)->nfields; i++) {
            fprintf(out, "%s%s: ", i ? ", " : "", desc_of(o)->fields[i]);
            write_value(out, o->fields[i], true);
        }
        fputs(" }", out);
        nprinting--;
    } else luma_panic("cannot print value of unknown type");
}

LumaValue luma_write(LumaValue v) {
    write_value(stdout, v, false);
    return LUMA_NIL;
}

/* The prelude's __show(v): v as print shows it inside a struct (strings
 * quoted), as a new string. */
LumaValue luma_show(LumaValue v) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) luma_panic("Out of memory.");
    write_value(f, v, true);
    if (fclose(f) != 0 || !buf) luma_panic("Out of memory.");
    LumaString *s = new_string(len);
    memcpy(s->bytes, buf, len);
    free(buf);
    return (LumaValue)(uintptr_t)s;
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
 * --mm=manual for shared structs (Rc, Arena, ...) and instances its escape analysis
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
    LumaStruct *s = need_instance(o, "fields");
    if (desc_of(s)->flags & LUMA_STRUCT_C) {
        int64_t k = cfield_index(s, name);
        if (k >= 0) return cfield_get(s, k);
    }
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
    LumaStruct *s = need_instance(o, "fields");
    if (desc_of(s)->flags & LUMA_STRUCT_C) {
        int64_t k = cfield_index(s, name);
        if (k >= 0) {
            cfield_set(s, k, v);
            luma_release(v); /* (a setfield takes over its value: C memory keeps a copy) */
            return LUMA_NIL;
        }
    }
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
 * it only checks its argument. free(nil) does nothing. Instances of shared
 * structs (Rc, Arena) free themselves when their last reference goes away. */
LumaValue luma_free(LumaValue v) {
    if (is_object(v) && (header_of(v) & 0xff) == LUMA_TYPE_FREED) luma_panic("Double free: this object was already freed.");
    check_valid(v);
    if (v == LUMA_NIL) return LUMA_NIL;
    if (is_string(v)) luma_panic("free() expects a struct instance, got str (strings are freed automatically).");
    if (!is_struct(v)) panicf("free() expects a struct instance, got %s.", value_type_name(v));
    LumaStruct *o = as_struct(v);
    const LumaStructDesc *d = desc_of(o);
    if (d->flags & LUMA_STRUCT_C)
        panicf("'%s' is a C struct: its value is freed automatically (and memory C owns stays C's).", d->name);
    if (d->flags & LUMA_STRUCT_SHARED)
        panicf("'%s' is a shared struct; it is freed when its last reference goes away.", d->name);
    if (is_dropping(o)) panicf("free() of an instance of '%s' whose drop() is running.", d->name);
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
    if (*rc_word(o) != 0 || in_arena(o) || is_dropping(o)) return LUMA_NIL;
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

/* ---- std library primitives (prelude intrinsics, used by the std files) ---
 * Like every runtime function they borrow their arguments and return a
 * reference the caller owns. Bad arguments are runtime errors: the std
 * library checks what users pass before calling these. */

static int64_t need_int(LumaValue v, const char *what) {
    check_valid(v);
    if (!is_fixnum(v)) panicf("%s expects int, got %s.", what, value_type_name(v));
    return fixnum_val(v);
}

static const LumaString *need_str(LumaValue v, const char *what) {
    check_valid(v);
    if (!is_string(v)) panicf("%s expects str, got %s.", what, value_type_name(v));
    return as_string(v);
}

static LumaArray *need_array(LumaValue v) {
    check_valid(v);
    if (!is_array(v)) panicf("internal error: array expected, got %s.", value_type_name(v));
    return as_array(v);
}

static LumaValue string_from_bytes(const char *p, size_t n) {
    LumaString *s = new_string(n);
    memcpy(s->bytes, p, n);
    return (LumaValue)(uintptr_t)s;
}

/* __array_new(n): n items, all nil. */
LumaValue luma_array_new(LumaValue n) {
    int64_t len = need_int(n, "array size");
    if (len < 0 || len > (int64_t)(SIZE_MAX / 16)) panicf("Bad array size %" PRId64 ".", len);
    LumaArray *a = alloc_object(sizeof *a + (size_t)len * sizeof(LumaValue));
    a->header = LUMA_TYPE_ARRAY | ((uint64_t)(luma_mm != LUMA_MM_NONE) << 32);
    a->len = (uint64_t)len;
    for (int64_t i = 0; i < len; i++) a->items[i] = LUMA_NIL;
    return (LumaValue)(uintptr_t)a;
}

LumaValue luma_array_len(LumaValue a) { return luma_fixnum((int64_t)need_array(a)->len); }

static uint64_t array_index(const LumaArray *a, LumaValue i) {
    int64_t k = need_int(i, "array index");
    if (k < 0 || (uint64_t)k >= a->len) panicf("Index %" PRId64 " is out of range (length %" PRIu64 ").", k, a->len);
    return (uint64_t)k;
}

LumaValue luma_array_get(LumaValue a, LumaValue i) {
    LumaArray *arr = need_array(a);
    LumaValue v = arr->items[array_index(arr, i)];
    luma_retain(v);
    return v;
}

LumaValue luma_array_set(LumaValue a, LumaValue i, LumaValue v) {
    LumaArray *arr = need_array(a);
    uint64_t k = array_index(arr, i);
    check_valid(v);
    luma_retain(v); /* the array holds its own reference */
    LumaValue old = arr->items[k];
    arr->items[k] = v;
    luma_release(old);
    return LUMA_NIL;
}

LumaValue luma_str_len(LumaValue s) { return luma_fixnum((int64_t)need_str(s, "Str::len")->len); }

/* __str_byte(s, i): the byte at i (0-255). */
LumaValue luma_str_byte(LumaValue s, LumaValue i) {
    const LumaString *x = need_str(s, "Str::byte");
    int64_t k = need_int(i, "Str::byte");
    if (k < 0 || (uint64_t)k >= x->len) panicf("Index %" PRId64 " is out of range (length %" PRIu64 ").", k, x->len);
    return luma_fixnum((unsigned char)x->bytes[k]);
}

/* __str_char(b): a one-byte string. */
LumaValue luma_str_char(LumaValue b) {
    int64_t c = need_int(b, "Str::char");
    if (c < 0 || c > 255) panicf("Str::char expects a byte (0-255), got %" PRId64 ".", c);
    char ch = (char)c;
    return string_from_bytes(&ch, 1);
}

/* __str_slice(s, from, to): bytes [from, to), 0 <= from <= to <= len. */
LumaValue luma_str_slice(LumaValue s, LumaValue from, LumaValue to) {
    const LumaString *x = need_str(s, "Str::slice");
    int64_t a = need_int(from, "Str::slice"), b = need_int(to, "Str::slice");
    if (a < 0 || b < a || (uint64_t)b > x->len)
        panicf("Slice [%" PRId64 ", %" PRId64 ") is out of range (length %" PRIu64 ").", a, b, x->len);
    return string_from_bytes(x->bytes + a, (size_t)(b - a));
}

/* __str_find(s, sub, from): the first index >= from where sub occurs, or -1. */
LumaValue luma_str_find(LumaValue s, LumaValue sub, LumaValue from) {
    const LumaString *x = need_str(s, "Str::find"), *y = need_str(sub, "Str::find");
    int64_t a = need_int(from, "Str::find");
    if (a < 0) a = 0;
    if ((uint64_t)a > x->len) return luma_fixnum(-1);
    for (uint64_t i = (uint64_t)a; i + y->len <= x->len; i++)
        if (memcmp(x->bytes + i, y->bytes, y->len) == 0) return luma_fixnum((int64_t)i);
    return luma_fixnum(-1);
}

/* __str_cmp(a, b): -1, 0 or 1 (bytewise). */
LumaValue luma_str_cmp(LumaValue a, LumaValue b) {
    const LumaString *x = need_str(a, "Str::compare"), *y = need_str(b, "Str::compare");
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->bytes, y->bytes, n);
    if (c == 0) c = x->len < y->len ? -1 : x->len > y->len;
    return luma_fixnum(c < 0 ? -1 : c > 0);
}

LumaValue luma_str_from_int(LumaValue n) {
    char buf[32];
    int len = snprintf(buf, sizeof buf, "%" PRId64, need_int(n, "Str::from_int"));
    return string_from_bytes(buf, (size_t)len);
}

/* __str_to_int(s): the decimal integer s spells (optional sign, digits,
 * nothing else), or nil. */
LumaValue luma_str_to_int(LumaValue s) {
    const LumaString *x = need_str(s, "Str::to_int");
    size_t i = 0;
    bool neg = false;
    if (i < x->len && (x->bytes[i] == '-' || x->bytes[i] == '+')) neg = x->bytes[i++] == '-';
    if (i == x->len) return LUMA_NIL;
    int64_t v = 0;
    for (; i < x->len; i++) {
        char c = x->bytes[i];
        if (c < '0' || c > '9') return LUMA_NIL;
        if (v > (LUMA_FIXNUM_MAX - (c - '0')) / 10) return LUMA_NIL; /* (also rejects -2^62: fine) */
        v = v * 10 + (c - '0');
    }
    return luma_fixnum(neg ? -v : v);
}

/* __str_case(s, upper): ASCII letters to upper (1) or lower (0) case. */
LumaValue luma_str_case(LumaValue s, LumaValue upper) {
    const LumaString *x = need_str(s, "Str::upper");
    LumaString *r = new_string(x->len);
    for (uint64_t i = 0; i < x->len; i++) {
        char c = x->bytes[i];
        if (upper == LUMA_TRUE && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (upper != LUMA_TRUE && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        r->bytes[i] = c;
    }
    return (LumaValue)(uintptr_t)r;
}

/* __str_join(items, n, sep): the first n items of an array (all strings),
 * separated by sep. */
LumaValue luma_str_join(LumaValue items, LumaValue n, LumaValue sep) {
    const LumaArray *a = need_array(items);
    int64_t k = need_int(n, "Str::join");
    const LumaString *s = need_str(sep, "Str::join");
    if (k < 0 || (uint64_t)k > a->len) k = (int64_t)a->len;
    size_t total = 0;
    for (int64_t i = 0; i < k; i++) {
        if (!is_string(a->items[i])) panicf("Str::join expects a list of str, got %s.", value_type_name(a->items[i]));
        total += as_string(a->items[i])->len + (i ? s->len : 0);
    }
    LumaString *r = new_string(total);
    size_t o = 0;
    for (int64_t i = 0; i < k; i++) {
        if (i) {
            memcpy(r->bytes + o, s->bytes, s->len);
            o += s->len;
        }
        const LumaString *x = as_string(a->items[i]);
        memcpy(r->bytes + o, x->bytes, x->len);
        o += x->len;
    }
    return (LumaValue)(uintptr_t)r;
}

/* __type(v): "int", "str", "bool", "nil", "array" or the struct's name. */
LumaValue luma_type_name(LumaValue v) {
    check_valid(v);
    return string_from_c(value_type_name(v));
}

/* __fail(msg): a runtime error with this message (Sys::fail, and the std
 * library's own checks). */
LumaValue luma_fail(LumaValue msg) {
    check_valid(msg);
    if (is_string(msg)) panicf("%s", as_string(msg)->bytes);
    luma_panic("failure");
}

/* __array_sort(a, n): sorts the first n items, all ints or all strs. */
static int compare_items(const void *x, const void *y) {
    LumaValue a = *(const LumaValue *)x, b = *(const LumaValue *)y;
    if (is_fixnum(a)) return fixnum_val(a) < fixnum_val(b) ? -1 : fixnum_val(a) > fixnum_val(b);
    return (int)fixnum_val(luma_str_cmp(a, b));
}

LumaValue luma_array_sort(LumaValue a, LumaValue n) {
    LumaArray *arr = need_array(a);
    int64_t k = need_int(n, "List::sort");
    if (k < 0 || (uint64_t)k > arr->len) k = (int64_t)arr->len;
    if (k == 0) return LUMA_NIL;
    bool ints = is_fixnum(arr->items[0]);
    for (int64_t i = 0; i < k; i++) {
        check_valid(arr->items[i]);
        if (ints ? !is_fixnum(arr->items[i]) : !is_string(arr->items[i]))
            panicf("List::sort sorts a list of ints or a list of strs, got %s and %s.", value_type_name(arr->items[0]),
                   value_type_name(arr->items[i]));
    }
    qsort(arr->items, (size_t)k, sizeof *arr->items, compare_items);
    return LUMA_NIL;
}

/* __argc() / __argv(i): the program's command line (argv[0] included). */
static int main_argc;
static char **main_argv;

LumaValue luma_argc(void) { return luma_fixnum(main_argc); }

LumaValue luma_argv(LumaValue i) {
    int64_t k = need_int(i, "Sys::arg");
    if (k < 0 || k >= main_argc) panicf("Index %" PRId64 " is out of range (%d arguments).", k, main_argc);
    return string_from_c(main_argv[k]);
}

/* __mem_string(p, n): n bytes at address p, as a new string. __mem_write(p,
 * s): copies s's bytes (no NUL) to address p. Addresses are ints (C ptr). */
LumaValue luma_mem_string(LumaValue p, LumaValue n) {
    int64_t a = need_int(p, "C::string"), k = need_int(n, "C::string");
    if (a == 0) luma_panic("C::string of a NULL pointer.");
    if (k < 0) panicf("C::string expects a length >= 0, got %" PRId64 ".", k);
    return string_from_bytes((const char *)(uintptr_t)a, (size_t)k);
}

LumaValue luma_mem_write(LumaValue p, LumaValue s) {
    int64_t a = need_int(p, "C::write");
    const LumaString *x = need_str(s, "C::write");
    if (a == 0) luma_panic("C::write to a NULL pointer.");
    memcpy((void *)(uintptr_t)a, x->bytes, x->len);
    return LUMA_NIL;
}

/* ---- arena intrinsics (used only by the prelude's Arena) ----------------- */

/* __arena_create(): a new, empty arena; its state word (pointer | 1). */
LumaValue luma_arena_create(void) {
    ArenaState *a = calloc(1, sizeof *a);
    if (!a) luma_panic("Out of memory.");
    a->next = live_arenas;
    if (live_arenas) live_arenas->prev = a;
    live_arenas = a;
    return (LumaValue)(uintptr_t)a | 1;
}

/* __arena_destroy(state): from Arena.drop(). */
LumaValue luma_arena_destroy(LumaValue state) {
    arena_destroy(state);
    return LUMA_NIL;
}

/* __arena_push(handle) / __arena_pop(): from Arena.enter() / leave(). */
LumaValue luma_arena_enter(LumaValue a) {
    check_valid(a);
    if (!is_struct(a)) panicf("internal error: luma_arena_enter of %s.", value_type_name(a));
    arena_state(as_struct(a))->handle = as_struct(a);
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
    const LumaStruct *named = s;
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
            for (const ArenaState *a = live_arenas; a; a = a->next) /* an arena reaches everything in it */
                if (a->handle == s)
                    for (size_t j = 0; j < a->objs.n; j++) vec_push(stack, a->objs.items[j]);
            for (int64_t k = 0; k < desc_of(s)->nfields; k++)
                if (is_object(s->fields[k])) vec_push(stack, (void *)(uintptr_t)s->fields[k]);
        } else if ((h & 0xff) == LUMA_TYPE_ARRAY) {
            const LumaArray *a = o;
            for (uint64_t k = 0; k < a->len; k++)
                if (is_object(a->items[k])) vec_push(stack, (void *)(uintptr_t)a->items[k]);
        }
    }
}

/* "2 Node, 1 str" for the live objects whose mark equals `which`. */
static void describe_objects(const unsigned char *marked, unsigned char which, char *buf, size_t n) {
    uint64_t nstructs = luma_structs[0];
    uint64_t *counts = calloc(nstructs + 3, sizeof *counts);
    if (!counts) luma_panic("Out of memory.");
    for (size_t i = 0; i < live.n; i++) {
        if (marked[i] != which) continue;
        const uint64_t *h = live.items[i];
        if ((*h & 0xff) == LUMA_TYPE_STRUCT) counts[desc_of((const LumaStruct *)h)->id]++;
        else if ((*h & 0xff) == LUMA_TYPE_ARRAY) counts[nstructs + 2]++;
        else counts[nstructs + 1]++;
    }
    buf[0] = '\0';
    for (uint64_t id = 1; id <= nstructs + 2; id++) {
        if (!counts[id]) continue;
        size_t len = strlen(buf);
        snprintf(buf + len, n - len, "%s%" PRIu64 " %s", len ? ", " : "", counts[id],
                 id <= nstructs ? struct_name((unsigned)id) : id == nstructs + 1 ? "str" : "array");
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

int main(int argc, char **argv) {
    main_argc = argc;
    main_argv = argv;
    install_stack_guard();
    const char *mc = getenv("LUMA_MEMCHECK");
    memcheck = mc && *mc && strcmp(mc, "0") != 0;
    luma_main();
    if (fflush(stdout) != 0) return 1; /* e.g. writing to a closed pipe/full disk */
    if (memcheck) memcheck_report();
    return 0;
}
