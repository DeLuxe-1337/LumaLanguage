/* opt_objects.c - object (struct) optimizations on non-SSA LIR, run last in
 * the -O2 pipeline (after out-of-SSA), so it sees exactly the IR that the
 * verifier will check and uses the same type inference.
 *
 *   1. Devirtualization. A dynamic access whose object the flow-sensitive
 *      types prove to be exactly struct S becomes static: `getfield %o, .x`
 *      -> `getfield %o, S.x` (when S has a field x), `setfield %o, .x, %v`
 *      likewise when %v also fits the field's type, and `callm %o, .m(...)`
 *      -> `call @<S's entry for m>(%o, ...)` when the arity matches. If the
 *      field or method does not exist, the dynamic form stays, so the
 *      runtime error is unchanged.
 *      A `check` that the (now more precise) types prove becomes a `mov`.
 *   2. Field load forwarding, within a block: a static `getfield %o, S.f`
 *      after an earlier read or write of the same %o S.f becomes a `mov`
 *      of the known value, unless something in between may have changed
 *      it: a write to field S.f of any object (it may be the same one), a
 *      dynamic write, a call to a Luma function or method, or a new
 *      assignment to %o or to the known value's vreg. */
#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"

typedef struct {
    int obj, sid, field, val;
} Known;

static void forget_vreg(Known *k, int *n, int v) {
    for (int i = 0; i < *n;)
        if (k[i].obj == v || k[i].val == v) k[i] = k[--*n];
        else i++;
}

static void forget_field(Known *k, int *n, int sid, int field) {
    for (int i = 0; i < *n;)
        if (k[i].sid == sid && k[i].field == field) k[i] = k[--*n];
        else i++;
}

static bool devirtualize(const IrModule *m, IrInstr *in, const IrTy *st) {
    if (in->op != IR_GETFIELD && in->op != IR_SETFIELD && in->op != IR_CALLM) return false;
    if ((in->op != IR_CALLM && in->sid >= 0) || in->a < 0) return false;
    int sid = ty_sid(st[in->a]);
    if (sid < 0 || !ir_ty_sub(st[in->a], ty_struct_of(sid))) return false;
    const IrStruct *sd = &m->structs[sid];
    if (in->op == IR_CALLM) {
        int k = ir_struct_method(sd, in->name);
        if (k < 0 || m->globals[sd->mglobals[k]].arity != in->nargs + 1) return false;
        int *args = xmalloc((size_t)(in->nargs + 1) * sizeof *args);
        args[0] = in->a;
        for (int i = 0; i < in->nargs; i++) args[i + 1] = in->args[i];
        free(in->args);
        free(in->name);
        in->name = NULL;
        in->args = args;
        in->nargs++;
        in->op = IR_CALL;
        in->global = sd->mglobals[k];
        in->a = IR_NONE;
        return true;
    }
    int k = ir_struct_field(sd, in->name);
    if (k < 0) return false;
    if (in->op == IR_SETFIELD && !ir_ty_sub(st[in->b], sd->ftys[k])) return false; /* the runtime check stays */
    free(in->name);
    in->name = NULL;
    in->sid = sid;
    in->field = k;
    return true;
}

bool opt_objects(IrModule *m, IrFunc *f) {
    if (m->nstructs == 0) return false;
    bool any = false;
    IrTypes t;
    ir_types_compute(m, f, &t);
    int nv = f->nvregs;
    IrTy *st = xmalloc((size_t)(nv ? nv : 1) * sizeof *st);
    Known *known = NULL;
    int nknown = 0, capknown = 0;
    for (int b = 0; b < f->nblocks; b++) {
        IrBlock *bl = &f->blocks[b];
        memcpy(st, &t.in[(size_t)b * nv], (size_t)nv * sizeof *st);
        nknown = 0;
        for (int k = 0; k < bl->n; k++) {
            IrInstr *in = &bl->instrs[k];
            any |= devirtualize(m, in, st);
            switch (in->op) {
            case IR_CHECK: /* devirtualized reads give precise types: some checks are now proven */
                if (st[in->a] && ir_ty_sub(st[in->a], in->ty)) {
                    in->op = IR_MOV;
                    in->global = IR_NONE;
                    any = true;
                }
                break;
            case IR_GETFIELD:
                if (in->sid < 0) break;
                for (int i = 0; i < nknown; i++)
                    if (known[i].obj == in->a && known[i].sid == in->sid && known[i].field == in->field) {
                        in->op = IR_MOV;
                        in->a = known[i].val;
                        in->sid = IR_NONE;
                        any = true;
                        break;
                    }
                break;
            case IR_SETFIELD:
                if (in->sid < 0) nknown = 0; /* a dynamic write may hit any field */
                else forget_field(known, &nknown, in->sid, in->field);
                break;
            case IR_CALLM: nknown = 0; break;
            case IR_CALL:
                if (m->globals[in->global].kind == IRG_FUNC) nknown = 0; /* Luma code may write fields */
                break;
            default: break;
            }
            if (in->dst >= 0) forget_vreg(known, &nknown, in->dst);
            int obj = -1, val = -1;
            if (in->op == IR_GETFIELD && in->sid >= 0 && in->a != in->dst) obj = in->a, val = in->dst;
            if (in->op == IR_SETFIELD && in->sid >= 0) obj = in->a, val = in->b;
            if (obj >= 0) {
                if (nknown == capknown) {
                    capknown = capknown ? capknown * 2 : 16;
                    known = xrealloc(known, (size_t)capknown * sizeof *known);
                }
                known[nknown++] = (Known){obj, in->sid, in->field, val};
            }
            if (in->op != IR_PHI) ir_types_step(m, in, st);
        }
    }
    free(known);
    free(st);
    ir_types_free(&t);
    return any;
}
