#include "asm.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Assembler state                                                          */
/*                                                                          */
/* The source is assembled in one or more full passes. Every jump to a     */
/* label starts out short (rel8). After a pass, any short jump whose target */
/* is out of range, in another section, global, or undefined is marked long */
/* and the source is assembled again. Jumps only ever grow, so this reaches */
/* a fixed point in at most (number of jumps + 1) passes. This matches GNU  */
/* as, which keeps generated code byte-identical with it.                    */

#define MAX_LINE 4096
#define MAX_OPERANDS 3

typedef enum {
    FX_PC32,  /* 4-byte field, R_X86_64_PC32 if it becomes a relocation */
    FX_PLT32, /* 4-byte field, R_X86_64_PLT32 if it becomes a relocation */
    FX_REL8,  /* 1-byte short-jump field; never a relocation */
} FixKind;

typedef struct {
    int section;     /* section containing the field */
    uint64_t offset; /* offset of the field */
    int symbol;      /* target symbol (index into ObjFile.symbols) */
    int64_t addend;  /* already includes the -size PC bias */
    FixKind kind;
    size_t jump;     /* FX_REL8: index into the jump-size table */
    int line;
} Fixup;

typedef struct {
    const char *path;
    int line;
    ObjFile *o;
    int cur; /* current section index, -1 before any section directive */
    Fixup *fixups;
    size_t nfixups;
    bool failed;
    /* branch relaxation */
    bool *jump_long; /* per jump (in source order): use the rel32 form */
    size_t njump_long;
    size_t jump_counter;
    bool grew; /* some jump was marked long during this pass */
} Asm;

