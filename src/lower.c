#include "lower.h"
#include "prelude.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_PARAMS 6 /* SysV register arguments; see docs/IR.md */
#define DISCARD (-2) /* lower_expr hint: the value is not needed */

/* ---- static types ---------------------------------------------------------
 * A static type is the set of runtime types a value may have. "any" is the
 * full set: unannotated variables, parameters and returns are any, so
 * untyped code behaves exactly as dynamic code. The masks match the runtime's
 * luma_check_type. */
typedef IrTy Ty; /* the IR's types: struct types carry the struct's index (ir.h) */
#define T_INT TY_INT
#define T_STR TY_STR
#define T_BOOL TY_BOOL
#define T_NIL TY_NIL
#define T_ANY TY_ANY

/* The Luma type a C type converts to/from at an FFI boundary. */
static Ty ctype_luma(CType c) {
    switch (c) {
    case CT_BOOL: return T_BOOL;
    case CT_CSTR: return T_STR;
    case CT_CSTR_OPT: return T_STR | T_NIL;
    case CT_VOID: return T_NIL;
    default: return T_INT; /* all integer types and ptr */
    }
}

/* ---- lowering state ---- */

typedef struct {
    char *name;
    int vreg;  /* -1 while the declaration's initializer is being lowered */
    int depth;
    Ty ty;     /* declared type (T_ANY if unannotated) */
} Binding;

/* A type parameter bound to a type (inside a generic struct's instance). */
typedef struct {
    const char *name;
    Ty ty;
} TArg;

typedef struct {
    const char *name; /* "f", or "Point::len" for functions of an impl (then owned) */
    int global;   /* IR global index (@fn.NAME / @m.STRUCT.NAME) */
    int arity;
    const Stmt *decl;
    Ty *ptypes;   /* per parameter (owned) */
    Ty ret;       /* declared return type (T_ANY if unannotated) */
    int sid;      /* impl functions: the struct; -1 for top-level functions */
    bool has_self;/* impl functions whose first parameter is `self` (methods) */
    char *owned_name;
    const TArg *tenv; /* functions of a generic struct's instance: its type arguments */
    int ntenv;
    bool instance;    /* (its method table entry is added when the instance is created) */
} FnInfo;

typedef struct {
    const char *name;   /* "Point", "Box<int>" (the IR's name) */
    int sid;            /* index of the IR struct */
    const Stmt *decl;   /* NULL for builtins */
    int *fns;           /* indices into L.fns of the impl's functions */
    int nfns;
    int generic;        /* instances of a generic struct: index into L.generics, else -1 */
    Ty *targs;          /* ... and its type arguments (owned) */
} StructInfo;

/* A generic struct (`struct Box<T> { ... }`) and its impl blocks. Each
 * distinct list of type arguments becomes one struct (an instance) when a
 * type, literal or call first needs it. */
typedef struct {
    const Stmt *decl;
    const Stmt **impls;
    int nimpls;
} Generic;

typedef struct {
    const char *name;
    int global;   /* IR global index (C symbol) */
    const Stmt *decl;
    CType *ctypes; /* per parameter (owned); CT_PTR for *S, CT_CSW0 for a struct by value */
    CType cret;    /* CT_PTR for *S, CT_CSRET for a struct by value */
    int *psid;     /* per parameter: the C struct (IR id) of *S or S, else -1 (owned) */
    bool *pnull;   /* per parameter: *S? (owned) */
    int rsid;      /* the C struct returned (*S, *S? or S), else -1 */
    bool rnull;    /* *S? */
} ExtInfo;

typedef struct {
    const char *name;
    int global;    /* IR global index (@var.NAME) */
    bool declared; /* top-level code: has its declaration been reached yet? */
    Ty ty;         /* declared type of the first declaration (T_ANY if unannotated) */
} GVar;

/* State for the function currently being lowered. */
typedef struct {
    int fi;
    bool is_main;
    int cur;
    int *order;
    int norder, cap_order;
    bool *is_var;
    int cap_is_var;
    Binding *scope;
    int nscope, cap_scope;
    int depth;
    int ntemps, nlabels;
    /* The subjects of the `with` blocks entered at this point, innermost
     * last, left (with their leave() method) before a return. */
    struct { int v; Ty ty; } *exits;
    int nexits, cap_exits;
    int info;     /* index into L.fns; -1 for luma_main */
    /* Temporaries that live to the end of their full expression (hold_temp) */
    int *held;
    int nheld, cap_held;
    int edepth;   /* nesting of lower_expr within the current statement */
} Fn;

typedef struct {
    const char *path;
    IrModule *m;
    FnInfo *fns;
    int nfns;
    ExtInfo *exts;
    int nexts;
    GVar *gvars;
    int ngvars;
    StructInfo *structs;
    int nstructs;
    Generic *generics;
    int ngenerics;
    const TArg *tenv; /* the type parameters in scope (in a generic instance) */
    int ntenv;
    TArg **tenvs;     /* every environment allocated (freed at the end) */
    int ntenvs;
    Ty expect;        /* the type the expression being lowered flows into, or 0 */
    int nstrings;
    int write_fn, space_fn, newline_fn, free_fn; /* runtime externs, created on first use */
    int dispose_fn, refcount_fn;
    int intrinsic_fn[64]; /* runtime externs of the prelude's intrinsics (INTRINSICS) */
    int cs_fn[5];         /* luma_cs_init, _get, _set, _addr, _view (C structs) */
    Fn *f;
    bool failed;
    int col; /* column of the expression being lowered (IrInstr.col, for diagnostics after lowering) */
} L;

static void ty_name(L *l, Ty t, char *buf, size_t n) { ir_ty_describe(l->m, t, " or ", buf, n); }

/* A lowered expression: the vreg holding the value and its static type. */
typedef struct {
    int v;
    Ty ty;
} Val;

static const Val NO_VAL = {-1, T_ANY};

static int find_struct(L *l, const char *name) {
    for (int i = 0; i < l->nstructs; i++)
        if (strcmp(l->structs[i].name, name) == 0) return i;
    return -1;
}

static int find_generic(L *l, const char *name) {
    for (int i = 0; i < l->ngenerics; i++)
        if (strcmp(l->generics[i].decl->name, name) == 0) return i;
    return -1;
}

static const FnInfo *cur_info(L *l) { return l->f->info >= 0 ? &l->fns[l->f->info] : NULL; }

/* The function `name` of struct si's impl, as an index into l->fns, or -1. */
static int find_impl_fn(L *l, int si, const char *name) {
    const StructInfo *st = &l->structs[si];
    for (int i = 0; i < st->nfns; i++)
        if (strcmp(l->fns[st->fns[i]].decl->name, name) == 0) return st->fns[i];
    return -1;
}

static IrFunc *fn(L *l) { return &l->m->funcs[l->f->fi]; }

