#include "lower.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_PARAMS 6 /* SysV register arguments; see docs/IR.md */
#define DISCARD (-2) /* lower_expr hint: the value is not needed */

typedef struct {
    char *name;
    int vreg;  /* -1 while the declaration's initializer is being lowered */
    int depth;
} Binding;

typedef struct {
    const char *name;
    int global;   /* IR global index (@fn.NAME) */
    int arity;
    const Stmt *decl;
} FnInfo;

typedef struct {
    const char *name;
    int global;   /* IR global index (@var.NAME) */
    bool declared; /* top-level code: has its declaration been reached yet? */
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
} Fn;

typedef struct {
    const char *path;
    IrModule *m;
    FnInfo *fns;
    int nfns;
    GVar *gvars;
    int ngvars;
    int nstrings;
    int write_fn, space_fn, newline_fn; /* externs, created on first use */
    Fn *f;
    bool failed;
} L;

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

/* ---- name resolution ---- */

typedef enum { NAME_NONE, NAME_LOCAL, NAME_GLOBAL, NAME_FUNCTION, NAME_BUILTIN } NameKind;

typedef struct {
    NameKind kind;
    int index;          /* vreg / gvar index / fn index */
    bool uninitialized; /* NAME_LOCAL: still inside its own initializer */
} Name;

static int find_fn(L *l, const char *name) {
    for (int i = 0; i < l->nfns; i++)
        if (strcmp(l->fns[i].name, name) == 0) return i;
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
            return (Name){NAME_LOCAL, f->scope[i].vreg, f->scope[i].vreg < 0};
    int g = find_gvar(l, name);
    /* Function bodies see every top-level variable; top-level code only those
     * already declared (later ones are compile errors, as in a script). */
    if (g >= 0 && (!f->is_main || l->gvars[g].declared)) return (Name){NAME_GLOBAL, g, false};
    int fi = find_fn(l, name);
    if (fi >= 0) return (Name){NAME_FUNCTION, fi, false};
    if (is_builtin(name)) return (Name){NAME_BUILTIN, 0, false};
    return (Name){NAME_NONE, 0, false};
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
        lerr(l, line, col, "'%s' is a function; functions are not first-class values yet, so it can only be called", name);
        break;
    case NAME_BUILTIN: lerr(l, line, col, "'%s' is a builtin function: call it as %s(...)", name, name); break;
    case NAME_GLOBAL: break;
    }
    return n;
}