static void asm_error(Asm *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void asm_error(Asm *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", a->path, a->line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    a->failed = true;
}

/* ------------------------------------------------------------------------ */
/* Sections                                                                 */

static int select_section(Asm *a, const char *name) {
    int i = obj_find_section(a->o, name);
    if (i >= 0) return a->cur = i;
    if (strcmp(name, ".text") == 0)
        i = obj_add_section(a->o, name, OBJ_SHT_PROGBITS, OBJ_SHF_ALLOC | OBJ_SHF_EXECINSTR, 16);
    else if (strcmp(name, ".rodata") == 0)
        i = obj_add_section(a->o, name, OBJ_SHT_PROGBITS, OBJ_SHF_ALLOC, 1);
    else if (strcmp(name, ".data") == 0)
        i = obj_add_section(a->o, name, OBJ_SHT_PROGBITS, OBJ_SHF_ALLOC | OBJ_SHF_WRITE, 1);
    else if (strcmp(name, ".note.GNU-stack") == 0)
        /* Empty, flags 0: tells the linker this object needs no executable stack. */
        i = obj_add_section(a->o, name, OBJ_SHT_PROGBITS, 0, 1);
    else {
        asm_error(a, "unsupported section '%s' (supported: .text .rodata .data .note.GNU-stack)", name);
        return -1;
    }
    return a->cur = i;
}

static Buf *code(Asm *a) {
    if (a->cur < 0) select_section(a, ".text"); /* GAS default */
    return &a->o->sections[a->cur].data;
}

static bool cur_is_exec(Asm *a) {
    code(a);
    return (a->o->sections[a->cur].flags & OBJ_SHF_EXECINSTR) != 0;
}

/* ------------------------------------------------------------------------ */
/* Lexical helpers                                                          */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

static bool is_sym_start(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static bool is_sym_char(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

static bool valid_symbol(const char *s) {
    if (!is_sym_start((unsigned char)*s)) return false;
    for (s++; *s; s++)
        if (!is_sym_char((unsigned char)*s)) return false;
    return true;
}

/* Removes a trailing '#' comment, respecting string literals. */
static void strip_comment(char *s) {
    bool in_str = false;
    for (char *p = s; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_str = false;
        } else if (*p == '"') {
            in_str = true;
        } else if (*p == '#') {
            *p = '\0';
            return;
        }
    }
}

/* Parses a whole string as a signed integer (decimal, 0x hex, 0 octal). */
static bool parse_int(const char *s, int64_t *out) {
    if (!*s) return false;
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!isdigit((unsigned char)*p)) return false;
    errno = 0;
    char *end;
    long long v = strtoll(s, &end, 0);
    if (errno == ERANGE || *end != '\0') {
        /* Allow unsigned 64-bit hex such as 0xffffffffffffffff. */
        if (*s != '-') {
            errno = 0;
            unsigned long long u = strtoull(s, &end, 0);
            if (errno == 0 && *end == '\0') {
                *out = (int64_t)u;
                return true;
            }
        }
        return false;
    }
    *out = v;
    return true;
}

/* Splits s on top-level commas (not inside [] or ""). Returns count or -1. */
static int split_operands(char *s, char **parts, int max) {
    int n = 0, depth = 0;
    bool in_str = false;
    char *start = s;
    if (!*trim(s)) return 0;
    for (char *p = s;; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') in_str = false;
            if (*p) continue;
        }
        if (*p == '"') { in_str = true; continue; }
        if (*p == '[') depth++;
        if (*p == ']') depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            bool last = *p == '\0';
            *p = '\0';
            if (n == max) return -1;
            parts[n++] = trim(start);
            if (last) break;
            start = p + 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Operands                                                                 */

typedef enum { OP_REG, OP_IMM, OP_MEM, OP_SYM } OpKind;

#define BASE_RIP (-1)
#define REG_RBP 5

typedef struct {
    OpKind kind;
    int reg;       /* OP_REG: 0..15; OP_MEM: base register (REG_RBP) or BASE_RIP */
    int size;      /* OP_REG: 32 or 64 */
    int64_t imm;   /* OP_IMM value; OP_MEM displacement; OP_SYM unused */
    char sym[256]; /* OP_MEM (rip only)/OP_SYM: symbol name, "" if none */
    bool plt;      /* OP_SYM written as name@PLT */
} Operand;

static const char *const REG64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *const REG32[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                      "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};

static bool lookup_reg(const char *s, int *reg, int *size) {
    for (int i = 0; i < 16; i++) {
        if (strcmp(s, REG64[i]) == 0) { *reg = i; *size = 64; return true; }
        if (strcmp(s, REG32[i]) == 0) { *reg = i; *size = 32; return true; }
    }
    return false;
}

static bool copy_sym(Asm *a, char *dst, const char *src) {
    if (strlen(src) >= 256) {
        asm_error(a, "symbol name too long");
        return false;
    }
    if (!valid_symbol(src)) {
        asm_error(a, "invalid symbol name '%s'", src);
        return false;
    }
    strcpy(dst, src);
    return true;
}

#define MEM_UNSUPPORTED "unsupported memory operand: only [rip + symbol] and [rbp +/- disp] are supported"

/* Parses "[rip + sym + n]" or "[rbp - n]" memory operands. */
static bool parse_mem(Asm *a, char *s, Operand *op) {
    size_t n = strlen(s);
    if (n < 2 || s[n - 1] != ']') {
        asm_error(a, "malformed memory operand '%s'", s);
        return false;
    }
    s[n - 1] = '\0';
    char *p = s + 1;
    op->kind = OP_MEM;
    op->imm = 0;
    op->sym[0] = '\0';
    bool have_base = false;
    int sign = 1;
    bool first = true;
    while (*(p = trim(p))) {
        if (!first) {
            if (*p == '+') sign = 1;
            else if (*p == '-') sign = -1;
            else {
                asm_error(a, "expected '+' or '-' in memory operand, found '%c'", *p);
                return false;
            }
            p = trim(p + 1);
        }
        char *end = p;
        while (*end && *end != '+' && *end != '-' && *end != ' ' && *end != '\t') end++;
        char save = *end;
        *end = '\0';
        char term[MAX_LINE];
        snprintf(term, sizeof term, "%s", p);
        *end = save;
        p = end;
        int64_t v;
        int r, sz;
        if (first) {
            if (strcmp(term, "rip") == 0) op->reg = BASE_RIP;
            else if (strcmp(term, "rbp") == 0) op->reg = REG_RBP;
            else {
                asm_error(a, MEM_UNSUPPORTED);
                return false;
            }
            have_base = true;
        } else if (parse_int(term, &v)) {
            if (v < INT32_MIN || v > INT32_MAX) {
                asm_error(a, "memory displacement out of 32-bit range");
                return false;
            }
            op->imm += sign * v;
        } else if (lookup_reg(term, &r, &sz)) {
            asm_error(a, MEM_UNSUPPORTED);
            return false;
        } else {
            if (op->reg != BASE_RIP) {
                asm_error(a, "symbols are only allowed in [rip + symbol] operands");
                return false;
            }
            if (sign < 0) {
                asm_error(a, "cannot subtract symbol '%s' in memory operand", term);
                return false;
            }
            if (op->sym[0]) {
                asm_error(a, "memory operand references more than one symbol");
                return false;
            }
            if (!copy_sym(a, op->sym, term)) return false;
        }
        first = false;
    }
    if (!have_base) {
        asm_error(a, "empty memory operand");
        return false;
    }
    if (op->imm < INT32_MIN || op->imm > INT32_MAX) {
        asm_error(a, "memory displacement out of 32-bit range");
        return false;
    }
    return true;
}

static bool parse_operand(Asm *a, char *s, Operand *op) {
    memset(op, 0, sizeof *op);
    if (*s == '[') return parse_mem(a, s, op);
    if (strstr(s, " ptr ") || strncmp(s, "qword", 5) == 0 || strncmp(s, "dword", 5) == 0) {
        asm_error(a, "size-qualified memory operands are not supported: '%s'", s);
        return false;
    }
    if (lookup_reg(s, &op->reg, &op->size)) {
        op->kind = OP_REG;
        return true;
    }
    if (parse_int(s, &op->imm)) {
        op->kind = OP_IMM;
        return true;
    }
    op->kind = OP_SYM;
    char *at = strchr(s, '@');
    if (at) {
        if (strcmp(at, "@PLT") != 0) {
            asm_error(a, "unsupported symbol modifier '%s' (only @PLT)", at);
            return false;
        }
        *at = '\0';
        op->plt = true;
    }
    return copy_sym(a, op->sym, s);
}

/* ------------------------------------------------------------------------ */
/* Encoding helpers                                                         */

static void emit_rex(Buf *b, int w, int r, int x, int base, bool force) {
    uint8_t rex = (uint8_t)(0x40 | (w << 3) | ((r >> 3) << 2) | ((x >> 3) << 1) | (base >> 3));
    if (rex != 0x40 || force) buf_byte(b, rex);
}

static uint8_t modrm(int mod, int reg, int rm) { return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)); }

static bool fits_i8(int64_t v) { return v >= -128 && v <= 127; }
static bool fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* Records a PC-relative field of `size` bytes at the current end of the
 * section and emits zero bytes for it. The field must be the last thing in
 * the instruction, so P + size is the address of the next instruction,
 * which is what the CPU adds the field to. */
static void add_fixup(Asm *a, const char *sym, int64_t addend, FixKind kind, size_t jump) {
    int size = kind == FX_REL8 ? 1 : 4;
    Fixup f;
    f.section = a->cur;
    f.offset = code(a)->len;
    f.symbol = obj_intern_symbol(a->o, sym);
    f.addend = addend - size;
    f.kind = kind;
    f.jump = jump;
    f.line = a->line;
    a->fixups = xrealloc(a->fixups, (a->nfixups + 1) * sizeof *a->fixups);
    a->fixups[a->nfixups++] = f;
    buf_zeros(code(a), (size_t)size);
}

/* Emits ModRM (+disp) for a memory operand with `reg` in the reg field.
 * Must be called last for the instruction (RIP fixups assume this).
 *   [rip + disp32]   mod=00 rm=101
 *   [rbp + disp8]    mod=01 rm=101   (also used for [rbp], since mod=00 rm=101 means RIP)
 *   [rbp + disp32]   mod=10 rm=101 */
static void emit_mem(Asm *a, int reg, const Operand *m) {
    Buf *b = code(a);
    if (m->reg == BASE_RIP) {
        buf_byte(b, modrm(0, reg, 5));
        if (m->sym[0]) add_fixup(a, m->sym, m->imm, FX_PC32, 0);
        else buf_u32(b, (uint32_t)m->imm);
    } else if (fits_i8(m->imm)) {
        buf_byte(b, modrm(1, reg, m->reg));
        buf_byte(b, (uint8_t)m->imm);
    } else {
        buf_byte(b, modrm(2, reg, m->reg));
        buf_u32(b, (uint32_t)m->imm);
    }
}

static bool want(Asm *a, const char *mn, int nops, int expected) {
    if (nops != expected) {
        asm_error(a, "'%s' expects %d operand%s, got %d", mn, expected, expected == 1 ? "" : "s", nops);
        return false;
    }
    return true;
}

static void bad_operands(Asm *a, const char *mn) { asm_error(a, "unsupported operand combination for '%s'", mn); }

/* ------------------------------------------------------------------------ */
/* Instruction tables                                                       */

/* Classic two-operand ALU group. Encodings:
 *   op r/m, r      rr  /r
 *   op r/m, imm8   83  /ext ib   (imm sign-extended)
 *   op rax, imm32  acc id        (accumulator short form, chosen by GAS)
 *   op r/m, imm32  81  /ext id */
typedef struct {
    const char *name;
    uint8_t rr;
    uint8_t ext;
    uint8_t acc;
} AluOp;

static const AluOp ALU_OPS[] = {
    {"add", 0x01, 0, 0x05}, {"or", 0x09, 1, 0x0D},  {"and", 0x21, 4, 0x25},
    {"sub", 0x29, 5, 0x2D}, {"xor", 0x31, 6, 0x35}, {"cmp", 0x39, 7, 0x3D},
};

/* Jcc condition codes (low nibble of 70+cc / 0F 80+cc). */
typedef struct {
    const char *name;
    uint8_t cc;
} CondCode;

static const CondCode CONDS[] = {
    {"o", 0x0},   {"no", 0x1}, {"b", 0x2},  {"c", 0x2},  {"nae", 0x2}, {"ae", 0x3}, {"nb", 0x3},
    {"nc", 0x3},  {"e", 0x4},  {"z", 0x4},  {"ne", 0x5}, {"nz", 0x5},  {"be", 0x6}, {"na", 0x6},
    {"a", 0x7},   {"nbe", 0x7}, {"s", 0x8}, {"ns", 0x9}, {"p", 0xA},   {"pe", 0xA}, {"np", 0xB},
    {"po", 0xB},  {"l", 0xC},  {"nge", 0xC}, {"ge", 0xD}, {"nl", 0xD}, {"le", 0xE}, {"ng", 0xE},
    {"g", 0xF},   {"nle", 0xF},
};

/* Returns the condition code for "jcc" mnemonics, 16 for "jmp", -1 otherwise. */
static int jump_kind(const char *mn) {
    if (strcmp(mn, "jmp") == 0) return 16;
    if (mn[0] != 'j') return -1;
    for (size_t i = 0; i < sizeof CONDS / sizeof *CONDS; i++)
        if (strcmp(mn + 1, CONDS[i].name) == 0) return CONDS[i].cc;
    return -1;
}

static void assemble_jump(Asm *a, const char *mn, int kind, Operand *ops, int n) {
    if (!want(a, mn, n, 1)) return;
    if (ops[0].kind != OP_SYM) { bad_operands(a, mn); return; }
    Buf *b = code(a);
    size_t idx = a->jump_counter++;
    bool lng = idx < a->njump_long && a->jump_long[idx];
    if (!lng) {
        buf_byte(b, kind == 16 ? 0xEB : (uint8_t)(0x70 + kind)); /* EB cb / 7x cb */
        add_fixup(a, ops[0].sym, 0, FX_REL8, idx);
    } else {
        if (kind == 16) buf_byte(b, 0xE9); /* E9 cd */
        else { buf_byte(b, 0x0F); buf_byte(b, (uint8_t)(0x80 + kind)); } /* 0F 8x cd */
        add_fixup(a, ops[0].sym, 0, FX_PLT32, 0);
    }
}

static void assemble_alu(Asm *a, const AluOp *op, Operand *ops, int n) {
    const char *mn = op->name;
    if (!want(a, mn, n, 2)) return;
    Operand *d = &ops[0], *s = &ops[1];
    Buf *b = code(a);
    if (d->kind != OP_REG) { bad_operands(a, mn); return; }
    if (s->kind == OP_REG) {
        if (d->size != s->size) { bad_operands(a, mn); return; }
        emit_rex(b, d->size == 64, s->reg, 0, d->reg, false);
        buf_byte(b, op->rr);
        buf_byte(b, modrm(3, s->reg, d->reg));
        return;
    }
    if (s->kind != OP_IMM) { bad_operands(a, mn); return; }
    if (!fits_i32(s->imm) && !(d->size == 32 && s->imm >= 0 && s->imm <= UINT32_MAX)) {
        asm_error(a, "immediate out of 32-bit range");
        return;
    }
    emit_rex(b, d->size == 64, 0, 0, d->reg, false);
    if (fits_i8(s->imm)) {
        buf_byte(b, 0x83);
        buf_byte(b, modrm(3, op->ext, d->reg));
        buf_byte(b, (uint8_t)s->imm);
    } else if (d->reg == 0) {
        buf_byte(b, op->acc);
        buf_u32(b, (uint32_t)s->imm);
    } else {
        buf_byte(b, 0x81);
        buf_byte(b, modrm(3, op->ext, d->reg));
        buf_u32(b, (uint32_t)s->imm);
    }
}

/* ------------------------------------------------------------------------ */
/* Instructions                                                             */

static void assemble_insn(Asm *a, const char *mn, Operand *ops, int n) {
    if (!cur_is_exec(a)) {
        asm_error(a, "instruction '%s' in non-executable section '%s'", mn, a->o->sections[a->cur].name);
        return;
    }
    Buf *b = code(a);

    if (strcmp(mn, "ret") == 0) {
        if (want(a, mn, n, 0)) buf_byte(b, 0xC3); /* C3: RET (near) */
        return;
    }
    if (strcmp(mn, "nop") == 0) {
        if (want(a, mn, n, 0)) buf_byte(b, 0x90);
        return;
    }
    if (strcmp(mn, "push") == 0 || strcmp(mn, "pop") == 0) {
        if (!want(a, mn, n, 1)) return;
        if (ops[0].kind != OP_REG || ops[0].size != 64) { bad_operands(a, mn); return; }
        /* 50+rd PUSH r64 / 58+rd POP r64; default operand size is 64. */
        emit_rex(b, 0, 0, 0, ops[0].reg, false);
        buf_byte(b, (uint8_t)((mn[1] == 'u' ? 0x50 : 0x58) + (ops[0].reg & 7)));
        return;
    }
    if (strcmp(mn, "mov") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind == OP_REG && s->kind == OP_REG) {
            if (d->size != s->size) { bad_operands(a, mn); return; }
            /* 89 /r MOV r/m, r  (REX.W for 64-bit) */
            emit_rex(b, d->size == 64, s->reg, 0, d->reg, false);
            buf_byte(b, 0x89);
            buf_byte(b, modrm(3, s->reg, d->reg));
            return;
        }
        if (d->kind == OP_REG && s->kind == OP_MEM) {
            if (d->size != 64) { bad_operands(a, mn); return; }
            /* REX.W 8B /r MOV r64, r/m64 */
            emit_rex(b, 1, d->reg, 0, 0, false);
            buf_byte(b, 0x8B);
            emit_mem(a, d->reg, s);
            return;
        }
        if (d->kind == OP_MEM && s->kind == OP_REG) {
            if (s->size != 64) { bad_operands(a, mn); return; }
            /* REX.W 89 /r MOV r/m64, r64 */
            emit_rex(b, 1, s->reg, 0, 0, false);
            buf_byte(b, 0x89);
            emit_mem(a, s->reg, d);
            return;
        }
        if (d->kind == OP_REG && s->kind == OP_IMM) {
            if (d->size == 32) {
                if (s->imm < INT32_MIN || s->imm > UINT32_MAX) {
                    asm_error(a, "immediate out of range for 32-bit register");
                    return;
                }
                /* B8+rd id MOV r32, imm32 (zero-extends into the 64-bit register) */
                emit_rex(b, 0, 0, 0, d->reg, false);
                buf_byte(b, (uint8_t)(0xB8 + (d->reg & 7)));
                buf_u32(b, (uint32_t)s->imm);
            } else if (fits_i32(s->imm)) {
                /* REX.W C7 /0 id MOV r/m64, imm32 (sign-extended) */
                emit_rex(b, 1, 0, 0, d->reg, false);
                buf_byte(b, 0xC7);
                buf_byte(b, modrm(3, 0, d->reg));
                buf_u32(b, (uint32_t)s->imm);
            } else {
                /* REX.W B8+rd io MOV r64, imm64 (movabs) */
                emit_rex(b, 1, 0, 0, d->reg, false);
                buf_byte(b, (uint8_t)(0xB8 + (d->reg & 7)));
                buf_u64(b, (uint64_t)s->imm);
            }
            return;
        }
        bad_operands(a, mn);
        return;
    }
    for (size_t i = 0; i < sizeof ALU_OPS / sizeof *ALU_OPS; i++) {
        if (strcmp(mn, ALU_OPS[i].name) == 0) {
            assemble_alu(a, &ALU_OPS[i], ops, n);
            return;
        }
    }
    if (strcmp(mn, "test") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG) { bad_operands(a, mn); return; }
        if (s->kind == OP_REG) {
            if (d->size != s->size) { bad_operands(a, mn); return; }
            /* 85 /r TEST r/m, r */
            emit_rex(b, d->size == 64, s->reg, 0, d->reg, false);
            buf_byte(b, 0x85);
            buf_byte(b, modrm(3, s->reg, d->reg));
            return;
        }
        if (s->kind == OP_IMM) {
            if (!fits_i32(s->imm) && !(d->size == 32 && s->imm >= 0 && s->imm <= UINT32_MAX)) {
                asm_error(a, "immediate out of 32-bit range");
                return;
            }
            /* A9 id TEST eax/rax, imm32 ; F7 /0 id TEST r/m, imm32 (no imm8 form exists) */
            emit_rex(b, d->size == 64, 0, 0, d->reg, false);
            if (d->reg == 0) {
                buf_byte(b, 0xA9);
            } else {
                buf_byte(b, 0xF7);
                buf_byte(b, modrm(3, 0, d->reg));
            }
            buf_u32(b, (uint32_t)s->imm);
            return;
        }
        bad_operands(a, mn);
        return;
    }
    if (strcmp(mn, "lea") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size != 64 || s->kind != OP_MEM) { bad_operands(a, mn); return; }
        /* REX.W 8D /r LEA r64, m */
        emit_rex(b, 1, d->reg, 0, 0, false);
        buf_byte(b, 0x8D);
        emit_mem(a, d->reg, s);
        return;
    }
    if (strcmp(mn, "call") == 0) {
        if (!want(a, mn, n, 1)) return;
        if (ops[0].kind != OP_SYM) { bad_operands(a, mn); return; }
        /* E8 cd CALL rel32. Always recorded as PLT32 so the linker may route
         * calls to shared-library functions through the PLT. */
        buf_byte(b, 0xE8);
        add_fixup(a, ops[0].sym, 0, FX_PLT32, 0);
        return;
    }
    int jk = jump_kind(mn);
    if (jk >= 0) {
        assemble_jump(a, mn, jk, ops, n);
        return;
    }
    asm_error(a, "unknown or unsupported instruction '%s'", mn);
}

