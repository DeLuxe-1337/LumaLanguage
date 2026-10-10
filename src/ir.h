/* ir.h - LIR, the Luma intermediate representation (docs/IR.md).
 *
 * A module holds globals (functions, externs, string data). A function is
 * a list of basic blocks; block 0 is the entry. Every block ends with
 * exactly one terminator (jmp/br/ret). Virtual registers ("vregs") are
 * function-local, mutable (not SSA), and all hold tagged LumaValues.
 * Parameters are vregs 0..nparams-1; other vregs are numbered in order of
 * first appearance, which is also their stack-slot order in the backend. */
#ifndef LUMA_IR_H
#define LUMA_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef enum {
    /* value-producing */
    IR_CONST_INT,   /* dst = imm (untagged integer) */
    IR_CONST_NIL,
    IR_CONST_TRUE,
    IR_CONST_FALSE,
    IR_CONST_DATA,  /* dst = &global (data) */
    IR_MOV,         /* dst = a */
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
    IR_NEG, IR_NOT, /* dst = op a */
    IR_CALL,        /* [dst =] global(args...) */
    IR_LOAD,        /* dst = value of global variable (runtime error if never assigned) */
    IR_STORE,       /* global variable = a   (no dst) */
    /* terminators */
    IR_JMP,         /* target[0] */
    IR_BR,          /* a truthy ? target[0] : target[1] */
    IR_RET,         /* return a */
} IrOp;

#define IR_NONE (-1)

typedef struct {
    IrOp op;
    int dst;        /* vreg or IR_NONE */
    int a, b;       /* operand vregs or IR_NONE */
    int64_t imm;    /* IR_CONST_INT */
    int global;     /* IR_CONST_DATA / IR_CALL / IR_LOAD / IR_STORE: index into module globals */
    int *args;      /* IR_CALL: vregs (owned) */
    int nargs;
    int target[2];  /* IR_JMP / IR_BR: block indices */
    int line;       /* source line for diagnostics (0 if unknown) */
} IrInstr;

typedef struct {
    char *label;
    IrInstr *instrs;
    int n, cap;
} IrBlock;

typedef enum { IRG_FUNC, IRG_EXTERN, IRG_DATA, IRG_VAR } IrGlobalKind;

typedef struct {
    IrGlobalKind kind;
    char *name;
    int arity;        /* IRG_FUNC / IRG_EXTERN */
    int func;         /* IRG_FUNC: index into IrModule.funcs */
    char *data;       /* IRG_DATA: string bytes (owned, NUL-terminated) */
    size_t data_len;
    int line;
} IrGlobal;

typedef struct {
    int global;       /* index of this function's IrGlobal */
    int nparams;
    char **vregs;     /* names, without '%' */
    int nvregs, cap_vregs;
    IrBlock *blocks;
    int nblocks, cap_blocks;
    int line;
} IrFunc;

typedef struct {
    char *source;     /* "module" string */
    IrGlobal *globals;
    int nglobals;
    IrFunc *funcs;
    int nfuncs;
} IrModule;

/* ---- construction ---- */
void ir_module_init(IrModule *m, const char *source);
void ir_module_free(IrModule *m);
int ir_find_global(const IrModule *m, const char *name);
int ir_add_extern(IrModule *m, const char *name, int arity);  /* returns global index */
int ir_add_data(IrModule *m, const char *name, const char *bytes, size_t len);
int ir_add_var(IrModule *m, const char *name);                /* module-level variable slot */
/* Name used in runtime messages for a global variable: the part after the
 * first '.', so lowering's "var.count" is reported as "count". */
const char *ir_var_display_name(const char *name);
int ir_add_func(IrModule *m, const char *name, int nparams);  /* returns func index */
int ir_func_vreg(IrFunc *f, const char *name);                /* find or create by name */
int ir_func_new_vreg(IrFunc *f, const char *name);            /* create; caller guarantees uniqueness */
/* Renumbers vregs into text order (params, then first appearance with the
 * destination before operands), which is the order ir_parse() creates them
 * in. Lowered and re-parsed modules therefore get identical stack slots. */
void ir_func_canonicalize(IrFunc *f);
int ir_func_find_vreg(const IrFunc *f, const char *name);
int ir_func_block(IrFunc *f, const char *label);              /* append a block */
int ir_func_find_block(const IrFunc *f, const char *label);
IrInstr *ir_emit(IrFunc *f, int block, IrOp op);              /* zero-initialised instr */
bool ir_block_terminated(const IrBlock *b);
bool ir_op_is_terminator(IrOp op);
bool ir_op_is_binary(IrOp op);
bool ir_op_is_unary(IrOp op);
const char *ir_op_name(IrOp op);
/* Name of the runtime function implementing a generic op ("luma_add", ...). */
const char *ir_op_runtime(IrOp op);

/* ---- text form ---- */
void ir_print_module(const IrModule *m, Buf *out);
void ir_print_instr(const IrModule *m, const IrFunc *f, const IrInstr *in, Buf *out);
/* Parses the text form. On error prints "path:line: error: ..." and returns false. */
bool ir_parse(const char *path, const char *src, size_t len, IrModule *out);

/* ---- verification ---- */
/* Checks the rules in docs/IR.md section 6. Prints diagnostics prefixed with
 * `path` and returns false on any violation. */
bool ir_verify(const IrModule *m, const char *path);

#endif