static void lerr(L *l, int line, int col, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void lerr(L *l, int line, int col, const char *fmt, ...) {
    if (l->failed) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", prelude_path(l->path, line), prelude_line(line), col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    l->failed = true;
}

/* ---- vregs and blocks ---- */

static void mark_var(L *l, int v, bool is_var) {
    Fn *f = l->f;
    if (v >= f->cap_is_var) {
        int n = f->cap_is_var ? f->cap_is_var : 64;
        while (n <= v) n *= 2;
        f->is_var = xrealloc(f->is_var, (size_t)n * sizeof *f->is_var);
        memset(f->is_var + f->cap_is_var, 0, (size_t)(n - f->cap_is_var) * sizeof *f->is_var);
        f->cap_is_var = n;
    }
    f->is_var[v] = is_var;
}

static int temp(L *l) {
    char name[32];
    snprintf(name, sizeof name, "%d", l->f->ntemps++);
    int v = ir_func_new_vreg(fn(l), name); /* numeric names never clash with identifiers */
    mark_var(l, v, false);
    return v;
}

static bool is_var_vreg(L *l, int v) { return v >= 0 && v < l->f->cap_is_var && l->f->is_var[v]; }

static int new_var_vreg(L *l, const char *name) {
    IrFunc *f = fn(l);
    char buf[300];
    snprintf(buf, sizeof buf, "%s", name);
    for (int k = 1; ir_func_find_vreg(f, buf) >= 0; k++) snprintf(buf, sizeof buf, "%s.%d", name, k);
    int v = ir_func_new_vreg(f, buf);
    mark_var(l, v, true);
    return v;
}

static int new_block(L *l, const char *kind, int n) {
    char label[64];
    snprintf(label, sizeof label, "%s.%d", kind, n);
    return ir_func_block(fn(l), label);
}

static void set_block(L *l, int b) {
    Fn *f = l->f;
    f->cur = b;
    if (f->norder == f->cap_order) {
        f->cap_order = f->cap_order ? f->cap_order * 2 : 16;
        f->order = xrealloc(f->order, (size_t)f->cap_order * sizeof *f->order);
    }
    f->order[f->norder++] = b;
}

static IrInstr *emit(L *l, IrOp op, int line) {
    IrInstr *in = ir_emit(fn(l), l->f->cur, op);
    in->line = line;
    in->col = l->col;
    return in;
}

static void emit_mov(L *l, int dst, int src, int line) {
    if (dst == src) return;
    IrInstr *in = emit(l, IR_MOV, line);
    in->dst = dst;
    in->a = src;
}

static void emit_jmp(L *l, int target, int line) {
    if (ir_block_terminated(&fn(l)->blocks[l->f->cur])) return;
    emit(l, IR_JMP, line)->target[0] = target;
}

static void emit_br(L *l, int cond, int t, int f, int line) {
    IrInstr *in = emit(l, IR_BR, line);
    in->a = cond;
    in->target[0] = t;
    in->target[1] = f;
}

static void emit_call(L *l, int dst, int global, const int *args, int nargs, int line) {
    IrInstr *in = emit(l, IR_CALL, line);
    in->dst = dst;
    in->global = global;
    in->nargs = nargs;
    if (nargs) {
        in->args = xmalloc((size_t)nargs * sizeof(int));
        memcpy(in->args, args, (size_t)nargs * sizeof(int));
    }
}

static int string_data(L *l, const char *s, size_t n) {
    for (int g = 0; g < l->m->nglobals; g++) {
        const IrGlobal *gl = &l->m->globals[g];
        if (gl->kind == IRG_DATA && gl->data_len == n && memcmp(gl->data, s, n) == 0) return g;
    }
    char name[32];
    snprintf(name, sizeof name, "s%d", l->nstrings++);
    return ir_add_data(l->m, name, s, n);
}

static int runtime_extern(L *l, int *slot, const char *name, int arity) {
    if (*slot < 0) *slot = ir_add_extern(l->m, name, arity);
    return *slot;
}

/* ---- typed boundaries ---- */

/* A value flows into a slot of type `slot` (an annotated variable, parameter
 * or return value). `what` names the slot ("variable 'n'", ...). If the
 * value's static type fits, nothing is emitted; if it cannot possibly fit,
 * that is a compile error; otherwise a runtime guard is inserted. Returns
 * the value's static type after the boundary. */
static Ty flow(L *l, Val v, Ty slot, const char *what, int line, int col) {
    if (v.v < 0 || ir_ty_sub(v.ty, slot)) return v.ty;
    char want[300], got[300];
    ty_name(l, slot, want, sizeof want);
    ty_name(l, v.ty, got, sizeof got);
    if (!ir_ty_inter(v.ty, slot)) {
        lerr(l, line, col, "%s expects %s, got %s", what, want, got);
        return slot;
    }
    /* gradual boundary: `%v = check %v, SLOT, @context` narrows the value in
     * place (the runtime message uses the same wording) */
    IrInstr *in = emit(l, IR_CHECK, line);
    in->dst = v.v;
    in->a = v.v;
    in->ty = slot;
    in->global = string_data(l, what, strlen(what));
    return ir_ty_inter(v.ty, slot);
}


static int instantiate(L *l, int gi, const Ty *targs, int line, int col);

/* The struct (index into l->structs) a type is exactly, or -1. */
static int static_struct_of(L *l, Ty t) {
    int sid = ty_sid(t);
    if (sid < 0 || !ir_ty_sub(t, ty_struct_of(sid))) return -1;
    for (int i = 0; i < l->nstructs; i++)
        if (l->structs[i].sid == sid) return i;
    return -1;
}

/* A type argument as the instance's name spells it: only types that can be
 * written (int, Point?, nil, any, ...); anything else (int|str, any struct)
 * becomes any. */
static Ty targ_norm(Ty t) {
    if (t == 0 || t == T_ANY) return T_ANY;
    int sid = ty_sid(t);
    if ((t & TY_STRUCT) && sid < 0) return T_ANY;
    int members = __builtin_popcount(t & (T_INT | T_STR | T_BOOL)) + ((t & TY_STRUCT) ? 1 : 0);
    if (members == 1 || (members == 0 && (t & T_NIL))) return t;
    return T_ANY;
}

static const TArg *tenv_find(L *l, const char *name) {
    for (int i = 0; i < l->ntenv; i++)
        if (strcmp(l->tenv[i].name, name) == 0) return &l->tenv[i];
    return NULL;
}

/* Resolves a Luma type annotation; T_ANY if absent. */
static Ty resolve_type(L *l, const TypeRef *t) {
    if (!t->name) return T_ANY;
    if (t->pointer) {
        lerr(l, t->line, t->col, "'*%s' is a C pointer type, only allowed in extern declarations (a Luma value of a C struct is just %s)",
             t->name, t->name);
        return T_ANY;
    }
    Ty nil = t->nullable ? T_NIL : 0;
    const TArg *ta = tenv_find(l, t->name);
    if (ta) {
        if (t->nargs) {
            lerr(l, t->line, t->col, "type parameter '%s' takes no type arguments", t->name);
            return T_ANY;
        }
        return ta->ty == T_ANY ? T_ANY : ir_ty_union(ta->ty, nil);
    }
    int gi = find_generic(l, t->name);
    if (gi >= 0) {
        const Stmt *d = l->generics[gi].decl;
        if (t->nargs != d->ntparams) {
            lerr(l, t->line, t->col, "'%s' takes %zu type argument%s, got %zu", t->name, d->ntparams,
                 d->ntparams == 1 ? "" : "s", t->nargs);
            return T_ANY;
        }
        Ty *args = xmalloc(t->nargs * sizeof *args);
        for (size_t i = 0; i < t->nargs; i++) args[i] = resolve_type(l, &t->args[i]);
        int si = l->failed ? -1 : instantiate(l, gi, args, t->line, t->col);
        free(args);
        return si >= 0 ? ty_struct_of(l->structs[si].sid) | nil : T_ANY;
    }
    if (t->nargs) {
        lerr(l, t->line, t->col, "'%s' takes no type arguments (it is not a generic struct)", t->name);
        return T_ANY;
    }
    static const struct { const char *name; Ty ty; } N[] = {
        {"int", T_INT}, {"str", T_STR}, {"bool", T_BOOL}, {"nil", T_NIL}, {"any", T_ANY}};
    for (size_t i = 0; i < sizeof N / sizeof *N; i++)
        if (strcmp(t->name, N[i].name) == 0) return N[i].ty | nil;
    int si = find_struct(l, t->name);
    if (si >= 0) return ty_struct_of(l->structs[si].sid) | nil;
    CType c;
    if (ctype_from_name(t->name, false, &c) || strcmp(t->name, "f32") == 0 || strcmp(t->name, "f64") == 0)
        lerr(l, t->line, t->col, "'%s' is a C type; it can only be used in extern declarations (Luma types: int, str, bool, nil, any, or a struct)", t->name);
    else
        lerr(l, t->line, t->col, "unknown type '%s' (Luma types: int, str, bool, nil, any, or a struct)", t->name);
    return T_ANY;
}

/* Type inference for a generic struct's type parameters (names[0..n)):
 * matches the annotation `pat` (as written in the generic's declaration)
 * against a value's static type, binding parameters it names. */
static void unify(L *l, const TypeRef *pat, Ty actual, const char *const *names, size_t n, Ty *binds, bool *bound) {
    if (!pat->name || actual == 0) return;
    if (!pat->nargs)
        for (size_t i = 0; i < n; i++)
            if (strcmp(pat->name, names[i]) == 0) {
                Ty a = pat->nullable && actual != T_ANY ? ir_ty_inter(actual, T_ANY & ~T_NIL) : actual;
                if (!a) return; /* only nil: says nothing about the parameter */
                binds[i] = bound[i] ? ir_ty_union(binds[i], a) : a;
                bound[i] = true;
                return;
            }
    int gi = find_generic(l, pat->name);
    if (gi < 0 || pat->nargs != l->generics[gi].decl->ntparams) return;
    int si = static_struct_of(l, ir_ty_inter(actual, T_ANY & ~T_NIL));
    if (si < 0 || l->structs[si].generic != gi) return;
    for (size_t k = 0; k < pat->nargs; k++) unify(l, &pat->args[k], l->structs[si].targs[k], names, n, binds, bound);
}

/* A C struct in an extern signature: `*S` / `*S?` (a pointer: CT_PTR) or
 * `S` (by value, at most 16 bytes: CT_CSW0 / CT_CSRET). Returns the
 * struct's IR id, or -1 when t names no C struct (or after an error). */
static int extern_struct_type(L *l, const TypeRef *t, bool is_return, CType *ct, bool *nullable) {
    *nullable = false;
    if (!t->name) return -1;
    int si = find_struct(l, t->name);
    bool cs = si >= 0 && l->structs[si].decl && l->structs[si].decl->cstruct;
    if (!cs) {
        if (t->pointer)
            lerr(l, t->line, t->col, "'*%s': only C structs (extern struct) have pointer types here; use ptr for other pointers", t->name);
        return -1;
    }
    int sid = l->structs[si].sid;
    if (t->pointer) {
        *ct = CT_PTR;
        *nullable = t->nullable;
        return sid;
    }
    if (t->nullable) {
        lerr(l, t->line, t->col, "a C struct passed by value cannot be nil; use *%s? for a pointer that may be NULL", t->name);
        return -1;
    }
    if (l->m->structs[sid].csize > 16) {
        lerr(l, t->line, t->col, "C struct '%s' is %d bytes: structs larger than 16 bytes are passed by pointer here (*%s)",
             t->name, l->m->structs[sid].csize, t->name);
        return -1;
    }
    *ct = is_return ? CT_CSRET : CT_CSW0;
    return sid;
}

/* The C struct (index into l->structs) a type is exactly, or -1. */
static int cstruct_of(L *l, Ty t);

/* Resolves a C type in an extern declaration. */
static CType resolve_ctype(L *l, const TypeRef *t, bool is_return) {
    CType c = CT_I64;
    if (strcmp(t->name, "f32") == 0 || strcmp(t->name, "f64") == 0) {
        lerr(l, t->line, t->col, "floating-point C types are not supported yet");
        return c;
    }
    if (!ctype_from_name(t->name, t->nullable, &c)) {
        if (t->nullable && ctype_from_name(t->name, false, &c))
            lerr(l, t->line, t->col, "only cstr has a nullable form (cstr?); '%s?' is not a C type", t->name);
        else if (strcmp(t->name, "int") == 0 || strcmp(t->name, "str") == 0 || strcmp(t->name, "bool") == 0 ||
                 strcmp(t->name, "nil") == 0 || strcmp(t->name, "any") == 0)
            lerr(l, t->line, t->col, "'%s' is a Luma type; extern declarations use C types "
                 "(i8..i64, u8..u64, bool, cstr, cstr?, ptr, void)", t->name);
        else
            lerr(l, t->line, t->col, "unknown C type '%s' (i8..i64, u8..u64, bool, cstr, cstr?, ptr, void)", t->name);
        return CT_I64;
    }
    if (c == CT_VOID && !is_return) lerr(l, t->line, t->col, "'void' is only allowed as a return type");
    return c;
}

/* ---- name resolution ---- */

typedef enum { NAME_NONE, NAME_LOCAL, NAME_GLOBAL, NAME_FUNCTION, NAME_EXTERN, NAME_BUILTIN } NameKind;

typedef struct {
    NameKind kind;
    int index;          /* vreg / gvar index / fn index / extern index */
    bool uninitialized; /* NAME_LOCAL: still inside its own initializer */
    Ty ty;              /* NAME_LOCAL / NAME_GLOBAL: declared type */
} Name;

static int find_fn(L *l, const char *name) {
    for (int i = 0; i < l->nfns; i++)
        if (strcmp(l->fns[i].name, name) == 0) return i;
    return -1;
}

static int find_ext(L *l, const char *name) {
    for (int i = 0; i < l->nexts; i++)
        if (strcmp(l->exts[i].name, name) == 0) return i;
    return -1;
}

static int find_gvar(L *l, const char *name) {
    for (int i = 0; i < l->ngvars; i++)
        if (strcmp(l->gvars[i].name, name) == 0) return i;
    return -1;
}

enum { BUILTIN_PRINT, BUILTIN_FREE, BUILTIN_DISPOSE, BUILTIN_REFCOUNT };
static int builtin_index(const char *name) {
    if (strcmp(name, "print") == 0) return BUILTIN_PRINT;
    if (strcmp(name, "free") == 0) return BUILTIN_FREE;
    if (strcmp(name, "dispose") == 0) return BUILTIN_DISPOSE;
    if (strcmp(name, "refcount") == 0) return BUILTIN_REFCOUNT;
    return -1;
}
static bool is_builtin(const char *name) { return builtin_index(name) >= 0; }

static Name lookup(L *l, const char *name) {
    Fn *f = l->f;
    for (int i = f->nscope - 1; i >= 0; i--)
        if (strcmp(f->scope[i].name, name) == 0)
            return (Name){NAME_LOCAL, f->scope[i].vreg, f->scope[i].vreg < 0, f->scope[i].ty};
    int g = find_gvar(l, name);
    /* Function bodies see every top-level variable; top-level code only those
     * already declared (later ones are compile errors, as in a script). */
    if (g >= 0 && (!f->is_main || l->gvars[g].declared)) return (Name){NAME_GLOBAL, g, false, l->gvars[g].ty};
    int fi = find_fn(l, name);
    if (fi >= 0) return (Name){NAME_FUNCTION, fi, false, T_ANY};
    int ei = find_ext(l, name);
    if (ei >= 0) return (Name){NAME_EXTERN, ei, false, T_ANY};
    if (is_builtin(name)) return (Name){NAME_BUILTIN, builtin_index(name), false, T_ANY};
    return (Name){NAME_NONE, 0, false, T_ANY};
}

/* Resolves a name used as a value or assignment target; reports errors. */
static Name resolve_variable(L *l, const char *name, int line, int col) {
    Name n = lookup(l, name);
    switch (n.kind) {
    case NAME_NONE:
        if (find_struct(l, name) >= 0)
            lerr(l, line, col, "'%s' is a struct, not a value; create one with %s { ... }", name, name);
        else
            lerr(l, line, col, "undefined variable '%s'", name);
        break;
    case NAME_LOCAL:
        if (n.uninitialized) {
            lerr(l, line, col, "cannot read local variable '%s' in its own initializer", name);
            n.kind = NAME_NONE;
        }
        break;
    case NAME_FUNCTION:
    case NAME_EXTERN:
        lerr(l, line, col, "'%s' is a function; functions are not first-class values yet, so it can only be called", name);
        break;
    case NAME_BUILTIN: lerr(l, line, col, "'%s' is a builtin function: call it as %s(...)", name, name); break;
    case NAME_GLOBAL: break;
    }
    return n;
}

static void bind(L *l, const char *name, int vreg, Ty ty) {
    Fn *f = l->f;
    if (f->nscope == f->cap_scope) {
        f->cap_scope = f->cap_scope ? f->cap_scope * 2 : 16;
        f->scope = xrealloc(f->scope, (size_t)f->cap_scope * sizeof *f->scope);
    }
    Binding *b = &f->scope[f->nscope++];
    b->name = xstrdup(name);
    b->vreg = vreg;
    b->depth = f->depth;
    b->ty = ty;
}

static void pop_scope(L *l) {
    Fn *f = l->f;
    while (f->nscope > 0 && f->scope[f->nscope - 1].depth == f->depth) free(f->scope[--f->nscope].name);
    f->depth--;
}

/* ---- expressions ---- */

static bool has_assign(const Expr *e) {
    if (!e) return false;
    if (e->kind == EXPR_ASSIGN || has_assign(e->left) || has_assign(e->right)) return true;
    for (size_t i = 0; i < e->nargs; i++)
        if (has_assign(e->args[i])) return true;
    return false;
}

static IrOp binary_op(TokenKind k) {
    switch (k) {
    case TOK_PLUS: return IR_ADD;
    case TOK_MINUS: return IR_SUB;
    case TOK_STAR: return IR_MUL;
    case TOK_SLASH: return IR_DIV;
    case TOK_EQUAL_EQUAL: return IR_EQ;
    case TOK_BANG_EQUAL: return IR_NE;
    case TOK_LESS: return IR_LT;
    case TOK_LESS_EQUAL: return IR_LE;
    case TOK_GREATER: return IR_GT;
    case TOK_GREATER_EQUAL: return IR_GE;
    default: return IR_ADD;
    }
}

static const char *op_text(TokenKind k) {
    switch (k) {
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_LESS: return "<";
    case TOK_LESS_EQUAL: return "<=";
    case TOK_GREATER: return ">";
    case TOK_GREATER_EQUAL: return ">=";
    default: return "?";
    }
}

/* Static result type of a binary operator, or 0 (with an error) when the
 * operand types make it fail on every execution. */
static Ty binary_type(L *l, const Expr *e, Ty a, Ty b) {
    char ta[300], tb[300];
    switch (e->op) {
    case TOK_EQUAL_EQUAL:
    case TOK_BANG_EQUAL: return T_BOOL;
    case TOK_PLUS: {
        Ty r = ((a & T_INT) && (b & T_INT) ? T_INT : 0) | ((a & T_STR) && (b & T_STR) ? T_STR : 0);
        if (r) return r;
        ty_name(l, a, ta, sizeof ta);
        ty_name(l, b, tb, sizeof tb);
        lerr(l, e->line, e->col, "operands of '+' must be two ints or two strs, got %s and %s", ta, tb);
        return 0;
    }
    default: {
        if ((a & T_INT) && (b & T_INT)) return (e->op == TOK_MINUS || e->op == TOK_STAR || e->op == TOK_SLASH) ? T_INT : T_BOOL;
        ty_name(l, a, ta, sizeof ta);
        ty_name(l, b, tb, sizeof tb);
        lerr(l, e->line, e->col, "operands of '%s' must be ints, got %s and %s", op_text(e->op), ta, tb);
        return 0;
    }
    }
}

static Val lower_expr(L *l, const Expr *e, int hint);

/* Temporaries live to the end of their full expression (as in C++): the
 * receiver of a method call or field access and the arguments of a call,
 * when they are not plain variables, get a `keep` after the outermost
 * expression. A value a method returns may be owned by its receiver (an
 * object whose drop() frees it): `make().peek().n` must not free make()'s
 * result before `.n` is read. Values that can't be instances are skipped. */
static void hold_temp(L *l, const Expr *src, Val v) {
    Fn *f = l->f;
    if (v.v < 0 || src->kind == EXPR_VAR || f->edepth == 0 || !ir_ty_inter(v.ty, TY_STRUCT)) return;
    if (f->nheld == f->cap_held) {
        f->cap_held = f->cap_held ? 2 * f->cap_held : 8;
        f->held = xrealloc(f->held, (size_t)f->cap_held * sizeof *f->held);
    }
    f->held[f->nheld++] = v.v;
}

/* Ends the temporaries held since mark (a `keep` each, unless the block
 * already ended). */
static void release_held(L *l, int mark, int line) {
    Fn *f = l->f;
    if (!ir_block_terminated(&fn(l)->blocks[f->cur]))
        for (int i = mark; i < f->nheld; i++) emit(l, IR_KEEP, line)->a = f->held[i];
    f->nheld = mark;
}

static Val lower_into(L *l, const Expr *e, int dst) {
    Val v = lower_expr(l, e, dst);
    if (v.v >= 0) emit_mov(l, dst, v.v, e->line);
    return v.v >= 0 ? (Val){dst, v.ty} : NO_VAL;
}

/* Lowers a list of operands left to right. If a later operand assigns to a
 * local variable that an earlier operand merely named, the earlier value is
 * snapshotted first (evaluation is strictly left to right). */
static bool lower_operands(L *l, Expr *const *es, size_t n, Val *out) {
    for (size_t i = 0; i < n; i++) {
        Val v = lower_expr(l, es[i], -1);
        if (v.v < 0) return false;
        bool later_assigns = false;
        for (size_t j = i + 1; j < n && !later_assigns; j++) later_assigns = has_assign(es[j]);
        if (is_var_vreg(l, v.v) && later_assigns) {
            int t = temp(l);
            emit_mov(l, t, v.v, es[i]->line);
            v.v = t;
        }
        out[i] = v;
    }
    return true;
}

static Val result(L *l, int hint, int *d) {
    if (hint == DISCARD) return NO_VAL;
    if (*d < 0) *d = temp(l);
    return (Val){*d, T_ANY};
}

/* The prelude's intrinsics: runtime entry points named __name, callable only
 * from prelude code (docs/DESIGN.md "The prelude"). */
static const struct {
    const char *name, *symbol;
    int arity;
    Ty ret;
} INTRINSICS[] = {
    {"__arena_create", "luma_arena_create", 0, T_ANY}, /* (results of runtime calls are any in the IR) */
    {"__arena_push", "luma_arena_enter", 1, T_NIL},
    {"__arena_pop", "luma_arena_leave", 0, T_NIL},
    {"__arena_destroy", "luma_arena_destroy", 1, T_NIL},
    {"__show", "luma_show", 1, T_ANY},
    /* std library primitives (runtime/luma_rt.c) */
    {"__array_new", "luma_array_new", 1, T_ANY},
    {"__array_len", "luma_array_len", 1, T_ANY},
    {"__array_get", "luma_array_get", 2, T_ANY},
    {"__array_set", "luma_array_set", 3, T_ANY},
    {"__str_len", "luma_str_len", 1, T_ANY},
    {"__str_byte", "luma_str_byte", 2, T_ANY},
    {"__str_char", "luma_str_char", 1, T_ANY},
    {"__str_slice", "luma_str_slice", 3, T_ANY},
    {"__str_find", "luma_str_find", 3, T_ANY},
    {"__str_cmp", "luma_str_cmp", 2, T_ANY},
    {"__str_from_int", "luma_str_from_int", 1, T_ANY},
    {"__str_to_int", "luma_str_to_int", 1, T_ANY},
    {"__str_case", "luma_str_case", 2, T_ANY},
    {"__str_join", "luma_str_join", 3, T_ANY},
    {"__fail", "luma_fail", 1, T_ANY},
    {"__type", "luma_type_name", 1, T_ANY},
    {"__array_sort", "luma_array_sort", 2, T_ANY},
    {"__argc", "luma_argc", 0, T_ANY},
    {"__argv", "luma_argv", 1, T_ANY},
    {"__mem_string", "luma_mem_string", 2, T_ANY},
    {"__mem_write", "luma_mem_write", 2, T_ANY},
};

static bool in_prelude(L *l) {
    const FnInfo *fi = cur_info(l);
    return fi && fi->decl->line >= LUMA_PRELUDE_LINE;
}

static size_t intrinsic_index(const char *name) {
    size_t k = 0;
    while (k < sizeof INTRINSICS / sizeof *INTRINSICS && strcmp(INTRINSICS[k].name, name) != 0) k++;
    return k;
}

static Val lower_intrinsic(L *l, const Expr *e, int hint) {
    size_t k = intrinsic_index(e->name);
    if ((int)e->nargs != INTRINSICS[k].arity) {
        lerr(l, e->line, e->col, "'%s' expects %d argument%s, got %zu", e->name, INTRINSICS[k].arity,
             INTRINSICS[k].arity == 1 ? "" : "s", e->nargs);
        return NO_VAL;
    }
    Val v[4];
    if (!lower_operands(l, e->args, e->nargs, v)) return NO_VAL;
    int d = hint >= 0 ? hint : -1;
    Val r = result(l, hint, &d);
    int g = runtime_extern(l, &l->intrinsic_fn[k], INTRINSICS[k].symbol, INTRINSICS[k].arity);
    int args[4];
    for (size_t i = 0; i < e->nargs; i++) args[i] = v[i].v;
    emit_call(l, hint == DISCARD ? IR_NONE : d, g, e->nargs ? args : NULL, (int)e->nargs, e->line);
    r.ty = INTRINSICS[k].ret;
    return r;
}

static Val lower_call(L *l, const Expr *e, int hint) {
    if (strncmp(e->name, "__", 2) == 0) { /* intrinsics and the std files' own helpers */
        if (!in_prelude(l)) {
            lerr(l, e->line, e->col, "'%s' is internal to the prelude (names starting with __ are reserved)", e->name);
            return NO_VAL;
        }
        if (intrinsic_index(e->name) < sizeof INTRINSICS / sizeof *INTRINSICS) return lower_intrinsic(l, e, hint);
    }
    Name n = lookup(l, e->name);
    if (n.kind == NAME_LOCAL || n.kind == NAME_GLOBAL) {
        lerr(l, e->line, e->col, "'%s' is a variable, not a function", e->name);
        return NO_VAL;
    }
    if (n.kind == NAME_NONE) {
        if (find_struct(l, e->name) >= 0)
            lerr(l, e->line, e->col, "'%s' is a struct, not a function; create one with %s { ... } (or call one of its functions, %s::name(...))",
                 e->name, e->name, e->name);
        else
            lerr(l, e->line, e->col, "undefined function '%s'", e->name);
        return NO_VAL;
    }
    int arity = n.kind == NAME_FUNCTION ? l->fns[n.index].arity
              : n.kind == NAME_EXTERN ? (int)l->exts[n.index].decl->nparams : -1;
    if (arity >= 0 && (int)e->nargs != arity) {
        lerr(l, e->line, e->col, "'%s' expects %d argument%s, got %zu", e->name, arity, arity == 1 ? "" : "s", e->nargs);
        return NO_VAL;
    }
    Val *vals = xmalloc((e->nargs ? e->nargs : 1) * sizeof *vals);
    int *args = xmalloc((e->nargs ? e->nargs : 1) * sizeof *args);
    Val r = NO_VAL;
    if (!lower_operands(l, e->args, e->nargs, vals)) goto done;
    for (size_t i = 0; i < e->nargs; i++) args[i] = vals[i].v;
    int d = hint >= 0 ? hint : -1;
    if (n.kind == NAME_FUNCTION)
        for (size_t i = 0; i < e->nargs; i++) hold_temp(l, e->args[i], vals[i]);

    if (n.kind == NAME_BUILTIN && n.index == BUILTIN_FREE) {
        /* free(x): releases a struct instance in --mm=manual (docs/DESIGN.md
         * "Memory"); the same program compiles in every mode */
        if (e->nargs != 1) {
            lerr(l, e->line, e->col, "'free' expects 1 argument, got %zu", e->nargs);
            goto done;
        }
        if (!(vals[0].ty & (TY_STRUCT | T_NIL))) {
            char tn[300];
            ty_name(l, vals[0].ty, tn, sizeof tn);
            lerr(l, e->args[0]->line, e->args[0]->col, "free() expects a struct instance, got %s%s", tn,
                 vals[0].ty == T_STR ? " (strings are freed automatically)" : "");
            goto done;
        }
        int ssi = static_struct_of(l, vals[0].ty);
        if (ssi >= 0 && l->m->structs[l->structs[ssi].sid].shared) {
            char tn[600];
            ir_struct_display(l->structs[ssi].name, tn, sizeof tn);
            if (l->m->structs[l->structs[ssi].sid].cstruct)
                lerr(l, e->args[0]->line, e->args[0]->col, "'%s' is a C struct: its value is freed automatically (and memory C owns stays C's)", tn);
            else
                lerr(l, e->args[0]->line, e->args[0]->col, "'%s' is a shared struct; it is freed when its last reference goes away", tn);
            goto done;
        }
        emit_call(l, IR_NONE, runtime_extern(l, &l->free_fn, "luma_free", 1), args, 1, e->line);
        if (hint != DISCARD) {
            r = result(l, hint, &d);
            emit(l, IR_CONST_NIL, e->line)->dst = d;
            r.ty = T_NIL;
        }
        goto done;
    }
    if (n.kind == NAME_BUILTIN && (n.index == BUILTIN_DISPOSE || n.index == BUILTIN_REFCOUNT)) {
        /* dispose(x): frees x if it is an instance managed by hand, else
         * nothing (docs/DESIGN.md "Memory"); refcount(x): x's reference count */
        bool dispose = n.index == BUILTIN_DISPOSE;
        if (e->nargs != 1) {
            lerr(l, e->line, e->col, "'%s' expects 1 argument, got %zu", e->name, e->nargs);
            goto done;
        }
        if (dispose) {
            emit_call(l, IR_NONE, runtime_extern(l, &l->dispose_fn, "luma_dispose", 1), args, 1, e->line);
            if (hint != DISCARD) {
                r = result(l, hint, &d);
                emit(l, IR_CONST_NIL, e->line)->dst = d;
                r.ty = T_NIL;
            }
        } else {
            r = result(l, hint, &d);
            emit_call(l, hint == DISCARD ? IR_NONE : d, runtime_extern(l, &l->refcount_fn, "luma_refcount", 1), args, 1, e->line);
            r.ty = T_ANY; /* (a runtime call: an int, checked where it must be) */
        }
        goto done;
    }
    if (n.kind == NAME_BUILTIN) { /* print(a, b, ...): all arguments are evaluated before anything is written */
        int w = runtime_extern(l, &l->write_fn, "luma_write", 1);
        for (size_t i = 0; i < e->nargs; i++) {
            if (i > 0) emit_call(l, IR_NONE, runtime_extern(l, &l->space_fn, "luma_write_space", 0), NULL, 0, e->line);
            emit_call(l, IR_NONE, w, &args[i], 1, e->line);
        }
        emit_call(l, IR_NONE, runtime_extern(l, &l->newline_fn, "luma_write_newline", 0), NULL, 0, e->line);
        if (hint != DISCARD) {
            r = result(l, hint, &d);
            emit(l, IR_CONST_NIL, e->line)->dst = d; /* print returns nil */
            r.ty = T_NIL;
        }
        goto done;
    }
    if (n.kind == NAME_EXTERN) {
        const ExtInfo *x = &l->exts[n.index];
        int cargs[MAX_PARAMS * 2], nc = 0;
        int kept[MAX_PARAMS], nkept = 0; /* *S arguments: alive until C returns */
        for (size_t i = 0; i < e->nargs; i++) {
            if (x->psid[i] >= 0) { /* a C struct: *S (its address) or S (its eightbytes) */
                char what[300];
                snprintf(what, sizeof what, "argument '%s' of '%s'", x->decl->params[i], x->name);
                Ty want = ty_struct_of(x->psid[i]) | (x->pnull[i] ? T_NIL : 0);
                vals[i].ty = flow(l, vals[i], want, what, e->args[i]->line, e->args[i]->col);
                if (l->failed) goto done;
                if (x->ctypes[i] == CT_PTR) {
                    int a = temp(l);
                    emit_call(l, a, runtime_extern(l, &l->cs_fn[3], "luma_cs_addr", 1), &vals[i].v, 1, e->line);
                    cargs[nc++] = a;
                    kept[nkept++] = vals[i].v;
                } else {
                    cargs[nc++] = vals[i].v;
                    if (l->m->structs[x->psid[i]].csize > 8) cargs[nc++] = vals[i].v;
                }
                continue;
            }
            cargs[nc++] = vals[i].v;
            /* Static check only: the runtime converter validates the value anyway. */
            Ty want = ctype_luma(x->ctypes[i]);
            if (!(vals[i].ty & want)) {
                char w[300], g[300];
                ty_name(l, want, w, sizeof w);
                ty_name(l, vals[i].ty, g, sizeof g);
                lerr(l, e->args[i]->line, e->args[i]->col, "argument '%s' of '%s' expects %s (C %s), got %s",
                     x->decl->params[i], x->name, w, ctype_name(x->ctypes[i]), g);
                goto done;
            }
        }
        if (x->rsid >= 0 && x->cret == CT_PTR) { /* *S: a view of C's memory (or nil for NULL with *S?) */
            int a = temp(l), z = temp(l), o = temp(l);
            emit_call(l, a, x->global, cargs, nc, e->line);
            for (int i = 0; i < nkept; i++) emit(l, IR_KEEP, e->line)->a = kept[i];
            emit(l, IR_CONST_INT, e->line)->dst = z;
            IrInstr *nw = emit(l, IR_NEW, e->line);
            nw->dst = o;
            nw->sid = x->rsid;
            nw->nargs = 2;
            nw->args = xmalloc(2 * sizeof(int));
            nw->args[0] = a;
            nw->args[1] = z;
            r = result(l, hint, &d);
            if (hint != DISCARD) {
                char what[300];
                snprintf(what, sizeof what, "the result of %s", x->name);
                int vargs[3] = {o, temp(l), temp(l)};
                emit(l, x->rnull ? IR_CONST_TRUE : IR_CONST_FALSE, e->line)->dst = vargs[1];
                IrInstr *c = emit(l, IR_CONST_DATA, e->line);
                c->dst = vargs[2];
                c->global = string_data(l, what, strlen(what));
                emit_call(l, d, runtime_extern(l, &l->cs_fn[4], "luma_cs_view", 3), vargs, 3, e->line);
                Ty t = ty_struct_of(x->rsid) | (x->rnull ? T_NIL : 0);
                r.ty = T_ANY;
                r.ty = flow(l, r, t, what, e->line, e->col); /* (the view is exactly that) */
            }
            goto done;
        }
        r = result(l, hint, &d);
        emit_call(l, hint == DISCARD ? IR_NONE : d, x->global, cargs, nc, e->line);
        for (int i = 0; i < nkept; i++) emit(l, IR_KEEP, e->line)->a = kept[i];
        r.ty = ctype_luma(x->cret);
        if (x->rsid >= 0 && hint != DISCARD) { /* a struct by value: the runtime boxed it */
            char what[300];
            snprintf(what, sizeof what, "the result of %s", x->name);
            r.ty = T_ANY;
            r.ty = flow(l, r, ty_struct_of(x->rsid), what, e->line, e->col);
        }
        goto done;
    }
    const FnInfo *fi = &l->fns[n.index];
    for (size_t i = 0; i < e->nargs; i++) {
        char what[300];
        snprintf(what, sizeof what, "argument '%s' of '%s'", fi->decl->params[i], fi->name);
        flow(l, vals[i], fi->ptypes[i], what, e->args[i]->line, e->args[i]->col);
    }
    if (l->failed) goto done;
    r = result(l, hint, &d);
    emit_call(l, hint == DISCARD ? IR_NONE : d, fi->global, args, (int)e->nargs, e->line);
    r.ty = fi->ret;
done:
    free(vals);
    free(args);
    return r;
}

/* ---- structs ---- */

/* Calls an impl function (or a method with an explicit receiver) whose
 * argument values are already lowered: vals[i] goes to parameter i. at[i] is
 * the expression of argument i for error positions (NULL: use line/col). */
static Val call_fn(L *l, const FnInfo *fi, const Val *vals, Expr *const *at, int hint, int line, int col) {
    int n = fi->arity;
    int *args = xmalloc((size_t)(n ? n : 1) * sizeof *args);
    for (int i = 0; i < n; i++) {
        char what[300];
        snprintf(what, sizeof what, "argument '%s' of '%s'", fi->decl->params[i], fi->name);
        flow(l, vals[i], fi->ptypes[i], what, at && at[i] ? at[i]->line : line, at && at[i] ? at[i]->col : col);
        args[i] = vals[i].v;
    }
    Val r = NO_VAL;
    if (!l->failed) {
        int d = hint >= 0 ? hint : -1;
        r = result(l, hint, &d);
        emit_call(l, hint == DISCARD ? IR_NONE : d, fi->global, args, n, line);
        r.ty = fi->ret;
    }
    free(args);
    return r;
}

/* The struct a value is statically known to be (exactly), or -1. */
static int static_struct(L *l, Ty t) {
    int sid = ty_sid(t);
    if (sid < 0 || !ir_ty_sub(t, ty_struct_of(sid))) return -1;
    for (int i = 0; i < l->nstructs; i++)
        if (l->structs[i].sid == sid) return i;
    return -1;
}

static int cstruct_of(L *l, Ty t) {
    int si = static_struct(l, t);
    return si >= 0 && l->m->structs[l->structs[si].sid].cstruct ? si : -1;
}

/* ---- C structs (extern struct): their fields live in C memory, reached
 * through runtime calls with the field's index (docs/DESIGN.md "C FFI"). */

static int const_int(L *l, int64_t v, int line) {
    int d = temp(l);
    IrInstr *in = emit(l, IR_CONST_INT, line);
    in->dst = d;
    in->imm = v;
    return d;
}

/* s.f / s.f = v on a value statically of C struct si. */
static Val cstruct_field(L *l, int si, Val obj, const Expr *e, Val *set, int hint) {
    const IrStruct *st = &l->m->structs[l->structs[si].sid];
    int k = ir_cstruct_field(st, e->name);
    if (k < 0) {
        lerr(l, e->line, e->col, "C struct '%s' has no field '%s'", st->name, e->name);
        return NO_VAL;
    }
    char what[600];
    snprintf(what, sizeof what, "field '%s' of %s", e->name, st->name);
    CType ct = st->cftys[k];
    int ki = const_int(l, k, e->line);
    if (set) {
        if (ct == CT_CSTR || ct == CT_CSTR_OPT) {
            lerr(l, e->line, e->col, "%s is a C string: it can be read, not written (its memory belongs to C)", what);
            return NO_VAL;
        }
        set->ty = flow(l, *set, ctype_luma(ct), what, e->right->line, e->right->col);
        int args[3] = {obj.v, ki, set->v};
        emit_call(l, IR_NONE, runtime_extern(l, &l->cs_fn[2], "luma_cs_set", 3), args, 3, e->line);
        if (hint >= 0) {
            emit_mov(l, hint, set->v, e->line);
            return (Val){hint, set->ty};
        }
        return *set;
    }
    int d = temp(l);
    int args[2] = {obj.v, ki};
    emit_call(l, d, runtime_extern(l, &l->cs_fn[1], "luma_cs_get", 2), args, 2, e->line);
    Val r = {d, T_ANY};
    r.ty = flow(l, r, ctype_luma(ct), what, e->line, e->col); /* (the runtime converted it to exactly this) */
    if (hint >= 0) {
        emit_mov(l, hint, d, e->line);
        r.v = hint;
    }
    return r;
}

/* S { f: v, ... } of a C struct: zeroed C memory, then the given fields. */
static Val cstruct_literal(L *l, int si, const Expr *e, int hint) {
    int sid = l->structs[si].sid;
    const IrStruct *st = &l->m->structs[sid];
    for (size_t i = 0; i < e->nargs; i++) {
        int k = ir_cstruct_field(st, e->fields[i]);
        if (k < 0) {
            lerr(l, e->field_line[i], e->field_col[i], "C struct '%s' has no field '%s'", st->name, e->fields[i]);
            return NO_VAL;
        }
        for (size_t j = 0; j < i; j++)
            if (strcmp(e->fields[j], e->fields[i]) == 0) {
                lerr(l, e->field_line[i], e->field_col[i], "field '%s' is given twice", e->fields[i]);
                return NO_VAL;
            }
        if (st->cftys[k] == CT_CSTR || st->cftys[k] == CT_CSTR_OPT) {
            lerr(l, e->field_line[i], e->field_col[i], "field '%s' of %s is a C string: it can be read, not written (its memory belongs to C)",
                 e->fields[i], st->name);
            return NO_VAL;
        }
    }
    Val *vals = xmalloc((e->nargs ? e->nargs : 1) * sizeof *vals);
    if (!lower_operands(l, e->args, e->nargs, vals)) {
        free(vals);
        return NO_VAL;
    }
    int z = const_int(l, 0, e->line), o = temp(l);
    IrInstr *in = emit(l, IR_NEW, e->line);
    in->dst = o;
    in->sid = sid;
    in->nargs = 2;
    in->args = xmalloc(2 * sizeof(int));
    in->args[0] = in->args[1] = z;
    emit_call(l, IR_NONE, runtime_extern(l, &l->cs_fn[0], "luma_cs_init", 1), &o, 1, e->line);
    for (size_t i = 0; i < e->nargs && !l->failed; i++) {
        int k = ir_cstruct_field(st, e->fields[i]);
        char what[600];
        snprintf(what, sizeof what, "field '%s' of %s", e->fields[i], st->name);
        vals[i].ty = flow(l, vals[i], ctype_luma(st->cftys[k]), what, e->args[i]->line, e->args[i]->col);
        int args[3] = {o, const_int(l, k, e->line), vals[i].v};
        emit_call(l, IR_NONE, runtime_extern(l, &l->cs_fn[2], "luma_cs_set", 3), args, 3, e->line);
    }
    free(vals);
    if (l->failed) return NO_VAL;
    if (hint == DISCARD) return NO_VAL;
    if (hint >= 0) {
        emit_mov(l, hint, o, e->line);
        return (Val){hint, ty_struct_of(sid)};
    }
    return (Val){o, ty_struct_of(sid)};
}

/* S::size(), S::at(address), S::addr(s): the functions every C struct has
 * (unless its impl defines one of these names). */
static bool cstruct_builtin(L *l, int si, const Expr *e, int hint, Val *out) {
    int sid = l->structs[si].sid;
    const IrStruct *st = &l->m->structs[sid];
    bool size = strcmp(e->str, "size") == 0, at = strcmp(e->str, "at") == 0, addr = strcmp(e->str, "addr") == 0;
    if (!size && !at && !addr) return false;
    if (e->nargs != (size ? 0u : 1u)) {
        lerr(l, e->line, e->col, "'%s::%s' expects %d argument%s, got %zu", st->name, e->str, size ? 0 : 1, size ? "s" : "", e->nargs);
        *out = NO_VAL;
        return true;
    }
    if (size) {
        int d = hint >= 0 ? hint : temp(l);
        IrInstr *in = emit(l, IR_CONST_INT, e->line);
        in->dst = d;
        in->imm = st->csize;
        *out = (Val){d, T_INT};
        return true;
    }
    Val v = lower_expr(l, e->args[0], -1);
    *out = NO_VAL;
    if (v.v < 0) return true;
    char what[600];
    snprintf(what, sizeof what, "argument of %s::%s", st->name, e->str);
    int d = hint >= 0 ? hint : temp(l);
    if (addr) { /* the address of its C memory */
        v.ty = flow(l, v, ty_struct_of(sid), what, e->args[0]->line, e->args[0]->col);
        emit_call(l, d, runtime_extern(l, &l->cs_fn[3], "luma_cs_addr", 1), &v.v, 1, e->line);
        Val r = {d, T_ANY};
        r.ty = flow(l, r, T_INT, what, e->line, e->col);
        *out = r;
        return true;
    }
    /* at: a view of the C struct at that address (memory C owns) */
    v.ty = flow(l, v, T_INT, what, e->args[0]->line, e->args[0]->col);
    int z = const_int(l, 0, e->line), o = temp(l);
    IrInstr *in = emit(l, IR_NEW, e->line);
    in->dst = o;
    in->sid = sid;
    in->nargs = 2;
    in->args = xmalloc(2 * sizeof(int));
    in->args[0] = v.v;
    in->args[1] = z;
    int vargs[3] = {o, temp(l), temp(l)};
    emit(l, IR_CONST_FALSE, e->line)->dst = vargs[1];
    IrInstr *c = emit(l, IR_CONST_DATA, e->line);
    c->dst = vargs[2];
    c->global = string_data(l, what, strlen(what));
    emit_call(l, d, runtime_extern(l, &l->cs_fn[4], "luma_cs_view", 3), vargs, 3, e->line);
    Val r = {d, T_ANY};
    r.ty = flow(l, r, ty_struct_of(sid), what, e->line, e->col);
    *out = r;
    return true;
}

/* A field access or method call on a value that can never be a struct. */
static bool not_an_instance(L *l, Ty t, const char *what, int line, int col) {
    if (ir_ty_inter(t, TY_STRUCT)) return false;
    char tn[300];
    ty_name(l, t, tn, sizeof tn);
    lerr(l, line, col, "only struct instances have %s, got %s", what, tn);
    return true;
}

/* The owners an access went through by deref (deref_member): the value
 * read lives only as long as its owner, so the caller ends the access with
 * `keep %owner` (keep_boxes). */
typedef struct {
    int v[16];
    int n;
} Boxes;

static void keep_boxes(L *l, const Boxes *b, int line) {
    for (int i = 0; i < b->n; i++) emit(l, IR_KEEP, line)->a = b->v[i];
}

static bool is_self_var(const Expr *e) { return e && e->kind == EXPR_VAR && strcmp(e->name, "self") == 0; }

/* The field a trivial deref returns: a body that is exactly
 * `return self.f;` with f a field of struct si. -1 otherwise. */
static int trivial_deref_field(L *l, int si, const FnInfo *fi) {
    const Stmt *d = fi->decl;
    if (d->n != 1 || d->stmts[0]->kind != STMT_RETURN) return -1;
    const Expr *e = d->stmts[0]->expr;
    if (!e || e->kind != EXPR_GET || !is_self_var(e->left)) return -1;
    const IrStruct *st = &l->m->structs[l->structs[si].sid];
    int k = ir_struct_field(st, e->name);
    /* the function's return check would accept every value of the field */
    return k >= 0 && ir_ty_sub(st->ftys[k], fi->ret) ? k : -1;
}

/* deref (docs/DESIGN.md "Memory"): an access to a member (a field, or a
 * method when `method`) that the value's struct lacks goes to what its
 * deref(self) returns, repeatedly (at most 8 steps). Applies only when the
 * struct is known statically; values known at run time are handled by the
 * runtime. Each owner the access went through is recorded in boxes (the
 * caller ends the access with keep_boxes). *first is set to the struct the
 * access started at when deref was followed (for messages), else -1. */
static Val deref_member(L *l, Val v, const char *name, bool method, int line, int col, Boxes *boxes, int *first) {
    *first = -1;
    int seen[9], nseen = 0;
    for (;;) {
        int si = static_struct(l, v.ty);
        if (si < 0 || !l->structs[si].decl) return v;
        bool has = method ? find_impl_fn(l, si, name) >= 0
                          : ir_struct_field(&l->m->structs[l->structs[si].sid], name) >= 0;
        int dfi = find_impl_fn(l, si, "deref");
        if (has || dfi < 0) return v;
        for (int i = 0; i < nseen; i++)
            if (seen[i] == si) {
                char a[600];
                ir_struct_display(l->structs[si].name, a, sizeof a);
                lerr(l, line, col, "deref of '%s' leads back to '%s' (a cycle)", a, a);
                return NO_VAL;
            }
        if (nseen == 8) {
            lerr(l, line, col, "deref chain longer than 8 steps");
            return NO_VAL;
        }
        seen[nseen++] = si;
        if (*first < 0) *first = si;
        if (boxes->n < 16) boxes->v[boxes->n++] = v.v;
        int k = trivial_deref_field(l, si, &l->fns[dfi]);
        if (k >= 0) { /* `return self.f;`: the field itself, no call */
            int sid = l->structs[si].sid;
            int d = temp(l);
            IrInstr *in = emit(l, IR_GETFIELD, line);
            in->dst = d;
            in->a = v.v;
            in->sid = sid;
            in->field = k;
            v = (Val){d, l->m->structs[sid].ftys[k]};
        } else {
            v = call_fn(l, &l->fns[dfi], &v, NULL, -1, line, col);
            if (v.v < 0) return NO_VAL;
            if (boxes->n < 16) boxes->v[boxes->n++] = v.v; /* (an owned result: keep it too) */
        }
    }
}


static void lower_stmt(L *l, const Stmt *s);

static void push_exit(L *l, int v, Ty ty) {
    Fn *f = l->f;
    if (f->nexits == f->cap_exits) {
        f->cap_exits = f->cap_exits ? 2 * f->cap_exits : 4;
        f->exits = xrealloc(f->exits, (size_t)f->cap_exits * sizeof *f->exits);
    }
    f->exits[f->nexits].v = v;
    f->exits[f->nexits].ty = ty;
    f->nexits++;
}

/* subject.enter() / subject.leave() for `with` (no arguments; the result is
 * discarded). Statically when the subject's struct is known, else a dynamic
 * method call. */
static bool call_hook(L *l, Val subj, const char *name, int line, int col) {
    int si = static_struct(l, subj.ty);
    if (si >= 0) {
        int fi = find_impl_fn(l, si, name);
        if (fi < 0 || !l->fns[fi].has_self || l->fns[fi].arity != 1) {
            char sn[600];
            ir_struct_display(l->structs[si].name, sn, sizeof sn);
            lerr(l, line, col, "with needs a value with enter() and leave() methods: '%s' has no method %s(self)", sn, name);
            return false;
        }
        call_fn(l, &l->fns[fi], &subj, NULL, DISCARD, line, col);
        return !l->failed;
    }
    IrInstr *in = emit(l, IR_CALLM, line);
    in->dst = IR_NONE;
    in->a = subj.v;
    in->name = xstrdup(name);
    return true;
}

/* Leaves the blocks entered since exit index `from` (innermost first). */
static void leave_exits(L *l, int from, int line) {
    for (int i = l->f->nexits; i-- > from && !l->failed;)
        call_hook(l, (Val){l->f->exits[i].v, l->f->exits[i].ty}, "leave", line, 0);
}

/* with SUBJECT { body }: subject.enter(), the body, subject.leave() on every
 * way out (the end, or a return inside). Its value is the subject. */
static Val lower_with(L *l, const Expr *e, int hint) {
    Val s = lower_expr(l, e->left, -1);
    if (s.v < 0) return NO_VAL;
    if (!ir_ty_inter(s.ty, TY_STRUCT)) {
        char tn[300];
        ty_name(l, s.ty, tn, sizeof tn);
        lerr(l, e->left->line, e->left->col, "with needs a struct instance with enter() and leave() methods, got %s", tn);
        return NO_VAL;
    }
    int t = temp(l); /* the subject, held for the whole block */
    emit_mov(l, t, s.v, e->line);
    Val subj = {t, s.ty};
    if (!call_hook(l, subj, "enter", e->line, e->col)) return NO_VAL;
    push_exit(l, t, s.ty);
    lower_stmt(l, e->body);
    l->f->nexits--;
    if (l->failed) return NO_VAL;
    if (!ir_block_terminated(&fn(l)->blocks[l->f->cur]) && !call_hook(l, subj, "leave", e->line, e->col)) return NO_VAL;
    if (hint == DISCARD) return NO_VAL;
    if (hint >= 0) {
        emit_mov(l, hint, t, e->line);
        return (Val){hint, s.ty};
    }
    return subj;
}

/* arena { body }  /  arena name { body }: everything allocated by hand in
 * the body (and in what it calls) goes into the arena. Its value is the
 * arena (a new one: freed when its last reference goes away). */
static Val lower_arena(L *l, const Expr *e, int hint) {
    /* sugar: arena { B } is with Arena::new() { B }; arena a { B } is with a { B } */
    Expr subj = {0};
    subj.line = e->line;
    subj.col = e->col;
    if (e->name) {
        subj.kind = EXPR_VAR;
        subj.name = e->name;
    } else {
        subj.kind = EXPR_ASSOC;
        subj.name = (char *)"Arena";
        subj.str = (char *)"new";
    }
    Expr w = *e;
    w.kind = EXPR_WITH;
    w.left = &subj;
    w.name = NULL;
    return lower_with(l, &w, hint);
}

/* The instance of generic gi whose type arguments are inferred from (in this
 * order) the type the value flows into, when that is an instance of gi, and
 * the static types of the values given for annotated parts (pats[i] against
 * tys[i]); parameters nothing determines are any. names: the parameter names
 * the patterns use. */
static int infer_instance(L *l, int gi, Ty expect, const char *const *names, const TypeRef *const *pats,
                          const Ty *tys, size_t n, const TypeRef *ret, int line, int col) {
    const Stmt *d = l->generics[gi].decl;
    size_t np = d->ntparams;
    Ty *binds = xcalloc(np, sizeof *binds);
    bool *bound = xcalloc(np, sizeof *bound);
    int esi = expect ? static_struct_of(l, ir_ty_inter(expect, T_ANY & ~T_NIL)) : -1;
    if (esi >= 0 && l->structs[esi].generic == gi && !ret) {
        for (size_t i = 0; i < np; i++) binds[i] = l->structs[esi].targs[i];
    } else {
        for (size_t i = 0; i < n; i++)
            if (pats[i]) unify(l, pats[i], tys[i], names, np, binds, bound);
        if (ret && expect) { /* Box::empty(): only the result names T */
            Ty *rb = xcalloc(np, sizeof *rb);
            bool *rbd = xcalloc(np, sizeof *rbd);
            unify(l, ret, expect, names, np, rb, rbd);
            for (size_t i = 0; i < np; i++)
                if (!bound[i] && rbd[i]) {
                    binds[i] = rb[i];
                    bound[i] = true;
                }
            free(rb);
            free(rbd);
        }
        for (size_t i = 0; i < np; i++)
            if (!bound[i]) binds[i] = T_ANY;
    }
    int si = instantiate(l, gi, binds, line, col);
    free(binds);
    free(bound);
    return si;
}

/* Name { field: value, ... } */
static Val lower_struct_literal(L *l, const Expr *e, int hint, Ty expect) {
    int si = find_struct(l, e->name);
    int gi = si < 0 ? find_generic(l, e->name) : -1;
    if (si >= 0 && l->m->structs[l->structs[si].sid].cstruct) return cstruct_literal(l, si, e, hint);
    if (si < 0 && gi < 0) {
        lerr(l, e->line, e->col, "unknown struct '%s'", e->name);
        return NO_VAL;
    }
    /* the fields as declared (for a generic struct, all instances share them) */
    const Stmt *decl = gi >= 0 ? l->generics[gi].decl : NULL;
    int nf = gi >= 0 ? (int)decl->nparams : l->m->structs[l->structs[si].sid].nfields;
    int *where = xmalloc((size_t)(nf ? nf : 1) * sizeof *where); /* field -> argument index */
    for (int k = 0; k < nf; k++) where[k] = -1;
    Val r = NO_VAL;
    for (size_t i = 0; i < e->nargs; i++) {
        int k = -1;
        if (gi >= 0) {
            for (int j = 0; j < nf && k < 0; j++)
                if (strcmp(decl->params[j], e->fields[i]) == 0) k = j;
        } else {
            k = ir_struct_field(&l->m->structs[l->structs[si].sid], e->fields[i]);
        }
        if (k < 0) {
            lerr(l, e->field_line[i], e->field_col[i], "struct '%s' has no field '%s'", e->name, e->fields[i]);
            goto done;
        }
        if (where[k] >= 0) {
            lerr(l, e->field_line[i], e->field_col[i], "field '%s' is given twice", e->fields[i]);
            goto done;
        }
        where[k] = (int)i;
    }
    for (int k = 0; k < nf; k++)
        if (where[k] < 0) {
            lerr(l, e->line, e->col, "missing field '%s' in %s literal", gi >= 0 ? decl->params[k] : l->m->structs[l->structs[si].sid].fields[k], e->name);
            goto done;
        }
    Val *vals = xmalloc((e->nargs ? e->nargs : 1) * sizeof *vals);
    if (lower_operands(l, e->args, e->nargs, vals)) {
        if (gi >= 0) { /* infer the type arguments from the field values */
            const TypeRef **pats = xmalloc((size_t)(nf ? nf : 1) * sizeof *pats);
            Ty *tys = xmalloc((size_t)(nf ? nf : 1) * sizeof *tys);
            for (int k = 0; k < nf; k++) {
                pats[k] = &decl->param_types[k];
                tys[k] = vals[where[k]].ty;
            }
            si = infer_instance(l, gi, expect, (const char *const *)decl->tparams, pats, tys, (size_t)nf, NULL, e->line, e->col);
            free(pats);
            free(tys);
        }
        if (si >= 0 && !l->failed) {
            const StructInfo *st = &l->structs[si];
            const IrStruct *ir = &l->m->structs[st->sid];
            char disp[600];
            ir_struct_display(ir->name, disp, sizeof disp);
            int *args = xmalloc((size_t)(nf ? nf : 1) * sizeof *args);
            for (int k = 0; k < nf; k++) {
                char what[700];
                snprintf(what, sizeof what, "field '%s' of '%s'", ir->fields[k], disp);
                const Expr *a = e->args[where[k]];
                flow(l, vals[where[k]], l->m->structs[st->sid].ftys[k], what, a->line, a->col);
                args[k] = vals[where[k]].v;
            }
            if (!l->failed) {
                int sid = l->structs[si].sid;
                int d = hint >= 0 ? hint : temp(l);
                IrInstr *in = emit(l, IR_NEW, e->line);
                in->dst = d;
                in->sid = sid;
                in->nargs = nf;
                if (nf) {
                    in->args = xmalloc((size_t)nf * sizeof(int));
                    memcpy(in->args, args, (size_t)nf * sizeof(int));
                }
                r = (Val){d, ty_struct_of(sid)};
            }
            free(args);
        }
    }
    free(vals);
done:
    free(where);
    return r;
}

/* obj.name   and   obj.name = value */
static Val lower_field(L *l, const Expr *e, int hint) {
    bool set = e->kind == EXPR_SET;
    Expr *ops[2] = {e->left, e->right};
    Val v[2];
    if (!lower_operands(l, ops, set ? 2 : 1, v)) return NO_VAL;
    if (not_an_instance(l, v[0].ty, "fields", e->line, e->col)) return NO_VAL;
    hold_temp(l, e->left, v[0]);
    Boxes boxes = {{0}, 0};
    int first;
    v[0] = deref_member(l, v[0], e->name, false, e->line, e->col, &boxes, &first);
    if (v[0].v < 0) return NO_VAL;
    int csi = cstruct_of(l, v[0].ty);
    if (csi >= 0 && ir_cstruct_field(&l->m->structs[l->structs[csi].sid], e->name) >= 0) {
        Val r = cstruct_field(l, csi, v[0], e, set ? &v[1] : NULL, hint);
        keep_boxes(l, &boxes, e->line);
        return r;
    }
    int si = static_struct(l, v[0].ty);
    int sid = -1, k = -1;
    if (si >= 0) {
        sid = l->structs[si].sid;
        const IrStruct *ir = &l->m->structs[sid];
        char sn[600], via[700] = "";
        ir_struct_display(ir->name, sn, sizeof sn);
        k = ir_struct_field(ir, e->name);
        if (k < 0) {
            if (first >= 0) { /* "struct 'W' has no field 'z' (nor does 'Point', through deref)" */
                char f0[600];
                ir_struct_display(l->structs[first].name, f0, sizeof f0);
                snprintf(via, sizeof via, " (nor does '%s', through deref)", sn);
                snprintf(sn, sizeof sn, "%s", f0);
            }
            int fi = find_impl_fn(l, si, e->name);
            if (fi >= 0 && first < 0)
                lerr(l, e->line, e->col, "struct '%s' has no field '%s' ('%s' is a function of its impl: call it with ()",
                     sn, e->name, e->name);
            else
                lerr(l, e->line, e->col, "struct '%s' has no field '%s'%s", sn, e->name, via);
            return NO_VAL;
        }
        if (set) {
            char what[700];
            snprintf(what, sizeof what, "field '%s' of '%s'", ir->fields[k], sn);
            v[1].ty = flow(l, v[1], ir->ftys[k], what, e->right->line, e->right->col);
            if (l->failed) return NO_VAL;
        }
    }
    IrInstr *in = emit(l, set ? IR_SETFIELD : IR_GETFIELD, e->line);
    in->a = v[0].v;
    in->sid = sid;
    in->field = k;
    if (sid < 0) in->name = xstrdup(e->name);
    if (set) {
        in->b = v[1].v;
        keep_boxes(l, &boxes, e->line);
        if (hint >= 0) {
            emit_mov(l, hint, v[1].v, e->line);
            return (Val){hint, v[1].ty};
        }
        return v[1];
    }
    int d = hint >= 0 ? hint : temp(l);
    in = &fn(l)->blocks[l->f->cur].instrs[fn(l)->blocks[l->f->cur].n - 1];
    in->dst = d;
    keep_boxes(l, &boxes, e->line);
    return (Val){d, sid >= 0 ? l->m->structs[sid].ftys[k] : T_ANY};
}

/* obj.name(args) */
static Val lower_method_call(L *l, const Expr *e, int hint) {
    size_t n = e->nargs + 1;
    Expr **ops = xmalloc(n * sizeof *ops);
    Val *vals = xmalloc(n * sizeof *vals);
    ops[0] = e->left;
    for (size_t i = 0; i < e->nargs; i++) ops[i + 1] = e->args[i];
    Val r = NO_VAL;
    if (!lower_operands(l, ops, n, vals)) goto done;
    if (not_an_instance(l, vals[0].ty, "methods", e->line, e->col)) goto done;
    for (size_t i = 0; i < n; i++) hold_temp(l, ops[i], vals[i]);
    Boxes boxes = {{0}, 0};
    if (strcmp(e->name, "drop") == 0) {
        lerr(l, e->line, e->col, "drop() runs by itself when an instance is freed; call free(x) or dispose(x) instead");
        goto done;
    }
    int first;
    vals[0] = deref_member(l, vals[0], e->name, true, e->line, e->col, &boxes, &first);
    if (vals[0].v < 0) goto done;
    int si = static_struct(l, vals[0].ty);
    if (si >= 0) {
        char sname[600];
        ir_struct_display(l->structs[si].name, sname, sizeof sname);
        int fi = find_impl_fn(l, si, e->name);
        if (fi < 0) {
            if (first >= 0) {
                char f0[600];
                ir_struct_display(l->structs[first].name, f0, sizeof f0);
                lerr(l, e->line, e->col, "struct '%s' has no method '%s' (nor does '%s', through deref)", f0, e->name, sname);
            } else if (ir_struct_field(&l->m->structs[l->structs[si].sid], e->name) >= 0)
                lerr(l, e->line, e->col, "struct '%s' has no method '%s' ('%s' is a field; functions are not first-class values yet)",
                     sname, e->name, e->name);
            else
                lerr(l, e->line, e->col, "struct '%s' has no method '%s'", sname, e->name);
            goto done;
        }
        const FnInfo *f = &l->fns[fi];
        if (!f->has_self) {
            lerr(l, e->line, e->col, "'%s' is an associated function of '%s' (it takes no self); call it as %s::%s(...)",
                 e->name, sname, sname, e->name);
            goto done;
        }
        if ((int)n != f->arity) {
            lerr(l, e->line, e->col, "'%s' expects %d argument%s, got %zu", f->name, f->arity - 1, f->arity == 2 ? "" : "s", e->nargs);
            goto done;
        }
        Expr **at = xmalloc(n * sizeof *at);
        at[0] = e->left;
        for (size_t i = 1; i < n; i++) at[i] = e->args[i - 1];
        r = call_fn(l, f, vals, at, hint, e->line, e->col);
        free(at);
        keep_boxes(l, &boxes, e->line); /* the method ran with the box's value as self */
        goto done;
    }
    if (n > MAX_PARAMS) {
        lerr(l, e->line, e->col, "method calls with more than %d arguments are not supported yet", MAX_PARAMS - 1);
        goto done;
    }
    {
        int d = hint >= 0 ? hint : -1;
        r = result(l, hint, &d);
        IrInstr *in = emit(l, IR_CALLM, e->line);
        in->dst = hint == DISCARD ? IR_NONE : d;
        in->a = vals[0].v;
        in->name = xstrdup(e->name);
        in->nargs = (int)e->nargs;
        if (e->nargs) {
            in->args = xmalloc(e->nargs * sizeof(int));
            for (size_t i = 0; i < e->nargs; i++) in->args[i] = vals[i + 1].v;
        }
        r.ty = T_ANY;
        keep_boxes(l, &boxes, e->line);
    }
done:
    free(ops);
    free(vals);
    return r;
}

/* Struct::name(args) */
static Val lower_assoc_call(L *l, const Expr *e, int hint, Ty expect) {
    int si = find_struct(l, e->name);
    int gi = si < 0 ? find_generic(l, e->name) : -1;
    if (si < 0 && gi < 0) {
        lerr(l, e->line, e->col, "unknown struct '%s'", e->name);
        return NO_VAL;
    }
    if (strcmp(e->str, "drop") == 0) {
        lerr(l, e->line, e->col, "drop() runs by itself when an instance is freed; call free(x) or dispose(x) instead");
        return NO_VAL;
    }
    if (si >= 0 && l->m->structs[l->structs[si].sid].cstruct && find_impl_fn(l, si, e->str) < 0) {
        Val r;
        if (cstruct_builtin(l, si, e, hint, &r)) return r;
    }
    if (gi >= 0) {
        /* Box::new(args): the instance comes from the argument types (and the
         * type the result flows into), so arguments are lowered first */
        const Stmt *fd = NULL, *im = NULL;
        const Generic *g = &l->generics[gi];
        for (int k = 0; k < g->nimpls && !fd; k++)
            for (size_t j = 0; j < g->impls[k]->n && !fd; j++)
                if (strcmp(g->impls[k]->stmts[j]->name, e->str) == 0) {
                    fd = g->impls[k]->stmts[j];
                    im = g->impls[k];
                }
        if (!fd) {
            lerr(l, e->line, e->col, "struct '%s' has no function '%s'", e->name, e->str);
            return NO_VAL;
        }
        if (e->nargs != fd->nparams) {
            lerr(l, e->line, e->col, "'%s::%s' expects %zu argument%s, got %zu", e->name, e->str, fd->nparams,
                 fd->nparams == 1 ? "" : "s", e->nargs);
            return NO_VAL;
        }
        Val *vals = xmalloc((e->nargs ? e->nargs : 1) * sizeof *vals);
        Val r = NO_VAL;
        if (lower_operands(l, e->args, e->nargs, vals)) {
            for (size_t k = 0; k < e->nargs; k++) hold_temp(l, e->args[k], vals[k]);
            const TypeRef **pats = xmalloc((e->nargs ? e->nargs : 1) * sizeof *pats);
            Ty *tys = xmalloc((e->nargs ? e->nargs : 1) * sizeof *tys);
            for (size_t k = 0; k < e->nargs; k++) {
                pats[k] = strcmp(fd->params[k], "self") == 0 ? NULL : &fd->param_types[k];
                tys[k] = vals[k].ty;
            }
            /* `self` passed explicitly (Box::get(b)) names the instance directly */
            int bsi = -1;
            if (e->nargs && !pats[0]) {
                bsi = static_struct_of(l, vals[0].ty);
                if (bsi >= 0 && l->structs[bsi].generic != gi) bsi = -1;
            }
            if (e->nargs && !pats[0] && bsi < 0 && !l->failed) {
                /* the receiver's instance is only known at run time: a method call */
                free(pats);
                free(tys);
                int d = hint >= 0 ? hint : -1;
                r = result(l, hint, &d);
                IrInstr *in = emit(l, IR_CALLM, e->line);
                in->dst = hint == DISCARD ? IR_NONE : d;
                in->a = vals[0].v;
                in->name = xstrdup(e->str);
                in->nargs = (int)e->nargs - 1;
                if (in->nargs) {
                    in->args = xmalloc((size_t)in->nargs * sizeof(int));
                    for (int k = 0; k < in->nargs; k++) in->args[k] = vals[k + 1].v;
                }
                r.ty = T_ANY;
                free(vals);
                return r;
            }
            int isi = bsi >= 0 ? bsi
                    : infer_instance(l, gi, expect, (const char *const *)im->tparams, pats, tys, e->nargs,
                                     fd->type.name ? &fd->type : NULL, e->line, e->col);
            free(pats);
            free(tys);
            int fi = isi >= 0 ? find_impl_fn(l, isi, e->str) : -1;
            if (fi >= 0 && !l->failed) r = call_fn(l, &l->fns[fi], vals, e->args, hint, e->line, e->col);
        }
        free(vals);
        return r;
    }
    int fi = find_impl_fn(l, si, e->str);
    if (fi < 0) {
        lerr(l, e->line, e->col, "struct '%s' has no function '%s'", e->name, e->str);
        return NO_VAL;
    }
    if ((int)e->nargs != l->fns[fi].arity) {
        const FnInfo *f = &l->fns[fi];
        lerr(l, e->line, e->col, "'%s' expects %d argument%s, got %zu", f->name, f->arity, f->arity == 1 ? "" : "s", e->nargs);
        return NO_VAL;
    }
    Val *vals = xmalloc((e->nargs ? e->nargs : 1) * sizeof *vals);
    Val r = NO_VAL;
    /* (lowering the arguments may create instances, which moves l->fns) */
    if (lower_operands(l, e->args, e->nargs, vals)) {
        for (size_t k = 0; k < e->nargs; k++) hold_temp(l, e->args[k], vals[k]);
        r = call_fn(l, &l->fns[fi], vals, e->args, hint, e->line, e->col);
    }
    free(vals);
    return r;
}

/* Assigns v to the variable named by n (already resolved). */
static Val assign_to(L *l, const Expr *e, Name n, int hint) {
    char what[300];
    snprintf(what, sizeof what, "variable '%s'", e->name);
    if (n.kind == NAME_LOCAL) {
        l->expect = n.ty;
        Val v = lower_into(l, e->right, n.index);
        if (v.v < 0) return NO_VAL;
        v.ty = flow(l, v, n.ty, what, e->right->line, e->right->col);
        if (hint >= 0) {
            emit_mov(l, hint, n.index, e->line);
            v.v = hint;
        }
        return v;
    }
    l->expect = n.ty;
    Val v = lower_expr(l, e->right, hint >= 0 ? hint : -1);
    if (v.v < 0) return NO_VAL;
    v.ty = flow(l, v, n.ty, what, e->right->line, e->right->col);
    IrInstr *in = emit(l, IR_STORE, e->line);
    in->global = l->gvars[n.index].global;
    in->a = v.v;
    return v;
}

/* Returns the value of e. If hint >= 0 the result is written to hint when
 * that is free (no extra mov). The hint is only ever written by the final
 * instruction of the expression, after all operands are read, so `x = x + 1`
 * can safely lower to `%x = add %x, %1`. DISCARD means the value is unused. */
static Val lower_expr_at(L *l, const Expr *e, int hint, Ty expect);

/* Instructions emitted for e (after its operands) carry e's column. The type
 * the caller set in l->expect (what e's value flows into) applies to e alone,
 * not to its operands. */
static Val lower_expr(L *l, const Expr *e, int hint) {
    int saved = l->col;
    Ty expect = l->expect;
    l->expect = 0;
    l->col = e->col;
    Fn *f = l->f;
    int mark = f->edepth == 0 ? f->nheld : -1; /* a full expression: its temporaries end here */
    f->edepth++;
    Val v = lower_expr_at(l, e, hint, expect);
    f->edepth--;
    if (mark >= 0) release_held(l, mark, e->line);
    l->col = saved;
    return v;
}

static Val lower_expr_at(L *l, const Expr *e, int hint, Ty expect) {
    if (l->failed) return NO_VAL;
    int d = hint >= 0 ? hint : -1;
    IrInstr *in;
    switch (e->kind) {
    case EXPR_INT:
        if (d < 0) d = temp(l);
        in = emit(l, IR_CONST_INT, e->line);
        in->dst = d;
        in->imm = e->ival;
        return (Val){d, T_INT};
    case EXPR_STRING: {
        int g = string_data(l, e->str, e->str_len);
        if (d < 0) d = temp(l);
        in = emit(l, IR_CONST_DATA, e->line);
        in->dst = d;
        in->global = g;
        return (Val){d, T_STR};
    }
    case EXPR_NIL:
    case EXPR_TRUE:
    case EXPR_FALSE:
        if (d < 0) d = temp(l);
        in = emit(l, e->kind == EXPR_NIL ? IR_CONST_NIL : e->kind == EXPR_TRUE ? IR_CONST_TRUE : IR_CONST_FALSE, e->line);
        in->dst = d;
        return (Val){d, e->kind == EXPR_NIL ? T_NIL : T_BOOL};
    case EXPR_VAR: {
        Name n = resolve_variable(l, e->name, e->line, e->col);
        if (n.kind == NAME_LOCAL) {
            if (d >= 0) {
                emit_mov(l, d, n.index, e->line);
                return (Val){d, n.ty};
            }
            return (Val){n.index, n.ty};
        }
        if (n.kind != NAME_GLOBAL) return NO_VAL;
        if (d < 0) d = temp(l);
        in = emit(l, IR_LOAD, e->line);
        in->dst = d;
        in->global = l->gvars[n.index].global;
        return (Val){d, n.ty};
    }
    case EXPR_ASSIGN: {
        Name n = lookup(l, e->name);
        if (n.kind == NAME_FUNCTION || n.kind == NAME_EXTERN || n.kind == NAME_BUILTIN) {
            lerr(l, e->line, e->col, "cannot assign to function '%s'", e->name);
            return NO_VAL;
        }
        n = resolve_variable(l, e->name, e->line, e->col);
        if (n.kind != NAME_LOCAL && n.kind != NAME_GLOBAL) return NO_VAL;
        return assign_to(l, e, n, hint);
    }
    case EXPR_UNARY: {
        Val a = lower_expr(l, e->right, -1);
        if (a.v < 0) return NO_VAL;
        Ty t = T_BOOL;
        if (e->op == TOK_MINUS) {
            if (!(a.ty & T_INT)) {
                char ta[300];
                ty_name(l, a.ty, ta, sizeof ta);
                lerr(l, e->line, e->col, "operand of '-' must be int, got %s", ta);
                return NO_VAL;
            }
            t = T_INT;
        }
        if (d < 0) d = temp(l);
        in = emit(l, e->op == TOK_MINUS ? IR_NEG : IR_NOT, e->line);
        in->dst = d;
        in->a = a.v;
        return (Val){d, t};
    }
    case EXPR_BINARY: {
        Expr *ops[2] = {e->left, e->right};
        Val v[2];
        if (!lower_operands(l, ops, 2, v)) return NO_VAL;
        Ty t = binary_type(l, e, v[0].ty, v[1].ty);
        if (!t) return NO_VAL;
        if (d < 0) d = temp(l);
        in = emit(l, binary_op(e->op), e->line);
        in->dst = d;
        in->a = v[0].v;
        in->b = v[1].v;
        return (Val){d, t};
    }
    case EXPR_LOGICAL: {
        /* `a or b`  : r = a; if r is truthy -> end, else r = b
         * `a and b` : r = a; if r is falsy  -> end, else r = b
         * r is always a fresh temp: writing a variable hint early would be
         * visible to the right operand. The result is one of the operands. */
        int r = temp(l);
        Val a = lower_into(l, e->left, r);
        if (l->failed) return NO_VAL;
        int n = l->f->nlabels++;
        bool is_or = e->op == TOK_OR;
        int rhs = new_block(l, is_or ? "or.rhs" : "and.rhs", n);
        int end = new_block(l, is_or ? "or.end" : "and.end", n);
        if (is_or) emit_br(l, r, end, rhs, e->line);
        else emit_br(l, r, rhs, end, e->line);
        set_block(l, rhs);
        int mark = l->f->nheld; /* the right operand's temporaries end in its own block */
        Val b = lower_into(l, e->right, r);
        release_held(l, mark, e->line);
        emit_jmp(l, end, e->line);
        set_block(l, end);
        Ty t = ir_ty_union(a.ty, b.ty);
        if (d >= 0) {
            emit_mov(l, d, r, e->line);
            return (Val){d, t};
        }
        return (Val){r, t};
    }
    case EXPR_CALL:
        return lower_call(l, e, hint);
    case EXPR_GET:
    case EXPR_SET:
        return lower_field(l, e, hint);
    case EXPR_METHOD:
        return lower_method_call(l, e, hint);
    case EXPR_ASSOC:
        return lower_assoc_call(l, e, hint, expect);
    case EXPR_STRUCT:
        return lower_struct_literal(l, e, hint, expect);
    case EXPR_ARENA:
        return lower_arena(l, e, hint);
    case EXPR_WITH:
        return lower_with(l, e, hint);
    }
    return NO_VAL;
}

/* ---- statements ---- */

static void lower_stmt(L *l, const Stmt *s);

/* After a `return`, following statements in the same block are unreachable;
 * they are lowered into a fresh block that is pruned at the end. */
static void start_dead_block(L *l) { set_block(l, new_block(l, "dead", l->f->nlabels++)); }

static void lower_var(L *l, const Stmt *s) {
    Fn *f = l->f;
    Ty ty = resolve_type(l, &s->type);
    char what[300];
    snprintf(what, sizeof what, "variable '%s'", s->name);
    if (!s->expr && !(ty & T_NIL)) {
        char tn[300];
        ty_name(l, ty, tn, sizeof tn);
        lerr(l, s->line, s->col, "variable '%s' of type %s needs an initializer", s->name, tn);
        return;
    }
    if (f->depth == 0) { /* top-level variable: a module global */
        int g = find_gvar(l, s->name);
        l->expect = l->gvars[g].ty;
        Val v = s->expr ? lower_expr(l, s->expr, -1) : NO_VAL;
        l->expect = 0;
        if (l->failed) return;
        if (v.v < 0) {
            v = (Val){temp(l), T_NIL};
            emit(l, IR_CONST_NIL, s->line)->dst = v.v;
        }
        flow(l, v, l->gvars[g].ty, what, s->expr ? s->expr->line : s->line, s->expr ? s->expr->col : s->col);
        IrInstr *in = emit(l, IR_STORE, s->line);
        in->global = l->gvars[g].global;
        in->a = v.v;
        l->gvars[g].declared = true; /* the initializer saw the previous declaration, if any */
        return;
    }
    for (int i = f->nscope - 1; i >= 0 && f->scope[i].depth == f->depth; i--)
        if (strcmp(f->scope[i].name, s->name) == 0) {
            lerr(l, s->line, s->col, "a variable named '%s' is already declared in this scope", s->name);
            return;
        }
    /* The name is in scope (but unreadable) during its initializer. */
    bind(l, s->name, -1, ty);
    int idx = f->nscope - 1;
    int v = new_var_vreg(l, s->name);
    if (s->expr) {
        l->expect = ty;
        Val iv = lower_into(l, s->expr, v);
        if (iv.v >= 0) flow(l, iv, ty, what, s->expr->line, s->expr->col);
    } else {
        emit(l, IR_CONST_NIL, s->line)->dst = v;
    }
    f->scope[idx].vreg = v;
}

static void lower_stmt_at(L *l, const Stmt *s);

/* A statement (also one inside an expression, as in an arena block) starts
 * its own full expressions. */
static void lower_stmt(L *l, const Stmt *s) {
    int depth = l->f->edepth;
    l->f->edepth = 0;
    lower_stmt_at(l, s);
    l->f->edepth = depth;
}

static void lower_stmt_at(L *l, const Stmt *s) {
    if (l->failed) return;
    switch (s->kind) {
    case STMT_EXPR:
        lower_expr(l, s->expr, DISCARD);
        return;
    case STMT_VAR:
        lower_var(l, s);
        return;
    case STMT_FUN:
    case STMT_EXTERN:
    case STMT_STRUCT:
    case STMT_IMPL:
        return; /* lowered separately (functions are hoisted) */
    case STMT_RETURN: {
        Val v;
        if (s->expr) {
            l->expect = cur_info(l) ? cur_info(l)->ret : 0;
            v = lower_expr(l, s->expr, -1);
            if (v.v < 0) return;
        } else {
            v = (Val){temp(l), T_NIL};
            emit(l, IR_CONST_NIL, s->line)->dst = v.v;
        }
        const FnInfo *fi = cur_info(l);
        if (fi) {
            char what[300];
            snprintf(what, sizeof what, "return value of '%s'", fi->name);
            flow(l, v, fi->ret, what, s->expr ? s->expr->line : s->line, s->expr ? s->expr->col : s->col);
        }
        leave_exits(l, 0, s->line); /* leave the arena and with blocks we are in */
        emit(l, IR_RET, s->line)->a = v.v;
        start_dead_block(l);
        return;
    }
    case STMT_BLOCK:
        l->f->depth++;
        for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
        pop_scope(l);
        return;
    case STMT_IF: {
        Val c = lower_expr(l, s->expr, -1);
        if (c.v < 0) return;
        int n = l->f->nlabels++;
        int then_b = new_block(l, "if.then", n);
        int else_b = s->else_branch ? new_block(l, "if.else", n) : -1;
        int end_b = new_block(l, "if.end", n);
        emit_br(l, c.v, then_b, else_b >= 0 ? else_b : end_b, s->line);
        set_block(l, then_b);
        lower_stmt(l, s->then_branch);
        emit_jmp(l, end_b, s->line);
        if (else_b >= 0) {
            set_block(l, else_b);
            lower_stmt(l, s->else_branch);
            emit_jmp(l, end_b, s->line);
        }
        set_block(l, end_b);
        return;
    }
    case STMT_WHILE: {
        int n = l->f->nlabels++;
        int cond_b = new_block(l, "while.cond", n);
        int body_b = new_block(l, "while.body", n);
        int end_b = new_block(l, "while.end", n);
        emit_jmp(l, cond_b, s->line);
        set_block(l, cond_b);
        if (s->expr->kind == EXPR_TRUE) {
            /* while (true) / for (;;): no exit edge, so code after the loop is
             * unreachable (it matters for "missing return" checks) */
            emit_jmp(l, body_b, s->line);
        } else {
            Val c = lower_expr(l, s->expr, -1);
            if (c.v < 0) return;
            emit_br(l, c.v, body_b, end_b, s->line);
        }
        set_block(l, body_b);
        lower_stmt(l, s->body);
        emit_jmp(l, cond_b, s->line);
        set_block(l, end_b);
        return;
    }
    }
}

/* Reorders blocks into the order they were filled (source order), then drops
 * blocks unreachable from the entry (code after `return`). Returns whether
 * the block with pre-cleanup index `track` survived (-1: don't care). */
static bool finish_blocks(L *l, int track) {
    IrFunc *f = fn(l);
    Fn *st = l->f;
    int nb = f->nblocks;
    int *map = xmalloc((size_t)nb * sizeof *map);
    for (int i = 0; i < nb; i++) map[i] = -1;
    int next = 0;
    for (int i = 0; i < st->norder; i++)
        if (map[st->order[i]] < 0) map[st->order[i]] = next++;
    for (int i = 0; i < nb; i++)
        if (map[i] < 0) map[i] = next++;
    if (track >= 0) track = map[track];
    IrBlock *nbk = xmalloc((size_t)nb * sizeof *nbk);
    for (int i = 0; i < nb; i++) nbk[map[i]] = f->blocks[i];
    memcpy(f->blocks, nbk, (size_t)nb * sizeof *nbk);
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            for (int t = 0; t < 2; t++)
                if (in->target[t] >= 0) in->target[t] = map[in->target[t]];
        }

    /* reachability from block 0 */
    bool *reach = xcalloc((size_t)nb, sizeof *reach);
    int *work = xmalloc((size_t)nb * sizeof *work);
    int nw = 0;
    reach[0] = true;
    work[nw++] = 0;
    while (nw > 0) {
        const IrBlock *b = &f->blocks[work[--nw]];
        if (b->n == 0) continue;
        const IrInstr *t = &b->instrs[b->n - 1];
        int ns = t->op == IR_JMP ? 1 : t->op == IR_BR ? 2 : 0;
        for (int k = 0; k < ns; k++)
            if (t->target[k] >= 0 && !reach[t->target[k]]) {
                reach[t->target[k]] = true;
                work[nw++] = t->target[k];
            }
    }
    bool tracked = track >= 0 && reach[track];
    next = 0;
    for (int i = 0; i < nb; i++) {
        if (reach[i]) {
            map[i] = next;
            nbk[next++] = f->blocks[i];
        } else {
            map[i] = -1;
            for (int k = 0; k < f->blocks[i].n; k++) ir_instr_free(&f->blocks[i].instrs[k]);
            free(f->blocks[i].instrs);
            free(f->blocks[i].label);
        }
    }
    memcpy(f->blocks, nbk, (size_t)next * sizeof *nbk);
    f->nblocks = next;
    for (int b = 0; b < f->nblocks; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            for (int t = 0; t < 2; t++)
                if (in->target[t] >= 0) in->target[t] = map[in->target[t]];
        }
    free(nbk);
    free(map);
    free(reach);
    free(work);
    return tracked;
}