/* ------------------------------------------------------------------------ */
/* Directives                                                               */

static bool parse_string_lit(Asm *a, const char *s, Buf *out) {
    if (*s != '"') {
        asm_error(a, "expected string literal");
        return false;
    }
    const char *p = s + 1;
    for (;;) {
        char c = *p;
        if (c == '\0') {
            asm_error(a, "unterminated string literal");
            return false;
        }
        if (c == '"') { p++; break; }
        if (c != '\\') { buf_byte(out, (uint8_t)c); p++; continue; }
        p++;
        c = *p;
        if (c >= '0' && c <= '7') {
            int v = 0, k = 0;
            while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; }
            if (v > 255) { asm_error(a, "octal escape out of range"); return false; }
            buf_byte(out, (uint8_t)v);
            continue;
        }
        switch (c) {
        case 'n': buf_byte(out, '\n'); break;
        case 't': buf_byte(out, '\t'); break;
        case 'r': buf_byte(out, '\r'); break;
        case '"': buf_byte(out, '"'); break;
        case '\\': buf_byte(out, '\\'); break;
        default: asm_error(a, "unsupported escape '\\%c' in string", c ? c : '?'); return false;
        }
        p++;
    }
    if (*trim((char *)p)) {
        asm_error(a, "unexpected text after string literal");
        return false;
    }
    return true;
}

