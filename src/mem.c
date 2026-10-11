/* mem.c - the compiler's memory-management analyses (see mem.h and
 * docs/DESIGN.md "Memory").
 *
 * Ownership model used by mem_lower. A *reference* to a counted object is
 * held by a vreg, a struct field or a global slot; each holder owns exactly
 * one count. For every instruction an operand is either
 *   consumed   the instruction takes over the reference: mov / check (the
 *              result holds it), store / setfield value (the slot holds it),
 *              new arguments (the fields hold them), ret (the caller gets it);
 *   borrowed   used for the instruction's duration only: everything else,
 *              including call and callm arguments (parameters are borrowed:
 *              the caller keeps its reference alive during the call).
 * Results are owned (+1: new objects, calls, mov, ...) except load and
 * getfield, whose result is the slot's reference and gets a `retain`.
 *
 * The walk goes backwards through each block with the live set:
 *   - a consumed operand that dies here is moved (no operation); one that is
 *     still live afterwards (or a borrowed parameter) is retained first;
 *   - a borrowed operand that dies here is released after the instruction;
 *   - a result that is never used is released at once;
 *   - a value live at the end of a block but not into a successor is
 *     released on that edge (at the start of the successor, or in a new
 *     block when the edge is critical).
 * Parameters are borrowed. A parameter vreg that the function reassigns is
 * retained on entry, so it always owns its value. Vregs that can only hold
 * non-counted values (ints, booleans, nil, string literals, and in
 * --mm=manual structs that are never counted) get no operations at all.
 *
 * Overwriting a global or a field must release the old value: mem_lower sets
 * imm = 1 on such store and static setfield instructions and the backends
 * release it (the runtime does it for dynamic setfield). */
#include "mem.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "opt_internal.h"

/* ---------------------------------------------------------------------- */
/* Compile-time use-after-free / double-free                               */

/* "p" for the vreg of variable p (lowering names shadowed ones "p.1");
 * NULL for temporaries (named by numbers). */
static const char *var_name(const IrFunc *f, int v, char *buf, size_t n) {
    const char *s = f->vregs[v];
    if (!*s || isdigit((unsigned char)s[0])) return NULL;
    snprintf(buf, n, "%s", s);
    char *dot = strchr(buf, '.');
    if (dot) *dot = '\0';
    return buf;
}

static bool is_free_call(const IrModule *m, const IrInstr *in) {
    return in->op == IR_CALL && in->nargs == 1 && m->globals[in->global].kind == IRG_EXTERN &&
           (strcmp(m->globals[in->global].name, "luma_free") == 0 || strcmp(m->globals[in->global].name, "luma_dispose") == 0);
}

/* "free" or "dispose", for messages */
static const char *free_word(const IrModule *m, const IrInstr *in) {
    return strcmp(m->globals[in->global].name, "luma_dispose") == 0 ? "dispose" : "free";
}

/* Rc::new(x) moves x into the box: `new Rc.S(%x)` or `call @luma_rc_new(%x)`.
 * Returns the moved vreg or -1. */
static int moved_into_rc(const IrModule *m, const IrInstr *in) {
    if (in->op == IR_NEW && in->nargs == 1 && m->structs[in->sid].kind == IR_STRUCT_RC) return in->args[0];
    if (in->op == IR_CALL && in->nargs == 1 && m->globals[in->global].kind == IRG_EXTERN &&
        strcmp(m->globals[in->global].name, "luma_rc_new") == 0)
        return in->args[0];
    return -1;
}

/* A call of a Luma function moves the arguments of its `move` parameters
 * (docs/DESIGN.md "Memory"). Writes the moved vregs to out (at most
 * in->nargs) and returns how many. */
static int moved_args(const IrModule *m, const IrInstr *in, int *out) {
    if (in->op != IR_CALL || m->globals[in->global].kind != IRG_FUNC) return 0;
    uint64_t mv = m->globals[in->global].moves;
    int n = 0;
    for (int i = 0; i < in->nargs && i < 64; i++)
        if ((mv >> i & 1) && in->args[i] >= 0) out[n++] = in->args[i];
    return n;
}

/* "Rc::new" for the symbol m.Rc..Point.new, "f" for fn.f */
static void fn_display(const char *sym, char *buf, size_t n) {
    if (strncmp(sym, "fn.", 3) == 0) {
        snprintf(buf, n, "%s", sym + 3);
        return;
    }
    if (strncmp(sym, "m.", 2) == 0) {
        const char *st = sym + 2, *dot = strchr(st, '.'), *last = strrchr(sym, '.');
        if (dot) {
            snprintf(buf, n, "%.*s::%s", (int)(dot - st), st, last + 1);
            return;
        }
    }
    snprintf(buf, n, "%s", sym);
}

static int *mover; /* per vreg: the global of the function a move went to (-1: Rc::new) */

#define DISPOSED (1 << 24) /* state bit: freed by dispose() rather than free() */

/* One step of the must-be-freed analysis. state[v] = line of the free()
 * that freed v on every path (or minus the line of the Rc::new that moved
 * it), or 0; how[v]: which call freed it (0 free, 1 dispose). With report
 * set, the first violation is printed and false returned. */