/* Ends the current function (implicit `return nil`), cleans up its blocks
 * and frees per-function state. */
static void end_function(L *l, int line, int col) {
    if (!l->failed) {
        int implicit = -1;
        if (!ir_block_terminated(&fn(l)->blocks[l->f->cur])) {
            implicit = l->f->cur;
            int r = temp(l);
            emit(l, IR_CONST_NIL, line)->dst = r;
            emit(l, IR_RET, line)->a = r;
        }
        bool reachable_end = finish_blocks(l, implicit);
        const FnInfo *fi = cur_info(l);
        if (reachable_end && fi && !(fi->ret & T_NIL)) {
            char tn[300];
            ty_name(l, fi->ret, tn, sizeof tn);
            lerr(l, line, col, "function '%s' must return %s, but can reach the end of its body (which returns nil)",
                 fi->name, tn);
        }
        if (!l->failed) ir_func_canonicalize(fn(l));
    }
    Fn *f = l->f;
    for (int i = 0; i < f->nscope; i++) free(f->scope[i].name);
    free(f->scope);
    free(f->order);
    free(f->is_var);
    free(f->held);
    free(f->exits);
    memset(f, 0, sizeof *f);
}

static void begin_function(L *l, Fn *f, int fi, int info) {
    memset(f, 0, sizeof *f);
    f->fi = fi;
    f->is_main = info < 0;
    f->info = info;
    l->f = f;
    set_block(l, ir_func_block(fn(l), "entry"));
}

