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
/* is out of range, in another section, or undefined is marked long     */
/* and the source is assembled again. Jumps only ever grow, so this reaches */
/* a fixed point in at most (number of jumps + 1) passes. This matches GNU  */
/* as, which keeps generated code byte-identical with it.                    */

#define MAX_LINE 4096
#define MAX_OPERANDS 3

typedef enum {
    FX_PC32,  /* 4-byte field, R_X86_64_PC32 if it becomes a relocation */
    FX_PLT32, /* 4-byte field, R_X86_64_PLT32 if it becomes a relocation */
    FX_JMP32, /* 4-byte jmp/jcc field: resolved here when the target is defined
                 in the same section (even if global, as GNU as does); else
                 R_X86_64_PLT32 */
    FX_REL8,  /* 1-byte short-jump field; never a relocation */
    FX_ABS64, /* 8-byte .quad SYMBOL: always R_X86_64_64 (an absolute address) */
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

#define NO_REG (-1)
#define BASE_RIP (-2)

typedef struct {
    OpKind kind;
    int reg;       /* OP_REG: 0..15 */
    int size;      /* OP_REG: 8, 32 or 64. OP_MEM: from "byte/dword/qword ptr", 0 if unspecified */
    bool rex_byte; /* OP_REG, size 8: spl/bpl/sil/dil, which need a (possibly empty) REX prefix */
    int base;      /* OP_MEM: 0..15, BASE_RIP or NO_REG */
    int index;     /* OP_MEM: 0..15 (never rsp) or NO_REG */
    int scale;     /* OP_MEM: 1, 2, 4 or 8 */
    int64_t imm;   /* OP_IMM: value. OP_MEM: displacement */
    char sym[256]; /* OP_MEM (rip only) / OP_SYM: symbol name, "" if none */
    bool plt;      /* OP_SYM written as name@PLT */
} Operand;

static const char *const REG64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *const REG32[16] = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                      "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const REG8[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                     "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

static bool lookup_reg(const char *s, int *reg, int *size) {
    for (int i = 0; i < 16; i++) {
        if (strcmp(s, REG64[i]) == 0) { *reg = i; *size = 64; return true; }
        if (strcmp(s, REG32[i]) == 0) { *reg = i; *size = 32; return true; }
        if (strcmp(s, REG8[i]) == 0) { *reg = i; *size = 8; return true; }
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

/* Parses an index term "reg*scale" or "scale*reg". Returns false if the term
 * is not of that shape (an error is reported only for malformed shapes). */
static bool parse_scaled(Asm *a, const char *term, int *reg, int *scale, bool *ok) {
    const char *star = strchr(term, '*');
    *ok = false;
    if (!star) return false;
    char left[64], right[64];
    size_t ln = (size_t)(star - term);
    if (ln >= sizeof left || strlen(star + 1) >= sizeof right) {
        asm_error(a, "malformed scaled index '%s'", term);
        return true;
    }
    memcpy(left, term, ln);
    left[ln] = '\0';
    strcpy(right, star + 1);
    int sz;
    int64_t sc;
    if (lookup_reg(left, reg, &sz) && parse_int(right, &sc)) {
    } else if (lookup_reg(right, reg, &sz) && parse_int(left, &sc)) {
    } else {
        asm_error(a, "malformed scaled index '%s' (expected reg*1, reg*2, reg*4 or reg*8)", term);
        return true;
    }
    if (sz != 64 || (sc != 1 && sc != 2 && sc != 4 && sc != 8)) {
        asm_error(a, "invalid scaled index '%s' (64-bit register, scale 1, 2, 4 or 8)", term);
        return true;
    }
    *scale = (int)sc;
    *ok = true;
    return true;
}

/* Parses "[ terms ]" where terms are joined by + or -:
 *   rip (first; then only a symbol and/or displacement may follow),
 *   a 64-bit base register, an index "reg*scale" (or a second plain register),
 *   integer displacements and, for rip only, one symbol. */
static bool parse_mem(Asm *a, char *s, Operand *op) {
    size_t n = strlen(s);
    if (n < 2 || s[0] != '[' || s[n - 1] != ']') {
        asm_error(a, "malformed memory operand '%s'", s);
        return false;
    }
    s[n - 1] = '\0';
    char *p = s + 1;
    op->kind = OP_MEM;
    op->base = NO_REG;
    op->index = NO_REG;
    op->scale = 1;
    op->imm = 0;
    op->sym[0] = '\0';
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
        } else if (*p == '-') { /* leading negative displacement: [-8 + rbp] */
            sign = -1;
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
        int r, sz, sc;
        bool ok;
        if (!*term) {
            asm_error(a, "empty term in memory operand");
            return false;
        }
        if (strcmp(term, "rip") == 0) {
            if (!first || sign < 0) {
                asm_error(a, "'rip' must be the first term of a memory operand");
                return false;
            }
            op->base = BASE_RIP;
        } else if (parse_scaled(a, term, &r, &sc, &ok)) {
            if (!ok) return false;
            if (sign < 0 || op->index != NO_REG || op->base == BASE_RIP) {
                asm_error(a, "invalid index register in memory operand");
                return false;
            }
            op->index = r;
            op->scale = sc;
        } else if (lookup_reg(term, &r, &sz)) {
            if (sz != 64) {
                asm_error(a, "memory operands need 64-bit registers, got '%s'", term);
                return false;
            }
            if (sign < 0 || op->base == BASE_RIP) {
                asm_error(a, "invalid register '%s' in memory operand", term);
                return false;
            }
            if (op->base == NO_REG) op->base = r;
            else if (op->index == NO_REG) op->index = r;
            else {
                asm_error(a, "memory operand has more than two registers");
                return false;
            }
        } else if (parse_int(term, &v)) {
            if (v < INT32_MIN || v > INT32_MAX) {
                asm_error(a, "memory displacement out of 32-bit range");
                return false;
            }
            op->imm += sign * v;
        } else {
            if (op->base != BASE_RIP) {
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
    if (op->base == NO_REG && op->index == NO_REG) {
        asm_error(a, "memory operand needs a base or index register (absolute addresses are not supported)");
        return false;
    }
    if (op->index == 4) {
        asm_error(a, "rsp cannot be an index register");
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
    static const struct { const char *prefix; int size; } SIZES[] = {
        {"qword ptr", 64}, {"dword ptr", 32}, {"byte ptr", 8}};
    for (size_t i = 0; i < sizeof SIZES / sizeof *SIZES; i++) {
        size_t n = strlen(SIZES[i].prefix);
        if (strncmp(s, SIZES[i].prefix, n) == 0 && (s[n] == ' ' || s[n] == '\t' || s[n] == '[')) {
            char *rest = trim(s + n);
            if (*rest != '[') {
                asm_error(a, "'%s' must be followed by a memory operand", SIZES[i].prefix);
                return false;
            }
            if (!parse_mem(a, rest, op)) return false;
            op->size = SIZES[i].size;
            return true;
        }
    }
    if (strstr(s, " ptr")) {
        asm_error(a, "unsupported size qualifier in '%s' (byte ptr, dword ptr or qword ptr)", s);
        return false;
    }
    if (*s == '[') return parse_mem(a, s, op);
    if (lookup_reg(s, &op->reg, &op->size)) {
        op->kind = OP_REG;
        op->rex_byte = op->size == 8 && op->reg >= 4 && op->reg <= 7;
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

static uint8_t modrm(int mod, int reg, int rm) { return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)); }

static bool fits_i8(int64_t v) { return v >= -128 && v <= 127; }
static bool fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* Records a PC-relative field of `size` bytes at the current end of the
 * section and emits zero bytes for it. `addend` already accounts for any
 * bytes that follow the field in the same instruction, so that the result
 * is relative to the address of the next instruction, as the CPU computes. */
static void add_fixup(Asm *a, const char *sym, int64_t addend, FixKind kind, size_t jump) {
    int size = kind == FX_REL8 ? 1 : kind == FX_ABS64 ? 8 : 4;
    Fixup f;
    f.section = a->cur;
    f.offset = code(a)->len;
    f.symbol = obj_intern_symbol(a->o, sym);
    f.addend = kind == FX_ABS64 ? addend : addend - size; /* absolute: no PC bias */
    f.kind = kind;
    f.jump = jump;
    f.line = a->line;
    a->fixups = xrealloc(a->fixups, (a->nfixups + 1) * sizeof *a->fixups);
    a->fixups[a->nfixups++] = f;
    buf_zeros(code(a), (size_t)size);
}

static int scale_bits(int scale) { return scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3; }

/* Emits [REX] opcode ModRM [SIB] [disp] for an instruction whose ModRM.reg
 * field holds `reg` (a register number or an opcode extension) and whose
 * ModRM.rm operand is `rm` (a register or a memory operand). `imm_size` is
 * the number of immediate bytes the caller emits afterwards (it matters for
 * RIP-relative fixups). `force_rex` emits an empty REX prefix, which byte
 * registers spl/bpl/sil/dil require. The encodings follow GNU as:
 *   [base]          mod=00 (mod=01 disp8 0 when base is rbp/r13)
 *   [base+disp8]    mod=01      [base+disp32]  mod=10
 *   rsp/r12 bases and any index register use a SIB byte;
 *   [index*s+disp]  SIB with base=101, mod=00, disp32;   [rip+disp]  mod=00 rm=101. */
static void encode(Asm *a, int w, int reg, bool force_rex, const uint8_t *opc, int nopc, const Operand *rm,
                   int imm_size) {
    Buf *b = code(a);
    int x = 0, base = 0;
    if (rm->kind == OP_REG) base = rm->reg;
    else {
        if (rm->index >= 0) x = rm->index;
        if (rm->base >= 0) base = rm->base;
    }
    uint8_t rex = (uint8_t)(0x40 | (w << 3) | (((reg >> 3) & 1) << 2) | (((x >> 3) & 1) << 1) | ((base >> 3) & 1));
    if (rex != 0x40 || force_rex) buf_byte(b, rex);
    buf_append(b, opc, (size_t)nopc);
    if (rm->kind == OP_REG) {
        buf_byte(b, modrm(3, reg, rm->reg));
        return;
    }
    if (rm->base == BASE_RIP) {
        buf_byte(b, modrm(0, reg, 5));
        if (rm->sym[0]) add_fixup(a, rm->sym, rm->imm - imm_size, FX_PC32, 0);
        else buf_u32(b, (uint32_t)rm->imm);
        return;
    }
    if (rm->base == NO_REG) { /* [index*scale + disp32] */
        buf_byte(b, modrm(0, reg, 4));
        buf_byte(b, (uint8_t)((scale_bits(rm->scale) << 6) | ((rm->index & 7) << 3) | 5));
        buf_u32(b, (uint32_t)rm->imm);
        return;
    }
    int mod = (rm->imm == 0 && (rm->base & 7) != 5) ? 0 : fits_i8(rm->imm) ? 1 : 2;
    if (rm->index != NO_REG || (rm->base & 7) == 4) {
        int idx = rm->index != NO_REG ? rm->index : 4; /* 100 = no index */
        buf_byte(b, modrm(mod, reg, 4));
        buf_byte(b, (uint8_t)((scale_bits(rm->scale) << 6) | ((idx & 7) << 3) | (rm->base & 7)));
    } else {
        buf_byte(b, modrm(mod, reg, rm->base));
    }
    if (mod == 1) buf_byte(b, (uint8_t)rm->imm);
    else if (mod == 2) buf_u32(b, (uint32_t)rm->imm);
}

static void emit_rex(Buf *b, int w, int r, int x, int base, bool force) {
    uint8_t rex = (uint8_t)(0x40 | (w << 3) | ((r >> 3) << 2) | ((x >> 3) << 1) | (base >> 3));
    if (rex != 0x40 || force) buf_byte(b, rex);
}

static bool want(Asm *a, const char *mn, int nops, int expected) {
    if (nops != expected) {
        asm_error(a, "'%s' expects %d operand%s, got %d", mn, expected, expected == 1 ? "" : "s", nops);
        return false;
    }
    return true;
}

static void bad_operands(Asm *a, const char *mn) { asm_error(a, "unsupported operand combination for '%s'", mn); }

static bool is_rm(const Operand *o) { return o->kind == OP_REG || o->kind == OP_MEM; }

/* Operand size of a register or a size-qualified memory operand (0 if unknown). */
static int opsize(const Operand *o) { return o->size; }

/* For reg/mem pairs: the size both must agree on. Reports an error and returns
 * 0 if they conflict or neither is known. */
static int pair_size(Asm *a, const char *mn, const Operand *x, const Operand *y) {
    int sx = is_rm(x) ? opsize(x) : 0, sy = is_rm(y) ? opsize(y) : 0;
    if (sx && sy && sx != sy) {
        asm_error(a, "operand size mismatch for '%s'", mn);
        return 0;
    }
    int s = sx ? sx : sy;
    if (!s) asm_error(a, "operand size of '%s' is ambiguous (use qword ptr / dword ptr / byte ptr)", mn);
    return s;
}

static bool needs_rex(const Operand *o) { return o->kind == OP_REG && o->rex_byte; }

/* ------------------------------------------------------------------------ */
/* Instruction tables                                                       */

/* Classic two-operand ALU group. Encodings (GNU as choices):
 *   op r/m, r      rr  /r          op r, r/m(mem)  rr+2 /r
 *   op r/m, imm8   83  /ext ib     (imm sign-extended)
 *   op rax, imm32  acc id          (accumulator short form)
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

/* Jcc/SETcc/CMOVcc condition codes (low nibble of the opcode). */
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

static int cond_code(const char *suffix) {
    for (size_t i = 0; i < sizeof CONDS / sizeof *CONDS; i++)
        if (strcmp(suffix, CONDS[i].name) == 0) return CONDS[i].cc;
    return -1;
}

/* Returns the condition code for "jcc" mnemonics, 16 for "jmp", -1 otherwise. */
static int jump_kind(const char *mn) {
    if (strcmp(mn, "jmp") == 0) return 16;
    if (mn[0] != 'j') return -1;
    return cond_code(mn + 1);
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
        add_fixup(a, ops[0].sym, 0, FX_JMP32, 0);
    }
}

static bool imm_fits(Asm *a, int64_t v, int size) {
    bool ok = size == 8 ? (v >= -128 && v <= 255)
            : size == 32 ? (v >= INT32_MIN && v <= (int64_t)UINT32_MAX)
            : fits_i32(v);
    if (!ok) asm_error(a, "immediate %lld out of range for a %d-bit operand", (long long)v, size);
    return ok;
}

static void assemble_alu(Asm *a, const AluOp *op, Operand *ops, int n) {
    const char *mn = op->name;
    if (!want(a, mn, n, 2)) return;
    Operand *d = &ops[0], *s = &ops[1];
    Buf *b = code(a);
    if (!is_rm(d) || (s->kind != OP_REG && s->kind != OP_MEM && s->kind != OP_IMM) ||
        (d->kind == OP_MEM && s->kind == OP_MEM)) {
        bad_operands(a, mn);
        return;
    }
    int size = pair_size(a, mn, d, s);
    if (!size) return;
    if (size == 8) { asm_error(a, "8-bit operands are not supported for '%s'", mn); return; }
    int w = size == 64;
    if (s->kind == OP_REG) { /* op r/m, r */
        uint8_t opc = op->rr;
        encode(a, w, s->reg, false, &opc, 1, d, 0);
        return;
    }
    if (s->kind == OP_MEM) { /* op r, m */
        uint8_t opc = (uint8_t)(op->rr + 2);
        encode(a, w, d->reg, false, &opc, 1, s, 0);
        return;
    }
    if (!imm_fits(a, s->imm, size)) return;
    if (fits_i8(s->imm)) {
        uint8_t opc = 0x83;
        encode(a, w, op->ext, false, &opc, 1, d, 1);
        buf_byte(b, (uint8_t)s->imm);
    } else if (d->kind == OP_REG && d->reg == 0) {
        emit_rex(b, w, 0, 0, 0, false);
        buf_byte(b, op->acc);
        buf_u32(b, (uint32_t)s->imm);
    } else {
        uint8_t opc = 0x81;
        encode(a, w, op->ext, false, &opc, 1, d, 4);
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
    if (strcmp(mn, "cqo") == 0) { /* REX.W 99: sign-extend rax into rdx:rax */
        if (want(a, mn, n, 0)) { buf_byte(b, 0x48); buf_byte(b, 0x99); }
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
        if (d->kind == OP_REG && s->kind == OP_IMM) {
            if (d->size == 8) {
                if (!imm_fits(a, s->imm, 8)) return;
                /* B0+rb ib */
                emit_rex(b, 0, 0, 0, d->reg, d->rex_byte);
                buf_byte(b, (uint8_t)(0xB0 + (d->reg & 7)));
                buf_byte(b, (uint8_t)s->imm);
            } else if (d->size == 32) {
                if (!imm_fits(a, s->imm, 32)) return;
                /* B8+rd id MOV r32, imm32 (zero-extends into the 64-bit register) */
                emit_rex(b, 0, 0, 0, d->reg, false);
                buf_byte(b, (uint8_t)(0xB8 + (d->reg & 7)));
                buf_u32(b, (uint32_t)s->imm);
            } else if (fits_i32(s->imm)) {
                /* REX.W C7 /0 id MOV r/m64, imm32 (sign-extended) */
                uint8_t opc = 0xC7;
                encode(a, 1, 0, false, &opc, 1, d, 4);
                buf_u32(b, (uint32_t)s->imm);
            } else {
                /* REX.W B8+rd io MOV r64, imm64 (movabs) */
                emit_rex(b, 1, 0, 0, d->reg, false);
                buf_byte(b, (uint8_t)(0xB8 + (d->reg & 7)));
                buf_u64(b, (uint64_t)s->imm);
            }
            return;
        }
        if (d->kind == OP_MEM && s->kind == OP_IMM) {
            int size = opsize(d);
            if (!size) { asm_error(a, "operand size of 'mov' is ambiguous (use qword ptr / dword ptr / byte ptr)"); return; }
            if (!imm_fits(a, s->imm, size)) return;
            uint8_t opc = size == 8 ? 0xC6 : 0xC7; /* C6 /0 ib, C7 /0 id */
            encode(a, size == 64, 0, false, &opc, 1, d, size == 8 ? 1 : 4);
            if (size == 8) buf_byte(b, (uint8_t)s->imm);
            else buf_u32(b, (uint32_t)s->imm);
            return;
        }
        if (!is_rm(d) || !is_rm(s) || (d->kind == OP_MEM && s->kind == OP_MEM)) { bad_operands(a, mn); return; }
        int size = pair_size(a, mn, d, s);
        if (!size) return;
        if (s->kind == OP_REG) { /* 89 /r MOV r/m, r   (88 for 8-bit) */
            uint8_t opc = size == 8 ? 0x88 : 0x89;
            encode(a, size == 64, s->reg, needs_rex(s) || needs_rex(d), &opc, 1, d, 0);
        } else { /* 8B /r MOV r, m   (8A for 8-bit) */
            uint8_t opc = size == 8 ? 0x8A : 0x8B;
            encode(a, size == 64, d->reg, needs_rex(d), &opc, 1, s, 0);
        }
        return;
    }
    if (strcmp(mn, "movzx") == 0) { /* 0F B6 /r MOVZX r32/64, r/m8 */
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size == 8 || !is_rm(s) || (s->kind == OP_REG ? s->size != 8 : s->size != 8)) {
            bad_operands(a, mn);
            return;
        }
        static const uint8_t opc[2] = {0x0F, 0xB6};
        encode(a, d->size == 64, d->reg, needs_rex(s), opc, 2, s, 0);
        return;
    }
    if (strcmp(mn, "lea") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size != 64 || s->kind != OP_MEM) { bad_operands(a, mn); return; }
        uint8_t opc = 0x8D; /* REX.W 8D /r LEA r64, m */
        encode(a, 1, d->reg, false, &opc, 1, s, 0);
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
        if (d->kind == OP_REG && s->kind == OP_MEM) { Operand t = *d; *d = *s; *s = t; } /* test is symmetric */
        if (!is_rm(d)) { bad_operands(a, mn); return; }
        if (s->kind == OP_REG) { /* 85 /r TEST r/m, r   (84 for 8-bit) */
            int size = pair_size(a, mn, d, s);
            if (!size) return;
            uint8_t opc = size == 8 ? 0x84 : 0x85;
            encode(a, size == 64, s->reg, needs_rex(s) || needs_rex(d), &opc, 1, d, 0);
            return;
        }
        if (s->kind != OP_IMM) { bad_operands(a, mn); return; }
        int size = opsize(d);
        if (!size) { asm_error(a, "operand size of 'test' is ambiguous (use qword ptr / dword ptr / byte ptr)"); return; }
        if (!imm_fits(a, s->imm, size)) return;
        if (d->kind == OP_REG && d->reg == 0) { /* A8 ib / A9 id: TEST al/eax/rax, imm */
            emit_rex(b, size == 64, 0, 0, 0, false);
            buf_byte(b, size == 8 ? 0xA8 : 0xA9);
        } else { /* F6 /0 ib, F7 /0 id */
            uint8_t opc = size == 8 ? 0xF6 : 0xF7;
            encode(a, size == 64, 0, needs_rex(d), &opc, 1, d, size == 8 ? 1 : 4);
        }
        if (size == 8) buf_byte(b, (uint8_t)s->imm);
        else buf_u32(b, (uint32_t)s->imm);
        return;
    }
    if (strcmp(mn, "imul") == 0) {
        if (n != 2 && n != 3) { asm_error(a, "'imul' expects 2 or 3 operands, got %d", n); return; }
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size == 8 || !is_rm(s)) { bad_operands(a, mn); return; }
        int size = pair_size(a, mn, d, s);
        if (!size) return;
        if (n == 2) { /* 0F AF /r IMUL r, r/m */
            static const uint8_t opc[2] = {0x0F, 0xAF};
            encode(a, size == 64, d->reg, false, opc, 2, s, 0);
            return;
        }
        if (ops[2].kind != OP_IMM || !fits_i32(ops[2].imm)) { bad_operands(a, mn); return; }
        bool small = fits_i8(ops[2].imm); /* 6B /r ib  or  69 /r id: IMUL r, r/m, imm */
        uint8_t opc = small ? 0x6B : 0x69;
        encode(a, size == 64, d->reg, false, &opc, 1, s, small ? 1 : 4);
        if (small) buf_byte(b, (uint8_t)ops[2].imm);
        else buf_u32(b, (uint32_t)ops[2].imm);
        return;
    }
    if (strcmp(mn, "neg") == 0 || strcmp(mn, "not") == 0 || strcmp(mn, "idiv") == 0) {
        if (!want(a, mn, n, 1)) return;
        Operand *d = &ops[0];
        int size = is_rm(d) ? opsize(d) : 0;
        if (!size) { if (is_rm(d)) asm_error(a, "operand size of '%s' is ambiguous", mn); else bad_operands(a, mn); return; }
        if (size == 8) { asm_error(a, "8-bit operands are not supported for '%s'", mn); return; }
        int ext = mn[0] == 'n' ? (mn[1] == 'e' ? 3 : 2) : 7; /* F7 /3 NEG, /2 NOT, /7 IDIV */
        uint8_t opc = 0xF7;
        encode(a, size == 64, ext, false, &opc, 1, d, 0);
        return;
    }
    if (strcmp(mn, "shl") == 0 || strcmp(mn, "shr") == 0 || strcmp(mn, "sar") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        int size = is_rm(d) ? opsize(d) : 0;
        if (!size || s->kind != OP_IMM) { bad_operands(a, mn); return; }
        if (size == 8) { asm_error(a, "8-bit operands are not supported for '%s'", mn); return; }
        if (s->imm < 0 || s->imm >= size) { asm_error(a, "shift count %lld out of range", (long long)s->imm); return; }
        int ext = mn[1] == 'h' ? (mn[2] == 'l' ? 4 : 5) : 7; /* /4 SHL, /5 SHR, /7 SAR */
        if (s->imm == 1) { /* D1 /n: shift by one */
            uint8_t opc = 0xD1;
            encode(a, size == 64, ext, false, &opc, 1, d, 0);
        } else { /* C1 /n ib */
            uint8_t opc = 0xC1;
            encode(a, size == 64, ext, false, &opc, 1, d, 1);
            buf_byte(b, (uint8_t)s->imm);
        }
        return;
    }
    if (strncmp(mn, "set", 3) == 0 && cond_code(mn + 3) >= 0) { /* 0F 90+cc /0 SETcc r/m8 */
        if (!want(a, mn, n, 1)) return;
        Operand *d = &ops[0];
        if (!is_rm(d) || opsize(d) != 8) { bad_operands(a, mn); return; }
        uint8_t opc[2] = {0x0F, (uint8_t)(0x90 + cond_code(mn + 3))};
        encode(a, 0, 0, needs_rex(d), opc, 2, d, 0);
        return;
    }
    if (strncmp(mn, "cmov", 4) == 0 && cond_code(mn + 4) >= 0) { /* 0F 40+cc /r CMOVcc r, r/m */
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size == 8 || !is_rm(s)) { bad_operands(a, mn); return; }
        int size = pair_size(a, mn, d, s);
        if (!size) return;
        uint8_t opc[2] = {0x0F, (uint8_t)(0x40 + cond_code(mn + 4))};
        encode(a, size == 64, d->reg, false, opc, 2, s, 0);
        return;
    }
    if (strcmp(mn, "call") == 0) {
        if (!want(a, mn, n, 1)) return;
        if (ops[0].kind == OP_REG && ops[0].size == 64) { /* FF /2 CALL r64 (indirect) */
            uint8_t opc = 0xFF;
            encode(a, 0, 2, false, &opc, 1, &ops[0], 0);
            return;
        }
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
            if (quad && !parse_int(ops[i], &v)) { /* .quad SYMBOL [+|- N]: an absolute address */
                char sym[256];
                int64_t add = 0;
                size_t len = strcspn(ops[i], "+- \t");
                if (len == 0 || len >= sizeof sym) { asm_error(a, "invalid quad value '%s'", ops[i]); return; }
                memcpy(sym, ops[i], len);
                sym[len] = '\0';
                char *rest = trim(ops[i] + len);
                if (*rest) {
                    char sign = *rest;
                    char *num = trim(rest + 1);
                    if ((sign != '+' && sign != '-') || !parse_int(num, &add)) { asm_error(a, "invalid quad value '%s'", ops[i]); return; }
                    if (sign == '-') add = -add;
                }
                if (!valid_symbol(sym)) { asm_error(a, "invalid quad value '%s'", ops[i]); return; }
                add_fixup(a, sym, add, FX_ABS64, 0);
                continue;
            }
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
 *  - Local symbol in another section: R_X86_64_PC32 (also for calls and
 *    jumps) against that section's section symbol with addend += symbol
 *    offset (".L" names never reach the symbol table).
 *  - Global or undefined symbol: relocation against the symbol itself, so the
 *    linker (or dynamic linker) can interpose/resolve it.
 *  - Jumps (rel8 and rel32) to a symbol defined in their own section are
 *    resolved here even when it is global (GNU as does the same; calls keep
 *    their relocation). Short jumps must also be in range; otherwise they are
 *    marked long for the next pass. */
static void resolve_fixups(Asm *a) {
    /* GNU as creates the relocations of long (relaxed) jumps only when it
     * finishes relaxation, after all others; lasm emits them in the same
     * order so that the relocation tables are identical. */
    for (size_t k = 0; k < 2 * a->nfixups; k++) {
        size_t i = k % a->nfixups;
        if ((k < a->nfixups) == (a->fixups[i].kind == FX_JMP32)) continue;
        Fixup *f = &a->fixups[i];
        ObjSymbol *s = &a->o->symbols[f->symbol];
        a->line = f->line;
        if (s->section == OBJ_UNDEF && s->local_label) {
            asm_error(a, "undefined local label '%s'", s->name);
            continue;
        }
        if (f->kind == FX_ABS64) { /* never resolved here: the address is only known at link time */
            if (s->section == OBJ_UNDEF) {
                s->global = true;
                obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, OBJ_R_X86_64_64, f->symbol, -1, f->addend});
            } else if (!s->global) {
                obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, OBJ_R_X86_64_64, -1, s->section,
                                               f->addend + (int64_t)s->value});
            } else {
                obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, OBJ_R_X86_64_64, f->symbol, -1, f->addend});
            }
            continue;
        }
        bool local_same = s->section != OBJ_UNDEF && !s->global && s->section == f->section;
        /* jumps bind to any definition in their own section, global or not */
        if (f->kind == FX_REL8 || f->kind == FX_JMP32) local_same = s->section != OBJ_UNDEF && s->section == f->section;
        int64_t v = local_same ? (int64_t)s->value + f->addend - (int64_t)f->offset : 0;
        if (f->kind == FX_REL8) {
            if (local_same && fits_i8(v)) a->o->sections[f->section].data.data[f->offset] = (uint8_t)v;
            else mark_long(a, f->jump);
            continue;
        }
        uint32_t rtype = f->kind == FX_PC32 ? OBJ_R_X86_64_PC32 : OBJ_R_X86_64_PLT32; /* PLT32 for calls and jumps */
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
            /* a local target needs no PLT: calls and jumps to it use PC32 too */
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, OBJ_R_X86_64_PC32, -1, s->section,
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
