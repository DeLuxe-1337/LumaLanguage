/* ir.h - LIR, the Luma intermediate representation (docs/IR.md).
 *
 * A module holds globals (functions, externs, string data). A function is
 * a list of basic blocks; block 0 is the entry. Every block ends with
 * exactly one terminator (jmp/br/ret). Virtual registers ("vregs") are
 * function-local, mutable (not SSA), and all hold tagged LumaValues.
 * Parameters are vregs 0..nparams-1; other vregs are numbered in order of
 * first appearance, which is also their stack-slot order in the -O0 backend.
 *
 * Types (v0.4): a type is a set of runtime types (IrTy). Function
 * parameters, function results and global variables may be declared with a
 * type; `check` narrows a value at runtime. Declared types are guarantees
 * the producer of the IR must uphold, and the verifier proves them with the
 * flow-sensitive inference in ir_types.c. */
#ifndef LUMA_IR_H
#define LUMA_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* A static type: the set of runtime types a value may have. The bit values
 * match the runtime's luma_check_type masks. 0 means "no value" (the code
 * that would produce it always fails, or is unreachable). */
typedef unsigned IrTy;
enum { TY_INT = 1, TY_STR = 2, TY_BOOL = 4, TY_NIL = 8, TY_ANY = 15 };

/* "int", "str?", "any", "int|str|bool" (and "never" for 0). */
void ir_ty_name(IrTy t, char *buf, size_t n);
/* Parses the forms ir_ty_name prints (except "never"). */
bool ir_ty_parse(const char *s, IrTy *out);

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
    IR_CHECK,       /* dst = a if a's runtime type is in `ty`; else runtime error naming `global` (data) */
    IR_PHI,         /* dst = args[k] when entered from block phi_blocks[k] (SSA form; optimizer-internal) */
    IR_NOP,         /* deleted instruction (optimizer-internal; removed before output) */
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
    int *args;      /* IR_CALL / IR_PHI: vregs (owned) */
    int nargs;
    int *phi_blocks;/* IR_PHI: predecessor block of each argument (owned) */
    IrTy ty;        /* IR_CHECK: the allowed types */
    int target[2];  /* IR_JMP / IR_BR: block indices */
    int line;       /* source line for diagnostics (0 if unknown) */
} IrInstr;

typedef struct {
    char *label;
    IrInstr *instrs;
    int n, cap;
} IrBlock;

typedef enum { IRG_FUNC, IRG_EXTERN, IRG_DATA, IRG_VAR, IRG_CEXTERN } IrGlobalKind;

/* C types for `extern c fn` declarations (the FFI). Calls to such a global
 * take and return ordinary Luma values; the backend converts at the call site
 * (docs/IR.md section 16). */
typedef enum {
    CT_I8, CT_I16, CT_I32, CT_I64, CT_U8, CT_U16, CT_U32, CT_U64,
    CT_BOOL, CT_CSTR, CT_CSTR_OPT, CT_PTR, CT_VOID,
    CT_COUNT
} CType;

/* "i32", "cstr?", ... ; and the reverse (returns false if unknown). */
const char *ctype_name(CType t);
bool ctype_from_name(const char *name, bool nullable, CType *out);