static void lower_function(L *l, int idx) {
    const FnInfo *info = &l->fns[idx];
    const Stmt *s = info->decl;
    Fn f;
    l->tenv = info->tenv; /* a generic instance's function sees its type arguments */
    l->ntenv = info->ntenv;
    begin_function(l, &f, l->m->globals[info->global].func, idx);
    /* parameters and the body's top-level declarations share one scope */
    f.depth = 1;
    for (size_t i = 0; i < s->nparams && !l->failed; i++) {
        for (size_t j = 0; j < i; j++)
            if (strcmp(s->params[i], s->params[j]) == 0)
                lerr(l, s->param_line[i], s->param_col[i], "duplicate parameter '%s'", s->params[i]);
        int v = ir_func_new_vreg(fn(l), s->params[i]); /* params are vregs 0..n-1 */
        mark_var(l, v, true);
        bind(l, s->params[i], v, l->fns[idx].ptypes[i]);
    }
    for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
    /* "can reach the end" errors point at the function name */
    end_function(l, s->line, s->col);
    l->tenv = NULL;
    l->ntenv = 0;
}

static bool name_taken(L *l, const Stmt *s) {
    if (is_builtin(s->name)) {
        lerr(l, s->line, s->col, "cannot redefine builtin function '%s'", s->name);
        return true;
    }
    if (find_fn(l, s->name) >= 0 || find_ext(l, s->name) >= 0) {
        lerr(l, s->line, s->col, "function '%s' is already defined", s->name);
        return true;
    }
    return false;
}

