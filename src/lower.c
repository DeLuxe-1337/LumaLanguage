#include "lower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    char *name;
    int vreg;  /* -1 while the declaration's initializer is being lowered */
    int depth; /* 0 = top level */
} Binding;

typedef struct {
    const char *path;
    IrModule *m;
    int fi;              /* index of @luma_main */
    int cur;             /* current block */
    int *order;          /* blocks in the order they become current */
    int norder, cap_order;
    bool *is_var;        /* per vreg: holds a source variable (mutable) */
    int cap_is_var;
    Binding *scope;
    int nscope, cap_scope;
    int depth;
    int ntemps, nlabels, nstrings;
    int print_fn;        /* global index of @luma_print */
    bool failed;
} L;

static IrFunc *fn(L *l) { return &l->m->funcs[l->fi]; }

static void lerr(L *l, int line, int col, const char *fmt, const char *arg) {
    if (l->failed) return;
    fprintf(stderr, "%s:%d:%d: error: ", l->path, line, col);
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    l->failed = true;
}

/* ---- vregs and blocks ---- */

static void mark_var(L *l, int v, bool is_var) {
    if (v >= l->cap_is_var) {
        int n = l->cap_is_var ? l->cap_is_var : 64;
        while (n <= v) n *= 2;
        l->is_var = xrealloc(l->is_var, (size_t)n * sizeof *l->is_var);
        memset(l->is_var + l->cap_is_var, 0, (size_t)(n - l->cap_is_var) * sizeof *l->is_var);
        l->cap_is_var = n;
    }
    l->is_var[v] = is_var;
}

static int temp(L *l) {
    char name[32];
    snprintf(name, sizeof name, "%d", l->ntemps++);
    int v = ir_func_new_vreg(fn(l), name); /* numeric names never clash with identifiers */
    mark_var(l, v, false);
    return v;
}

static bool is_var_vreg(L *l, int v) { return v >= 0 && v < l->cap_is_var && l->is_var[v]; }

static int new_var_vreg(L *l, const char *name) {
    IrFunc *f = fn(l);
    char buf[300];
    snprintf(buf, sizeof buf, "%s", name);
    for (int k = 1; ir_func_find_vreg(f, buf) >= 0; k++) snprintf(buf, sizeof buf, "%s.%d", name, k);
    int v = ir_func_new_vreg(f, buf);
    mark_var(l, v, true);
    return v;
}

/* Creates a block; it is placed in the final order when it becomes current. */
static int new_block(L *l, const char *kind, int n) {
    char label[64];
    snprintf(label, sizeof label, "%s.%d", kind, n);
    return ir_func_block(fn(l), label);
}

static void set_block(L *l, int b) {
    l->cur = b;
    if (l->norder == l->cap_order) {
        l->cap_order = l->cap_order ? l->cap_order * 2 : 16;
        l->order = xrealloc(l->order, (size_t)l->cap_order * sizeof *l->order);
    }
    l->order[l->norder++] = b;
}

static IrInstr *emit(L *l, IrOp op, int line) {
    IrInstr *in = ir_emit(fn(l), l->cur, op);
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
    if (ir_block_terminated(&fn(l)->blocks[l->cur])) return;
    emit(l, IR_JMP, line)->target[0] = target;
}

static void emit_br(L *l, int cond, int t, int f, int line) {
    IrInstr *in = emit(l, IR_BR, line);
    in->a = cond;
    in->target[0] = t;
    in->target[1] = f;
}

/* ---- scopes ---- */

static Binding *lookup(L *l, const char *name) {
    for (int i = l->nscope - 1; i >= 0; i--)
        if (strcmp(l->scope[i].name, name) == 0) return &l->scope[i];
    return NULL;
}

static Binding *bind(L *l, const char *name, int vreg) {
    if (l->nscope == l->cap_scope) {
        l->cap_scope = l->cap_scope ? l->cap_scope * 2 : 16;
        l->scope = xrealloc(l->scope, (size_t)l->cap_scope * sizeof *l->scope);
    }
    Binding *b = &l->scope[l->nscope++];
    b->name = xstrdup(name);
    b->vreg = vreg;
    b->depth = l->depth;
    return b;
}

static void pop_scope(L *l) {
    while (l->nscope > 0 && l->scope[l->nscope - 1].depth == l->depth) free(l->scope[--l->nscope].name);
    l->depth--;
}

static int resolve(L *l, const char *name, int line, int col) {
    Binding *b = lookup(l, name);
    if (!b) {
        lerr(l, line, col, "undefined variable '%s'", name);
        return -1;
    }
    if (b->vreg < 0) {
        lerr(l, line, col, "cannot read local variable '%s' in its own initializer", name);
        return -1;
    }
    return b->vreg;
}

/* ---- expressions ---- */