static bool freed_step(const IrModule *m, const IrFunc *f, const IrInstr *in, int *state, bool report, const char *path) {
    int *ptrs[80];
    int **pp = in->nargs + 2 > 80 ? xmalloc((size_t)(in->nargs + 2) * sizeof *pp) : ptrs;
    int nu = ir_instr_uses((IrInstr *)in, pp);
    bool ok = true;
    bool freeing = is_free_call(m, in);
    for (int i = 0; i < nu && ok; i++) {
        int v = *pp[i];
        if (v < 0 || state[v] == 0) continue;
        char nb[256];
        const char *name = var_name(f, v, nb, sizeof nb);
        if (!name) continue;
        if (report) {
            char at[64];
            if (in->col > 0) snprintf(at, sizeof at, "%d:%d", in->line, in->col);
            else snprintf(at, sizeof at, "%d", in->line);
            if (state[v] < 0 && mover[v] >= 0) {
                char fd[300];
                fn_display(m->globals[mover[v]].name, fd, sizeof fd);
                fprintf(stderr, "%s:%s: error: '%s' is %s after it was moved into %s() on line %d\n", path, at, name,
                        freeing ? "freed" : "used", fd, -state[v]);
            } else if (state[v] < 0)
                fprintf(stderr, "%s:%s: error: '%s' was moved into an Rc on line %d; use the Rc instead%s\n", path, at,
                        name, -state[v], freeing ? " (the Rc frees it)" : "");
            else if (freeing)
                fprintf(stderr, "%s:%s: error: '%s' is freed twice (%s(%s) on line %d already freed it)\n", path, at,
                        name, state[v] & DISPOSED ? "dispose" : "free", name, state[v] & ~DISPOSED);
            else
                fprintf(stderr, "%s:%s: error: '%s' is used after %s(%s) on line %d\n", path, at, name,
                        state[v] & DISPOSED ? "dispose" : "free", name, state[v] & ~DISPOSED);
        }
        ok = false;
    }
    if (pp != ptrs) free(pp);
    if (in->dst >= 0) state[in->dst] = 0;
    if (freeing && in->args[0] >= 0)
        state[in->args[0]] = (in->line > 0 ? in->line : 1) | (strcmp(free_word(m, in), "dispose") == 0 ? DISPOSED : 0);
    int mv = moved_into_rc(m, in);
    if (mv >= 0) {
        state[mv] = -(in->line > 0 ? in->line : 1);
        mover[mv] = -1;
    }
    int movedv[64];
    int nm = in->nargs <= 64 ? moved_args(m, in, movedv) : 0;
    for (int i = 0; i < nm; i++) {
        state[movedv[i]] = -(in->line > 0 ? in->line : 1);
        mover[movedv[i]] = in->global;
    }
    return ok || !report;
}

static bool check_func_frees(const IrModule *m, const IrFunc *f, const char *path) {
    int nb = f->nblocks, nv = f->nvregs;
    if (nb == 0 || nv == 0) return true;
    bool any_free = false;
    for (int b = 0; b < nb && !any_free; b++)
        for (int k = 0; k < f->blocks[b].n; k++)
            if (is_free_call(m, &f->blocks[b].instrs[k]) || moved_into_rc(m, &f->blocks[b].instrs[k]) >= 0 ||
                (f->blocks[b].instrs[k].op == IR_CALL && m->globals[f->blocks[b].instrs[k].global].kind == IRG_FUNC &&
                 m->globals[f->blocks[b].instrs[k].global].moves))
                any_free = true;
    if (!any_free) return true;
    int *mover_saved = mover;
    mover = xmalloc((size_t)nv * sizeof *mover);
    for (int v = 0; v < nv; v++) mover[v] = -1;
    Cfg cfg;
    cfg_build(f, &cfg);
    int *out = xcalloc((size_t)nb * (size_t)nv, sizeof *out);
    bool *done = xcalloc((size_t)nb, sizeof *done);
    int *st = xmalloc((size_t)nv * sizeof *st);
    /* must-analysis: meet = intersection over the predecessors seen so far;
     * states only shrink, so iterating in reverse postorder converges */
    for (bool changed = true; changed;) {
        changed = false;
        for (int r = 0; r < cfg.nrpo; r++) {
            int b = cfg.rpo[r];
            bool first = true;
            for (int v = 0; v < nv; v++) st[v] = 0;
            for (int p = 0; p < cfg.npred[b]; p++) {
                int pb = cfg.preds[b][p];
                if (!done[pb]) continue;
                for (int v = 0; v < nv; v++) {
                    int x = out[(size_t)pb * nv + v];
                    st[v] = first ? x : (st[v] && x && (st[v] > 0) == (x > 0) ? st[v] : 0);
                }
                first = false;
            }
            for (int k = 0; k < f->blocks[b].n; k++) freed_step(m, f, &f->blocks[b].instrs[k], st, false, path);
            if (!done[b] || memcmp(st, &out[(size_t)b * nv], (size_t)nv * sizeof *st) != 0) {
                memcpy(&out[(size_t)b * nv], st, (size_t)nv * sizeof *st);
                done[b] = true;
                changed = true;
            }
        }
    }
    bool ok = true;
    for (int r = 0; r < cfg.nrpo && ok; r++) {
        int b = cfg.rpo[r];
        bool first = true;
        for (int v = 0; v < nv; v++) st[v] = 0;
        for (int p = 0; p < cfg.npred[b]; p++) {
            int pb = cfg.preds[b][p];
            for (int v = 0; v < nv; v++) {
                int x = out[(size_t)pb * nv + v];
                st[v] = first ? x : (st[v] && x && (st[v] > 0) == (x > 0) ? st[v] : 0);
            }
            first = false;
        }
        for (int k = 0; k < f->blocks[b].n && ok; k++) ok = freed_step(m, f, &f->blocks[b].instrs[k], st, true, path);
    }
    free(out);
    free(done);
    free(st);
    free(mover);
    mover = mover_saved;
    cfg_free(&cfg);
    return ok;
}