static bool is_type_name(const char *n) {
    static const char *const T[] = {"int", "str", "bool", "nil", "any", "struct", "self"};
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (strcmp(n, T[i]) == 0) return true;
    return false;
}

/* Registers every struct (so types anywhere may name any struct), then
 * resolves their field types. */
static void collect_structs(L *l, const Program *prog) {
    for (size_t i = 0; i < prog->len && !l->failed; i++) {
        const Stmt *s = prog->stmts[i];
        if (s->kind != STMT_STRUCT) continue;
        if (is_type_name(s->name)) {
            lerr(l, s->line, s->col, "'%s' is a builtin type name and cannot name a struct", s->name);
            return;
        }
        if (find_struct(l, s->name) >= 0 || find_generic(l, s->name) >= 0) {
            lerr(l, s->line, s->col, "struct '%s' is already defined", s->name);
            return;
        }
        if (s->ntparams && s->cstruct) {
            lerr(l, s->line, s->col, "a C struct cannot be generic");
            return;
        }
        if (s->ntparams) { /* generic: instantiated on demand */
            for (size_t k = 0; k < s->ntparams; k++)
                if (is_type_name(s->tparams[k])) {
                    lerr(l, s->line, s->col, "'%s' is a builtin type name and cannot name a type parameter", s->tparams[k]);
                    return;
                }
            l->generics = xrealloc(l->generics, (size_t)(l->ngenerics + 1) * sizeof *l->generics);
            l->generics[l->ngenerics++] = (Generic){s, NULL, 0};
            continue;
        }
        l->structs = xrealloc(l->structs, (size_t)(l->nstructs + 1) * sizeof *l->structs);
        l->structs[l->nstructs++] = (StructInfo){s->name, ir_add_struct(l->m, s->name), s, NULL, 0, -1, NULL};
        l->m->structs[l->structs[l->nstructs - 1].sid].shared = s->shared;
    }
    /* impl blocks of generic structs: their functions are added per instance */
    for (size_t i = 0; i < prog->len && !l->failed; i++) {
        const Stmt *s = prog->stmts[i];
        if (s->kind != STMT_IMPL) continue;
        int gi = find_generic(l, s->name);
        if (gi < 0) continue;
        Generic *g = &l->generics[gi];
        if (s->ntparams != g->decl->ntparams) {
            if (!s->ntparams)
                lerr(l, s->line, s->col, "impl for generic struct '%s' needs its type parameters: impl %s<%s%s> { ... }", s->name,
                     s->name, g->decl->tparams[0], g->decl->ntparams > 1 ? ", ..." : "");
            else
                lerr(l, s->line, s->col, "'%s' has %zu type parameter%s, but this impl declares %zu", s->name, g->decl->ntparams,
                     g->decl->ntparams == 1 ? "" : "s", s->ntparams);
            return;
        }
        for (size_t k = 0; k < s->ntparams; k++)
            if (is_type_name(s->tparams[k])) {
                lerr(l, s->line, s->col, "'%s' is a builtin type name and cannot name a type parameter", s->tparams[k]);
                return;
            }
        g->impls = xrealloc(g->impls, (size_t)(g->nimpls + 1) * sizeof *g->impls);
        g->impls[g->nimpls++] = s;
    }
    int nfixed = l->nstructs; /* (instances created while resolving fields set their own) */
    for (int si = 0; si < nfixed && !l->failed; si++) {
        const Stmt *s = l->structs[si].decl;
        if (s->cstruct) { /* extern struct: C field types, C layout */
            CType *ct = xmalloc((s->nparams ? s->nparams : 1) * sizeof *ct);
            for (size_t k = 0; k < s->nparams && !l->failed; k++) {
                for (size_t j = 0; j < k; j++)
                    if (strcmp(s->params[k], s->params[j]) == 0)
                        lerr(l, s->param_line[k], s->param_col[k], "duplicate field '%s'", s->params[k]);
                const TypeRef *t = &s->param_types[k];
                if (!t->name || t->pointer || t->nargs || !ctype_from_name(t->name, t->nullable, &ct[k]) || !ctype_is_field(ct[k]))
                    lerr(l, t->name ? t->line : s->param_line[k], t->name ? t->col : s->param_col[k],
                         "field '%s' of C struct '%s' needs a C type: i8..i64, u8..u64, bool, ptr, cstr or cstr?",
                         s->params[k], s->name);
            }
            if (!l->failed)
                ir_cstruct_set_fields(l->m, l->structs[si].sid, (int)s->nparams, (const char *const *)s->params, ct);
            free(ct);
            continue;
        }
        Ty *tys = xmalloc((s->nparams ? s->nparams : 1) * sizeof *tys);
        for (size_t k = 0; k < s->nparams; k++) {
            for (size_t j = 0; j < k; j++)
                if (strcmp(s->params[k], s->params[j]) == 0)
                    lerr(l, s->param_line[k], s->param_col[k], "duplicate field '%s'", s->params[k]);
            tys[k] = resolve_type(l, &s->param_types[k]);
        }
        ir_struct_set_fields(l->m, l->structs[si].sid, (int)s->nparams, (const char *const *)s->params, tys);
        free(tys);
    }
}