static void bind(L *l, const char *name, int vreg) {
    Fn *f = l->f;
    if (f->nscope == f->cap_scope) {
        f->cap_scope = f->cap_scope ? f->cap_scope * 2 : 16;
        f->scope = xrealloc(f->scope, (size_t)f->cap_scope * sizeof *f->scope);
    }
    Binding *b = &f->scope[f->nscope++];
    b->name = xstrdup(name);
    b->vreg = vreg;
    b->depth = f->depth;
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

static int lower_expr(L *l, const Expr *e, int hint);

static void lower_into(L *l, const Expr *e, int dst) {
    int v = lower_expr(l, e, dst);
    if (v >= 0) emit_mov(l, dst, v, e->line);
}

/* Lowers a list of operands left to right. If a later operand assigns to a
 * local variable that an earlier operand merely named, the earlier value is
 * snapshotted first (Lox evaluates strictly left to right). */
static bool lower_operands(L *l, Expr *const *es, size_t n, int *out) {
    for (size_t i = 0; i < n; i++) {
        int v = lower_expr(l, es[i], -1);
        if (v < 0) return false;
        bool later_assigns = false;
        for (size_t j = i + 1; j < n && !later_assigns; j++) later_assigns = has_assign(es[j]);
        if (is_var_vreg(l, v) && later_assigns) {
            int t = temp(l);
            emit_mov(l, t, v, es[i]->line);
            v = t;
        }
        out[i] = v;
    }
    return true;
}

static int lower_call(L *l, const Expr *e, int hint) {
    Name n = lookup(l, e->name);
    if (n.kind == NAME_LOCAL || n.kind == NAME_GLOBAL) {
        lerr(l, e->line, e->col, "'%s' is a variable, not a function", e->name);
        return -1;
    }
    if (n.kind == NAME_NONE) {
        lerr(l, e->line, e->col, "undefined function '%s'", e->name);
        return -1;
    }
    int *args = xmalloc((e->nargs ? e->nargs : 1) * sizeof(int));
    if (!lower_operands(l, e->args, e->nargs, args)) {
        free(args);
        return -1;
    }
    int d = hint >= 0 ? hint : -1;
    if (n.kind == NAME_BUILTIN) { /* print(a, b, ...): all arguments are evaluated before anything is written */
        int w = runtime_extern(l, &l->write_fn, "luma_write", 1);
        for (size_t i = 0; i < e->nargs; i++) {
            if (i > 0) emit_call(l, IR_NONE, runtime_extern(l, &l->space_fn, "luma_write_space", 0), NULL, 0, e->line);
            emit_call(l, IR_NONE, w, &args[i], 1, e->line);
        }
        emit_call(l, IR_NONE, runtime_extern(l, &l->newline_fn, "luma_write_newline", 0), NULL, 0, e->line);
        free(args);
        if (hint == DISCARD) return -1;
        if (d < 0) d = temp(l);
        emit(l, IR_CONST_NIL, e->line)->dst = d; /* print returns nil */
        return d;
    }
    const FnInfo *fi = &l->fns[n.index];
    if ((int)e->nargs != fi->arity) {
        lerr(l, e->line, e->col, "'%s' expects %d argument%s, got %zu", e->name, fi->arity, fi->arity == 1 ? "" : "s", e->nargs);
        free(args);
        return -1;
    }
    if (hint != DISCARD && d < 0) d = temp(l);
    emit_call(l, hint == DISCARD ? IR_NONE : d, fi->global, args, (int)e->nargs, e->line);
    free(args);
    return d;
}

/* Returns the vreg holding e's value. If hint >= 0 the result is written to
 * hint when that is free (no extra mov). The hint is only ever written by
 * the final instruction of the expression, after all operands are read, so
 * `x = x + 1` can safely lower to `%x = add %x, %1`. DISCARD means the value
 * is unused (expression statements). */
static int lower_expr(L *l, const Expr *e, int hint) {
    if (l->failed) return -1;
    int d = hint >= 0 ? hint : -1;
    IrInstr *in;
    switch (e->kind) {
    case EXPR_INT:
        if (d < 0) d = temp(l);
        in = emit(l, IR_CONST_INT, e->line);
        in->dst = d;
        in->imm = e->ival;
        return d;
    case EXPR_STRING: {
        int g = string_data(l, e->str, e->str_len);
        if (d < 0) d = temp(l);
        in = emit(l, IR_CONST_DATA, e->line);
        in->dst = d;
        in->global = g;
        return d;
    }
    case EXPR_NIL:
    case EXPR_TRUE:
    case EXPR_FALSE:
        if (d < 0) d = temp(l);
        in = emit(l, e->kind == EXPR_NIL ? IR_CONST_NIL : e->kind == EXPR_TRUE ? IR_CONST_TRUE : IR_CONST_FALSE, e->line);
        in->dst = d;
        return d;
    case EXPR_VAR: {
        Name n = resolve_variable(l, e->name, e->line, e->col);
        if (n.kind == NAME_LOCAL) {
            if (d >= 0) {
                emit_mov(l, d, n.index, e->line);
                return d;
            }
            return n.index;
        }
        if (n.kind != NAME_GLOBAL) return -1;
        if (d < 0) d = temp(l);
        in = emit(l, IR_LOAD, e->line);
        in->dst = d;
        in->global = l->gvars[n.index].global;
        return d;
    }
    case EXPR_ASSIGN: {
        Name n = lookup(l, e->name);
        if (n.kind == NAME_FUNCTION || n.kind == NAME_BUILTIN) {
            lerr(l, e->line, e->col, "cannot assign to function '%s'", e->name);
            return -1;
        }
        n = resolve_variable(l, e->name, e->line, e->col);
        if (n.kind == NAME_LOCAL) {
            lower_into(l, e->right, n.index);
            if (d >= 0) {
                emit_mov(l, d, n.index, e->line);
                return d;
            }
            return n.index;
        }
        if (n.kind != NAME_GLOBAL) return -1;
        int v = lower_expr(l, e->right, d);
        if (v < 0) return -1;
        in = emit(l, IR_STORE, e->line);
        in->global = l->gvars[n.index].global;
        in->a = v;
        return v;
    }
    case EXPR_UNARY: {
        int a = lower_expr(l, e->right, -1);
        if (a < 0) return -1;
        if (d < 0) d = temp(l);
        in = emit(l, e->op == TOK_MINUS ? IR_NEG : IR_NOT, e->line);
        in->dst = d;
        in->a = a;
        return d;
    }
    case EXPR_BINARY: {
        Expr *ops[2] = {e->left, e->right};
        int v[2];
        if (!lower_operands(l, ops, 2, v)) return -1;
        if (d < 0) d = temp(l);
        in = emit(l, binary_op(e->op), e->line);
        in->dst = d;
        in->a = v[0];
        in->b = v[1];
        return d;
    }
    case EXPR_LOGICAL: {
        /* `a or b`  : r = a; if r is truthy -> end, else r = b
         * `a and b` : r = a; if r is falsy  -> end, else r = b
         * r is always a fresh temp: writing a variable hint early would be
         * visible to the right operand. */
        int r = temp(l);
        lower_into(l, e->left, r);
        if (l->failed) return -1;
        int n = l->f->nlabels++;
        bool is_or = e->op == TOK_OR;
        int rhs = new_block(l, is_or ? "or.rhs" : "and.rhs", n);
        int end = new_block(l, is_or ? "or.end" : "and.end", n);
        if (is_or) emit_br(l, r, end, rhs, e->line);
        else emit_br(l, r, rhs, end, e->line);
        set_block(l, rhs);
        lower_into(l, e->right, r);
        emit_jmp(l, end, e->line);
        set_block(l, end);
        if (d >= 0) {
            emit_mov(l, d, r, e->line);
            return d;
        }
        return r;
    }
    case EXPR_CALL:
        return lower_call(l, e, hint);
    }
    return -1;
}

/* ---- statements ---- */

static void lower_stmt(L *l, const Stmt *s);

/* After a `return`, following statements in the same block are unreachable;
 * they are lowered into a fresh block that is pruned at the end. */
static void start_dead_block(L *l) { set_block(l, new_block(l, "dead", l->f->nlabels++)); }

static void lower_var(L *l, const Stmt *s) {
    Fn *f = l->f;
    if (f->depth == 0) { /* top-level variable: a module global */
        int g = find_gvar(l, s->name);
        int v = s->expr ? lower_expr(l, s->expr, -1) : -1;
        if (l->failed) return;
        if (v < 0) {
            v = temp(l);
            emit(l, IR_CONST_NIL, s->line)->dst = v;
        }
        IrInstr *in = emit(l, IR_STORE, s->line);
        in->global = l->gvars[g].global;
        in->a = v;
        l->gvars[g].declared = true; /* the initializer saw the previous declaration, if any */
        return;
    }
    for (int i = f->nscope - 1; i >= 0 && f->scope[i].depth == f->depth; i--)
        if (strcmp(f->scope[i].name, s->name) == 0) {
            lerr(l, s->line, s->col, "a variable named '%s' is already declared in this scope", s->name);
            return;
        }
    /* The name is in scope (but unreadable) during its initializer. */
    bind(l, s->name, -1);
    int idx = f->nscope - 1;
    int v = new_var_vreg(l, s->name);
    if (s->expr) lower_into(l, s->expr, v);
    else emit(l, IR_CONST_NIL, s->line)->dst = v;
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
        return; /* lowered separately (functions are hoisted) */
    case STMT_RETURN: {
        int v;
        if (s->expr) {
            v = lower_expr(l, s->expr, -1);
            if (v < 0) return;
        } else {
            v = temp(l);
            emit(l, IR_CONST_NIL, s->line)->dst = v;
        }
        emit(l, IR_RET, s->line)->a = v;
        start_dead_block(l);
        return;
    }
    case STMT_BLOCK:
        l->f->depth++;
        for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
        pop_scope(l);
        return;
    case STMT_IF: {
        int c = lower_expr(l, s->expr, -1);
        if (c < 0) return;
        int n = l->f->nlabels++;
        int then_b = new_block(l, "if.then", n);
        int else_b = s->else_branch ? new_block(l, "if.else", n) : -1;
        int end_b = new_block(l, "if.end", n);
        emit_br(l, c, then_b, else_b >= 0 ? else_b : end_b, s->line);
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
        int c = lower_expr(l, s->expr, -1);
        if (c < 0) return;
        emit_br(l, c, body_b, end_b, s->line);
        set_block(l, body_b);
        lower_stmt(l, s->body);
        emit_jmp(l, cond_b, s->line);
        set_block(l, end_b);
        return;
    }
    }
}

