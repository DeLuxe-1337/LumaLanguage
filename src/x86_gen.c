/* x86_gen.c - the optimizing x86-64 backend (see x86_gen.h, docs/DESIGN.md).
 *
 * Per function:
 *   1. analysis: CFG, liveness, flow-sensitive types (ir_types.c);
 *   2. instruction numbering in layout order (block order, unreachable
 *      blocks dropped). Instruction i reads its operands at position 2i and
 *      writes its destination at 2i+1;
 *   3. selection decisions that affect liveness: compare+branch fusion and
 *      tail calls;
 *   4. one live interval per vreg (a conservative [start, end] hull of its
 *      liveness), rematerialization of single-definition constants;
 *   5. linear-scan register allocation. Intervals live across a call get
 *      callee-saved registers; spills get their own frame slot;
 *   6. emission into a line list, out-of-line stubs appended after the body,
 *      then a peephole pass over the lines.
 *
 * Registers: allocatable caller-saved rax rcx rdx rsi rdi r8 r9, callee-saved
 * rbx r12-r15; r10 and r11 are scratch (never allocated); rbp is the frame
 * pointer when the function needs a frame.
 *
 * Fixnums are tagged (n << 1) | 1, so with a = 2x+1, b = 2y+1:
 *   a + b - 1 = 2(x+y)+1          add: (a - 1) + b, jo -> overflow
 *   a - (b - 1) = 2(x-y)+1        sub
 *   (a - 1) * (b >> 1) | 1        mul
 *   -a + 2 = 2(-x)+1              neg
 * and the 64-bit overflow flag is exactly "result outside the 63-bit fixnum
 * range". Tagged fixnums compare like the integers they represent. */
#include "x86_gen.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "value.h"
#include "x86_isel.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, NREGS };

static const char *const R64[NREGS] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                       "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *const R8B[NREGS] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                       "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
static const char *const R32[NREGS] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                       "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const int ARGREG[6] = {RDI, RSI, RDX, RCX, R8, R9};
/* allocation preference: caller-saved first (no save/restore cost) */
static const int ALLOC_ORDER[] = {RAX, RCX, RDX, RSI, RDI, R8, R9, RBX, R12, R13, R14, R15};
#define NALLOC 12
#define NCALLER 7

static bool callee_saved(int r) { return r == RBX || r >= R12; }
static bool allocatable(int r) { return r >= 0 && r != RSP && r != RBP && r != R10 && r != R11; }
static bool fits32(int64_t w) { return w >= INT32_MIN && w <= INT32_MAX; }

/* ------------------------------------------------------------------------ */
/* Locations and output lines                                               */

typedef enum { LOC_NONE, LOC_REG, LOC_STACK, LOC_IMM, LOC_DATA, LOC_SYM } LocKind;

typedef struct {
    LocKind kind;
    int reg;     /* LOC_REG */
    int slot;    /* LOC_STACK */
    int64_t imm; /* LOC_IMM: the tagged word */
    int global;  /* LOC_DATA */
    const char *sym; /* LOC_SYM: address of a local label (lea) */
} Loc;

static Loc loc_reg(int r) { return (Loc){.kind = LOC_REG, .reg = r}; }
static Loc loc_imm(int64_t w) { return (Loc){.kind = LOC_IMM, .imm = w}; }

typedef struct {
    char **v;
    int n, cap;
} Lines;

static void lines_push(Lines *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
}

