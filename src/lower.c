#include "lower.h"

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
typedef unsigned Ty;
enum { T_INT = 1, T_STR = 2, T_BOOL = 4, T_NIL = 8, T_ANY = 15 };

static void ty_name(Ty t, char *buf, size_t n) {
    static const struct { Ty bit; const char *name; } T[] = {
        {T_INT, "int"}, {T_STR, "str"}, {T_BOOL, "bool"}, {T_NIL, "nil"}};
    if ((t & T_ANY) == T_ANY) { snprintf(buf, n, "any"); return; }
    Ty rest = t & ~(Ty)T_NIL;
    if ((t & T_NIL) && rest && (rest & (rest - 1)) == 0) {
        for (int i = 0; i < 3; i++)
            if (T[i].bit == rest) { snprintf(buf, n, "%s?", T[i].name); return; }
    }
    buf[0] = '\0';
    for (int i = 0; i < 4; i++) {
        if (!(t & T[i].bit)) continue;
        if (buf[0]) strncat(buf, " or ", n - strlen(buf) - 1);
        strncat(buf, T[i].name, n - strlen(buf) - 1);
    }
    if (!buf[0]) snprintf(buf, n, "nothing");
}

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

typedef struct {
    const char *name;
    int global;   /* IR global index (@fn.NAME) */
    int arity;
    const Stmt *decl;
    Ty *ptypes;   /* per parameter (owned) */
    Ty ret;       /* declared return type (T_ANY if unannotated) */
} FnInfo;

typedef struct {
    const char *name;
    int global;   /* IR global index (C symbol) */
    const Stmt *decl;
    CType *ctypes; /* per parameter (owned) */
    CType cret;
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
    const FnInfo *info; /* NULL for luma_main */
    int cur;
    int *order;
    int norder, cap_order;
    bool *is_var;
    int cap_is_var;
    Binding *scope;
    int nscope, cap_scope;
    int depth;
    int ntemps, nlabels;
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
    int nstrings;
    int write_fn, space_fn, newline_fn; /* runtime externs, created on first use */
    Fn *f;
    bool failed;
} L;

/* A lowered expression: the vreg holding the value and its static type. */
typedef struct {
    int v;
    Ty ty;
} Val;

static const Val NO_VAL = {-1, T_ANY};

static IrFunc *fn(L *l) { return &l->m->funcs[l->f->fi]; }