bool mem_check_frees(const IrModule *m, const char *path) {
    for (int i = 0; i < m->nfuncs; i++)
        if (!check_func_frees(m, &m->funcs[i], path)) return false;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Escape analysis (--mm=manual)                                            */

static int uf_find(int *p, int x) {
    while (p[x] != x) x = p[x] = p[p[x]];
    return x;
}

/* Runtime functions that only look at their arguments. */
static bool extern_borrows(const char *name) {
    return strcmp(name, "luma_write") == 0 || strcmp(name, "luma_write_space") == 0 ||
           strcmp(name, "luma_write_newline") == 0 || strcmp(name, "luma_print") == 0 ||
           strcmp(name, "luma_refcount") == 0;
}

/* Union-find over f's vregs joined by mov / check (they hold the same
 * objects), and which classes escape given the parameter summaries. */
static void escape_classes(const IrModule *m, const IrFunc *f, bool **pesc, int *parent, bool *esc) {
    int nv = f->nvregs;
    for (int v = 0; v < nv; v++) {
        parent[v] = v;
        esc[v] = false;
    }
    for (int b = 0; b < f->nblocks; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            const IrInstr *in = &f->blocks[b].instrs[k];
            if ((in->op == IR_MOV || in->op == IR_CHECK) && in->dst >= 0 && in->a >= 0) {
                int x = uf_find(parent, in->dst), y = uf_find(parent, in->a);
                if (x != y) parent[x] = y;
            }
        }
#define ESC(v) do { if ((v) >= 0) esc[uf_find(parent, (v))] = true; } while (0)
    for (int b = 0; b < f->nblocks; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            const IrInstr *in = &f->blocks[b].instrs[k];
            switch (in->op) {
            case IR_STORE: case IR_RET: ESC(in->a); break;
            case IR_SETFIELD: ESC(in->b); break;
            case IR_NEW: for (int i = 0; i < in->nargs; i++) ESC(in->args[i]); break;
            case IR_CALLM:
                /* it can only reach a method of that name and arity in some
                 * struct's table (anything else is a runtime error) */
                for (int s = 0; s < m->nstructs; s++) {
                    const IrStruct *sd = &m->structs[s];
                    int k2 = ir_struct_method(sd, in->name);
                    if (k2 < 0) continue;
                    const IrGlobal *g = &m->globals[sd->mglobals[k2]];
                    if (g->kind != IRG_FUNC || g->arity != in->nargs + 1) continue;
                    if (pesc[g->func][0]) ESC(in->a);
                    for (int i = 0; i < in->nargs; i++)
                        if (pesc[g->func][i + 1]) ESC(in->args[i]);
                }
                break;
            case IR_CALL: {
                const IrGlobal *g = &m->globals[in->global];
                if (g->kind == IRG_CEXTERN) break; /* converted to C values; nothing is kept */
                for (int i = 0; i < in->nargs; i++) {
                    bool e = g->kind == IRG_FUNC ? pesc[g->func][i] : !extern_borrows(g->name); /* luma_free: escapes */
                    if (e) ESC(in->args[i]);
                }
                break;
            }
            default: break;
            }
        }
#undef ESC
    for (int v = 0; v < nv; v++)
        if (esc[v]) esc[uf_find(parent, v)] = true;
}

/* Marks (imm = 1) the `new` instructions whose instances are reference
 * counted, and reports which structs ever have counted instances. */
static void choose_counted(IrModule *m, bool *sid_counted) {
    if (m->mm == IR_MM_NONE) {
        for (int s = 0; s < m->nstructs; s++) sid_counted[s] = false;
        return;
    }
    /* Rc boxes and arena handles are always counted (Rc::new of a dynamic
     * value creates boxes in the runtime, where no `new` shows them) */
    for (int s = 0; s < m->nstructs; s++) sid_counted[s] = m->structs[s].kind != IR_STRUCT_USER || m->structs[s].shared;
    bool **pesc = xcalloc((size_t)m->nfuncs + 1, sizeof *pesc);
    for (int i = 0; i < m->nfuncs; i++) pesc[i] = xcalloc((size_t)m->funcs[i].nparams + 1, sizeof **pesc);
    int maxv = 1;
    for (int i = 0; i < m->nfuncs; i++)
        if (m->funcs[i].nvregs > maxv) maxv = m->funcs[i].nvregs;
    int *parent = xmalloc((size_t)maxv * sizeof *parent);
    bool *esc = xmalloc((size_t)maxv * sizeof *esc);
    if (m->mm == IR_MM_MANUAL) {
        /* parameter summaries: least fixpoint (a parameter escapes if a value
         * of its class escapes) */
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = 0; i < m->nfuncs; i++) {
                const IrFunc *f = &m->funcs[i];
                escape_classes(m, f, pesc, parent, esc);
                for (int p = 0; p < f->nparams; p++)
                    if (!pesc[i][p] && esc[uf_find(parent, p)]) pesc[i][p] = changed = true;
            }
        }
    }
    for (int i = 0; i < m->nfuncs; i++) {
        IrFunc *f = &m->funcs[i];
        if (m->mm == IR_MM_MANUAL) escape_classes(m, f, pesc, parent, esc);
        for (int b = 0; b < f->nblocks; b++)
            for (int k = 0; k < f->blocks[b].n; k++) {
                IrInstr *in = &f->blocks[b].instrs[k];
                if (in->op != IR_NEW) continue;
                /* Rc boxes and arena handles are always counted: their lifetime
                 * is the point; user structs when they never escape */
                in->imm = m->structs[in->sid].kind != IR_STRUCT_USER || m->structs[in->sid].shared ||
                          (in->dst >= 0 && !esc[uf_find(parent, in->dst)]);
                if (in->imm) sid_counted[in->sid] = true;
            }
    }
    for (int i = 0; i < m->nfuncs; i++) free(pesc[i]);
    free(pesc);
    free(parent);
    free(esc);
}

