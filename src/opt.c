/* opt.c - optimization pipeline driver (see opt.h). */
#include "opt.h"

#include <stdlib.h>
#include <string.h>

#include "opt_internal.h"
#include "util.h"

static bool optimize_function(IrModule *m, IrFunc *f) {
    opt_simplify_cfg(m, f, true);
    if (!opt_to_ssa(f)) return false;
    for (int round = 0; round < 4; round++) {
        bool changed = false;
        changed |= opt_sccp(m, f);
        changed |= opt_gvn(m, f);
        changed |= opt_licm(m, f);
        changed |= opt_dce(m, f);
        if (!changed) break;
    }
    opt_from_ssa(f);
    opt_coalesce(f);
    opt_simplify_cfg(m, f, false);
    ir_func_compact(f);
    ir_func_canonicalize(f);
    return true;
}

void opt_module(IrModule *m, int level) {
    if (level < 2) return;
    for (int i = 0; i < m->nfuncs; i++) {
        IrFunc *f = &m->funcs[i];
        IrFunc copy;
        ir_func_clone(f, &copy);
        if (optimize_function(m, &copy)) {
            ir_func_free_body(f);
            *f = copy;
        } else {
            ir_func_free_body(&copy);
        }
    }
}