/* `self` inside drop(): the instance is about to be freed, so drop must not
 * keep it. Storing, returning or passing it on (except as the receiver of a
 * field access or method call, or to print/refcount) is a compile error; a
 * reference kept some other way is caught at run time. */
static bool is_self(const Expr *e) { return e && e->kind == EXPR_VAR && strcmp(e->name, "self") == 0; }

static const Expr *self_kept_expr(const Expr *e) {
    if (!e) return NULL;
    const Expr *r;
    switch (e->kind) {
    case EXPR_ASSIGN: case EXPR_SET:
        if (is_self(e->right)) return e->right;
        break;
    case EXPR_STRUCT:
        for (size_t i = 0; i < e->nargs; i++)
            if (is_self(e->args[i])) return e->args[i];
        break;
    case EXPR_CALL:
        if (strcmp(e->name, "print") != 0 && strcmp(e->name, "refcount") != 0)
            for (size_t i = 0; i < e->nargs; i++)
                if (is_self(e->args[i])) return e->args[i];
        break;
    case EXPR_METHOD:
        for (size_t i = 0; i < e->nargs; i++)
            if (is_self(e->args[i])) return e->args[i];
        break;
    case EXPR_ASSOC:
        for (size_t i = 0; i < e->nargs; i++)
            if (is_self(e->args[i]) && (i > 0 || strcmp(e->name, "Rc") == 0)) return e->args[i];
        break;
    default: break;
    }
    if ((r = self_kept_expr(e->left)) || (r = self_kept_expr(e->right))) return r;
    for (size_t i = 0; i < e->nargs; i++)
        if ((r = self_kept_expr(e->args[i]))) return r;
    return NULL;
}

