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

#define MAX_LINE 4096
#define MAX_OPERANDS 3

typedef struct {
    int section;     /* section containing the field */
    uint64_t offset; /* offset of the 32-bit field */
    int symbol;      /* target symbol (index into ObjFile.symbols) */
    int64_t addend;  /* already includes the -4 PC bias */
    uint32_t type;   /* OBJ_R_X86_64_PC32 or OBJ_R_X86_64_PLT32 */
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

typedef enum { OP_REG, OP_IMM, OP_MEM_RIP, OP_SYM } OpKind;

typedef struct {
    OpKind kind;
    int reg;       /* OP_REG: 0..15 */
    int size;      /* OP_REG: 32 or 64 */
    int64_t imm;   /* OP_IMM value; OP_MEM_RIP/OP_SYM: constant addend */
    char sym[256]; /* OP_MEM_RIP/OP_SYM: symbol name, "" if none */
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

/* Parses "[rip + sym + n]" style memory operands. Only RIP-relative
 * addressing is supported in this milestone. */
static bool parse_mem(Asm *a, char *s, Operand *op) {
    size_t n = strlen(s);
    if (n < 2 || s[n - 1] != ']') {
        asm_error(a, "malformed memory operand '%s'", s);
        return false;
    }
    s[n - 1] = '\0';
    char *p = s + 1;
    op->kind = OP_MEM_RIP;
    op->imm = 0;
    op->sym[0] = '\0';
    bool saw_rip = false;
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
            if (strcmp(term, "rip") != 0) {
                asm_error(a, "unsupported memory operand: only [rip + symbol] addressing is supported");
                return false;
            }
            saw_rip = true;
        } else if (parse_int(term, &v)) {
            op->imm += sign * v;
        } else if (lookup_reg(term, &r, &sz)) {
            asm_error(a, "unsupported memory operand: only [rip + symbol] addressing is supported");
            return false;
        } else {
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
    if (!saw_rip) {
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

/* Records a 32-bit PC-relative field at the current end of the section. The
 * field must be the last thing in the instruction, so P + 4 is the address
 * of the next instruction, which is what the CPU adds the field to. */
static void add_fixup(Asm *a, const char *sym, int64_t addend, uint32_t type) {
    Fixup f;
    f.section = a->cur;
    f.offset = code(a)->len;
    f.symbol = obj_intern_symbol(a->o, sym);
    f.addend = addend - 4;
    f.type = type;
    f.line = a->line;
    a->fixups = xrealloc(a->fixups, (a->nfixups + 1) * sizeof *a->fixups);
    a->fixups[a->nfixups++] = f;
    buf_u32(code(a), 0);
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
/* Instructions                                                             */

static void assemble_insn(Asm *a, const char *mn, Operand *ops, int n) {
    if (a->cur >= 0 && !(a->o->sections[a->cur].flags & OBJ_SHF_EXECINSTR)) {
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
        { bad_operands(a, mn); return; }
    }
    if (strcmp(mn, "xor") == 0 || strcmp(mn, "add") == 0 || strcmp(mn, "sub") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        /* opcode for "op r/m, r" and the /digit for "op r/m, imm" */
        uint8_t rr = mn[0] == 'x' ? 0x31 : mn[0] == 'a' ? 0x01 : 0x29;
        int ext = mn[0] == 'x' ? 6 : mn[0] == 'a' ? 0 : 5;
        if (d->kind != OP_REG) { bad_operands(a, mn); return; }
        if (s->kind == OP_REG) {
            if (d->size != s->size) { bad_operands(a, mn); return; }
            emit_rex(b, d->size == 64, s->reg, 0, d->reg, false);
            buf_byte(b, rr);
            buf_byte(b, modrm(3, s->reg, d->reg));
            return;
        }
        if (s->kind == OP_IMM) {
            if (!fits_i32(s->imm)) {
                asm_error(a, "immediate out of 32-bit range");
                return;
            }
            emit_rex(b, d->size == 64, 0, 0, d->reg, false);
            if (fits_i8(s->imm)) { /* 83 /digit ib (sign-extended imm8) */
                buf_byte(b, 0x83);
                buf_byte(b, modrm(3, ext, d->reg));
                buf_byte(b, (uint8_t)s->imm);
            } else { /* 81 /digit id */
                buf_byte(b, 0x81);
                buf_byte(b, modrm(3, ext, d->reg));
                buf_u32(b, (uint32_t)s->imm);
            }
            return;
        }
        { bad_operands(a, mn); return; }
    }
    if (strcmp(mn, "lea") == 0) {
        if (!want(a, mn, n, 2)) return;
        Operand *d = &ops[0], *s = &ops[1];
        if (d->kind != OP_REG || d->size != 64 || s->kind != OP_MEM_RIP) { bad_operands(a, mn); return; }
        /* REX.W 8D /r LEA r64, m ; ModRM mod=00 rm=101 => [rip + disp32] */
        emit_rex(b, 1, d->reg, 0, 0, false);
        buf_byte(b, 0x8D);
        buf_byte(b, modrm(0, d->reg, 5));
        if (s->sym[0]) add_fixup(a, s->sym, s->imm, OBJ_R_X86_64_PC32);
        else buf_u32(b, (uint32_t)s->imm);
        return;
    }
    if (strcmp(mn, "call") == 0) {
        if (!want(a, mn, n, 1)) return;
        if (ops[0].kind != OP_SYM) { bad_operands(a, mn); return; }
        /* E8 cd CALL rel32. Always recorded as PLT32 so the linker may route
         * calls to shared-library functions through the PLT. */
        buf_byte(b, 0xE8);
        add_fixup(a, ops[0].sym, 0, OBJ_R_X86_64_PLT32);
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
        char *e = ops[1];
        /* tolerate spaces: ". - main" */
        char compact[300];
        size_t k = 0;
        for (; *e && k + 1 < sizeof compact; e++)
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
    if (strcmp(name, ".byte") == 0) {
        int n = split_operands(args, ops, 16);
        if (n <= 0) { asm_error(a, "'.byte' expects 1 to 16 values per line"); return; }
        for (int i = 0; i < n; i++) {
            int64_t v;
            if (!parse_int(ops[i], &v) || v < -128 || v > 255) {
                asm_error(a, "invalid byte value '%s'", ops[i]);
                return;
            }
            buf_byte(code(a), (uint8_t)v);
        }
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

/* Resolves every recorded fixup after all labels are known.
 *  - Local symbol in the same section: patched now (S + A - P); no relocation.
 *  - Local symbol in another section: relocation against that section's
 *    section symbol with addend += symbol offset (".L" names never reach the
 *    symbol table).
 *  - Global or undefined symbol: relocation against the symbol itself, so the
 *    linker (or dynamic linker) can interpose/resolve it. */
static void resolve_fixups(Asm *a) {
    for (size_t i = 0; i < a->nfixups; i++) {
        Fixup *f = &a->fixups[i];
        ObjSymbol *s = &a->o->symbols[f->symbol];
        a->line = f->line;
        if (s->section == OBJ_UNDEF) {
            if (s->local_label) {
                asm_error(a, "undefined local label '%s'", s->name);
                continue;
            }
            s->global = true; /* undefined => external */
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, f->type, f->symbol, -1, f->addend});
        } else if (!s->global && s->section == f->section) {
            int64_t v = (int64_t)s->value + f->addend - (int64_t)f->offset;
            if (!fits_i32(v)) {
                asm_error(a, "PC-relative displacement to '%s' out of range", s->name);
                continue;
            }
            buf_patch_u32(&a->o->sections[f->section].data, f->offset, (uint32_t)v);
        } else if (!s->global) {
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, f->type, -1, s->section,
                                           f->addend + (int64_t)s->value});
        } else {
            obj_add_reloc(a->o, (ObjReloc){f->section, f->offset, f->type, f->symbol, -1, f->addend});
        }
    }
}

bool assemble(const char *path, const char *src, size_t len, ObjFile *out) {
    memset(out, 0, sizeof *out);
    Asm a = {0};
    a.path = path;
    a.o = out;
    a.cur = -1;
    out->source_name = NULL;

    size_t pos = 0;
    char line[MAX_LINE];
    while (pos < len && !a.failed) {
        a.line++;
        size_t start = pos;
        while (pos < len && src[pos] != '\n') {
            if (src[pos] == '\0') {
                asm_error(&a, "NUL byte in assembly source");
                break;
            }
            pos++;
        }
        if (a.failed) break;
        size_t n = pos - start;
        if (pos < len) pos++; /* skip '\n' */
        if (n >= MAX_LINE) {
            asm_error(&a, "line too long (max %d bytes)", MAX_LINE - 1);
            break;
        }
        memcpy(line, src + start, n);
        line[n] = '\0';
        assemble_line(&a, line);
    }
    if (!a.failed) resolve_fixups(&a);
    for (int i = 0; i < out->nsymbols && !a.failed; i++) {
        ObjSymbol *s = &out->symbols[i];
        if (s->local_label && s->section == OBJ_UNDEF) {
            a.line = 0;
            asm_error(&a, "local label '%s' used but never defined", s->name);
        }
    }
    free(a.fixups);
    if (a.failed) {
        obj_free(out);
        return false;
    }
    return true;
}