/* Reorders blocks into the order they were filled (source order), then drops
 * blocks unreachable from the entry (code after `return`). */
static void finish_blocks(L *l) {
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
}

/* Ends the current function (implicit `return nil`), cleans up its blocks
 * and frees per-function state. */
static void end_function(L *l, int line) {
    if (!l->failed) {
        if (!ir_block_terminated(&fn(l)->blocks[l->f->cur])) {
            int r = temp(l);
            emit(l, IR_CONST_NIL, line)->dst = r;
            emit(l, IR_RET, line)->a = r;
        }
        finish_blocks(l);
        ir_func_canonicalize(fn(l));
    }
    Fn *f = l->f;
    for (int i = 0; i < f->nscope; i++) free(f->scope[i].name);
    free(f->scope);
    free(f->order);
    free(f->is_var);
    memset(f, 0, sizeof *f);
}

static void begin_function(L *l, Fn *f, int fi, bool is_main) {
    memset(f, 0, sizeof *f);
    f->fi = fi;
    f->is_main = is_main;
    l->f = f;
    set_block(l, ir_func_block(fn(l), "entry"));
}

static void lower_function(L *l, const FnInfo *info) {
    const Stmt *s = info->decl;
    Fn f;
    begin_function(l, &f, l->m->globals[info->global].func, false);
    /* parameters and the body's top-level declarations share one scope (Lox) */
    f.depth = 1;
    for (size_t i = 0; i < s->nparams && !l->failed; i++) {
        for (size_t j = 0; j < i; j++)
            if (strcmp(s->params[i], s->params[j]) == 0)
                lerr(l, s->param_line[i], s->param_col[i], "duplicate parameter '%s'", s->params[i]);
        int v = ir_func_new_vreg(fn(l), s->params[i]); /* params are vregs 0..n-1 */
        mark_var(l, v, true);
        bind(l, s->params[i], v);
    }
    for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
    end_function(l, s->line);
}