static void define_label(Asm *a, const char *name) {
    if (!valid_symbol(name)) {
        asm_error(a, "invalid label name '%s'", name);
        return;
    }
    Buf *b = code(a);
    int i = obj_intern_symbol(a->o, name);
    ObjSymbol *s = &a->o->symbols[i];
    if (s->section != OBJ_UNDEF) {
        asm_error(a, "symbol '%s' is already defined", name);
        return;
    }
    s->section = a->cur;
    s->value = b->len;
}

static void directive(Asm *a, char *name, char *args) {
    char *ops[16];
    if (strcmp(name, ".intel_syntax") == 0) {
        if (strcmp(args, "noprefix") != 0) asm_error(a, "only '.intel_syntax noprefix' is supported");
        return;
    }
    if (strcmp(name, ".text") == 0 || strcmp(name, ".data") == 0) {
        if (*args) asm_error(a, "'%s' takes no arguments", name);
        else select_section(a, name);
        return;
    }
    if (strcmp(name, ".section") == 0) {
        if (!*args || strchr(args, ',') || strchr(args, ' ')) {
            asm_error(a, "'.section' expects a single section name (flags are implied)");
            return;
        }
        select_section(a, args);
        return;
    }
    if (strcmp(name, ".globl") == 0 || strcmp(name, ".global") == 0 || strcmp(name, ".extern") == 0) {
        int n = split_operands(args, ops, 16);
        if (n <= 0) { asm_error(a, "'%s' expects symbol names", name); return; }
        for (int i = 0; i < n; i++) {
            if (!valid_symbol(ops[i])) { asm_error(a, "invalid symbol name '%s'", ops[i]); return; }
            int s = obj_intern_symbol(a->o, ops[i]);
            if (a->o->symbols[s].local_label) { asm_error(a, "'.L' symbols cannot be global"); return; }
            a->o->symbols[s].global = true;
        }
        return;
    }
    if (strcmp(name, ".type") == 0) {
        int n = split_operands(args, ops, 16);
        if (n != 2 || !valid_symbol(ops[0])) { asm_error(a, "usage: .type symbol, @function|@object"); return; }
        int s = obj_intern_symbol(a->o, ops[0]);
        if (strcmp(ops[1], "@function") == 0) a->o->symbols[s].type = OBJ_STT_FUNC;
        else if (strcmp(ops[1], "@object") == 0) a->o->symbols[s].type = OBJ_STT_OBJECT;
        else asm_error(a, "unsupported symbol type '%s'", ops[1]);
        return;
    }
    if (strcmp(name, ".size") == 0) {
        int n = split_operands(args, ops, 16);
        if (n != 2 || !valid_symbol(ops[0])) { asm_error(a, "usage: .size symbol, .-symbol | integer"); return; }
        int s = obj_find_symbol(a->o, ops[0]);
        int64_t v;
        if (parse_int(ops[1], &v) && v >= 0) {
            if (s < 0) s = obj_intern_symbol(a->o, ops[0]);
            a->o->symbols[s].size = (uint64_t)v;
            return;
        }
        char expect[300];
        snprintf(expect, sizeof expect, ".-%s", ops[0]);
        char compact[300];
        size_t k = 0;
        for (char *e = ops[1]; *e && k + 1 < sizeof compact; e++)
            if (*e != ' ' && *e != '\t') compact[k++] = *e;
        compact[k] = '\0';
        if (strcmp(compact, expect) != 0) { asm_error(a, "unsupported .size expression '%s'", ops[1]); return; }
        if (s < 0 || a->o->symbols[s].section != a->cur) {
            asm_error(a, ".size: symbol '%s' is not defined in the current section", ops[0]);
            return;
        }
        a->o->symbols[s].size = code(a)->len - a->o->symbols[s].value;
        return;
    }
    if (strcmp(name, ".byte") == 0 || strcmp(name, ".quad") == 0) {
        bool quad = name[1] == 'q';
        int n = split_operands(args, ops, 16);
        if (n <= 0) { asm_error(a, "'%s' expects 1 to 16 values per line", name); return; }
        for (int i = 0; i < n; i++) {
            int64_t v;
            if (!parse_int(ops[i], &v) || (!quad && (v < -128 || v > 255))) {
                asm_error(a, "invalid %s value '%s'", quad ? "quad" : "byte", ops[i]);
                return;
            }
            if (quad) buf_u64(code(a), (uint64_t)v);
            else buf_byte(code(a), (uint8_t)v);
        }
        return;
    }
    if (strcmp(name, ".p2align") == 0) {
        int64_t v;
        if (!parse_int(args, &v) || v < 0 || v > 12) {
            asm_error(a, "usage: .p2align N (0 <= N <= 12)");
            return;
        }
        if (cur_is_exec(a)) {
            asm_error(a, ".p2align is only supported in data sections (GAS pads code with multi-byte NOPs)");
            return;
        }
        ObjSection *sec = &a->o->sections[a->cur];
        uint64_t al = (uint64_t)1 << v;
        if (sec->align < al) sec->align = al;
        buf_align(&sec->data, (size_t)al);
        return;
    }
    if (strcmp(name, ".ascii") == 0 || strcmp(name, ".asciz") == 0 || strcmp(name, ".string") == 0) {
        Buf tmp = {0};
        if (parse_string_lit(a, args, &tmp)) {
            buf_append(code(a), tmp.data, tmp.len);
            if (strcmp(name, ".ascii") != 0) buf_byte(code(a), 0); /* .asciz/.string add NUL */
        }
        buf_free(&tmp);
        return;
    }
    asm_error(a, "unknown or unsupported directive '%s'", name);
}