static void lines_free(Lines *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* module-wide state */
static int nlabel;      /* .Lk<N> local labels */
static Buf ffi_data;    /* .rodata context strings of FFI calls */
static int ffi_labels;

typedef struct {
    const IrModule *m;
    const IrFunc *f;
    const char *name;
    Cfg cfg;
    Liveness lv;
    IrTypes types;
    int *layout, nlayout; /* reachable blocks in emission order */
    int *layout_pos;      /* block -> index in layout, or -1 */
    int *first;           /* block -> index of its first instruction */
    int ninstr;
    const IrInstr **ins;  /* by index */
    IrTy *tya, *tyb;      /* operand types before each instruction */
    bool *fused;          /* compare emitted as part of a later br of its block */
    int *fuse_cmp;        /* br index -> index of its fused compare, or -1 */
    int *fuse_br;         /* compare index -> index of the br it is fused into, or -1 */
    bool *tail;           /* call emitted as a tail jump (the ret after it is skipped) */
    bool *skip;
    Loc *loc;             /* per vreg */
    int *start, *end;     /* live interval hull per vreg (start > end: none) */
    bool *cross;          /* live across a call */
    bool *across_div;     /* live across an idiv (which clobbers rax and rdx) */
    int *hreg, *hvreg;    /* allocation hints: register / copy-related vreg */
    int nslots;           /* spill slots */
    int ffi_slots;        /* scratch slots for raw FFI arguments (after the spill slots) */
    int cs[5], ncs;       /* callee-saved registers used */
    bool framed;
    int frame_size;       /* bytes reserved below the callee-saved pushes */
    Lines body, stubs, *cur;
    int ovf, divz;        /* labels of the noreturn error stubs, -1 until used */
    int *undef;           /* per global: undefined-variable stub label, or -1 */
} G;

static void E(G *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void E(G *g, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    char *s;
    if (n < 0) n = 0;
    if ((size_t)n < sizeof tmp) {
        s = xstrdup(tmp);
    } else {
        s = xmalloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(s, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    lines_push(g->cur, s);
}

/* Small rotating text buffers for operand strings used within one E() call. */
static char *tbuf(void) {
    static char ring[16][128];
    static int i;
    return ring[i++ & 15];
}

static int new_label(void) { return nlabel++; }

static const char *blabel(G *g, int b) {
    char *t = tbuf();
    snprintf(t, 128, ".L%s.%s", g->name, g->f->blocks[b].label);
    return t;
}

static int next_block(G *g, int b) {
    int p = g->layout_pos[b];
    return p >= 0 && p + 1 < g->nlayout ? g->layout[p + 1] : -1;
}

/* ------------------------------------------------------------------------ */
/* Operands                                                                 */

static int slot_off(G *g, int slot) { return 8 * g->ncs + 8 * (slot + 1); }

static const char *mem_text(G *g, int slot, const char *size) {
    char *t = tbuf();
    snprintf(t, 128, "%s ptr [rbp - %d]", size, slot_off(g, slot));
    return t;
}

static const char *num_text(int64_t w) {
    char *t = tbuf();
    snprintf(t, 128, "%" PRId64, w);
    return t;
}

static void emit_imm(G *g, int r, int64_t w) {
    if (w >= 0 && w <= (int64_t)0xFFFFFFFF) E(g, "    mov %s, %" PRId64, R32[r], w);
    else E(g, "    mov %s, %" PRId64, R64[r], w);
}

static void load_loc(G *g, int r, Loc L) {
    switch (L.kind) {
    case LOC_REG:
        if (L.reg != r) E(g, "    mov %s, %s", R64[r], R64[L.reg]);
        break;
    case LOC_STACK: E(g, "    mov %s, %s", R64[r], mem_text(g, L.slot, "qword")); break;
    case LOC_IMM: emit_imm(g, r, L.imm); break;
    case LOC_DATA: E(g, "    lea %s, [rip + .Ldata.%s]", R64[r], g->m->globals[L.global].name); break;
    case LOC_SYM: E(g, "    lea %s, [rip + %s]", R64[r], L.sym); break;
    case LOC_NONE: emit_imm(g, r, (int64_t)LUMA_NIL); break; /* never read in practice */
    }
}

/* D <- A (D is a register or a stack slot). Uses r11 for memory-to-memory. */
static void move_loc(G *g, Loc D, Loc A) {
    if (D.kind == LOC_REG) {
        load_loc(g, D.reg, A);
    } else if (D.kind == LOC_STACK) {
        if (A.kind == LOC_REG) E(g, "    mov %s, %s", mem_text(g, D.slot, "qword"), R64[A.reg]);
        else if (A.kind == LOC_IMM && fits32(A.imm)) E(g, "    mov %s, %s", mem_text(g, D.slot, "qword"), num_text(A.imm));
        else if (A.kind == LOC_STACK && A.slot == D.slot) return;
        else {
            load_loc(g, R11, A);
            E(g, "    mov %s, r11", mem_text(g, D.slot, "qword"));
        }
    }
    /* other destinations (rematerialized or dead) are never written */
}

static void store_reg(G *g, int v, int r) {
    if (v == IR_NONE) return;
    move_loc(g, g->loc[v], loc_reg(r));
}

static void move_v(G *g, int d, int a) { move_loc(g, g->loc[d], g->loc[a]); }

/* Register or memory operand text; anything else is loaded into scratch. */
static const char *use_rm(G *g, int v, int scratch) {
    Loc L = g->loc[v];
    if (L.kind == LOC_REG) return R64[L.reg];
    if (L.kind == LOC_STACK) return mem_text(g, L.slot, "qword");
    load_loc(g, scratch, L);
    return R64[scratch];
}

/* Like use_rm, but immediates that fit a sign-extended imm32 stay immediates. */
static const char *use_rmi(G *g, int v, int scratch) {
    Loc L = g->loc[v];
    if (L.kind == LOC_IMM && fits32(L.imm)) return num_text(L.imm);
    return use_rm(g, v, scratch);
}

static int use_r(G *g, int v, int scratch) {
    Loc L = g->loc[v];
    if (L.kind == LOC_REG) return L.reg;
    load_loc(g, scratch, L);
    return scratch;
}

static const char *byte_rm(G *g, int v, int scratch) {
    Loc L = g->loc[v];
    if (L.kind == LOC_REG) return R8B[L.reg];
    if (L.kind == LOC_STACK) return mem_text(g, L.slot, "byte");
    load_loc(g, scratch, L);
    return R8B[scratch];
}

/* Register to compute d's value in: d's own register unless it is `avoid`,
 * else r10 (the caller then stores r10 into d). */
static int dst_reg(G *g, int d, int avoid) {
    if (d != IR_NONE && g->loc[d].kind == LOC_REG && g->loc[d].reg != avoid) return g->loc[d].reg;
    return R10;
}

/* Parallel move into registers / stack slots (destinations are distinct).
 * Cycles among registers are broken through r11. */
static void parallel_move(G *g, const Loc *dst, const Loc *src, int n) {
    /* 1. stack destinations first: they read registers before any is overwritten */
    for (int i = 0; i < n; i++)
        if (dst[i].kind == LOC_STACK) move_loc(g, dst[i], src[i]);
    /* 2. register to register */
    int pd[16], ps[16], np = 0;
    for (int i = 0; i < n; i++)
        if (dst[i].kind == LOC_REG && src[i].kind == LOC_REG && dst[i].reg != src[i].reg) {
            pd[np] = dst[i].reg;
            ps[np] = src[i].reg;
            np++;
        }
    while (np > 0) {
        int j = -1;
        for (int i = 0; i < np && j < 0; i++) {
            bool blocked = false;
            for (int k = 0; k < np; k++)
                if (k != i && ps[k] == pd[i]) blocked = true;
            if (!blocked) j = i;
        }
        if (j < 0) {
            /* every destination is still needed as a source: save one */
            E(g, "    mov r11, %s", R64[pd[0]]);
            for (int k = 0; k < np; k++)
                if (ps[k] == pd[0]) ps[k] = R11;
            continue;
        }
        E(g, "    mov %s, %s", R64[pd[j]], R64[ps[j]]);
        pd[j] = pd[np - 1];
        ps[j] = ps[np - 1];
        np--;
    }
    /* 3. registers from immediates, data addresses and stack slots */
    for (int i = 0; i < n; i++)
        if (dst[i].kind == LOC_REG && src[i].kind != LOC_REG) load_loc(g, dst[i].reg, src[i]);
}

/* Caller-saved registers holding values live across position [lo, hi]
 * (other than `exclude`). */
static int live_caller(G *g, int lo, int hi, int exclude, int *out) {
    int n = 0;
    bool seen[NREGS] = {0};
    for (int v = 0; v < g->f->nvregs; v++) {
        Loc L = g->loc[v];
        if (v == exclude || L.kind != LOC_REG || callee_saved(L.reg) || seen[L.reg]) continue;
        if (g->start[v] <= lo && g->end[v] >= hi) {
            seen[L.reg] = true;
            out[n++] = L.reg;
        }
    }
    return n;
}

static bool reg_live_across(G *g, int idx, int r, int exclude) {
    for (int v = 0; v < g->f->nvregs; v++)
        if (v != exclude && g->loc[v].kind == LOC_REG && g->loc[v].reg == r && g->start[v] <= 2 * idx &&
            g->end[v] >= 2 * idx + 2)
            return true;
    return false;
}

/* Calls a runtime function from the middle of the function body, keeping
 * the live caller-saved registers intact. Returns the register holding the
 * result: rax if nothing had to be restored, else r11. */
static int preserving_call(G *g, const int *saved, int ns, const char *fn, const Loc *args, int na) {
    for (int i = 0; i < ns; i++) E(g, "    push %s", R64[saved[i]]);
    bool pad = ((g->framed ? 0 : 8) + 8 * ns) % 16 != 0;
    if (pad) E(g, "    sub rsp, 8");
    Loc dst[6];
    for (int i = 0; i < na; i++) dst[i] = loc_reg(ARGREG[i]);
    parallel_move(g, dst, args, na);
    E(g, "    call %s@PLT", fn);
    if (!pad && ns == 0) return RAX;
    E(g, "    mov r11, rax");
    if (pad) E(g, "    add rsp, 8");
    for (int i = ns - 1; i >= 0; i--) E(g, "    pop %s", R64[saved[i]]);
    return R11;
}

/* d = runtime_op(a[, b]) via a preserving call (the generic slow path). */
static void call_runtime(G *g, int idx, const IrInstr *in) {
    int saved[NREGS];
    int ns = live_caller(g, 2 * idx, 2 * idx + 2, in->dst, saved);
    Loc args[2];
    int na = 0;
    args[na++] = g->loc[in->a];
    if (in->b != IR_NONE) args[na++] = g->loc[in->b];
    store_reg(g, in->dst, preserving_call(g, saved, ns, ir_op_runtime(in->op), args, na));
}

/* Out-of-line slow path: label `slow`, generic runtime call, back to `cont`. */
static void slow_stub(G *g, int idx, const IrInstr *in, int slow, int cont) {
    Lines *save = g->cur;
    g->cur = &g->stubs;
    E(g, ".Lk%d:", slow);
    call_runtime(g, idx, in);
    E(g, "    jmp .Lk%d", cont);
    g->cur = save;
}

static int ovf_label(G *g) {
    if (g->ovf < 0) g->ovf = new_label();
    return g->ovf;
}

static int divz_label(G *g) {
    if (g->divz < 0) g->divz = new_label();
    return g->divz;
}

static int undef_label(G *g, int global) {
    if (g->undef[global] < 0) g->undef[global] = new_label();
    return g->undef[global];
}

/* Jumps to `slow` unless the needed operands are fixnums. */
static void tag_check(G *g, int a, bool need_a, int b, bool need_b, int slow) {
    if (!need_a && !need_b) return;
    if (need_a && need_b) {
        load_loc(g, R11, g->loc[a]);
        E(g, "    and r11, %s", use_rmi(g, b, R10));
        E(g, "    test r11b, 1");
    } else {
        int v = need_a ? a : b;
        Loc L = g->loc[v];
        if (L.kind == LOC_IMM || L.kind == LOC_DATA) {
            if (L.kind == LOC_DATA || !(L.imm & 1)) E(g, "    jmp .Lk%d", slow);
            return;
        }
        E(g, "    test %s, 1", byte_rm(g, v, R11));
    }
    E(g, "    jz .Lk%d", slow);
}

static bool ty_int_only(IrTy t) { return t != 0 && (t & ~(IrTy)TY_INT) == 0; }

static const char *cc_of(IrOp op) {
    switch (op) {
    case IR_LT: return "l";
    case IR_LE: return "le";
    case IR_GT: return "g";
    case IR_GE: return "ge";
    case IR_EQ: return "e";
    default: return "ne";
    }
}

static const char *cc_swap(const char *cc) {
    if (!strcmp(cc, "l")) return "g";
    if (!strcmp(cc, "g")) return "l";
    if (!strcmp(cc, "le")) return "ge";
    if (!strcmp(cc, "ge")) return "le";
    return cc;
}

static const char *cc_invert(const char *cc) {
    static const char *const pairs[][2] = {{"e", "ne"}, {"l", "ge"}, {"le", "g"}, {"z", "nz"},
                                           {"s", "ns"}, {"o", "no"}, {"a", "be"}, {"b", "ae"}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        if (!strcmp(cc, pairs[i][0])) return pairs[i][1];
        if (!strcmp(cc, pairs[i][1])) return pairs[i][0];
    }
    return NULL;
}

/* Emits `cmp a, b` (operands possibly swapped) and returns the condition
 * code that holds when `a <op> b`. */
static const char *emit_cmp(G *g, int a, int b, IrOp op) {
    const char *cc = cc_of(op);
    Loc A = g->loc[a], B = g->loc[b];
    bool a_rm = A.kind == LOC_REG || A.kind == LOC_STACK;
    bool b_rm = B.kind == LOC_REG || B.kind == LOC_STACK;
    if (!a_rm && b_rm) {
        int t = a;
        a = b;
        b = t;
        cc = cc_swap(cc);
        A = g->loc[a];
        B = g->loc[b];
    }
    const char *at;
    if (A.kind == LOC_REG) at = R64[A.reg];
    else if (A.kind == LOC_STACK && B.kind != LOC_STACK) at = mem_text(g, A.slot, "qword");
    else {
        load_loc(g, R11, A);
        at = "r11";
    }
    const char *bt = use_rmi(g, b, R10);
    E(g, "    cmp %s, %s", at, bt);
    return cc;
}

/* d = (cc holds) ? true : false   -- true = 10, false = 2 */
static void set_bool(G *g, const char *cc, int d) {
    int T = dst_reg(g, d, -1);
    E(g, "    set%s r11b", cc);
    E(g, "    movzx r11d, r11b");
    E(g, "    lea %s, [r11*8 + 2]", R64[T]);
    store_reg(g, d, T);
}

/* Jumps to t when cc holds, else to f, falling through where possible. */
static void emit_branch(G *g, int b, const char *cc, int t, int f) {
    int nx = next_block(g, b);
    if (t == f) {
        if (t != nx) E(g, "    jmp %s", blabel(g, t));
    } else if (t == nx) {
        E(g, "    j%s %s", cc_invert(cc), blabel(g, f));
    } else if (f == nx) {
        E(g, "    j%s %s", cc, blabel(g, t));
    } else {
        E(g, "    j%s %s", cc, blabel(g, t));
        E(g, "    jmp %s", blabel(g, f));
    }
}

static void emit_goto(G *g, int b, int t) {
    if (t != next_block(g, b)) E(g, "    jmp %s", blabel(g, t));
}

/* Emits the truthiness test of v (falsy <=> nil or false) and returns the
 * condition code that holds when v is TRUTHY; or NULL with *always set to
 * the statically known truthiness. */
static const char *truth_test(G *g, int v, IrTy ty, int *always) {
    Loc L = g->loc[v];
    if (L.kind == LOC_IMM) {
        *always = ((uint64_t)L.imm & LUMA_FALSY_MASK) != 2;
        return NULL;
    }
    if (L.kind == LOC_DATA || (ty != 0 && !(ty & (TY_BOOL | TY_NIL)))) {
        *always = 1;
        return NULL;
    }
    if (ty == TY_NIL) {
        *always = 0;
        return NULL;
    }
    if (ty != 0 && !(ty & TY_BOOL)) {
        E(g, "    cmp %s, 6", use_rm(g, v, R11)); /* nil is the only falsy value */
    } else if (ty != 0 && !(ty & TY_NIL)) {
        E(g, "    cmp %s, 2", use_rm(g, v, R11)); /* false is the only falsy value */
    } else {
        load_loc(g, R11, L);
        E(g, "    and r11, -5");
        E(g, "    cmp r11, 2");
    }
    return "ne";
}

static void branch_truthy(G *g, int b, int v, IrTy ty, int t, int f) {
    int always = 0;
    const char *cc = truth_test(g, v, ty, &always);
    if (!cc) emit_goto(g, b, always ? t : f);
    else emit_branch(g, b, cc, t, f);
}

static void epilogue(G *g) {
    if (!g->framed) return;
    if (g->frame_size) {
        if (g->ncs) E(g, "    lea rsp, [rbp - %d]", 8 * g->ncs);
        else E(g, "    mov rsp, rbp");
    }
    for (int i = g->ncs - 1; i >= 0; i--) E(g, "    pop %s", R64[g->cs[i]]);
    E(g, "    pop rbp");
}

/* ------------------------------------------------------------------------ */
/* FFI                                                                      */

static const char *ctype_suffix(CType t) { return t == CT_CSTR_OPT ? "cstr_opt" : ctype_name(t); }

static int ffi_context(const char *text) {
    int n = ffi_labels++;
    buf_printf(&ffi_data, ".Lffi.%d:\n", n);
    x86_emit_ascii(&ffi_data, text, strlen(text));
    buf_printf(&ffi_data, "    .byte 0\n");
    return n;
}

static void emit_ffi_call(G *g, const IrInstr *in) {
    const IrGlobal *gl = &g->m->globals[in->global];
    int base = g->nslots;
    /* raw values go through scratch slots: the converters clobber caller-saved registers */
    for (int i = 0; i < in->nargs; i++) move_loc(g, (Loc){.kind = LOC_STACK, .slot = base + i}, g->loc[in->args[i]]);
    for (int i = 0; i < in->nargs; i++) {
        char text[600];
        snprintf(text, sizeof text, "argument %d ('%s') of %s", i + 1, gl->cnames[i], gl->name);
        int c = ffi_context(text);
        E(g, "    mov rdi, %s", mem_text(g, base + i, "qword"));
        E(g, "    lea rsi, [rip + .Lffi.%d]", c);
        E(g, "    call luma_ffi_arg_%s@PLT", ctype_suffix(gl->cparams[i]));
        E(g, "    mov %s, rax", mem_text(g, base + i, "qword"));
    }
    for (int i = 0; i < in->nargs; i++) E(g, "    mov %s, %s", R64[ARGREG[i]], mem_text(g, base + i, "qword"));
    E(g, "    xor eax, eax    # no vector registers used (variadic-safe)");
    E(g, "    call %s@PLT", gl->name);
    if (gl->cret == CT_VOID) {
        emit_imm(g, RAX, (int64_t)LUMA_NIL);
    } else {
        char text[600];
        snprintf(text, sizeof text, "return value of %s", gl->name);
        int c = ffi_context(text);
        E(g, "    mov rdi, rax");
        E(g, "    lea rsi, [rip + .Lffi.%d]", c);
        E(g, "    call luma_ffi_ret_%s@PLT", ctype_suffix(gl->cret));
    }
    store_reg(g, in->dst, RAX);
}

/* ------------------------------------------------------------------------ */
/* Instruction selection                                                    */

static int64_t untag(int64_t w) { return w >> 1; }

static void emit_arith(G *g, int idx, const IrInstr *in) {
    IrTy ta = g->tya[idx], tb = g->tyb[idx];
    if (!(ta & TY_INT) || !(tb & TY_INT)) {
        call_runtime(g, idx, in); /* never both fixnums: strings or a type error */
        return;
    }
    bool na = !ty_int_only(ta), nb = !ty_int_only(tb);
    int slow = -1, cont = -1;
    if (na || nb) {
        slow = new_label();
        cont = new_label();
        tag_check(g, in->a, na, in->b, nb, slow);
    }
    int d = in->dst;
    Loc A = g->loc[in->a], B = g->loc[in->b];
    int T;
    switch (in->op) {
    case IR_ADD:
        if (B.kind == LOC_IMM && fits32(B.imm - 1)) {
            T = dst_reg(g, d, -1);
            load_loc(g, T, A);
            E(g, "    add %s, %" PRId64, R64[T], B.imm - 1);
        } else if (A.kind == LOC_IMM && fits32(A.imm - 1)) {
            T = dst_reg(g, d, -1);
            load_loc(g, T, B);
            E(g, "    add %s, %" PRId64, R64[T], A.imm - 1);
        } else {
            int bv = in->b;
            if (B.kind == LOC_REG && d != IR_NONE && g->loc[d].kind == LOC_REG && g->loc[d].reg == B.reg) {
                /* (b - 1) + a: lets d's register (b's, which dies here) take the sum */
                Loc t = A;
                A = B;
                B = t;
                bv = in->a;
            }
            T = dst_reg(g, d, B.kind == LOC_REG ? B.reg : -1);
            if (A.kind == LOC_REG) E(g, "    lea %s, [%s - 1]", R64[T], R64[A.reg]);
            else if (A.kind == LOC_IMM) emit_imm(g, T, A.imm - 1);
            else {
                load_loc(g, T, A);
                E(g, "    sub %s, 1", R64[T]);
            }
            E(g, "    add %s, %s", R64[T], use_rmi(g, bv, R11));
        }
        E(g, "    jo .Lk%d", ovf_label(g));
        break;
    case IR_SUB:
        if (B.kind == LOC_IMM && fits32(B.imm - 1)) {
            T = dst_reg(g, d, -1);
            load_loc(g, T, A);
            E(g, "    sub %s, %" PRId64, R64[T], B.imm - 1);
        } else {
            if (B.kind == LOC_REG) E(g, "    lea r11, [%s - 1]", R64[B.reg]);
            else if (B.kind == LOC_IMM) emit_imm(g, R11, B.imm - 1);
            else {
                load_loc(g, R11, B);
                E(g, "    sub r11, 1");
            }
            T = dst_reg(g, d, -1);
            load_loc(g, T, A);
            E(g, "    sub %s, r11", R64[T]);
        }
        E(g, "    jo .Lk%d", ovf_label(g));
        break;
    default: { /* IR_MUL */
        int other = -1;
        int64_t k = 0;
        if (B.kind == LOC_IMM && fits32(untag(B.imm))) {
            other = in->a;
            k = untag(B.imm);
        } else if (A.kind == LOC_IMM && fits32(untag(A.imm))) {
            other = in->b;
            k = untag(A.imm);
        }
        T = dst_reg(g, d, -1);
        if (other >= 0) {
            Loc O = g->loc[other];
            if (O.kind == LOC_REG) E(g, "    lea %s, [%s - 1]", R64[T], R64[O.reg]);
            else if (O.kind == LOC_IMM) emit_imm(g, T, O.imm - 1);
            else {
                load_loc(g, T, O);
                E(g, "    sub %s, 1", R64[T]);
            }
            E(g, "    imul %s, %s, %" PRId64, R64[T], R64[T], k);
        } else {
            load_loc(g, R11, B);
            E(g, "    sar r11, 1");
            if (A.kind == LOC_REG) E(g, "    lea %s, [%s - 1]", R64[T], R64[A.reg]);
            else if (A.kind == LOC_IMM) emit_imm(g, T, A.imm - 1);
            else {
                load_loc(g, T, A);
                E(g, "    sub %s, 1", R64[T]);
            }
            E(g, "    imul %s, r11", R64[T]);
        }
        E(g, "    jo .Lk%d", ovf_label(g));
        E(g, "    or %s, 1", R64[T]);
        break;
    }
    }
    store_reg(g, d, T);
    if (slow >= 0) {
        E(g, ".Lk%d:", cont);
        slow_stub(g, idx, in, slow, cont);
    }
}

static void emit_divmod(G *g, int idx, const IrInstr *in) {
    IrTy ta = g->tya[idx], tb = g->tyb[idx];
    Loc A = g->loc[in->a], B = g->loc[in->b];
    bool is_div = in->op == IR_DIV;
    bool b_const = B.kind == LOC_IMM && (B.imm & 1);
    int64_t k = b_const ? untag(B.imm) : 0;
    if (!(ta & TY_INT) || !(tb & TY_INT) || (b_const && k == 0)) {
        call_runtime(g, idx, in);
        return;
    }
    bool na = !ty_int_only(ta), nb = !b_const && !ty_int_only(tb);
    int slow = -1, cont = -1;
    if (na || nb) {
        slow = new_label();
        cont = new_label();
        tag_check(g, in->a, na, in->b, nb, slow);
    }
    int d = in->dst;
    int s = 0;
    bool pow2 = b_const && k > 0 && (k & (k - 1)) == 0;
    if (pow2) s = __builtin_ctzll((unsigned long long)k);
    if (pow2 && (is_div || s <= 30)) {
        /* floor division / modulo by 2^s on the tagged word */
        int T = dst_reg(g, d, -1);
        load_loc(g, T, A);
        if (is_div) {
            if (s > 0) {
                E(g, "    sar %s, %d", R64[T], s + 1);
                E(g, "    lea %s, [%s + %s + 1]", R64[T], R64[T], R64[T]);
            }
        } else {
            E(g, "    and %s, %" PRId64, R64[T], ((int64_t)1 << (s + 1)) - 1);
        }
        store_reg(g, d, T);
    } else {
        if (!b_const) {
            E(g, "    cmp %s, 1", use_rm(g, in->b, R11)); /* tagged 0 */
            E(g, "    je .Lk%d", divz_label(g));
        }
        bool save_rax = reg_live_across(g, idx, RAX, d), save_rdx = reg_live_across(g, idx, RDX, d);
        /* the result goes straight to d's register: rax/rdx are only restored
         * when they hold some other value, never d's */
        int R = d != IR_NONE && g->loc[d].kind == LOC_REG ? g->loc[d].reg : R11;
        if (save_rax) E(g, "    push rax");
        if (save_rdx) E(g, "    push rdx");
        load_loc(g, R10, B);
        E(g, "    sar r10, 1");
        load_loc(g, RAX, A);
        E(g, "    sar rax, 1");
        E(g, "    cqo");
        E(g, "    idiv r10");
        int L = new_label();
        E(g, "    test rdx, rdx");
        E(g, "    je .Lk%d", L);
        if (is_div) {
            /* round toward negative infinity: q -= 1 when the remainder and divisor differ in sign */
            E(g, "    xor rdx, r10");
            E(g, "    jns .Lk%d", L);
            E(g, "    sub rax, 1");
            E(g, ".Lk%d:", L);
            E(g, "    add rax, rax");
            E(g, "    jo .Lk%d", ovf_label(g));
            E(g, "    lea %s, [rax + 1]", R64[R]);
        } else {
            /* the result takes the sign of the divisor: r += y when they differ */
            E(g, "    mov rax, rdx");
            E(g, "    xor rax, r10");
            E(g, "    jns .Lk%d", L);
            E(g, "    add rdx, r10");
            E(g, ".Lk%d:", L);
            E(g, "    lea %s, [rdx + rdx + 1]", R64[R]);
        }
        if (save_rdx) E(g, "    pop rdx");
        if (save_rax) E(g, "    pop rax");
        store_reg(g, d, R);
    }
    if (slow >= 0) {
        E(g, ".Lk%d:", cont);
        slow_stub(g, idx, in, slow, cont);
    }
}

static void emit_compare(G *g, int idx, const IrInstr *in) {
    IrTy ta = g->tya[idx], tb = g->tyb[idx];
    int d = in->dst;
    if (in->op == IR_EQ || in->op == IR_NE) {
        if (!(ta & TY_STR) || !(tb & TY_STR)) {
            /* equal <=> same word unless both may be strings */
            set_bool(g, emit_cmp(g, in->a, in->b, in->op), d);
            return;
        }
        int slow = new_label(), cont = new_label(), yes = new_label(), no = new_label(), done = new_label();
        int ra = use_r(g, in->a, R11), rb = use_r(g, in->b, R10);
        bool eq = in->op == IR_EQ;
        E(g, "    cmp %s, %s", R64[ra], R64[rb]);
        E(g, "    je .Lk%d", yes);
        if (g->loc[in->a].kind != LOC_DATA && ta != TY_STR) {
            E(g, "    test %s, 7", R8B[ra]);
            E(g, "    jnz .Lk%d", no);
        }
        if (g->loc[in->b].kind != LOC_DATA && tb != TY_STR) {
            E(g, "    test %s, 7", R8B[rb]);
            E(g, "    jnz .Lk%d", no);
        }
        E(g, "    jmp .Lk%d", slow);
        int T = dst_reg(g, d, -1);
        E(g, ".Lk%d:", no);
        emit_imm(g, T, eq ? (int64_t)LUMA_FALSE : (int64_t)LUMA_TRUE);
        E(g, "    jmp .Lk%d", done);
        E(g, ".Lk%d:", yes);
        emit_imm(g, T, eq ? (int64_t)LUMA_TRUE : (int64_t)LUMA_FALSE);
        E(g, ".Lk%d:", done);
        store_reg(g, d, T);
        E(g, ".Lk%d:", cont);
        slow_stub(g, idx, in, slow, cont);
        return;
    }
    if (!(ta & TY_INT) || !(tb & TY_INT)) {
        call_runtime(g, idx, in);
        return;
    }
    bool na = !ty_int_only(ta), nb = !ty_int_only(tb);
    int slow = -1, cont = -1;
    if (na || nb) {
        slow = new_label();
        cont = new_label();
        tag_check(g, in->a, na, in->b, nb, slow);
    }
    set_bool(g, emit_cmp(g, in->a, in->b, in->op), d);
    if (slow >= 0) {
        E(g, ".Lk%d:", cont);
        slow_stub(g, idx, in, slow, cont);
    }
}

/* br on the result of a compare/not earlier in the block (fused): the
 * compare is emitted here, as cmp + jcc. */
static void emit_fused_br(G *g, int b, int idx, const IrInstr *br) {
    int ci = g->fuse_cmp[idx];
    const IrInstr *c = g->ins[ci];
    int t = br->target[0], f = br->target[1];
    if (c->op == IR_NOT) {
        branch_truthy(g, b, c->a, g->tya[ci], f, t);
        return;
    }
    IrTy ta = g->tya[ci], tb = g->tyb[ci];
    bool ordered = c->op != IR_EQ && c->op != IR_NE;
    bool na = ordered && !ty_int_only(ta), nb = ordered && !ty_int_only(tb);
    int slow = -1;
    if (na || nb) {
        slow = new_label();
        tag_check(g, c->a, na, c->b, nb, slow);
    }
    emit_branch(g, b, emit_cmp(g, c->a, c->b, c->op), t, f);
    if (slow >= 0) {
        Lines *save = g->cur;
        g->cur = &g->stubs;
        E(g, ".Lk%d:", slow);
        int saved[NREGS];
        int ns = live_caller(g, 2 * idx, 2 * idx + 2, -1, saved);
        Loc args[2] = {g->loc[c->a], g->loc[c->b]};
        int r = preserving_call(g, saved, ns, ir_op_runtime(c->op), args, 2);
        E(g, "    cmp %s, %d", R64[r], (int)LUMA_TRUE);
        E(g, "    je %s", blabel(g, t));
        E(g, "    jmp %s", blabel(g, f));
        g->cur = save;
    }
}

static void emit_check(G *g, int idx, const IrInstr *in) {
    IrTy ta = g->tya[idx];
    if (ta != 0 && ir_ty_sub(ta, in->ty)) {
        move_v(g, in->dst, in->a); /* statically proven */
        return;
    }
    int ok = new_label(), slow = new_label(), cont = new_label();
    int ra = use_r(g, in->a, R11);
    if ((in->ty & TY_INT) && (ta & TY_INT)) {
        E(g, "    test %s, 1", R8B[ra]);
        E(g, "    jnz .Lk%d", ok);
    }
    if ((in->ty & TY_NIL) && (ta & TY_NIL)) {
        E(g, "    cmp %s, %d", R64[ra], (int)LUMA_NIL);
        E(g, "    je .Lk%d", ok);
    }
    if ((in->ty & TY_BOOL) && (ta & TY_BOOL)) {
        E(g, "    mov r10, %s", R64[ra]);
        E(g, "    and r10, -9"); /* false 2, true 10 -> 2 */
        E(g, "    cmp r10, 2");
        E(g, "    je .Lk%d", ok);
    }
    if (ty_sid(in->ty) >= 0 && ir_ty_inter(ta, in->ty) & TY_STRUCT) {
        /* an instance of exactly this struct: an object whose header is
         * LUMA_TYPE_STRUCT | id << 8 */
        int no = new_label();
        E(g, "    test %s, 7", R8B[ra]);
        E(g, "    jnz .Lk%d", no);
        E(g, "    cmp qword ptr [%s], %d", R64[ra], LUMA_TYPE_STRUCT | ((ty_sid(in->ty) + 1) << LUMA_STRUCT_ID_SHIFT));
        E(g, "    je .Lk%d", ok);
        E(g, ".Lk%d:", no);
    }
    E(g, "    jmp .Lk%d", slow);
    E(g, ".Lk%d:", ok);
    move_v(g, in->dst, in->a);
    E(g, ".Lk%d:", cont);
    /* strings and failures: luma_check_type(v, mask, context) returns v only
     * if its type matches. The call may clobber a (when a dies here), so d
     * is taken from the result. */
    Lines *save = g->cur;
    g->cur = &g->stubs;
    E(g, ".Lk%d:", slow);
    int saved[NREGS];
    int ns = live_caller(g, 2 * idx, 2 * idx + 2, in->dst, saved);
    Loc args[3] = {g->loc[in->a], loc_imm((int64_t)luma_fixnum((int64_t)in->ty)),
                   (Loc){.kind = LOC_DATA, .global = in->global}};
    store_reg(g, in->dst, preserving_call(g, saved, ns, "luma_check_type", args, 3));
    E(g, "    jmp .Lk%d", cont);
    g->cur = save;
}

static Loc const_loc(const IrInstr *in) {
    switch (in->op) {
    case IR_CONST_INT: return loc_imm((int64_t)luma_fixnum(in->imm));
    case IR_CONST_NIL: return loc_imm((int64_t)LUMA_NIL);
    case IR_CONST_TRUE: return loc_imm((int64_t)LUMA_TRUE);
    case IR_CONST_FALSE: return loc_imm((int64_t)LUMA_FALSE);
    default: return (Loc){.kind = LOC_DATA, .global = in->global};
    }
}

static void emit_instr(G *g, int b, int idx, const IrInstr *in) {
    const IrModule *m = g->m;
    int d = in->dst;
    switch (in->op) {
    case IR_CONST_INT: case IR_CONST_NIL: case IR_CONST_TRUE: case IR_CONST_FALSE: case IR_CONST_DATA: {
        LocKind k = g->loc[d].kind;
        if (k == LOC_REG || k == LOC_STACK) move_loc(g, g->loc[d], const_loc(in));
        break;
    }
    case IR_MOV: move_v(g, d, in->a); break;
    case IR_ADD: case IR_SUB: case IR_MUL: emit_arith(g, idx, in); break;
    case IR_DIV: case IR_MOD: emit_divmod(g, idx, in); break;
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: emit_compare(g, idx, in); break;
    case IR_NEG: {
        IrTy ta = g->tya[idx];
        if (!(ta & TY_INT)) {
            call_runtime(g, idx, in);
            break;
        }
        int slow = -1, cont = -1;
        if (!ty_int_only(ta)) {
            slow = new_label();
            cont = new_label();
            tag_check(g, in->a, true, IR_NONE, false, slow);
        }
        int T = dst_reg(g, d, -1);
        load_loc(g, T, g->loc[in->a]);
        E(g, "    neg %s", R64[T]);
        E(g, "    add %s, 2", R64[T]);
        E(g, "    jo .Lk%d", ovf_label(g));
        store_reg(g, d, T);
        if (slow >= 0) {
            E(g, ".Lk%d:", cont);
            slow_stub(g, idx, in, slow, cont);
        }
        break;
    }
    case IR_NOT: {
        IrTy ta = g->tya[idx];
        if (ta != 0 && (ta & ~(IrTy)TY_BOOL) == 0) {
            int T = dst_reg(g, d, -1);
            load_loc(g, T, g->loc[in->a]);
            E(g, "    xor %s, 8", R64[T]); /* false 2 <-> true 10 */
            store_reg(g, d, T);
            break;
        }
        int always = 0;
        const char *cc = truth_test(g, in->a, ta, &always);
        if (!cc) move_loc(g, g->loc[d], loc_imm(always ? (int64_t)LUMA_FALSE : (int64_t)LUMA_TRUE));
        else set_bool(g, cc_invert(cc), d);
        break;
    }
    case IR_CALL: {
        const IrGlobal *gl = &m->globals[in->global];
        if (gl->kind == IRG_CEXTERN) {
            emit_ffi_call(g, in);
            break;
        }
        Loc dst[6], src[6];
        for (int i = 0; i < in->nargs; i++) {
            dst[i] = loc_reg(ARGREG[i]);
            src[i] = g->loc[in->args[i]];
        }
        parallel_move(g, dst, src, in->nargs);
        if (g->tail[idx]) {
            epilogue(g);
            E(g, "    jmp %s    # tail call", gl->name);
            break;
        }
        E(g, "    call %s%s", gl->name, gl->kind == IRG_EXTERN ? "@PLT" : "");
        store_reg(g, d, RAX);
        break;
    }
    case IR_LOAD: {
        const char *name = m->globals[in->global].name;
        int T = dst_reg(g, d, -1);
        E(g, "    mov %s, qword ptr [rip + .Lvar.%s]", R64[T], name);
        E(g, "    test %s, %s", R64[T], R64[T]);
        E(g, "    je .Lk%d", undef_label(g, in->global));
        store_reg(g, d, T);
        break;
    }
    case IR_STORE: {
        const char *name = m->globals[in->global].name;
        Loc A = g->loc[in->a];
        if (A.kind == LOC_IMM && fits32(A.imm)) E(g, "    mov qword ptr [rip + .Lvar.%s], %" PRId64, name, A.imm);
        else E(g, "    mov qword ptr [rip + .Lvar.%s], %s", name, R64[use_r(g, in->a, R11)]);
        break;
    }
    case IR_CHECK: emit_check(g, idx, in); break;
    case IR_NEW: {
        /* allocate (a call: the field values live across it in callee-saved
         * registers or slots), then store the fields */
        E(g, "    lea rdi, [rip + .Lstruct.%d]", in->sid);
        E(g, "    call luma_new_struct@PLT");
        for (int i = 0; i < in->nargs; i++) {
            Loc A = g->loc[in->args[i]];
            char *dst = tbuf();
            snprintf(dst, 128, "qword ptr [rax + %d]", LUMA_STRUCT_FIELDS_OFFSET + 8 * i);
            if (A.kind == LOC_REG) E(g, "    mov %s, %s", dst, R64[A.reg]);
            else if (A.kind == LOC_IMM && fits32(A.imm)) E(g, "    mov %s, %s", dst, num_text(A.imm));
            else {
                load_loc(g, R11, A);
                E(g, "    mov %s, r11", dst);
            }
        }
        store_reg(g, d, RAX);
        break;
    }
    case IR_GETFIELD:
    case IR_SETFIELD: {
        if (in->sid >= 0) {
            int ro = use_r(g, in->a, R11);
            char *mem = tbuf();
            snprintf(mem, 128, "qword ptr [%s + %d]", R64[ro], LUMA_STRUCT_FIELDS_OFFSET + 8 * in->field);
            if (in->op == IR_GETFIELD) {
                int T = dst_reg(g, d, -1);
                E(g, "    mov %s, %s", R64[T], mem);
                store_reg(g, d, T);
            } else {
                Loc B = g->loc[in->b];
                if (B.kind == LOC_REG) E(g, "    mov %s, %s", mem, R64[B.reg]);
                else if (B.kind == LOC_IMM && fits32(B.imm)) E(g, "    mov %s, %s", mem, num_text(B.imm));
                else {
                    load_loc(g, R10, B);
                    E(g, "    mov %s, r10", mem);
                }
            }
            break;
        }
        /* dynamic: the runtime looks the field up by name */
        char *label = tbuf();
        snprintf(label, 128, ".Lname.%s", in->name);
        int saved[NREGS];
        int ns = live_caller(g, 2 * idx, 2 * idx + 2, d, saved);
        Loc args[3] = {g->loc[in->a], (Loc){.kind = LOC_SYM, .sym = label}, in->op == IR_SETFIELD ? g->loc[in->b] : loc_imm(0)};
        int r = preserving_call(g, saved, ns, in->op == IR_GETFIELD ? "luma_getfield" : "luma_setfield", args,
                                in->op == IR_GETFIELD ? 2 : 3);
        if (in->op == IR_GETFIELD) store_reg(g, d, r);
        break;
    }
    case IR_CALLM: {
        /* self and the arguments go to their registers first; they are kept
         * (pushed) across the method lookup, then the method is called */
        int n = in->nargs + 1 > 6 ? 6 : in->nargs + 1; /* the verifier allows at most 6 */
        Loc dst[6] = {{0}}, src[6] = {{0}};
        for (int i = 0; i < n; i++) {
            dst[i] = loc_reg(ARGREG[i]);
            src[i] = g->loc[i == 0 ? in->a : in->args[i - 1]];
        }
        parallel_move(g, dst, src, n);
        for (int i = 0; i < n; i++) E(g, "    push %s", R64[ARGREG[i]]);
        bool pad = n % 2 != 0; /* the frame keeps rsp 16-aligned at calls */
        if (pad) E(g, "    sub rsp, 8");
        E(g, "    lea rsi, [rip + .Lname.%s]", in->name);
        E(g, "    mov edx, %d", in->nargs);
        E(g, "    call luma_method@PLT");
        E(g, "    mov r11, rax");
        if (pad) E(g, "    add rsp, 8");
        for (int i = n - 1; i >= 0; i--) E(g, "    pop %s", R64[ARGREG[i]]);
        E(g, "    call r11");
        store_reg(g, d, RAX);
        break;
    }
    case IR_PHI: case IR_NOP: break; /* never reach the backend */
    case IR_JMP: emit_goto(g, b, in->target[0]); break;
    case IR_BR:
        if (g->fuse_cmp[idx] >= 0) emit_fused_br(g, b, idx, in);
        else branch_truthy(g, b, in->a, g->tya[idx], in->target[0], in->target[1]);
        break;
    case IR_RET:
        load_loc(g, RAX, g->loc[in->a]);
        epilogue(g);
        E(g, "    ret");
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Analysis and register allocation                                         */

static bool is_const_op(IrOp op) {
    return op == IR_CONST_INT || op == IR_CONST_NIL || op == IR_CONST_TRUE || op == IR_CONST_FALSE || op == IR_CONST_DATA;
}

static void ext(G *g, int v, int pos) {
    if (v < 0) return;
    if (pos < g->start[v]) g->start[v] = pos;
    if (pos > g->end[v]) g->end[v] = pos;
}

static int uses_of(IrInstr *in, int **buf) { return ir_instr_uses(in, buf); }

static const G *sort_g;
static int by_start(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    if (sort_g->start[a] != sort_g->start[b]) return sort_g->start[a] < sort_g->start[b] ? -1 : 1;
    return a - b;
}

static void analyze(G *g) {
    const IrFunc *f = g->f;
    int nv = f->nvregs;
    cfg_build(f, &g->cfg);
    liveness_compute(f, &g->cfg, &g->lv);
    ir_types_compute_refined(g->m, f, &g->types);

    g->layout = xcalloc((size_t)f->nblocks + 1, sizeof(int));
    g->layout_pos = xmalloc(((size_t)f->nblocks + 1) * sizeof(int));
    g->first = xcalloc((size_t)f->nblocks + 1, sizeof(int));
    for (int b = 0; b < f->nblocks; b++) {
        g->layout_pos[b] = -1;
        if (!cfg_reachable(&g->cfg, b)) continue;
        g->layout_pos[b] = g->nlayout;
        g->layout[g->nlayout++] = b;
        g->first[b] = g->ninstr;
        g->ninstr += f->blocks[b].n;
    }
    int n = g->ninstr ? g->ninstr : 1;
    g->ins = xcalloc((size_t)n, sizeof *g->ins);
    g->tya = xcalloc((size_t)n, sizeof(IrTy));
    g->tyb = xcalloc((size_t)n, sizeof(IrTy));
    g->fused = xcalloc((size_t)n, sizeof(bool));
    g->fuse_cmp = xmalloc((size_t)n * sizeof(int));
    g->fuse_br = xmalloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) g->fuse_cmp[i] = g->fuse_br[i] = -1;
    g->tail = xcalloc((size_t)n, sizeof(bool));
    g->skip = xcalloc((size_t)n, sizeof(bool));
    int vn = nv ? nv : 1;
    g->loc = xcalloc((size_t)vn, sizeof(Loc));
    g->start = xmalloc((size_t)vn * sizeof(int));
    g->end = xmalloc((size_t)vn * sizeof(int));
    g->cross = xcalloc((size_t)vn, sizeof(bool));
    g->across_div = xcalloc((size_t)vn, sizeof(bool));
    g->hreg = xmalloc((size_t)vn * sizeof(int));
    g->hvreg = xmalloc((size_t)vn * sizeof(int));
    for (int v = 0; v < vn; v++) {
        g->start[v] = INT_MAX;
        g->end[v] = INT_MIN;
        g->hreg[v] = -1;
        g->hvreg[v] = -1;
    }

    /* operand types before each instruction */
    IrTy *state = xcalloc((size_t)vn, sizeof(IrTy));
    for (int li = 0; li < g->nlayout; li++) {
        int b = g->layout[li];
        memcpy(state, g->types.in + (size_t)b * nv, (size_t)nv * sizeof(IrTy));
        for (int k = 0; k < f->blocks[b].n; k++) {
            int idx = g->first[b] + k;
            const IrInstr *in = &f->blocks[b].instrs[k];
            g->ins[idx] = in;
            if (in->a >= 0) g->tya[idx] = state[in->a];
            if (in->b >= 0) g->tyb[idx] = state[in->b];
            if (in->op != IR_PHI) {
                ir_types_step(g->m, in, state);
                ir_types_refine(in, state);
            }
        }
    }
    free(state);

    /* definition and use counts; rematerializable constants */
    int *defs = xcalloc((size_t)vn, sizeof(int)), *uses = xcalloc((size_t)vn, sizeof(int));
    int *ubuf[64];
    for (int idx = 0; idx < g->ninstr; idx++) {
        IrInstr *in = (IrInstr *)g->ins[idx];
        if (in->dst >= 0) defs[in->dst]++;
        int nu = uses_of(in, ubuf);
        for (int i = 0; i < nu; i++)
            if (*ubuf[i] >= 0) uses[*ubuf[i]]++;
    }
    for (int idx = 0; idx < g->ninstr; idx++) {
        const IrInstr *in = g->ins[idx];
        if (in->dst >= f->nparams && defs[in->dst] == 1 && is_const_op(in->op)) g->loc[in->dst] = const_loc(in);
    }

    /* tail calls and compare/branch fusion */
    for (int li = 0; li < g->nlayout; li++) {
        int b = g->layout[li];
        const IrBlock *bl = &f->blocks[b];
        for (int k = 0; k + 1 < bl->n; k++) {
            const IrInstr *in = &bl->instrs[k], *nx = &bl->instrs[k + 1];
            int idx = g->first[b] + k;
            if (in->op == IR_CALL && in->dst >= 0 && g->m->globals[in->global].kind == IRG_FUNC && nx->op == IR_RET &&
                nx->a == in->dst) {
                g->tail[idx] = true;
                g->skip[idx + 1] = true;
            }
        }
        /* compare/branch fusion: the br's condition is defined by a compare
         * earlier in the block, with only copies and constants in between
         * that leave the compare's operands alone */
        if (bl->n == 0 || bl->instrs[bl->n - 1].op != IR_BR) continue;
        const IrInstr *br = &bl->instrs[bl->n - 1];
        int c = br->a;
        if (c < 0 || uses[c] != 1 || g->loc[c].kind != LOC_NONE) continue;
        int j = bl->n - 2;
        while (j >= 0 && bl->instrs[j].dst != c &&
               (bl->instrs[j].op == IR_MOV || is_const_op(bl->instrs[j].op)))
            j--;
        if (j < 0 || bl->instrs[j].dst != c) continue;
        const IrInstr *in = &bl->instrs[j];
        bool clobbered = false;
        for (int q = j + 1; q < bl->n - 1; q++) {
            int dq = bl->instrs[q].dst;
            if (dq >= 0 && (dq == in->a || dq == in->b)) clobbered = true;
        }
        if (clobbered || in->a == c || in->b == c) continue;
        int idx = g->first[b] + j;
        IrTy ta = g->tya[idx], tb = g->tyb[idx];
        bool ok = false;
        switch (in->op) {
        case IR_LT: case IR_LE: case IR_GT: case IR_GE: ok = (ta & TY_INT) && (tb & TY_INT); break;
        case IR_EQ: case IR_NE: ok = !(ta & TY_STR) || !(tb & TY_STR); break;
        case IR_NOT: ok = true; break;
        default: break;
        }
        if (!ok) continue;
        int bidx = g->first[b] + bl->n - 1;
        g->fused[idx] = true;
        g->fuse_br[idx] = bidx;
        g->fuse_cmp[bidx] = idx;
    }

    /* live intervals */
    bool *param_live = xcalloc((size_t)(f->nparams + 1), sizeof(bool));
    if (g->nlayout > 0) {
        bool *defined = xcalloc((size_t)(f->nparams + 1), sizeof(bool));
        const IrBlock *e = &f->blocks[0];
        for (int k = 0; k < e->n; k++) {
            IrInstr *in = &e->instrs[k];
            int nu = uses_of(in, ubuf);
            for (int i = 0; i < nu; i++) {
                int v = *ubuf[i];
                if (v >= 0 && v < f->nparams && !defined[v]) param_live[v] = true;
            }
            if (in->dst >= 0 && in->dst < f->nparams) defined[in->dst] = true;
        }
        for (int p = 0; p < f->nparams; p++)
            if (!defined[p] && bit_get(live_out(&g->lv, 0), p)) param_live[p] = true;
        free(defined);
    }
    for (int p = 0; p < f->nparams; p++)
        if (param_live[p]) ext(g, p, -1);
    free(param_live);
    for (int li = 0; li < g->nlayout; li++) {
        int b = g->layout[li];
        int f0 = g->first[b], l0 = f0 + f->blocks[b].n - 1;
        for (int v = 0; v < nv; v++) {
            if (bit_get(live_in(&g->lv, b), v)) ext(g, v, 2 * f0);
            if (bit_get(live_out(&g->lv, b), v)) ext(g, v, 2 * l0 + 2);
        }
    }
    for (int idx = 0; idx < g->ninstr; idx++) {
        IrInstr *in = (IrInstr *)g->ins[idx];
        int upos = g->fused[idx] ? 2 * g->fuse_br[idx] : 2 * idx; /* fused: operands are read at the br */
        if (in->op == IR_NEW) upos = 2 * idx + 2; /* the fields are stored after the allocation call */
        int nu = uses_of(in, ubuf);
        for (int i = 0; i < nu; i++) {
            int v = *ubuf[i];
            if (g->fuse_cmp[idx] >= 0) continue; /* the fused condition */
            ext(g, v, upos);
        }
        if (in->dst >= 0 && !g->fused[idx]) ext(g, in->dst, 2 * idx + 1);
    }
    for (int v = 0; v < nv; v++)
        if (g->loc[v].kind != LOC_NONE || (g->start[v] > g->end[v])) {
            g->start[v] = INT_MAX;
            g->end[v] = INT_MIN;
        }
    /* the fused condition never exists */
    for (int idx = 0; idx < g->ninstr; idx++)
        if (g->fused[idx]) {
            int c = g->ins[idx]->dst;
            g->start[c] = INT_MAX;
            g->end[c] = INT_MIN;
        }

    /* values live across a call need callee-saved registers */
    for (int idx = 0; idx < g->ninstr; idx++) {
        const IrInstr *in = g->ins[idx];
        if (!((in->op == IR_CALL && !g->tail[idx]) || in->op == IR_NEW || in->op == IR_CALLM)) continue;
        for (int v = 0; v < nv; v++)
            if (g->start[v] <= 2 * idx && g->end[v] >= 2 * idx + 2) g->cross[v] = true;
    }

    /* values live across an idiv would have to be saved around it if in rax/rdx */
    for (int idx = 0; idx < g->ninstr; idx++) {
        const IrInstr *in = g->ins[idx];
        if ((in->op != IR_DIV && in->op != IR_MOD) || g->loc[in->b].kind == LOC_IMM) continue;
        for (int v = 0; v < nv; v++)
            if (g->start[v] <= 2 * idx && g->end[v] >= 2 * idx + 2) g->across_div[v] = true;
    }

    /* hints */
    for (int p = 0; p < f->nparams && p < 6; p++) g->hreg[p] = ARGREG[p];
    for (int idx = 0; idx < g->ninstr; idx++) {
        const IrInstr *in = g->ins[idx];
        if (in->op == IR_CALL && g->m->globals[in->global].kind != IRG_CEXTERN) {
            for (int i = 0; i < in->nargs && i < 6; i++) {
                int v = in->args[i];
                if (g->end[v] == 2 * idx && g->hreg[v] < 0) g->hreg[v] = ARGREG[i];
            }
            if (in->dst >= 0 && g->hreg[in->dst] < 0) g->hreg[in->dst] = RAX;
        } else if (in->op == IR_RET) {
            if (g->hreg[in->a] < 0) g->hreg[in->a] = RAX;
        } else if (in->op == IR_MOV) {
            if (g->hvreg[in->dst] < 0) g->hvreg[in->dst] = in->a;
            if (g->hvreg[in->a] < 0) g->hvreg[in->a] = in->dst;
        }
    }
    free(defs);
    free(uses);
}

static void allocate(G *g) {
    int nv = g->f->nvregs;
    int *order = xmalloc(((size_t)nv + 1) * sizeof(int)), n = 0;
    for (int v = 0; v < nv; v++)
        if (g->start[v] <= g->end[v]) order[n++] = v;
    sort_g = g;
    qsort(order, (size_t)n, sizeof(int), by_start);
    int owner[NREGS];
    for (int r = 0; r < NREGS; r++) owner[r] = -1;
    int *active = xmalloc(((size_t)nv + 1) * sizeof(int)), na = 0;
    for (int i = 0; i < n; i++) {
        int cur = order[i];
        for (int j = 0; j < na;) {
            int a = active[j];
            if (g->end[a] < g->start[cur]) {
                owner[g->loc[a].reg] = -1;
                active[j] = active[--na];
            } else {
                j++;
            }
        }
        bool cross = g->cross[cur];
#define FREE_OK(x) (allocatable(x) && owner[x] < 0 && (!cross || callee_saved(x)))
        int r = -1;
        int h = g->hreg[cur], hv = g->hvreg[cur];
        if (h >= 0 && FREE_OK(h)) r = h;
        else if (hv >= 0 && g->loc[hv].kind == LOC_REG && FREE_OK(g->loc[hv].reg)) r = g->loc[hv].reg;
        else {
            for (int k = cross ? NCALLER : 0; k < NALLOC && r < 0; k++) {
                int x = ALLOC_ORDER[k];
                if (FREE_OK(x) && !(g->across_div[cur] && (x == RAX || x == RDX))) r = x;
            }
            for (int k = cross ? NCALLER : 0; k < NALLOC && r < 0; k++)
                if (FREE_OK(ALLOC_ORDER[k])) r = ALLOC_ORDER[k];
        }
#undef FREE_OK
        if (r < 0) {
            /* spill whichever eligible interval ends last */
            int victim = -1, vj = -1;
            for (int j = 0; j < na; j++) {
                int a = active[j];
                if (cross && !callee_saved(g->loc[a].reg)) continue;
                if (victim < 0 || g->end[a] > g->end[victim]) {
                    victim = a;
                    vj = j;
                }
            }
            if (victim >= 0 && g->end[victim] > g->end[cur]) {
                r = g->loc[victim].reg;
                g->loc[victim] = (Loc){.kind = LOC_STACK, .slot = g->nslots++};
                active[vj] = active[--na];
                owner[r] = -1;
            } else {
                g->loc[cur] = (Loc){.kind = LOC_STACK, .slot = g->nslots++};
                continue;
            }
        }
        g->loc[cur] = loc_reg(r);
        owner[r] = cur;
        active[na++] = cur;
    }
    free(order);
    free(active);

    bool used[NREGS] = {0};
    for (int v = 0; v < nv; v++)
        if (g->loc[v].kind == LOC_REG) used[g->loc[v].reg] = true;
    static const int CS[5] = {RBX, R12, R13, R14, R15};
    for (int i = 0; i < 5; i++)
        if (used[CS[i]]) g->cs[g->ncs++] = CS[i];
    bool calls = false;
    for (int idx = 0; idx < g->ninstr; idx++) {
        const IrInstr *in = g->ins[idx];
        if ((in->op == IR_CALL && !g->tail[idx]) || in->op == IR_NEW || in->op == IR_CALLM) calls = true;
        if (in->op == IR_CALL && g->m->globals[in->global].kind == IRG_CEXTERN && in->nargs > g->ffi_slots)
            g->ffi_slots = in->nargs;
    }
    g->framed = calls || g->nslots || g->ncs || g->ffi_slots;
    int bytes = 8 * (g->nslots + g->ffi_slots);
    while ((8 * g->ncs + bytes) % 16) bytes += 8;
    g->frame_size = bytes;
}

/* ------------------------------------------------------------------------ */
/* Peephole                                                                 */

static bool is_label_line(const char *s, const char *label) {
    size_t n = strlen(label);
    return strncmp(s, label, n) == 0 && s[n] == ':' && s[n + 1] == '\0';
}

static void peephole(Lines *l) {
    for (bool changed = true; changed;) {
        changed = false;
        int w = 0;
        for (int i = 0; i < l->n; i++) {
            char *s = l->v[i];
            /* mov X, X */
            if (strncmp(s, "    mov ", 8) == 0) {
                const char *comma = strstr(s + 8, ", ");
                if (comma && !strchr(s + 8, '[') && (size_t)(comma - (s + 8)) == strlen(comma + 2) &&
                    strncmp(s + 8, comma + 2, (size_t)(comma - (s + 8))) == 0) {
                    free(s);
                    changed = true;
                    continue;
                }
            }
            /* lea X, [X + 1] ; lea X, [X - 1]  (re-tagging immediately undone; lea leaves the flags alone) */
            if (strncmp(s, "    lea ", 8) == 0 && i + 1 < l->n) {
                bool undone = false;
                for (int r = 0; r < NREGS && !undone; r++) {
                    char up[48], down[48];
                    snprintf(up, sizeof up, "    lea %s, [%s + 1]", R64[r], R64[r]);
                    snprintf(down, sizeof down, "    lea %s, [%s - 1]", R64[r], R64[r]);
                    undone = !strcmp(s, up) && !strcmp(l->v[i + 1], down);
                }
                if (undone) {
                    free(s);
                    free(l->v[i + 1]);
                    i++;
                    changed = true;
                    continue;
                }
            }
            /* jmp X ; X: */
            if (strncmp(s, "    jmp ", 8) == 0 && i + 1 < l->n && is_label_line(l->v[i + 1], s + 8)) {
                free(s);
                changed = true;
                continue;
            }
            /* jCC X ; jmp Y ; X:  ->  jNCC Y ; X: */
            if (strncmp(s, "    j", 5) == 0 && strncmp(s, "    jmp ", 8) != 0 && i + 2 < l->n &&
                strncmp(l->v[i + 1], "    jmp ", 8) == 0) {
                const char *sp = strchr(s + 5, ' ');
                if (sp && is_label_line(l->v[i + 2], sp + 1)) {
                    char cc[8];
                    size_t cn = (size_t)(sp - (s + 5));
                    if (cn < sizeof cc) {
                        memcpy(cc, s + 5, cn);
                        cc[cn] = '\0';
                        const char *inv = cc_invert(cc);
                        if (inv) {
                            const char *target = l->v[i + 1] + 8;
                            size_t len = strlen(inv) + strlen(target) + 8;
                            char *ns = xmalloc(len);
                            snprintf(ns, len, "    j%s %s", inv, target);
                            free(s);
                            free(l->v[i + 1]);
                            l->v[w++] = ns;
                            i++;
                            changed = true;
                            continue;
                        }
                    }
                }
            }
            l->v[w++] = s;
        }
        l->n = w;
    }
}

/* ------------------------------------------------------------------------ */
/* Functions and the module                                                 */

static void gen_func(const IrModule *m, const IrFunc *f, Buf *out) {
    G g;
    memset(&g, 0, sizeof g);
    g.m = m;
    g.f = f;
    g.name = m->globals[f->global].name;
    g.ovf = g.divz = -1;
    g.undef = xmalloc(((size_t)m->nglobals + 1) * sizeof(int));
    for (int i = 0; i < m->nglobals; i++) g.undef[i] = -1;
    analyze(&g);
    allocate(&g);

    g.cur = &g.body;
    if (g.framed) {
        E(&g, "    push rbp");
        E(&g, "    mov rbp, rsp");
        for (int i = 0; i < g.ncs; i++) E(&g, "    push %s", R64[g.cs[i]]);
        if (g.frame_size) E(&g, "    sub rsp, %d", g.frame_size);
    }
    /* parameters: from the argument registers to their allocated homes */
    Loc pd[6], ps[6];
    int np = 0;
    for (int p = 0; p < f->nparams && p < 6; p++)
        if (g.start[p] == -1 && (g.loc[p].kind == LOC_REG || g.loc[p].kind == LOC_STACK)) {
            pd[np] = g.loc[p];
            ps[np] = loc_reg(ARGREG[p]);
            np++;
        }
    parallel_move(&g, pd, ps, np);

    Buf tmp = {0};
    for (int li = 0; li < g.nlayout; li++) {
        int b = g.layout[li];
        E(&g, "%s:", blabel(&g, b));
        for (int k = 0; k < f->blocks[b].n; k++) {
            int idx = g.first[b] + k;
            const IrInstr *in = g.ins[idx];
            tmp.len = 0;
            ir_print_instr(m, f, in, &tmp);
            buf_byte(&tmp, 0);
            const char *note = g.fused[idx] ? "    (fused into the branch)" : g.skip[idx] ? "    (tail call)" : "";
            E(&g, "    # %s%s", (const char *)tmp.data, note);
            if (g.fused[idx] || g.skip[idx]) continue;
            emit_instr(&g, b, idx, in);
        }
    }
    buf_free(&tmp);

    /* noreturn error stubs (called with the stack realigned) */
    g.cur = &g.stubs;
    if (g.ovf >= 0) {
        E(&g, ".Lk%d:", g.ovf);
        E(&g, "    and rsp, -16");
        E(&g, "    call luma_int_overflow@PLT");
    }
    if (g.divz >= 0) {
        E(&g, ".Lk%d:", g.divz);
        E(&g, "    and rsp, -16");
        E(&g, "    call luma_div_zero@PLT");
    }
    for (int i = 0; i < m->nglobals; i++)
        if (g.undef[i] >= 0) {
            E(&g, ".Lk%d:", g.undef[i]);
            E(&g, "    lea rdi, [rip + .Lvarname.%s]", m->globals[i].name);
            E(&g, "    and rsp, -16");
            E(&g, "    call luma_undefined_variable@PLT");
        }

    Lines all = {0};
    for (int i = 0; i < g.body.n; i++) lines_push(&all, g.body.v[i]);
    for (int i = 0; i < g.stubs.n; i++) lines_push(&all, g.stubs.v[i]);
    free(g.body.v);
    free(g.stubs.v);
    peephole(&all);

    int nregs = 0;
    for (int v = 0; v < f->nvregs; v++) nregs += g.loc[v].kind == LOC_REG;
    buf_printf(out, "\n    .text\n");
    buf_printf(out, "    .globl %s\n", g.name);
    buf_printf(out, "    .type %s, @function\n", g.name);
    buf_printf(out, "%s:    # %d vreg%s: %d in registers, %d spilled; %s", g.name, f->nvregs, f->nvregs == 1 ? "" : "s",
               nregs, g.nslots, g.framed ? "framed" : "frameless");
    if (g.ncs) {
        buf_printf(out, ", saves");
        for (int i = 0; i < g.ncs; i++) buf_printf(out, " %s", R64[g.cs[i]]);
    }
    buf_printf(out, "\n");
    for (int i = 0; i < all.n; i++) buf_printf(out, "%s\n", all.v[i]);
    buf_printf(out, "    .size %s, .-%s\n", g.name, g.name);
    lines_free(&all);

    cfg_free(&g.cfg);
    liveness_free(&g.lv);
    ir_types_free(&g.types);
    free(g.layout);
    free(g.layout_pos);
    free(g.first);
    free(g.ins);
    free(g.tya);
    free(g.tyb);
    free(g.fused);
    free(g.fuse_cmp);
    free(g.fuse_br);
    free(g.tail);
    free(g.skip);
    free(g.loc);
    free(g.start);
    free(g.end);
    free(g.cross);
    free(g.across_div);
    free(g.hreg);
    free(g.hvreg);
    free(g.undef);
}

void x86_gen_module(const IrModule *m, Buf *out) {
    buf_printf(out, "# Generated by luma from %s -- do not edit.\n", m->source);
    buf_printf(out, "# Optimizing backend: linear-scan registers, inline fixnum fast paths.\n");
    buf_printf(out, "    .intel_syntax noprefix\n");
    x86_emit_data(m, out);
    nlabel = 0;
    ffi_labels = 0;
    buf_free(&ffi_data);
    for (int i = 0; i < m->nglobals; i++)
        if (m->globals[i].kind == IRG_FUNC) gen_func(m, &m->funcs[m->globals[i].func], out);
    if (ffi_data.len) {
        buf_printf(out, "\n    .section .rodata    # FFI error-message contexts\n");
        buf_append(out, ffi_data.data, ffi_data.len);
    }
    buf_free(&ffi_data);
    buf_printf(out, "\n    .section .note.GNU-stack\n");
}