/* Collects top-level functions and variables, checking for conflicts. */
static void collect_declarations(L *l, const Program *prog) {
    for (size_t i = 0; i < prog->len && !l->failed; i++) {
        const Stmt *s = prog->stmts[i];
        if (s->kind == STMT_FUN) {
            if (is_builtin(s->name)) {
                lerr(l, s->line, s->col, "cannot redefine builtin function '%s'", s->name);
            } else if (find_fn(l, s->name) >= 0) {
                lerr(l, s->line, s->col, "function '%s' is already defined", s->name);
            } else if (s->nparams > MAX_PARAMS) {
                lerr(l, s->line, s->col, "functions with more than %d parameters are not supported yet", MAX_PARAMS);
            } else {
                char sym[300];
                snprintf(sym, sizeof sym, "fn.%s", s->name);
                int fi = ir_add_func(l->m, sym, (int)s->nparams);
                l->fns = xrealloc(l->fns, (size_t)(l->nfns + 1) * sizeof *l->fns);
                l->fns[l->nfns++] = (FnInfo){s->name, l->m->funcs[fi].global, (int)s->nparams, s};
            }
        } else if (s->kind == STMT_VAR && find_gvar(l, s->name) < 0) {
            if (is_builtin(s->name)) {
                lerr(l, s->line, s->col, "'%s' is a builtin function and cannot be redeclared as a variable", s->name);
                continue;
            }
            char sym[300];
            snprintf(sym, sizeof sym, "var.%s", s->name);
            l->gvars = xrealloc(l->gvars, (size_t)(l->ngvars + 1) * sizeof *l->gvars);
            l->gvars[l->ngvars++] = (GVar){s->name, ir_add_var(l->m, sym), false};
        }
    }
    for (int i = 0; i < l->ngvars && !l->failed; i++) {
        int f = find_fn(l, l->gvars[i].name);
        if (f >= 0) {
            const Stmt *d = l->fns[f].decl;
            lerr(l, d->line, d->col, "'%s' is declared as both a function and a variable", d->name);
        }
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

    begin_function(&l, &main_fn, main_fi, true);
    for (size_t i = 0; i < prog->len && !l.failed; i++) lower_stmt(&l, prog->stmts[i]);
    end_function(&l, 0);

    for (int i = 0; i < l.nfns && !l.failed; i++) lower_function(&l, &l.fns[i]);

    free(l.fns);
    free(l.gvars);
    if (l.failed) {
        ir_module_free(out);
        return false;
    }
    return true;
}