static bool has_assign(const Expr *e) {
    if (!e) return false;
    return e->kind == EXPR_ASSIGN || has_assign(e->left) || has_assign(e->right);
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

/* Lowers e so that its value ends up in dst. */
static void lower_into(L *l, const Expr *e, int dst) {
    int v = lower_expr(l, e, dst);
    if (v >= 0) emit_mov(l, dst, v, e->line);
}

/* Returns the vreg holding e's value. If hint >= 0 the result is written to
 * hint when that is free (no extra mov). The hint is only ever written by
 * the final instruction of the expression, after all operands are read,
 * so `x = x + 1` can safely lower to `%x = add %x, %1`. */
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
        int v = resolve(l, e->name, e->line, e->col);
        if (v < 0) return -1;
        if (d >= 0) {
            emit_mov(l, d, v, e->line);
            return d;
        }
        return v;
    }
    case EXPR_ASSIGN: {
        int x = resolve(l, e->name, e->line, e->col);
        if (x < 0) return -1;
        lower_into(l, e->right, x);
        if (d >= 0) {
            emit_mov(l, d, x, e->line);
            return d;
        }
        return x;
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
        int a = lower_expr(l, e->left, -1);
        if (a < 0) return -1;
        /* Lox evaluates left to right: if the right operand assigns to the
         * variable we just read, snapshot the left value first. */
        if (is_var_vreg(l, a) && has_assign(e->right)) {
            int t = temp(l);
            emit_mov(l, t, a, e->line);
            a = t;
        }
        int b = lower_expr(l, e->right, -1);
        if (b < 0) return -1;
        if (d < 0) d = temp(l);
        in = emit(l, binary_op(e->op), e->line);
        in->dst = d;
        in->a = a;
        in->b = b;
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
        int n = l->nlabels++;
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
    }
    return -1;
}

/* ---- statements ---- */

static void lower_stmt(L *l, const Stmt *s);

static void lower_var(L *l, const Stmt *s) {
    if (l->depth > 0) {
        for (int i = l->nscope - 1; i >= 0 && l->scope[i].depth == l->depth; i--)
            if (strcmp(l->scope[i].name, s->name) == 0) {
                lerr(l, s->line, s->col, "a variable named '%s' is already declared in this scope", s->name);
                return;
            }
        /* Block-local: the name is in scope (but unreadable) during its initializer. */
        Binding *b = bind(l, s->name, -1);
        int idx = (int)(b - l->scope);
        int v = new_var_vreg(l, s->name);
        if (s->expr) lower_into(l, s->expr, v);
        else emit(l, IR_CONST_NIL, s->line)->dst = v;
        l->scope[idx].vreg = v;
        return;
    }
    /* Top level: the initializer sees any previous declaration of the name. */
    int v = new_var_vreg(l, s->name);
    if (s->expr) lower_into(l, s->expr, v);
    else emit(l, IR_CONST_NIL, s->line)->dst = v;
    if (!l->failed) bind(l, s->name, v);
}

static void lower_stmt(L *l, const Stmt *s) {
    if (l->failed) return;
    switch (s->kind) {
    case STMT_PRINT: {
        int v = lower_expr(l, s->expr, -1);
        if (v < 0) return;
        IrInstr *in = emit(l, IR_CALL, s->line);
        in->global = l->print_fn;
        in->args = xmalloc(sizeof(int));
        in->args[0] = v;
        in->nargs = 1;
        return;
    }
    case STMT_EXPR:
        lower_expr(l, s->expr, -1);
        return;
    case STMT_VAR:
        lower_var(l, s);
        return;
    case STMT_BLOCK:
        l->depth++;
        for (size_t i = 0; i < s->n && !l->failed; i++) lower_stmt(l, s->stmts[i]);
        pop_scope(l);
        return;
    case STMT_IF: {
        int c = lower_expr(l, s->expr, -1);
        if (c < 0) return;
        int n = l->nlabels++;
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
        int n = l->nlabels++;
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

/* Reorders blocks into the order they were filled (source order), so the
 * printed IR reads top to bottom and fall-through jumps can be elided. */
static void reorder_blocks(L *l) {
    IrFunc *f = fn(l);
    int nb = f->nblocks;
    int *map = xmalloc((size_t)nb * sizeof *map);
    for (int i = 0; i < nb; i++) map[i] = -1;
    int next = 0;
    for (int i = 0; i < l->norder; i++)
        if (map[l->order[i]] < 0) map[l->order[i]] = next++;
    for (int i = 0; i < nb; i++)
        if (map[i] < 0) map[i] = next++;
    IrBlock *nbk = xmalloc((size_t)nb * sizeof *nbk);
    for (int i = 0; i < nb; i++) nbk[map[i]] = f->blocks[i];
    memcpy(f->blocks, nbk, (size_t)nb * sizeof *nbk);
    free(nbk);
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            IrInstr *in = &f->blocks[b].instrs[k];
            for (int t = 0; t < 2; t++)
                if (in->target[t] >= 0) in->target[t] = map[in->target[t]];
        }
    free(map);
}

bool lower_program(const Program *prog, IrModule *out) {
    ir_module_init(out, prog->source_path);
    L l = {0};
    l.path = prog->source_path;
    l.m = out;
    l.print_fn = ir_add_extern(out, "luma_print", 1);
    l.fi = ir_add_func(out, "luma_main", 0);
    set_block(&l, ir_func_block(fn(&l), "entry"));
    for (size_t i = 0; i < prog->len && !l.failed; i++) lower_stmt(&l, prog->stmts[i]);
    if (!l.failed) {
        int r = temp(&l);
        emit(&l, IR_CONST_NIL, 0)->dst = r;
        emit(&l, IR_RET, 0)->a = r;
        reorder_blocks(&l);
        ir_func_canonicalize(fn(&l));
    }
    for (int i = 0; i < l.nscope; i++) free(l.scope[i].name);
    free(l.scope);
    free(l.order);
    free(l.is_var);
    if (l.failed) {
        ir_module_free(out);
        return false;
    }
    return true;
}