static void lerr(L *l, int line, int col, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void lerr(L *l, int line, int col, const char *fmt, ...) {
    if (l->failed) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", l->path, line, col);
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
    if (v.v < 0 || (v.ty & ~slot) == 0) return v.ty;
    char want[64], got[64];
    ty_name(slot, want, sizeof want);
    ty_name(v.ty, got, sizeof got);
    if ((v.ty & slot) == 0) {
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
    return v.ty & slot;
}

/* Resolves a Luma type annotation; T_ANY if absent. */
static Ty resolve_type(L *l, const TypeRef *t) {
    if (!t->name) return T_ANY;
    static const struct { const char *name; Ty ty; } N[] = {
        {"int", T_INT}, {"str", T_STR}, {"bool", T_BOOL}, {"nil", T_NIL}, {"any", T_ANY}};
    for (size_t i = 0; i < sizeof N / sizeof *N; i++)
        if (strcmp(t->name, N[i].name) == 0) return N[i].ty | (t->nullable ? T_NIL : 0);
    CType c;
    if (ctype_from_name(t->name, false, &c) || strcmp(t->name, "f32") == 0 || strcmp(t->name, "f64") == 0)
        lerr(l, t->line, t->col, "'%s' is a C type; it can only be used in extern declarations (Luma types: int, str, bool, nil, any)", t->name);
    else
        lerr(l, t->line, t->col, "unknown type '%s' (Luma types: int, str, bool, nil, any)", t->name);
    return T_ANY;
}

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

static bool is_builtin(const char *name) { return strcmp(name, "print") == 0; }

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
    if (is_builtin(name)) return (Name){NAME_BUILTIN, 0, false, T_ANY};
    return (Name){NAME_NONE, 0, false, T_ANY};
}

/* Resolves a name used as a value or assignment target; reports errors. */
static Name resolve_variable(L *l, const char *name, int line, int col) {
    Name n = lookup(l, name);
    switch (n.kind) {
    case NAME_NONE: lerr(l, line, col, "undefined variable '%s'", name); break;
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
    char ta[64], tb[64];
    switch (e->op) {
    case TOK_EQUAL_EQUAL:
    case TOK_BANG_EQUAL: return T_BOOL;
    case TOK_PLUS: {
        Ty r = ((a & T_INT) && (b & T_INT) ? T_INT : 0) | ((a & T_STR) && (b & T_STR) ? T_STR : 0);
        if (r) return r;
        ty_name(a, ta, sizeof ta);
        ty_name(b, tb, sizeof tb);
        lerr(l, e->line, e->col, "operands of '+' must be two ints or two strs, got %s and %s", ta, tb);
        return 0;
    }
    default: {
        if ((a & T_INT) && (b & T_INT)) return (e->op == TOK_MINUS || e->op == TOK_STAR || e->op == TOK_SLASH) ? T_INT : T_BOOL;
        ty_name(a, ta, sizeof ta);
        ty_name(b, tb, sizeof tb);
        lerr(l, e->line, e->col, "operands of '%s' must be ints, got %s and %s", op_text(e->op), ta, tb);
        return 0;
    }
    }
}

static Val lower_expr(L *l, const Expr *e, int hint);

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

static Val lower_call(L *l, const Expr *e, int hint) {
    Name n = lookup(l, e->name);
    if (n.kind == NAME_LOCAL || n.kind == NAME_GLOBAL) {
        lerr(l, e->line, e->col, "'%s' is a variable, not a function", e->name);
        return NO_VAL;
    }
    if (n.kind == NAME_NONE) {
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
        for (size_t i = 0; i < e->nargs; i++) {
            /* Static check only: the runtime converter validates the value anyway. */
            Ty want = ctype_luma(x->ctypes[i]);
            if (!(vals[i].ty & want)) {
                char w[64], g[64];
                ty_name(want, w, sizeof w);
                ty_name(vals[i].ty, g, sizeof g);
                lerr(l, e->args[i]->line, e->args[i]->col, "argument '%s' of '%s' expects %s (C %s), got %s",
                     x->decl->params[i], x->name, w, ctype_name(x->ctypes[i]), g);
                goto done;
            }
        }
        r = result(l, hint, &d);
        emit_call(l, hint == DISCARD ? IR_NONE : d, x->global, args, (int)e->nargs, e->line);
        r.ty = ctype_luma(x->cret);
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

/* Assigns v to the variable named by n (already resolved). */
static Val assign_to(L *l, const Expr *e, Name n, int hint) {
    char what[300];
    snprintf(what, sizeof what, "variable '%s'", e->name);
    if (n.kind == NAME_LOCAL) {
        Val v = lower_into(l, e->right, n.index);
        if (v.v < 0) return NO_VAL;
        v.ty = flow(l, v, n.ty, what, e->right->line, e->right->col);
        if (hint >= 0) {
            emit_mov(l, hint, n.index, e->line);
            v.v = hint;
        }
        return v;
    }
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
static Val lower_expr(L *l, const Expr *e, int hint) {
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
                char ta[64];
                ty_name(a.ty, ta, sizeof ta);
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
        Val b = lower_into(l, e->right, r);
        emit_jmp(l, end, e->line);
        set_block(l, end);
        Ty t = a.ty | b.ty;
        if (d >= 0) {
            emit_mov(l, d, r, e->line);
            return (Val){d, t};
        }
        return (Val){r, t};
    }
    case EXPR_CALL:
        return lower_call(l, e, hint);
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
        char tn[64];
        ty_name(ty, tn, sizeof tn);
        lerr(l, s->line, s->col, "variable '%s' of type %s needs an initializer", s->name, tn);
        return;
    }
    if (f->depth == 0) { /* top-level variable: a module global */
        int g = find_gvar(l, s->name);
        Val v = s->expr ? lower_expr(l, s->expr, -1) : NO_VAL;
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
        Val iv = lower_into(l, s->expr, v);
        if (iv.v >= 0) flow(l, iv, ty, what, s->expr->line, s->expr->col);
    } else {
        emit(l, IR_CONST_NIL, s->line)->dst = v;
    }
    f->scope[idx].vreg = v;
}

static void lower_stmt(L *l, const Stmt *s) {
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
        return; /* lowered separately (functions are hoisted) */
    case STMT_RETURN: {
        Val v;
        if (s->expr) {
            v = lower_expr(l, s->expr, -1);
            if (v.v < 0) return;
        } else {
            v = (Val){temp(l), T_NIL};
            emit(l, IR_CONST_NIL, s->line)->dst = v.v;
        }
        const FnInfo *fi = l->f->info;
        if (fi) {
            char what[300];
            snprintf(what, sizeof what, "return value of '%s'", fi->name);
            flow(l, v, fi->ret, what, s->expr ? s->expr->line : s->line, s->expr ? s->expr->col : s->col);
        }
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
            for (int k = 0; k < f->blocks[i].n; k++) free(f->blocks[i].instrs[k].args);
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
        const FnInfo *fi = l->f->info;
        if (reachable_end && fi && !(fi->ret & T_NIL)) {
            char tn[64];
            ty_name(fi->ret, tn, sizeof tn);
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
    memset(f, 0, sizeof *f);
}

static void begin_function(L *l, Fn *f, int fi, const FnInfo *info) {
    memset(f, 0, sizeof *f);
    f->fi = fi;
    f->is_main = info == NULL;
    f->info = info;
    l->f = f;
    set_block(l, ir_func_block(fn(l), "entry"));
}

static void lower_function(L *l, const FnInfo *info) {
    const Stmt *s = info->decl;
    Fn f;
    begin_function(l, &f, l->m->globals[info->global].func, info);
    /* parameters and the body's top-level declarations share one scope */
    f.depth = 1;
    for (size_t i = 0; i < s->nparams && !l->failed; i++) {
        for (size_t j = 0; j < i; j++)
            if (strcmp(s->params[i], s->params[j]) == 0)
                lerr(l, s->param_line[i], s->param_col[i], "duplicate parameter '%s'", s->params[i]);
        int v = ir_func_new_vreg(fn(l), s->params[i]); /* params are vregs 0..n-1 */
        mark_var(l, v, true);
        bind(l, s->params[i], v, info->ptypes[i]);
    }
    for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
    /* "can reach the end" errors point at the function name */
    end_function(l, s->line, s->col);
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

/* Collects top-level functions, externs and variables, checking conflicts. */
static void collect_declarations(L *l, const Program *prog) {
    for (size_t i = 0; i < prog->len && !l->failed; i++) {
        const Stmt *s = prog->stmts[i];
        if (s->kind == STMT_FUN) {
            if (name_taken(l, s)) continue;
            if (s->nparams > MAX_PARAMS) {
                lerr(l, s->line, s->col, "functions with more than %d parameters are not supported yet", MAX_PARAMS);
                continue;
            }
            char sym[300];
            snprintf(sym, sizeof sym, "fn.%s", s->name);
            int fi = ir_add_func(l->m, sym, (int)s->nparams);
            FnInfo info = {s->name, l->m->funcs[fi].global, (int)s->nparams, s, NULL, T_ANY};
            info.ptypes = xmalloc((s->nparams ? s->nparams : 1) * sizeof *info.ptypes);
            for (size_t k = 0; k < s->nparams; k++) info.ptypes[k] = resolve_type(l, &s->param_types[k]);
            info.ret = resolve_type(l, &s->type);
            /* the signature is part of the IR: callers and returns must uphold it */
            IrGlobal *g = &l->m->globals[info.global];
            g->ty = info.ret;
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
            ExtInfo x = {s->name, -1, s, NULL, CT_VOID};
            x.ctypes = xmalloc((s->nparams ? s->nparams : 1) * sizeof *x.ctypes);
            for (size_t k = 0; k < s->nparams; k++) x.ctypes[k] = resolve_ctype(l, &s->param_types[k], false);
            x.cret = resolve_ctype(l, &s->type, true);
            if (l->failed) {
                free(x.ctypes);
                continue;
            }
            x.global = ir_add_cextern(l->m, s->name, (int)s->nparams, x.ctypes, (const char *const *)s->params, x.cret);
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
                    char a[64], b[64];
                    ty_name(l->gvars[g].ty, a, sizeof a);
                    ty_name(ty, b, sizeof b);
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
    for (int i = 0; i < l->ngvars && !l->failed; i++) {
        int f = find_fn(l, l->gvars[i].name);
        const Stmt *d = f >= 0 ? l->fns[f].decl : NULL;
        int x = find_ext(l, l->gvars[i].name);
        if (x >= 0) d = l->exts[x].decl;
        if (d) lerr(l, d->line, d->col, "'%s' is declared as both a function and a variable", d->name);
    }
}

bool lower_program(const Program *prog, IrModule *out) {
    ir_module_init(out, prog->source_path);
    L l = {0};
    l.path = prog->source_path;
    l.m = out;
    l.write_fn = l.space_fn = l.newline_fn = -1;
    Fn main_fn;
    int main_fi = ir_add_func(out, "luma_main", 0);
    collect_declarations(&l, prog);

    if (!l.failed) {
        begin_function(&l, &main_fn, main_fi, NULL);
        for (size_t i = 0; i < prog->len && !l.failed; i++) lower_stmt(&l, prog->stmts[i]);
        end_function(&l, 0, 0);
    }
    for (int i = 0; i < l.nfns && !l.failed; i++) lower_function(&l, &l.fns[i]);

    for (int i = 0; i < l.nfns; i++) free(l.fns[i].ptypes);
    for (int i = 0; i < l.nexts; i++) free(l.exts[i].ctypes);
    free(l.fns);
    free(l.exts);
    free(l.gvars);
    if (l.failed) {
        ir_module_free(out);
        return false;
    }
    return true;
}