/* ---------------------------------------------------------------------- */
/* Ownership: retain / release insertion                                    */

typedef struct {
    const IrModule *m;
    const bool *sid_counted;
    bool any_counted; /* some struct has counted instances */
} Mem;

/* May a value of type t be a counted object? */
static bool heap_ty(const Mem *mm, IrTy t) {
    if (t & TY_STR) return true;
    if (!(t & TY_STRUCT)) return false;
    int sid = ty_sid(t);
    return sid >= 0 ? mm->sid_counted[sid] : mm->any_counted;
}

typedef struct {
    int v, consume, borrow;
} UseCount;

static void add_use(UseCount *u, int *n, int v, bool consume) {
    if (v < 0) return;
    for (int i = 0; i < *n; i++)
        if (u[i].v == v) {
            if (consume) u[i].consume++;
            else u[i].borrow++;
            return;
        }
    u[*n] = (UseCount){v, consume ? 1 : 0, consume ? 0 : 1};
    (*n)++;
}

/* Distinct operands of `in` with how many times each is consumed / borrowed.
 * `u` needs room for nargs + 2 entries. */
static int collect_uses(const IrInstr *in, UseCount *u) {
    int n = 0;
    switch (in->op) {
    case IR_MOV: case IR_CHECK: case IR_STORE: case IR_RET: add_use(u, &n, in->a, true); break;
    case IR_SETFIELD:
        add_use(u, &n, in->a, false);
        add_use(u, &n, in->b, true);
        break;
    case IR_NEW:
        for (int i = 0; i < in->nargs; i++) add_use(u, &n, in->args[i], true);
        break;
    default: {
        int **pp = xmalloc((size_t)(in->nargs + 2) * sizeof *pp);
        int nu = ir_instr_uses((IrInstr *)in, pp);
        for (int i = 0; i < nu; i++) add_use(u, &n, *pp[i], false);
        free(pp);
        break;
    }
    }
    return n;
}

/* load and a static getfield return the slot's own reference; a dynamic
 * getfield (the runtime, which may go through deref) returns a new one. */
static bool result_borrowed(const IrInstr *in) { return in->op == IR_LOAD || (in->op == IR_GETFIELD && in->sid >= 0); }

/* Operations that fail (a runtime error) unless every operand is a fixnum:
 * after them an operand that dies needs no release. */
static bool int_only_op(IrOp op) {
    switch (op) {
    case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD: case IR_NEG:
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: return true;
    default: return false;
    }
}

/* Runtime functions that return nil. */
static bool extern_returns_nil(const char *name) {
    return (extern_borrows(name) && strcmp(name, "luma_refcount") != 0) || strcmp(name, "luma_free") == 0 ||
           strcmp(name, "luma_dispose") == 0;
}

/* May a call whose result is discarded (no dst) still return a counted
 * object, which then has to be released? */
static bool dropped_result_heap(const Mem *mm, const IrInstr *in) {
    if (in->dst >= 0) return false;
    if (in->op == IR_CALLM) return heap_ty(mm, TY_ANY);
    if (in->op != IR_CALL) return false;
    const IrGlobal *g = &mm->m->globals[in->global];
    switch (g->kind) {
    case IRG_FUNC: return heap_ty(mm, g->ty);
    case IRG_CEXTERN: return heap_ty(mm, ir_ctype_ty(g->cret));
    default: return !extern_returns_nil(g->name) && heap_ty(mm, TY_ANY);
    }
}

static IrInstr rc_instr(IrOp op, int v, int line) {
    IrInstr in;
    memset(&in, 0, sizeof in);
    in.op = op;
    in.dst = IR_NONE;
    in.a = v;
    in.b = IR_NONE;
    in.global = IR_NONE;
    in.sid = IR_NONE;
    in.target[0] = in.target[1] = IR_NONE;
    in.line = line;
    return in;
}