/* ------------------------------------------------------------------------ */
/* Line driver                                                              */

static void assemble_line(Asm *a, char *line) {
    strip_comment(line);
    char *p = trim(line);
    /* Leading labels: "name:" (possibly several). */
    for (;;) {
        char *q = p;
        while (is_sym_char((unsigned char)*q)) q++;
        if (q > p && *q == ':' && is_sym_start((unsigned char)*p)) {
            *q = '\0';
            define_label(a, p);
            p = trim(q + 1);
            continue;
        }
        break;
    }
    if (!*p) return;

    /* Split mnemonic/directive name from the rest. */
    char *rest = p;
    while (*rest && *rest != ' ' && *rest != '\t') rest++;
    if (*rest) *rest++ = '\0';
    rest = trim(rest);

    if (*p == '.') {
        directive(a, p, rest);
        return;
    }
    for (char *c = p; *c; c++) {
        if (!isalpha((unsigned char)*c)) {
            asm_error(a, "invalid mnemonic '%s'", p);
            return;
        }
        *c = (char)tolower((unsigned char)*c);
    }
    char *parts[MAX_OPERANDS + 1];
    int n = split_operands(rest, parts, MAX_OPERANDS);
    if (n < 0) {
        asm_error(a, "too many operands");
        return;
    }
    Operand ops[MAX_OPERANDS];
    for (int i = 0; i < n; i++) {
        if (!*parts[i]) {
            asm_error(a, "empty operand");
            return;
        }
        if (!parse_operand(a, parts[i], &ops[i])) return;
    }
    assemble_insn(a, p, ops, n);
}