static const Expr *self_kept(const Stmt *s) {
    if (!s) return NULL;
    if ((s->kind == STMT_VAR || s->kind == STMT_RETURN) && is_self(s->expr)) return s->expr;
    const Expr *r = self_kept_expr(s->expr);
    for (size_t i = 0; i < s->n && !r; i++) r = self_kept(s->stmts[i]);
    if (!r) r = self_kept(s->then_branch);
    if (!r) r = self_kept(s->else_branch);
    if (!r) r = self_kept(s->body);
    return r;
}

static bool check_drop(L *l, const Stmt *f) {
    if (f->nparams != 1 || strcmp(f->params[0], "self") != 0) {
        lerr(l, f->line, f->col, "drop must take only self: fun drop(self) { ... }");
        return false;
    }
    if (f->type.name) {
        lerr(l, f->type.line, f->type.col, "drop returns nothing (remove the return type)");
        return false;
    }
    for (size_t i = 0; i < f->n; i++) {
        const Expr *k = self_kept(f->stmts[i]);
        if (k) {
            lerr(l, k->line, k->col, "drop() must not keep 'self': the instance is freed when drop returns");
            return false;
        }
    }
    return true;
}

/* Symbol-safe form of a struct name for function symbols: Pair<int,Box<str?>>
 * -> Pair..int.Box..str_opt (the assembler's names have no <, > or ?). */
static void mangle(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *c = in; *c && o + 5 < n; c++) {
        if (*c == '<') { out[o++] = '.'; out[o++] = '.'; }
        else if (*c == ',') out[o++] = '.';
        else if (*c == '>') continue;
        else if (*c == '?') { memcpy(out + o, "_opt", 4); o += 4; }
        else out[o++] = *c;
    }
    out[o] = '\0';
}

/* Adds the functions of impl block s to struct si; tenv binds the impl's
 * type parameters (an instance of a generic struct), else NULL. */
static void collect_impl_fns(L *l, const Stmt *s, int si, const TArg *tenv, int ntenv) {
    const TArg *saved = l->tenv;
    int nsaved = l->ntenv;
    l->tenv = tenv;
    l->ntenv = ntenv;
    const char *sname = l->structs[si].name;
    char msym[600];
    mangle(sname, msym, sizeof msym);
    for (size_t i = 0; i < s->n && !l->failed; i++) {
        const Stmt *f = s->stmts[i];
        if (find_impl_fn(l, si, f->name) >= 0) {
            lerr(l, f->line, f->col, "'%s::%s' is already defined", s->name, f->name);
            break;
        }
        bool has_self = f->nparams > 0 && strcmp(f->params[0], "self") == 0;
        bool bad = false;
        for (size_t k = 0; k < f->nparams && !bad; k++) {
            if (strcmp(f->params[k], "self") != 0) continue;
            if (k > 0) {
                lerr(l, f->param_line[k], f->param_col[k], "'self' must be the first parameter");
                bad = true;
            } else if (f->param_types[k].name) {
                lerr(l, f->param_types[k].line, f->param_types[k].col, "'self' cannot have a type annotation (it is always %s)", s->name);
                bad = true;
            }
        }
        if (bad) break;
        if (f->nparams > MAX_PARAMS) {
            lerr(l, f->line, f->col, "functions with more than %d parameters are not supported yet", MAX_PARAMS);
            break;
        }
        if (strcmp(f->name, "drop") == 0 && !check_drop(l, f)) break;
        if (strcmp(f->name, "deref") == 0 && (f->nparams != 1 || strcmp(f->params[0], "self") != 0)) {
            lerr(l, f->line, f->col, "deref must take only self: fun deref(self) { ... }");
            break;
        }
        char sym[700];
        snprintf(sym, sizeof sym, "m.%s.%s", msym, f->name);
        for (int k = 2; ir_find_global(l->m, sym) >= 0; k++) snprintf(sym, sizeof sym, "m.%s.%d.%s", msym, k, f->name);
        int irf = ir_add_func(l->m, sym, (int)f->nparams);
        FnInfo info = {NULL, l->m->funcs[irf].global, (int)f->nparams, f, NULL, T_ANY, l->structs[si].sid, has_self, NULL, tenv, ntenv, tenv != NULL};
        char dn[600];
        ir_struct_display(sname, dn, sizeof dn);
        size_t dl = strlen(dn);
        snprintf(dn + dl, sizeof dn - dl, "::%s", f->name);
        info.owned_name = xstrdup(dn);
        info.name = info.owned_name;
        int sid = l->structs[si].sid;
        info.ptypes = xmalloc((f->nparams ? f->nparams : 1) * sizeof *info.ptypes);
        for (size_t k = 0; k < f->nparams; k++)
            info.ptypes[k] = (has_self && k == 0) ? ty_struct_of(sid) : resolve_type(l, &f->param_types[k]);
        info.ret = resolve_type(l, &f->type);
        IrGlobal *g = &l->m->globals[info.global];
        g->ty = info.ret;
        for (size_t k = 0; k < f->nparams && k < 64; k++)
            if (f->param_move && f->param_move[k]) g->moves |= (uint64_t)1 << k;
        bool typed = false;
        for (size_t k = 0; k < f->nparams; k++) typed |= info.ptypes[k] != T_ANY;
        if (typed) {
            g->ptys = xmalloc(f->nparams * sizeof *g->ptys);
            for (size_t k = 0; k < f->nparams; k++) g->ptys[k] = info.ptypes[k];
        }
        l->fns = xrealloc(l->fns, (size_t)(l->nfns + 1) * sizeof *l->fns);
        l->fns[l->nfns++] = info;
        StructInfo *st = &l->structs[si];
        st->fns = xrealloc(st->fns, (size_t)(st->nfns + 1) * sizeof *st->fns);
        st->fns[st->nfns++] = l->nfns - 1;
    }
    l->tenv = saved;
    l->ntenv = nsaved;
}

/* Adds the functions of one impl block of a non-generic struct. */
static void collect_impl(L *l, const Stmt *s) {
    if (find_generic(l, s->name) >= 0) return; /* registered with its generic struct */
    int si = find_struct(l, s->name);
    if (si < 0) {
        lerr(l, s->line, s->col, "impl for unknown struct '%s'", s->name);
        return;
    }
    if (s->ntparams) {
        lerr(l, s->line, s->col, "struct '%s' is not generic; write impl %s { ... }", s->name, s->name);
        return;
    }
    collect_impl_fns(l, s, si, NULL, 0);
}

static int dynamic_entry(L *l, const FnInfo *fi);

/* The struct for generic gi with these type arguments (created on first
 * use): fields resolved with the arguments bound, plus the functions of every
 * impl block and their method table entries. */
static int instantiate(L *l, int gi, const Ty *targs_in, int line, int col) {
    const Generic *g = &l->generics[gi];
    const Stmt *d = g->decl;
    size_t n = d->ntparams;
    Ty *targs = xmalloc(n * sizeof *targs);
    char name[1024];
    size_t o = (size_t)snprintf(name, sizeof name, "%s<", d->name);
    for (size_t i = 0; i < n; i++) {
        targs[i] = targ_norm(targs_in[i]);
        char tn[512];
        ir_ty_name(l->m, targs[i], tn, sizeof tn);
        o += (size_t)snprintf(name + o, o < sizeof name ? sizeof name - o : 0, "%s%s", i ? "," : "", tn);
    }
    if (o + 2 >= sizeof name) {
        lerr(l, line, col, "type '%s<...>' is too long", d->name);
        free(targs);
        return -1;
    }
    strcat(name, ">");
    int si = find_struct(l, name);
    if (si >= 0) {
        free(targs);
        return si;
    }
    int depth = 0, maxd = 0;
    for (const char *c = name; *c; c++) {
        depth += *c == '<' ? 1 : *c == '>' ? -1 : 0;
        if (depth > maxd) maxd = depth;
    }
    if (maxd > 32) {
        lerr(l, line, col, "type arguments nested more than 32 levels deep while instantiating '%s'", d->name);
        free(targs);
        return -1;
    }
    int sid = ir_add_struct(l->m, name);
    l->m->structs[sid].shared = d->shared;
    l->structs = xrealloc(l->structs, (size_t)(l->nstructs + 1) * sizeof *l->structs);
    si = l->nstructs++;
    l->structs[si] = (StructInfo){l->m->structs[sid].name, sid, d, NULL, 0, gi, targs};
    /* fields, with the struct's own parameter names bound */
    TArg *env = xmalloc(n * sizeof *env);
    for (size_t i = 0; i < n; i++) env[i] = (TArg){d->tparams[i], targs[i]};
    l->tenvs = xrealloc(l->tenvs, (size_t)(l->ntenvs + 1) * sizeof *l->tenvs);
    l->tenvs[l->ntenvs++] = env;
    const TArg *saved = l->tenv;
    int nsaved = l->ntenv;
    l->tenv = env;
    l->ntenv = (int)n;
    Ty *tys = xmalloc((d->nparams ? d->nparams : 1) * sizeof *tys);
    for (size_t k = 0; k < d->nparams; k++) tys[k] = resolve_type(l, &d->param_types[k]);
    ir_struct_set_fields(l->m, sid, (int)d->nparams, (const char *const *)d->params, tys);
    free(tys);
    l->tenv = saved;
    l->ntenv = nsaved;
    /* functions: each impl block binds its own parameter names */
    for (int k = 0; k < g->nimpls && !l->failed; k++) {
        const Stmt *im = g->impls[k];
        TArg *ienv = xmalloc(n * sizeof *ienv);
        for (size_t i = 0; i < n; i++) ienv[i] = (TArg){im->tparams[i], targs[i]};
        l->tenvs = xrealloc(l->tenvs, (size_t)(l->ntenvs + 1) * sizeof *l->tenvs);
        l->tenvs[l->ntenvs++] = ienv;
        collect_impl_fns(l, im, si, ienv, (int)n);
    }
    for (int k = 0; k < l->structs[si].nfns && !l->failed; k++) {
        int fi = l->structs[si].fns[k];
        if (l->fns[fi].has_self) {
            int mg = dynamic_entry(l, &l->fns[fi]);
            ir_struct_add_method(l->m, sid, l->fns[fi].decl->name, mg);
        }
    }
    return l->failed ? -1 : si;
}

/* A method's entry for dynamic calls (callm), which pass their arguments
 * unchecked: when the method has typed parameters, a wrapper checks them
 *   fn @m.P.f.dyn(%self, %a) { entry: %self = check %self, P, @ctx  ...
 *                                     %r = call @m.P.f(%self, %a)  ret %r }
 * Returns the global to register in the struct's method table. */
static int dynamic_entry(L *l, const FnInfo *fi) {
    bool typed = false;
    for (int k = 1; k < fi->arity; k++) typed |= fi->ptypes[k] != T_ANY;
    if (!typed) return fi->global;
    char sym[700];
    snprintf(sym, sizeof sym, "%s.dyn", l->m->globals[fi->global].name);
    /* contexts first: string_data adds globals */
    int *ctx = xmalloc((size_t)fi->arity * sizeof *ctx);
    for (int k = 0; k < fi->arity; k++) {
        char what[700];
        snprintf(what, sizeof what, "argument '%s' of '%s'", fi->decl->params[k], fi->name);
        ctx[k] = (k == 0 || fi->ptypes[k] != T_ANY) ? string_data(l, what, strlen(what)) : -1;
    }
    int wf = ir_add_func(l->m, sym, fi->arity);
    IrFunc *f = &l->m->funcs[wf];
    for (int k = 0; k < fi->arity; k++) ir_func_new_vreg(f, fi->decl->params[k]);
    int b = ir_func_block(f, "entry");
    for (int k = 0; k < fi->arity; k++) {
        if (ctx[k] < 0) continue;
        IrInstr *in = ir_emit(f, b, IR_CHECK);
        in->dst = in->a = k;
        in->ty = fi->ptypes[k];
        in->global = ctx[k];
        in->line = fi->decl->line;
    }
    int r = ir_func_new_vreg(f, "0");
    IrInstr *in = ir_emit(f, b, IR_CALL);
    in->dst = r;
    in->global = fi->global;
    in->nargs = fi->arity;
    in->args = xmalloc((size_t)fi->arity * sizeof(int));
    for (int k = 0; k < fi->arity; k++) in->args[k] = k;
    in->line = fi->decl->line;
    in = ir_emit(f, b, IR_RET);
    in->a = r;
    in->line = fi->decl->line;
    free(ctx);
    return f->global;
}