/* A growable instruction list. */
typedef struct {
    IrInstr *items;
    int n, cap;
} IList;

static void ilist_push(IList *l, IrInstr in) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = xrealloc(l->items, (size_t)l->cap * sizeof *l->items);
    }
    l->items[l->n++] = in;
}

typedef struct {
    int block, succ; /* the edge block -> succ (target index t of its br) */
    int t;
    int *vregs, n;
} EdgeRel;

/* The compare a block's br tests, when the backend may fuse the two (only
 * copies and constants in between, operands not reassigned): its operands
 * are then kept alive until the br, so no release separates them. */
static int fusable_compare(const IrBlock *bl) {
    if (bl->n < 2 || bl->instrs[bl->n - 1].op != IR_BR) return -1;
    int c = bl->instrs[bl->n - 1].a;
    int j = bl->n - 2;
    while (j >= 0 && bl->instrs[j].dst != c &&
           (bl->instrs[j].op == IR_MOV || bl->instrs[j].op == IR_KEEP || bl->instrs[j].op == IR_CONST_INT || bl->instrs[j].op == IR_CONST_NIL ||
            bl->instrs[j].op == IR_CONST_TRUE || bl->instrs[j].op == IR_CONST_FALSE || bl->instrs[j].op == IR_CONST_DATA))
        j--;
    if (j < 0 || bl->instrs[j].dst != c) return -1;
    const IrInstr *in = &bl->instrs[j];
    switch (in->op) {
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: case IR_NOT: break;
    default: return -1;
    }
    for (int q = j + 1; q < bl->n - 1; q++) {
        int d = bl->instrs[q].dst;
        if (d >= 0 && (d == in->a || d == in->b)) return -1;
    }
    if (in->a == c || in->b == c) return -1;
    return j;
}

/* Can executing `in` free an object (or run Luma code that might)? Runtime
 * operations only allocate; C functions never see Luma objects. */
static bool may_free(const IrModule *m, const IrInstr *in) {
    switch (in->op) {
    case IR_STORE: case IR_SETFIELD: case IR_CALLM: case IR_RETAIN: case IR_RELEASE: return true;
    case IR_GETFIELD: return in->sid < 0; /* the runtime may call deref */
    case IR_CALL: {
        const IrGlobal *g = &m->globals[in->global];
        if (g->kind == IRG_CEXTERN) return false;
        if (g->kind == IRG_FUNC) return true;
        static const char *const SAFE[] = {"luma_write", "luma_write_space", "luma_write_newline", "luma_print", "luma_refcount",
                                           "luma_rc_count", "luma_rc_get", "luma_rc_new"};
        for (size_t i = 0; i < sizeof SAFE / sizeof *SAFE; i++)
            if (strcmp(g->name, SAFE[i]) == 0) return false;
        return true;
    }
    default: return false;
    }
}

/* Borrowed reads: `%d = load @g` / `%d = getfield %o, ...` whose value is
 * used only in the same block, only by borrowing uses, with nothing in
 * between that could free an object, needs no retain/release: the slot
 * keeps the value alive. (For getfield, %o must outlive the uses.) */
static void find_borrowed_reads(const IrModule *m, const IrFunc *f, const Liveness *lv, const bool *managed,
                                bool *borrowed) {
    int nv = f->nvregs;
    int *ndefs = xcalloc((size_t)nv + 1, sizeof *ndefs);
    for (int b = 0; b < f->nblocks; b++)
        for (int k = 0; k < f->blocks[b].n; k++)
            if (f->blocks[b].instrs[k].dst >= 0) ndefs[f->blocks[b].instrs[k].dst]++;
    UseCount *uc = NULL;
    int ucap = 0;
    /* base[v]: for a borrowed getfield, the object it was read from */
    int *base = xmalloc(((size_t)nv + 1) * sizeof *base);
    for (int v = 0; v <= nv; v++) base[v] = -1;
    for (int b = 0; b < f->nblocks; b++) {
        const IrBlock *bl = &f->blocks[b];
        for (int k = 0; k < bl->n; k++) {
            const IrInstr *in = &bl->instrs[k];
            int d = in->dst;
            if (!result_borrowed(in) || d < 0 || !managed[d] || borrowed[d] || ndefs[d] != 1 ||
                bit_get(live_out(lv, b), d))
                continue;
            int o = in->op == IR_GETFIELD ? in->a : -1;
            /* a borrowed object is only as alive as what it was read from:
             * walk up to the owning reference, which must outlive our uses */
            while (o >= 0 && borrowed[o] && base[o] >= 0) o = base[o];
            int last = -1;
            bool ok = true;
            for (int j = k + 1; j < bl->n && ok; j++) {
                const IrInstr *x = &bl->instrs[j];
                if (x->nargs + 2 > ucap) {
                    ucap = x->nargs + 2;
                    uc = xrealloc(uc, (size_t)ucap * sizeof *uc);
                }
                int nu = collect_uses(x, uc);
                for (int i = 0; i < nu; i++)
                    if (uc[i].v == d) {
                        if (uc[i].consume) ok = false;
                        last = j;
                    }
                if (x->dst == d || (o >= 0 && x->dst == o)) ok = false;
            }
            if (!ok || last < 0) continue;
            for (int j = k + 1; j <= last && ok; j++) ok = !may_free(m, &bl->instrs[j]);
            if (ok && o >= 0 && !borrowed[o]) { /* the object must still be held after the last use */
                bool held = bit_get(live_out(lv, b), o);
                for (int j = last + 1; j < bl->n && !held; j++) {
                    const IrInstr *x = &bl->instrs[j];
                    int nu = collect_uses(x, uc);
                    for (int i = 0; i < nu; i++) held |= uc[i].v == o;
                    if (x->dst == o) break;
                }
                ok = held;
            }
            if (ok) {
                borrowed[d] = true;
                if (in->op == IR_GETFIELD) base[d] = o;
            }
        }
    }
    free(base);
    free(uc);
    free(ndefs);
}