static void mark_long(Asm *a, size_t jump) {
    if (jump >= a->njump_long) {
        size_t n = a->jump_counter > jump ? a->jump_counter : jump + 1;
        a->jump_long = xrealloc(a->jump_long, n * sizeof *a->jump_long);
        memset(a->jump_long + a->njump_long, 0, (n - a->njump_long) * sizeof *a->jump_long);
        a->njump_long = n;
    }
    a->jump_long[jump] = true;
    a->grew = true;
}

/* Resolves every recorded fixup after all labels are known.
 *  - Local symbol in the same section: patched now (S + A - P); no relocation.
 *  - Local symbol in another section: relocation against that section's
 *    section symbol with addend += symbol offset (".L" names never reach the
 *    symbol table).
 *  - Global or undefined symbol: relocation against the symbol itself, so the
 *    linker (or dynamic linker) can interpose/resolve it.
 *  - Short jumps (rel8) must be local, same-section, and in range; otherwise
 *    they are marked long for the next pass. */
static void resolve_fixups(Asm *a) {
    for (size_t i = 0; i < a->nfixups; i++) {
        Fixup *f = &a->fixups[i];
        ObjSymbol *s = &a->o->symbols[f->symbol];
        a->line = f->line;
        if (s->section == OBJ_UNDEF && s->local_label) {
            asm_error(a, "undefined local label '%s'", s->name);
            continue;
        }
        bool local_same = s->section != OBJ_UNDEF && !s->global && s->section == f->section;
        int64_t v = local_same ? (int64_t)s->value + f->addend - (int64_t)f->offset : 0;
        if (f->kind == FX_REL8) {
            if (local_same && fits_i8(v)) a->o->sections[f->section].data.data[f->offset] = (uint8_t)v;
            else mark_long(a, f->jump);
            continue;
        }
        uint32_t rtype = f->kind == FX_PC32 ? OBJ_R_X86_64_PC32 : OBJ_R_X86_64_PLT32;
        if (s->section == OBJ_UNDEF) {
            s->global = true; /* undefined => external */
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, rtype, f->symbol, -1, f->addend});
        } else if (local_same) {
            if (!fits_i32(v)) {
                asm_error(a, "PC-relative displacement to '%s' out of range", s->name);
                continue;
            }
            buf_patch_u32(&a->o->sections[f->section].data, f->offset, (uint32_t)v);
        } else if (!s->global) {
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, rtype, -1, s->section,
                                           f->addend + (int64_t)s->value});
        } else {
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, rtype, f->symbol, -1, f->addend});
        }
    }
}