typedef struct {
    IrGlobalKind kind;
    char *name;
    int arity;        /* IRG_FUNC / IRG_EXTERN */
    int func;         /* IRG_FUNC: index into IrModule.funcs */
    char *data;       /* IRG_DATA: string bytes (owned, NUL-terminated) */
    size_t data_len;
    CType *cparams;   /* IRG_CEXTERN: parameter C types (arity entries, owned) */
    char **cnames;    /* IRG_CEXTERN: parameter names, for runtime messages (owned) */
    CType cret;       /* IRG_CEXTERN: return C type */
    IrTy *ptys;       /* IRG_FUNC: declared parameter types (arity entries, owned; NULL = all any) */
    IrTy ty;          /* IRG_FUNC: declared result type; IRG_VAR: declared variable type */
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
/* A C function: params/names have `arity` entries (copied). */
int ir_add_cextern(IrModule *m, const char *name, int arity, const CType *params, const char *const *names, CType ret);
/* Name used in runtime messages for a global variable: the part after the
 * first '.', so lowering's "var.count" is reported as "count". */
const char *ir_var_display_name(const char *name);
int ir_add_func(IrModule *m, const char *name, int nparams);  /* returns func index */
int ir_func_vreg(IrFunc *f, const char *name);                /* find or create by name */
int ir_func_new_vreg(IrFunc *f, const char *name);            /* create; caller guarantees uniqueness */
/* Renumbers vregs into text order (params, then first appearance with the
 * destination before operands), which is the order ir_parse() creates them
 * in. Lowered and re-parsed modules therefore get identical stack slots.
 * Vregs that no instruction references (other than parameters) are removed. */
void ir_func_canonicalize(IrFunc *f);
int ir_func_find_vreg(const IrFunc *f, const char *name);
int ir_func_block(IrFunc *f, const char *label);              /* append a block */
int ir_func_find_block(const IrFunc *f, const char *label);
IrInstr *ir_emit(IrFunc *f, int block, IrOp op);              /* zero-initialised instr */
bool ir_block_terminated(const IrBlock *b);
bool ir_op_is_terminator(IrOp op);
bool ir_op_is_binary(IrOp op);
bool ir_op_is_unary(IrOp op);
/* Declared type of parameter i of function global g (TY_ANY if untyped). */
IrTy ir_param_ty(const IrGlobal *g, int i);
/* The Luma type a C type converts to/from at an FFI boundary. */
IrTy ir_ctype_ty(CType c);
/* Frees the owned parts of one instruction (args, phi_blocks). */
void ir_instr_free(IrInstr *in);
/* Deep copy of an instruction (args and phi_blocks are duplicated). */
IrInstr ir_instr_clone(const IrInstr *in);
/* Pointers to every vreg the instruction reads (so passes can rewrite them).
 * Returns the count; `ptrs` must have room for in->nargs + 2 entries. */
int ir_instr_uses(IrInstr *in, int **ptrs);
/* Deep copy / destruction of a function's body (not its IrGlobal). */
void ir_func_clone(const IrFunc *src, IrFunc *dst);
void ir_func_free_body(IrFunc *f);
/* Removes IR_NOP instructions from every block. */
void ir_func_compact(IrFunc *f);
const char *ir_op_name(IrOp op);
/* Name of the runtime function implementing a generic op ("luma_add", ...). */
const char *ir_op_runtime(IrOp op);

/* ---- text form ---- */
void ir_print_module(const IrModule *m, Buf *out);
void ir_print_instr(const IrModule *m, const IrFunc *f, const IrInstr *in, Buf *out);
/* Parses the text form. On error prints "path:line: error: ..." and returns false. */
bool ir_parse(const char *path, const char *src, size_t len, IrModule *out);

/* ---- type inference (ir_types.c) ----
 * Forward dataflow over a function: the type of every vreg at the start of
 * every block (union over predecessors; parameters start at their declared
 * types, everything else at 0 = unassigned). Works on SSA (phis) too. */
typedef struct {
    int nblocks, nvregs;
    IrTy *in;   /* in[b * nvregs + v] */
    IrTy *out;  /* out[b * nvregs + v] */
} IrTypes;

void ir_types_compute(const IrModule *m, const IrFunc *f, IrTypes *t);
void ir_types_free(IrTypes *t);
/* Type of in->dst given the types of the vregs before it (state). */
IrTy ir_types_result(const IrModule *m, const IrInstr *in, const IrTy *state);
/* Applies one instruction to a state (non-phi instructions). */
void ir_types_step(const IrModule *m, const IrInstr *in, IrTy *state);
/* Narrows the operands of an instruction that would have raised a runtime
 * error otherwise (sub/mul/div/mod/neg/ordered compares: int; check: its
 * type). Call after ir_types_step; operands that are also the destination
 * keep the result type. */
void ir_types_refine(const IrInstr *in, IrTy *state);
/* ir_types_compute with ir_types_refine applied after every instruction
 * (non-SSA code only: a vreg's type then differs before and after a use).
 * Used by the backend to drop redundant tag checks. */
void ir_types_compute_refined(const IrModule *m, const IrFunc *f, IrTypes *t);

/* ---- verification ---- */
/* Checks the rules in docs/IR.md section 6. Prints diagnostics prefixed with
 * `path` and returns false on any violation. */
bool ir_verify(const IrModule *m, const char *path);

#endif