/* Is parameter p's incoming value used? (cfg liveness counts parameters as
 * defined at the top of the entry block, so live_in(0) never has them.) */
static bool param_live_at_entry(const IrFunc *f, const Liveness *lv, int p) {
    const IrBlock *e = &f->blocks[0];
    for (int k = 0; k < e->n; k++) {
        IrInstr *in = &e->instrs[k];
        int **pp = xmalloc((size_t)(in->nargs + 2) * sizeof *pp);
        int nu = ir_instr_uses(in, pp);
        bool used = false;
        for (int i = 0; i < nu; i++) used |= *pp[i] == p;
        free(pp);
        if (used) return true;
        if (in->dst == p) return false;
    }
    return bit_get(live_out(lv, 0), p);
}

static void lower_func(Mem *mm, IrFunc *f) {
    const IrModule *m = mm->m;
    opt_remove_unreachable(f);
    int nb = f->nblocks, nv0 = f->nvregs;
    if (nb == 0) return;

    /* which vregs may hold a counted object */
    IrTypes types;
    ir_types_compute(m, f, &types);
    bool *managed = xcalloc((size_t)nv0 + 1, sizeof *managed);
    const IrGlobal *fg = &m->globals[f->global];
    for (int p = 0; p < f->nparams; p++) managed[p] = heap_ty(mm, ir_param_ty(fg, p));
    IrTy *st = xmalloc(((size_t)nv0 + 1) * sizeof *st);
    for (bool changed = true; changed;) {
        changed = false;
        for (int b = 0; b < nb; b++) {
            memcpy(st, &types.in[(size_t)b * nv0], (size_t)nv0 * sizeof *st);
            for (int k = 0; k < f->blocks[b].n; k++) {
                const IrInstr *in = &f->blocks[b].instrs[k];
                if (in->dst >= 0 && !managed[in->dst]) {
                    IrTy t = ir_types_result(m, in, st);
                    bool may;
                    switch (in->op) {
                    case IR_CONST_INT: case IR_CONST_NIL: case IR_CONST_TRUE: case IR_CONST_FALSE: case IR_CONST_DATA:
                        may = false; /* immediates and static literals are never counted */
                        break;
                    case IR_MOV: case IR_CHECK: may = managed[in->a] && heap_ty(mm, t); break;
                    default: may = heap_ty(mm, t); break;
                    }
                    if (may) managed[in->dst] = changed = true;
                }
                ir_types_step(m, in, st);
            }
        }
    }
    free(st);
    ir_types_free(&types);

    /* parameters the function never reassigns stay borrowed */
    bool *borrowed = xcalloc((size_t)nv0 + 1, sizeof *borrowed);
    for (int p = 0; p < f->nparams; p++) borrowed[p] = true;
    for (int b = 0; b < nb; b++)
        for (int k = 0; k < f->blocks[b].n; k++) {
            int d = f->blocks[b].instrs[k].dst;
            if (d >= 0 && d < f->nparams) borrowed[d] = false;
        }
    /* ... and so do copies of borrowed values (mov / check) that are only
     * ever defined that way and never handed on (no consuming use): they
     * live no longer than the call that lent the original */
    {
        bool *cand = xcalloc((size_t)nv0 + 1, sizeof *cand);
        for (int v = f->nparams; v < nv0; v++) cand[v] = true;
        for (int b = 0; b < nb; b++)
            for (int k = 0; k < f->blocks[b].n; k++) {
                const IrInstr *in = &f->blocks[b].instrs[k];
                UseCount u[2];
                if (in->dst >= f->nparams && in->op != IR_MOV && in->op != IR_CHECK) cand[in->dst] = false;
                if (in->op == IR_STORE || in->op == IR_RET || in->op == IR_SETFIELD || in->op == IR_NEW) {
                    int n = in->op == IR_NEW ? 0 : collect_uses(in, u);
                    for (int i = 0; i < n; i++)
                        if (u[i].consume) cand[u[i].v] = false;
                    for (int i = 0; in->op == IR_NEW && i < in->nargs; i++) cand[in->args[i]] = false;
                }
            }
        for (bool changed = true; changed;) {
            changed = false;
            for (int b = 0; b < nb; b++)
                for (int k = 0; k < f->blocks[b].n; k++) {
                    const IrInstr *in = &f->blocks[b].instrs[k];
                    bool src_borrowed = in->a >= 0 && (borrowed[in->a] || (in->a >= f->nparams && cand[in->a]));
                    if ((in->op == IR_MOV || in->op == IR_CHECK) && in->dst >= f->nparams && cand[in->dst] && !src_borrowed) {
                        cand[in->dst] = false; /* a copy of an owned value owns its own reference */
                        changed = true;
                    }
                    /* copying a candidate into an owned vreg consumes it: it must own */
                    if ((in->op == IR_MOV || in->op == IR_CHECK) && in->a >= f->nparams && cand[in->a] &&
                        in->dst >= 0 && !cand[in->dst] && !borrowed[in->dst]) {
                        cand[in->a] = false;
                        changed = true;
                    }
                }
        }
        for (int v = f->nparams; v < nv0; v++)
            if (cand[v]) borrowed[v] = true;
        free(cand);
    }

    Cfg cfg;
    cfg_build(f, &cfg);
    Liveness lv;
    liveness_compute(f, &cfg, &lv);
    int nw = lv.nwords;
    Word *live = xmalloc((size_t)(nw ? nw : 1) * sizeof *live);
    bool *entry_live = xcalloc((size_t)f->nparams + 1, sizeof *entry_live);
    for (int p = 0; p < f->nparams; p++) entry_live[p] = param_live_at_entry(f, &lv, p);
    find_borrowed_reads(m, f, &lv, managed, borrowed);

    EdgeRel *edges = NULL;
    int nedges = 0;
    UseCount *uc = NULL;
    int ucap = 0;
    IList pre = {0}, post = {0};

    for (int b = 0; b < nb; b++) {
        IrBlock *bl = &f->blocks[b];
        if (bl->n == 0) continue;
        IList out = {0}; /* reversed */
        memcpy(live, live_out(&lv, b), (size_t)nw * sizeof *live);
        int fj = fusable_compare(bl);
        IrInstr term = bl->instrs[bl->n - 1];
        int line = term.line;
        if (term.op == IR_BR) {
            if (term.a >= 0) bit_set(live, term.a);
            if (fj >= 0 && !int_only_op(bl->instrs[fj].op)) { /* (an ordered compare's operands are ints after it) */
                const IrInstr *c = &bl->instrs[fj];
                if (c->a >= 0) bit_set(live, c->a);
                if (c->b >= 0) bit_set(live, c->b);
            }
            for (int t = 0; t < 2; t++) {
                int s = term.target[t];
                if (t == 1 && s == term.target[0]) break; /* one edge */
                const Word *in_s = live_in(&lv, s);
                int *vs = NULL, n = 0;
                for (int v = 0; v < nv0; v++)
                    if (bit_get(live, v) && !bit_get(in_s, v) && managed[v] && !borrowed[v]) {
                        vs = xrealloc(vs, (size_t)(n + 1) * sizeof *vs);
                        vs[n++] = v;
                    }
                if (n) {
                    edges = xrealloc(edges, (size_t)(nedges + 1) * sizeof *edges);
                    edges[nedges++] = (EdgeRel){b, s, t, vs, n};
                }
            }
            ilist_push(&out, term);
        } else if (term.op == IR_RET) {
            ilist_push(&out, term);
            if (term.a >= 0 && managed[term.a] && borrowed[term.a]) ilist_push(&out, rc_instr(IR_RETAIN, term.a, line));
            for (int w = 0; w < nw; w++) live[w] = 0;
            if (term.a >= 0) bit_set(live, term.a);
        } else {
            ilist_push(&out, term);
        }

        for (int k = bl->n - 2; k >= 0; k--) {
            IrInstr in = bl->instrs[k];
            line = in.line;
            pre.n = post.n = 0;
            int d = in.dst;
            if (in.nargs + 2 > ucap) {
                ucap = in.nargs + 2;
                uc = xrealloc(uc, (size_t)ucap * sizeof *uc);
            }
            int nu = collect_uses(&in, uc);
            bool copy_borrowed = (in.op == IR_MOV || in.op == IR_CHECK) && d >= 0 && d < nv0 && borrowed[d];
            if (copy_borrowed) /* a borrowed copy takes no reference of its own */
                for (int i = 0; i < nu; i++) {
                    uc[i].borrow += uc[i].consume;
                    uc[i].consume = 0;
                }
            if (dropped_result_heap(mm, &in)) { /* give the result a vreg, and release it */
                in.dst = opt_fresh_vreg(f, "drop");
                ilist_push(&post, rc_instr(IR_RELEASE, in.dst, line));
            }
            /* the result */
            if (d >= 0 && managed[d] && !borrowed[d]) {
                if (result_borrowed(&in)) {
                    if (bit_get(live, d)) ilist_push(&post, rc_instr(IR_RETAIN, d, line));
                } else if (!bit_get(live, d)) {
                    ilist_push(&post, rc_instr(IR_RELEASE, d, line));
                }
            }
            /* the operands */
            for (int i = 0; i < nu; i++) {
                int v = uc[i].v;
                if (!managed[v]) continue;
                bool dies = v == d || !bit_get(live, v);
                if (k == fj && !int_only_op(in.op)) dies = false; /* released on the br's edges instead */
                if (int_only_op(in.op) && uc[i].consume == 0) dies = false; /* a fixnum if we get past it */
                int retains = uc[i].consume;
                if (!borrowed[v] && dies) {
                    if (uc[i].consume > 0) {
                        retains--; /* the last reference moves into the instruction */
                    } else if (v == d) {
                        /* `%v = op %v, ...` with v's old value dying: keep it in a
                         * temporary so it can be released after v is overwritten */
                        int t = opt_fresh_vreg(f, f->vregs[v]);
                        IrInstr mv = rc_instr(IR_MOV, v, line);
                        mv.dst = t;
                        ilist_push(&pre, mv);
                        int **pp = xmalloc((size_t)(in.nargs + 2) * sizeof *pp);
                        int npp = ir_instr_uses(&in, pp);
                        for (int q = 0; q < npp; q++)
                            if (*pp[q] == v) *pp[q] = t;
                        free(pp);
                        ilist_push(&post, rc_instr(IR_RELEASE, t, line));
                    } else {
                        ilist_push(&post, rc_instr(IR_RELEASE, v, line));
                    }
                }
                for (int r = 0; r < retains; r++) ilist_push(&pre, rc_instr(IR_RETAIN, v, line));
            }
            /* overwriting a slot that may hold a counted value releases it */
            if (in.op == IR_STORE) in.imm = heap_ty(mm, m->globals[in.global].ty);
            if (in.op == IR_SETFIELD && in.sid >= 0) in.imm = heap_ty(mm, m->structs[in.sid].ftys[in.field]);
            /* emit (reversed): post in reverse order, the instruction, pre in reverse */
            for (int i = post.n - 1; i >= 0; i--) ilist_push(&out, post.items[i]);
            ilist_push(&out, in);
            for (int i = pre.n - 1; i >= 0; i--) ilist_push(&out, pre.items[i]);
            /* live before = (live after - dst) + operands */
            if (d >= 0 && d < nv0) bit_clear(live, d);
            for (int i = 0; i < nu; i++)
                if (uc[i].v < nv0) bit_set(live, uc[i].v);
        }
        /* install the rebuilt block (instructions moved, not copied) */
        free(bl->instrs);
        bl->instrs = xmalloc((size_t)(out.n ? out.n : 1) * sizeof *bl->instrs);
        for (int i = 0; i < out.n; i++) bl->instrs[i] = out.items[out.n - 1 - i];
        bl->n = bl->cap = out.n;
        free(out.items);
    }

    /* releases on CFG edges */
    for (int e = 0; e < nedges; e++) {
        EdgeRel *er = &edges[e];
        int target = er->succ;
        if (cfg.npred[target] == 1 && target != 0) { /* (the entry block is also entered from outside) */
            for (int i = er->n - 1; i >= 0; i--) opt_insert(f, target, 0, rc_instr(IR_RELEASE, er->vregs[i], 0));
        } else {
            int nbk = opt_insert_block(f, f->nblocks, "rel");
            for (int i = 0; i < er->n; i++) opt_append(f, nbk, rc_instr(IR_RELEASE, er->vregs[i], 0));
            IrInstr j = rc_instr(IR_JMP, IR_NONE, 0);
            j.a = IR_NONE;
            j.target[0] = target;
            opt_append(f, nbk, j);
            f->blocks[er->block].instrs[f->blocks[er->block].n - 1].target[er->t] = nbk;
        }
        free(er->vregs);
    }
    free(edges);

    /* reassigned parameters own their value from the start (decided on the
     * entry block as it was before the walk) */
    int nretain = 0;
    for (int p = 0; p < f->nparams; p++)
        if (managed[p] && !borrowed[p] && entry_live[p]) nretain++;
    if (nretain) {
        int entry = 0;
        if (cfg.npred[0] > 0) { /* the entry block is a loop header: add a preamble */
            int nbk = opt_insert_block(f, 0, "entry.mem");
            IrInstr j = rc_instr(IR_JMP, IR_NONE, 0);
            j.a = IR_NONE;
            j.target[0] = 1;
            opt_append(f, nbk, j);
            entry = nbk;
        }
        for (int p = f->nparams - 1; p >= 0; p--)
            if (managed[p] && !borrowed[p] && entry_live[p])
                opt_insert(f, entry, 0, rc_instr(IR_RETAIN, p, f->line));
    }

    free(pre.items);
    free(post.items);
    free(uc);
    free(live);
    free(managed);
    free(borrowed);
    free(entry_live);
    liveness_free(&lv);
    cfg_free(&cfg);
}

void mem_lower(IrModule *m) {
    m->mem_lowered = true;
    if (m->mm == IR_MM_NONE) return;
    bool *sid_counted = xcalloc((size_t)m->nstructs + 1, sizeof *sid_counted);
    choose_counted(m, sid_counted);
    Mem mm = {m, sid_counted, false};
    for (int s = 0; s < m->nstructs; s++) mm.any_counted |= sid_counted[s];
    for (int i = 0; i < m->nfuncs; i++) lower_func(&mm, &m->funcs[i]);
    free(sid_counted);
}