/* One full pass over the source. */
static void run_pass(Asm *a, const char *src, size_t len) {
    size_t pos = 0;
    char line[MAX_LINE];
    while (pos < len && !a->failed) {
        a->line++;
        size_t start = pos;
        while (pos < len && src[pos] != '\n') {
            if (src[pos] == '\0') {
                asm_error(a, "NUL byte in assembly source");
                return;
            }
            pos++;
        }
        size_t n = pos - start;
        if (pos < len) pos++; /* skip '\n' */
        if (n >= MAX_LINE) {
            asm_error(a, "line too long (max %d bytes)", MAX_LINE - 1);
            return;
        }
        memcpy(line, src + start, n);
        line[n] = '\0';
        assemble_line(a, line);
    }
    if (!a->failed) resolve_fixups(a);
    for (int i = 0; i < a->o->nsymbols && !a->failed; i++) {
        ObjSymbol *s = &a->o->symbols[i];
        if (s->local_label && s->section == OBJ_UNDEF) {
            a->line = 0;
            asm_error(a, "local label '%s' used but never defined", s->name);
        }
    }
}

bool assemble(const char *path, const char *src, size_t len, ObjFile *out) {
    bool *jump_long = NULL;
    size_t njump_long = 0;
    for (size_t pass = 0;; pass++) {
        memset(out, 0, sizeof *out);
        Asm a = {0};
        a.path = path;
        a.o = out;
        a.cur = -1;
        a.jump_long = jump_long;
        a.njump_long = njump_long;
        run_pass(&a, src, len);
        free(a.fixups);
        jump_long = a.jump_long;
        njump_long = a.njump_long;
        if (a.failed) {
            obj_free(out);
            free(jump_long);
            return false;
        }
        if (!a.grew) break;
        obj_free(out);
        if (pass > a.jump_counter + 1) { /* cannot happen: jumps only grow */
            fprintf(stderr, "%s: error: internal: branch relaxation did not converge\n", path);
            free(jump_long);
            return false;
        }
    }
    free(jump_long);
    return true;
}