/* ---- which builtins the program uses (their structs are added only then) ---- */

/* Collects top-level functions, externs and variables, checking conflicts. */
static void collect_declarations(L *l, const Program *prog) {
    collect_structs(l, prog);
    for (size_t i = 0; i < prog->len && !l->failed; i++) {
        const Stmt *s = prog->stmts[i];
        if (s->kind == STMT_IMPL) {
            collect_impl(l, s);
        } else if (s->kind == STMT_FUN) {
            if (name_taken(l, s)) continue;
            if (s->nparams > MAX_PARAMS) {
                lerr(l, s->line, s->col, "functions with more than %d parameters are not supported yet", MAX_PARAMS);
                continue;
            }
            char sym[300];
            snprintf(sym, sizeof sym, "fn.%s", s->name);
            int fi = ir_add_func(l->m, sym, (int)s->nparams);
            for (size_t k = 0; k < s->nparams; k++)
                if (strcmp(s->params[k], "self") == 0) {
                    lerr(l, s->param_line[k], s->param_col[k], "'self' is only allowed as the first parameter of a function in an impl block");
                    return;
                }
            FnInfo info = {s->name, l->m->funcs[fi].global, (int)s->nparams, s, NULL, T_ANY, -1, false, NULL, NULL, 0, false};
            info.ptypes = xmalloc((s->nparams ? s->nparams : 1) * sizeof *info.ptypes);
            for (size_t k = 0; k < s->nparams; k++) info.ptypes[k] = resolve_type(l, &s->param_types[k]);
            info.ret = resolve_type(l, &s->type);
            /* the signature is part of the IR: callers and returns must uphold it */
            IrGlobal *g = &l->m->globals[info.global];
            g->ty = info.ret;
            for (size_t k = 0; k < s->nparams && k < 64; k++)
                if (s->param_move && s->param_move[k]) g->moves |= (uint64_t)1 << k;
            bool typed = false;
            for (size_t k = 0; k < s->nparams; k++) typed |= info.ptypes[k] != T_ANY;
            if (typed) {
                g->ptys = xmalloc(s->nparams * sizeof *g->ptys);
                for (size_t k = 0; k < s->nparams; k++) g->ptys[k] = info.ptypes[k];
            }
            l->fns = xrealloc(l->fns, (size_t)(l->nfns + 1) * sizeof *l->fns);
            l->fns[l->nfns++] = info;
        } else if (s->kind == STMT_EXTERN) {
            if (name_taken(l, s)) continue;
            if (strncmp(s->name, "luma_", 5) == 0) {
                lerr(l, s->line, s->col, "names starting with 'luma_' are reserved for the Luma runtime");
                continue;
            }
            if (s->nparams > MAX_PARAMS) {
                lerr(l, s->line, s->col, "C functions with more than %d parameters are not supported yet", MAX_PARAMS);
                continue;
            }
            ExtInfo x = {s->name, -1, s, NULL, CT_VOID, NULL, NULL, -1, false};
            size_t np = s->nparams ? s->nparams : 1;
            x.ctypes = xmalloc(np * sizeof *x.ctypes);
            x.psid = xmalloc(np * sizeof *x.psid);
            x.pnull = xmalloc(np * sizeof *x.pnull);
            /* the C parameters: a struct by value is one or two eightbytes */
            CType cps[MAX_PARAMS * 2];
            const char *cns[MAX_PARAMS * 2];
            int ncp = 0;
            for (size_t k = 0; k < s->nparams; k++) {
                x.psid[k] = extern_struct_type(l, &s->param_types[k], false, &x.ctypes[k], &x.pnull[k]);
                if (x.psid[k] < 0) x.ctypes[k] = resolve_ctype(l, &s->param_types[k], false);
                int words = x.ctypes[k] == CT_CSW0 ? (l->m->structs[x.psid[k]].csize > 8 ? 2 : 1) : 1;
                for (int w = 0; w < words; w++) {
                    if (ncp == MAX_PARAMS) {
                        lerr(l, s->line, s->col, "C functions with more than %d parameters (counting each eightbyte of a struct passed by value) are not supported yet",
                             MAX_PARAMS);
                        break;
                    }
                    cps[ncp] = w ? CT_CSW1 : x.ctypes[k];
                    cns[ncp++] = s->params[k];
                }
            }
            x.rsid = extern_struct_type(l, &s->type, true, &x.cret, &x.rnull);
            if (x.rsid < 0) x.cret = resolve_ctype(l, &s->type, true);
            if (l->failed) {
                free(x.ctypes);
                free(x.psid);
                free(x.pnull);
                continue;
            }
            x.global = ir_add_cextern(l->m, s->name, ncp, cps, cns, x.cret);
            if (x.cret == CT_CSRET) l->m->globals[x.global].cret_sid = x.rsid;
            if (s->csym) l->m->globals[x.global].csym = xstrdup(s->csym);
            l->exts = xrealloc(l->exts, (size_t)(l->nexts + 1) * sizeof *l->exts);
            l->exts[l->nexts++] = x;
        } else if (s->kind == STMT_VAR) {
            if (is_builtin(s->name)) {
                lerr(l, s->line, s->col, "'%s' is a builtin function and cannot be redeclared as a variable", s->name);
                continue;
            }
            Ty ty = resolve_type(l, &s->type);
            int g = find_gvar(l, s->name);
            if (g >= 0) {
                if (l->gvars[g].ty != ty) {
                    char a[300], b[300];
                    ty_name(l, l->gvars[g].ty, a, sizeof a);
                    ty_name(l, ty, b, sizeof b);
                    lerr(l, s->line, s->col, "global '%s' was declared as %s; redeclaring it as %s is not allowed",
                         s->name, a, b);
                }
                continue;
            }
            char sym[300];
            snprintf(sym, sizeof sym, "var.%s", s->name);
            l->gvars = xrealloc(l->gvars, (size_t)(l->ngvars + 1) * sizeof *l->gvars);
            int gi = ir_add_var(l->m, sym);
            l->m->globals[gi].ty = ty;
            l->gvars[l->ngvars++] = (GVar){s->name, gi, false, ty};
        }
    }
    for (int i = 0; i < l->nstructs && !l->failed; i++) {
        const Stmt *d = l->structs[i].decl;
        if (!d || l->structs[i].generic >= 0) continue; /* builtin, or an instance (checked below) */
        if (find_fn(l, d->name) >= 0 || find_ext(l, d->name) >= 0 || find_gvar(l, d->name) >= 0 || is_builtin(d->name))
            lerr(l, d->line, d->col, "'%s' is declared as a struct and as a function or variable", d->name);
    }
    for (int i = 0; i < l->ngenerics && !l->failed; i++) {
        const Stmt *d = l->generics[i].decl;
        if (find_fn(l, d->name) >= 0 || find_ext(l, d->name) >= 0 || find_gvar(l, d->name) >= 0 || is_builtin(d->name))
            lerr(l, d->line, d->col, "'%s' is declared as a struct and as a function or variable", d->name);
    }
    /* method tables for calls on values whose struct is not known statically */
    for (int i = 0; i < l->nfns && !l->failed; i++) {
        const FnInfo *fi = &l->fns[i];
        if (fi->sid >= 0 && fi->has_self && !fi->instance) {
            int g = dynamic_entry(l, &l->fns[i]);
            ir_struct_add_method(l->m, l->fns[i].sid, l->fns[i].decl->name, g);
        }
    }
    for (int i = 0; i < l->ngvars && !l->failed; i++) {
        int f = find_fn(l, l->gvars[i].name);
        const Stmt *d = f >= 0 ? l->fns[f].decl : NULL;
        int x = find_ext(l, l->gvars[i].name);
        if (x >= 0) d = l->exts[x].decl;
        if (d) lerr(l, d->line, d->col, "'%s' is declared as both a function and a variable", d->name);
    }
}

/* ---- the prelude ---- */

typedef struct {
    const char **v;
    size_t n, cap;
} Names;

static bool names_has(const Names *s, const char *n) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->v[i], n) == 0) return true;
    return false;
}

static void names_add(Names *s, const char *n) {
    if (!n || names_has(s, n)) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 16;
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = n;
}

/* The names a piece of code mentions: struct names (types, literals, X::f
 * calls; an arena block mentions Arena) and called functions. */
static void mention_stmt(const Stmt *s, Names *out);

static void mention_type(const TypeRef *t, Names *out) {
    if (!t->name) return;
    names_add(out, t->name);
    for (size_t i = 0; i < t->nargs; i++) mention_type(&t->args[i], out);
}

static void mention_expr(const Expr *e, Names *out) {
    if (!e) return;
    if (e->kind == EXPR_ASSOC || e->kind == EXPR_STRUCT || e->kind == EXPR_CALL) names_add(out, e->name);
    if (e->kind == EXPR_ARENA) names_add(out, "Arena");
    mention_expr(e->left, out);
    mention_expr(e->right, out);
    for (size_t i = 0; i < e->nargs; i++) mention_expr(e->args[i], out);
    mention_stmt(e->body, out);
}

static void mention_stmt(const Stmt *s, Names *out) {
    if (!s) return;
    mention_expr(s->expr, out);
    mention_type(&s->type, out);
    for (size_t i = 0; s->param_types && i < s->nparams; i++) mention_type(&s->param_types[i], out);
    for (size_t i = 0; i < s->n; i++) mention_stmt(s->stmts[i], out);
    mention_stmt(s->then_branch, out);
    mention_stmt(s->else_branch, out);
    mention_stmt(s->body, out);
}

/* The name a top-level statement defines (an impl adds to its struct's). */
static const char *defines(const Stmt *s) {
    return s->kind == STMT_STRUCT || s->kind == STMT_IMPL || s->kind == STMT_FUN || s->kind == STMT_EXTERN ||
                   s->kind == STMT_VAR
               ? s->name
               : NULL;
}

static bool same_typeref(const TypeRef *a, const TypeRef *b) {
    if (!a->name || !b->name) return !a->name && !b->name;
    if (strcmp(a->name, b->name) != 0 || a->nullable != b->nullable || a->nargs != b->nargs) return false;
    for (size_t i = 0; i < a->nargs; i++)
        if (!same_typeref(&a->args[i], &b->args[i])) return false;
    return true;
}

/* Two `extern fun` declarations of the same C function that agree. */
static bool same_extern(const Stmt *a, const Stmt *b) {
    if (a->nparams != b->nparams || !same_typeref(&a->type, &b->type)) return false;
    for (size_t i = 0; i < a->nparams; i++)
        if (!same_typeref(&a->param_types[i], &b->param_types[i])) return false;
    return true;
}

/* The program followed by the std declarations it uses (into all, which
 * borrows the statements of both). std/prelude.luma's names are reserved;
 * a library module's name the program defines itself is the program's,
 * unless std code needs the library's. False after an error. */
static bool with_prelude(const char *path, const Program *prog, const Program *pre, Program *all) {
    bool ok = true;
    Names own = {0}; /* what the program defines */
    for (size_t i = 0; i < prog->len; i++) {
        const Stmt *s = prog->stmts[i];
        const char *n = defines(s);
        if (!n) continue;
        names_add(&own, n);
        bool core = false;
        for (size_t k = 0; k < pre->len && !core; k++)
            core = prelude_is_core(pre->stmts[k]->line) && pre->stmts[k]->kind == STMT_STRUCT &&
                   strcmp(pre->stmts[k]->name, n) == 0;
        if (strncmp(n, "__", 2) == 0 || core) {
            fprintf(stderr, "%s:%d:%d: error: '%s' is %s%s\n", path, s->line, s->col, n,
                    core ? "defined by the prelude" : "reserved (names starting with __ belong to the prelude)",
                    core && s->kind == STMT_IMPL ? "; it cannot have another impl block" : "");
            ok = false;
        }
    }
    if (!ok) {
        free(own.v);
        return false;
    }
    /* what the program uses and does not define itself */
    Names used = {0}, mine = {0};
    for (size_t i = 0; i < prog->len; i++) mention_stmt(prog->stmts[i], &mine);
    for (size_t i = 0; i < mine.n; i++)
        if (!names_has(&own, mine.v[i])) names_add(&used, mine.v[i]);
    bool *keep = xcalloc(pre->len ? pre->len : 1, sizeof *keep);
    for (bool changed = true; changed && ok;) { /* std code may use more std items */
        changed = false;
        for (size_t k = 0; k < pre->len && ok; k++) {
            const Stmt *s = pre->stmts[k];
            const char *n = defines(s);
            if (keep[k] || !n || !names_has(&used, n)) continue;
            if (names_has(&own, n)) {
                /* the program defines it too: an agreeing extern is shared */
                const Stmt *u = NULL;
                for (size_t i = 0; i < prog->len && !u; i++)
                    if (defines(prog->stmts[i]) && strcmp(defines(prog->stmts[i]), n) == 0) u = prog->stmts[i];
                if (s->kind == STMT_EXTERN && u->kind == STMT_EXTERN && same_extern(s, u)) continue;
                fprintf(stderr, "%s:%d:%d: error: '%s' is defined by the program and by the standard library (%s), which needs its own\n",
                        path, u->line, u->col, n, prelude_path(path, s->line));
                ok = false;
                break;
            }
            keep[k] = changed = true;
            Names more = {0};
            mention_stmt(s, &more);
            for (size_t i = 0; i < more.n; i++) names_add(&used, more.v[i]);
            free(more.v);
        }
    }
    if (ok) {
        all->source_path = prog->source_path;
        all->cap = pre->len + prog->len + 1;
        all->stmts = xmalloc(all->cap * sizeof *all->stmts);
        all->len = 0;
        /* the program's structs first: struct ids, and memcheck's lists, are in this order */
        for (size_t i = 0; i < prog->len; i++) all->stmts[all->len++] = prog->stmts[i];
        for (size_t k = 0; k < pre->len; k++)
            if (keep[k]) all->stmts[all->len++] = pre->stmts[k];
    }
    free(keep);
    free(used.v);
    free(mine.v);
    free(own.v);
    return ok;
}

bool lower_program(const Program *user, IrModule *out) {
    ir_module_init(out, user->source_path);
    Program pre = {0}, merged = {0};
    if (!prelude_load(&pre) || !with_prelude(user->source_path, user, &pre, &merged)) {
        program_free(&pre);
        ir_module_free(out);
        return false;
    }
    const Program *prog = &merged;
    L l = {0};
    l.path = prog->source_path;
    l.m = out;
    l.write_fn = l.space_fn = l.newline_fn = l.free_fn = -1;
    l.dispose_fn = l.refcount_fn = -1;
    for (int i = 0; i < 64; i++) l.intrinsic_fn[i] = -1;
    for (int i = 0; i < 5; i++) l.cs_fn[i] = -1;
    Fn main_fn;
    int main_fi = ir_add_func(out, "luma_main", 0);
    collect_declarations(&l, prog);

    if (!l.failed) {
        begin_function(&l, &main_fn, main_fi, -1);
        for (size_t i = 0; i < user->len && !l.failed; i++) lower_stmt(&l, user->stmts[i]);
        end_function(&l, 0, 0);
    }
    for (int i = 0; i < l.nfns && !l.failed; i++) lower_function(&l, i); /* (instances may add more) */

    for (int i = 0; i < l.nfns; i++) {
        free(l.fns[i].ptypes);
        free(l.fns[i].owned_name);
    }
    for (int i = 0; i < l.nstructs; i++) {
        free(l.structs[i].fns);
        free(l.structs[i].targs);
    }
    free(l.structs);
    for (int i = 0; i < l.ngenerics; i++) free(l.generics[i].impls);
    free(l.generics);
    for (int i = 0; i < l.ntenvs; i++) free(l.tenvs[i]);
    free(l.tenvs);
    for (int i = 0; i < l.nexts; i++) {
        free(l.exts[i].ctypes);
        free(l.exts[i].psid);
        free(l.exts[i].pnull);
    }
    free(l.fns);
    free(l.exts);
    free(l.gvars);
    free(merged.stmts); /* (the statements belong to the prelude and the program) */
    program_free(&pre);
    if (l.failed) {
        ir_module_free(out);
        return false;
    }
    return true;
}
