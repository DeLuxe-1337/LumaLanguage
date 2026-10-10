/* ir_parse.c - reader for the LIR text form (grammar in docs/IR.md §4).
 *
 * Globals and block labels may be referenced before they are defined; such
 * references are recorded and resolved at the end of the module/function. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"

typedef enum { T_EOF, T_NL, T_IDENT, T_GLOBAL, T_VREG, T_INT, T_STRING, T_PUNCT } TKind;

typedef struct {
    TKind kind;
    int line;
    char text[256]; /* identifier/name (without sigil) or punctuation */
    int64_t ival;
    Buf str;        /* T_STRING decoded bytes */
} Tok;

typedef struct {
    int func, block, instr;
    int which; /* 0/1: branch target slot; -1: global reference */
    char name[256];
    int line;
} Pending;

typedef struct {
    int sid;
    char method[256], global[256];
    int line;
} PendingMethod;

typedef struct {
    const char *path;
    const char *src;
    size_t len, pos;
    int line;
    Tok tok;
    bool failed;
    IrModule *m;
    Pending *pend;
    size_t npend, cap_pend;
    PendingMethod *pmeth;
    size_t npmeth;
} P;

static void perr(P *p, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void perr(P *p, int line, const char *fmt, ...) {
    if (p->failed) return; /* report only the first error */
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", p->path, line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    p->failed = true;
}

static bool name_char(int c, bool first) {
    return isalpha(c) || c == '_' || (!first && (isdigit(c) || c == '.'));
}

/* Reads the next token into p->tok. */
static void next(P *p) {
    Tok *t = &p->tok;
    t->str.len = 0;
    for (;;) {
        while (p->pos < p->len && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t' || p->src[p->pos] == '\r'))
            p->pos++;
        if (p->pos < p->len && p->src[p->pos] == ';') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        break;
    }
    t->line = p->line;
    t->text[0] = '\0';
    if (p->pos >= p->len) { t->kind = T_EOF; return; }
    unsigned char c = (unsigned char)p->src[p->pos];
    if (c == '\n') {
        p->pos++;
        p->line++;
        t->kind = T_NL;
        return;
    }
    if (c == '@' || c == '%' || name_char(c, true)) {
        size_t start = p->pos;
        bool sigil = c == '@' || c == '%';
        if (sigil) p->pos++;
        size_t ns = p->pos;
        /* vreg names may start with a digit (%0) */
        bool first = c != '%';
        while (p->pos < p->len && name_char((unsigned char)p->src[p->pos], first && p->pos == ns)) p->pos++;
        size_t n = p->pos - ns;
        if (n == 0) {
            perr(p, p->line, "expected a name after '%c'", c);
            t->kind = T_EOF;
            return;
        }
        if (n >= sizeof t->text) {
            perr(p, p->line, "name too long");
            t->kind = T_EOF;
            return;
        }
        memcpy(t->text, p->src + ns, n);
        t->text[n] = '\0';
        t->kind = c == '@' ? T_GLOBAL : c == '%' ? T_VREG : T_IDENT;
        (void)start;
        return;
    }
    if (isdigit(c) || (c == '-' && p->pos + 1 < p->len && isdigit((unsigned char)p->src[p->pos + 1]))) {
        size_t start = p->pos;
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
        char num[64];
        size_t n = p->pos - start;
        if (n >= sizeof num) { perr(p, p->line, "integer literal too long"); t->kind = T_EOF; return; }
        memcpy(num, p->src + start, n);
        num[n] = '\0';
        errno = 0;
        long long v = strtoll(num, NULL, 10);
        if (errno == ERANGE) { perr(p, p->line, "integer literal out of range"); t->kind = T_EOF; return; }
        t->kind = T_INT;
        t->ival = v;
        return;
    }
    if (c == '"') {
        p->pos++;
        for (;;) {
            if (p->pos >= p->len || p->src[p->pos] == '\n') {
                perr(p, p->line, "unterminated string literal");
                t->kind = T_EOF;
                return;
            }
            char ch = p->src[p->pos++];
            if (ch == '"') break;
            if (ch != '\\') { buf_byte(&t->str, (uint8_t)ch); continue; }
            if (p->pos >= p->len) continue;
            char e = p->src[p->pos++];
            switch (e) {
            case '"': buf_byte(&t->str, '"'); break;
            case '\\': buf_byte(&t->str, '\\'); break;
            case 'n': buf_byte(&t->str, '\n'); break;
            case 't': buf_byte(&t->str, '\t'); break;
            case 'x': {
                int v = 0;
                for (int k = 0; k < 2; k++) {
                    char h = p->pos < p->len ? p->src[p->pos] : 0;
                    int d = isdigit((unsigned char)h) ? h - '0'
                          : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                          : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0) { perr(p, p->line, "invalid \\x escape (expected two hex digits)"); t->kind = T_EOF; return; }
                    v = v * 16 + d;
                    p->pos++;
                }
                buf_byte(&t->str, (uint8_t)v);
                break;
            }
            default:
                perr(p, p->line, "unsupported escape '\\%c' in string", e);
                t->kind = T_EOF;
                return;
            }
        }
        t->kind = T_STRING;
        return;
    }
    if (strchr("(){},=:?|.", c)) {
        p->pos++;
        t->kind = T_PUNCT;
        t->text[0] = (char)c;
        t->text[1] = '\0';
        return;
    }
    perr(p, p->line, "unexpected character '%c'", isprint(c) ? c : '?');
    t->kind = T_EOF;
}

static bool is_punct(P *p, char c) { return p->tok.kind == T_PUNCT && p->tok.text[0] == c; }
static bool is_ident(P *p, const char *s) { return p->tok.kind == T_IDENT && strcmp(p->tok.text, s) == 0; }

static const char *tok_desc(const Tok *t) {
    switch (t->kind) {
    case T_EOF: return "end of file";
    case T_NL: return "end of line";
    case T_IDENT: return "identifier";
    case T_GLOBAL: return "global name";
    case T_VREG: return "virtual register";
    case T_INT: return "integer";
    case T_STRING: return "string";
    case T_PUNCT: return "punctuation";
    }
    return "?";
}

static bool expect_punct(P *p, char c) {
    if (!is_punct(p, c)) {
        perr(p, p->tok.line, "expected '%c', found %s", c, tok_desc(&p->tok));
        return false;
    }
    next(p);
    return true;
}

static bool expect_ident(P *p, const char *s) {
    if (!is_ident(p, s)) {
        perr(p, p->tok.line, "expected '%s', found %s", s, tok_desc(&p->tok));
        return false;
    }
    next(p);
    return true;
}

static bool expect_nl(P *p) {
    if (p->tok.kind != T_NL && p->tok.kind != T_EOF) {
        perr(p, p->tok.line, "expected end of line, found %s", tok_desc(&p->tok));
        return false;
    }
    if (p->tok.kind == T_NL) next(p);
    return true;
}

static void skip_nl(P *p) {
    while (p->tok.kind == T_NL) next(p);
}

static bool take_vreg(P *p, IrFunc *f, int *out) {
    if (p->tok.kind != T_VREG) {
        perr(p, p->tok.line, "expected virtual register, found %s", tok_desc(&p->tok));
        return false;
    }
    *out = ir_func_vreg(f, p->tok.text);
    next(p);
    return true;
}

/* type := NAME ( '|' NAME )* '?'?   with NAME in int, str, bool, nil, any,
 * struct, or the name of a struct */
static bool take_type(P *p, IrTy *out) {
    char text[256] = "";
    int line = p->tok.line;
    for (;;) {
        if (p->tok.kind != T_IDENT) {
            perr(p, p->tok.line, "expected a type (int, str, bool, nil, any), found %s", tok_desc(&p->tok));
            return false;
        }
        if (strlen(text) + strlen(p->tok.text) + 2 >= sizeof text) { perr(p, line, "type too long"); return false; }
        strcat(text, p->tok.text);
        next(p);
        if (!is_punct(p, '|')) break;
        strcat(text, "|");
        next(p);
    }
    if (is_punct(p, '?')) {
        strcat(text, "?");
        next(p);
    }
    if (!ir_ty_parse(p->m, text, out)) {
        perr(p, line, "invalid type '%s' (int, str, bool, nil, any, struct, a struct name, T?, A|B)", text);
        return false;
    }
    return true;
}

static void add_pending(P *p, int func, int block, int instr, int which, const char *name, int line) {
    if (p->npend == p->cap_pend) {
        p->cap_pend = p->cap_pend ? p->cap_pend * 2 : 16;
        p->pend = xrealloc(p->pend, p->cap_pend * sizeof *p->pend);
    }
    Pending *q = &p->pend[p->npend++];
    q->func = func;
    q->block = block;
    q->instr = instr;
    q->which = which;
    snprintf(q->name, sizeof q->name, "%s", name);
    q->line = line;
}

static bool take_label_ref(P *p, int fi, int bi, int ii, int which) {
    if (p->tok.kind != T_IDENT) {
        perr(p, p->tok.line, "expected block label, found %s", tok_desc(&p->tok));
        return false;
    }
    add_pending(p, fi, bi, ii, which, p->tok.text, p->tok.line);
    next(p);
    return true;
}

static bool take_global_ref(P *p, int fi, int bi, int ii) {
    if (p->tok.kind != T_GLOBAL) {
        perr(p, p->tok.line, "expected global name, found %s", tok_desc(&p->tok));
        return false;
    }
    add_pending(p, fi, bi, ii, -1, p->tok.text, p->tok.line);
    next(p);
    return true;
}

static bool parse_call_tail(P *p, int fi, int bi) {
    IrFunc *f = &p->m->funcs[fi];
    IrInstr *in = &f->blocks[bi].instrs[f->blocks[bi].n - 1];
    int ii = f->blocks[bi].n - 1;
    if (!take_global_ref(p, fi, bi, ii)) return false;
    if (!expect_punct(p, '(')) return false;
    while (!is_punct(p, ')')) {
        if (in->nargs > 0 && !expect_punct(p, ',')) return false;
        int v;
        if (!take_vreg(p, f, &v)) return false;
        f = &p->m->funcs[fi]; /* (no reallocation of funcs here, but keep it simple) */
        in = &f->blocks[bi].instrs[ii];
        in->args = xrealloc(in->args, (size_t)(in->nargs + 1) * sizeof(int));
        in->args[in->nargs++] = v;
    }
    next(p);
    return true;
}

/* STRUCT.FIELD (static) or .FIELD (dynamic) into in->sid/field or in->name. */
static bool take_field_ref(P *p, IrInstr *in) {
    if (is_punct(p, '.')) {
        next(p);
        if (p->tok.kind != T_IDENT || strchr(p->tok.text, '.')) {
            perr(p, p->tok.line, "expected a field name after '.', found %s", tok_desc(&p->tok));
            return false;
        }
        in->sid = IR_NONE;
        in->name = xstrdup(p->tok.text);
        next(p);
        return true;
    }
    if (p->tok.kind != T_IDENT || !strchr(p->tok.text, '.')) {
        perr(p, p->tok.line, "expected STRUCT.FIELD or .FIELD, found %s", tok_desc(&p->tok));
        return false;
    }
    char sname[256];
    snprintf(sname, sizeof sname, "%s", p->tok.text);
    char *dot = strchr(sname, '.');
    *dot = '\0';
    int sid = ir_find_struct(p->m, sname);
    if (sid < 0) {
        perr(p, p->tok.line, "unknown struct '%s'", sname);
        return false;
    }
    int k = ir_struct_field(&p->m->structs[sid], dot + 1);
    if (k < 0) {
        perr(p, p->tok.line, "struct '%s' has no field '%s'", sname, dot + 1);
        return false;
    }
    in->sid = sid;
    in->field = k;
    next(p);
    return true;
}

/* (%a, %b, ...) into the last instruction of block bi */
static bool take_arg_list(P *p, int fi, int bi) {
    if (!expect_punct(p, '(')) return false;
    int ii = p->m->funcs[fi].blocks[bi].n - 1;
    while (!is_punct(p, ')')) {
        IrInstr *in = &p->m->funcs[fi].blocks[bi].instrs[ii];
        if (in->nargs > 0 && !expect_punct(p, ',')) return false;
        int v;
        if (!take_vreg(p, &p->m->funcs[fi], &v)) return false;
        in = &p->m->funcs[fi].blocks[bi].instrs[ii];
        in->args = xrealloc(in->args, (size_t)(in->nargs + 1) * sizeof(int));
        in->args[in->nargs++] = v;
    }
    next(p);
    return true;
}

/* callm %o, .NAME(%args)   (after "callm") */
static bool parse_callm_tail(P *p, int fi, int bi, int dst, int line) {
    IrFunc *f = &p->m->funcs[fi];
    int o;
    if (!take_vreg(p, f, &o) || !expect_punct(p, ',') || !expect_punct(p, '.')) return false;
    if (p->tok.kind != T_IDENT || strchr(p->tok.text, '.')) {
        perr(p, p->tok.line, "expected a method name after '.', found %s", tok_desc(&p->tok));
        return false;
    }
    IrInstr *in = ir_emit(f, bi, IR_CALLM);
    in->dst = dst;
    in->a = o;
    in->line = line;
    in->name = xstrdup(p->tok.text);
    next(p);
    return take_arg_list(p, fi, bi);
}

static const struct { const char *name; IrOp op; } BINOPS[] = {
    {"add", IR_ADD}, {"sub", IR_SUB}, {"mul", IR_MUL}, {"div", IR_DIV}, {"mod", IR_MOD}, {"eq", IR_EQ},
    {"ne", IR_NE},   {"lt", IR_LT},   {"le", IR_LE},   {"gt", IR_GT},   {"ge", IR_GE},
};

/* Parses one instruction (the current token starts it). */
static bool parse_instr(P *p, int fi, int bi) {
    IrFunc *f = &p->m->funcs[fi];
    int line = p->tok.line;
    if (p->tok.kind == T_VREG) {
        int dst;
        take_vreg(p, f, &dst);
        if (!expect_punct(p, '=')) return false;
        if (p->tok.kind != T_IDENT) {
            perr(p, p->tok.line, "expected an operation after '=', found %s", tok_desc(&p->tok));
            return false;
        }
        char op[256];
        snprintf(op, sizeof op, "%s", p->tok.text);
        next(p);
        if (strcmp(op, "const") == 0) {
            IrInstr *in;
            if (p->tok.kind == T_INT) {
                in = ir_emit(f, bi, IR_CONST_INT);
                in->imm = p->tok.ival;
                next(p);
            } else if (is_ident(p, "nil") || is_ident(p, "true") || is_ident(p, "false")) {
                in = ir_emit(f, bi, is_ident(p, "nil") ? IR_CONST_NIL : is_ident(p, "true") ? IR_CONST_TRUE : IR_CONST_FALSE);
                next(p);
            } else if (p->tok.kind == T_GLOBAL) {
                in = ir_emit(f, bi, IR_CONST_DATA);
                in->dst = dst;
                in->line = line;
                return take_global_ref(p, fi, bi, f->blocks[bi].n - 1);
            } else {
                perr(p, p->tok.line, "expected integer, nil, true, false or @data after 'const'");
                return false;
            }
            in->dst = dst;
            in->line = line;
            return true;
        }
        if (strcmp(op, "mov") == 0 || strcmp(op, "neg") == 0 || strcmp(op, "not") == 0) {
            int a;
            if (!take_vreg(p, f, &a)) return false;
            IrInstr *in = ir_emit(f, bi, op[0] == 'm' ? IR_MOV : op[0] == 'n' && op[1] == 'e' ? IR_NEG : IR_NOT);
            in->dst = dst;
            in->a = a;
            in->line = line;
            return true;
        }
        for (size_t i = 0; i < sizeof BINOPS / sizeof *BINOPS; i++) {
            if (strcmp(op, BINOPS[i].name) != 0) continue;
            int a, b;
            if (!take_vreg(p, f, &a) || !expect_punct(p, ',') || !take_vreg(p, f, &b)) return false;
            IrInstr *in = ir_emit(f, bi, BINOPS[i].op);
            in->dst = dst;
            in->a = a;
            in->b = b;
            in->line = line;
            return true;
        }
        if (strcmp(op, "call") == 0) {
            IrInstr *in = ir_emit(f, bi, IR_CALL);
            in->dst = dst;
            in->line = line;
            return parse_call_tail(p, fi, bi);
        }
        if (strcmp(op, "check") == 0) { /* %d = check %a, TYPE, @context */
            int a;
            IrTy ty;
            if (!take_vreg(p, f, &a) || !expect_punct(p, ',') || !take_type(p, &ty) || !expect_punct(p, ',')) return false;
            IrInstr *in = ir_emit(f, bi, IR_CHECK);
            in->dst = dst;
            in->a = a;
            in->ty = ty;
            in->line = line;
            return take_global_ref(p, fi, bi, f->blocks[bi].n - 1);
        }
        if (strcmp(op, "new") == 0) { /* %d = new STRUCT(%a, ...) */
            if (p->tok.kind != T_IDENT) { perr(p, p->tok.line, "expected a struct name after 'new'"); return false; }
            int sid = ir_find_struct(p->m, p->tok.text);
            if (sid < 0) { perr(p, p->tok.line, "unknown struct '%s'", p->tok.text); return false; }
            next(p);
            IrInstr *in = ir_emit(f, bi, IR_NEW);
            in->dst = dst;
            in->sid = sid;
            in->line = line;
            return take_arg_list(p, fi, bi);
        }
        if (strcmp(op, "getfield") == 0) { /* %d = getfield %o, STRUCT.FIELD | .FIELD */
            int o;
            if (!take_vreg(p, f, &o) || !expect_punct(p, ',')) return false;
            IrInstr *in = ir_emit(f, bi, IR_GETFIELD);
            in->dst = dst;
            in->a = o;
            in->line = line;
            return take_field_ref(p, in);
        }
        if (strcmp(op, "callm") == 0) return parse_callm_tail(p, fi, bi, dst, line);
        if (strcmp(op, "load") == 0) {
            IrInstr *in = ir_emit(f, bi, IR_LOAD);
            in->dst = dst;
            in->line = line;
            return take_global_ref(p, fi, bi, f->blocks[bi].n - 1);
        }
        perr(p, line, "unknown operation '%s'", op);
        return false;
    }
    if (is_ident(p, "call")) {
        next(p);
        IrInstr *in = ir_emit(f, bi, IR_CALL);
        in->line = line;
        return parse_call_tail(p, fi, bi);
    }
    if (is_ident(p, "setfield")) { /* setfield %o, STRUCT.FIELD | .FIELD, %v */
        next(p);
        int o, v;
        if (!take_vreg(p, f, &o) || !expect_punct(p, ',')) return false;
        IrInstr *in = ir_emit(f, bi, IR_SETFIELD);
        in->a = o;
        in->line = line;
        if (!take_field_ref(p, in) || !expect_punct(p, ',') || !take_vreg(p, f, &v)) return false;
        p->m->funcs[fi].blocks[bi].instrs[p->m->funcs[fi].blocks[bi].n - 1].b = v;
        return true;
    }
    if (is_ident(p, "callm")) {
        next(p);
        return parse_callm_tail(p, fi, bi, IR_NONE, line);
    }
    if (is_ident(p, "store")) {
        next(p);
        IrInstr *in = ir_emit(f, bi, IR_STORE);
        in->line = line;
        int ii = f->blocks[bi].n - 1;
        if (!take_global_ref(p, fi, bi, ii) || !expect_punct(p, ',')) return false;
        int v;
        if (!take_vreg(p, f, &v)) return false;
        p->m->funcs[fi].blocks[bi].instrs[ii].a = v;
        return true;
    }
    if (is_ident(p, "jmp")) {
        next(p);
        IrInstr *in = ir_emit(f, bi, IR_JMP);
        in->line = line;
        return take_label_ref(p, fi, bi, f->blocks[bi].n - 1, 0);
    }
    if (is_ident(p, "br")) {
        next(p);
        int c;
        if (!take_vreg(p, f, &c) || !expect_punct(p, ',')) return false;
        IrInstr *in = ir_emit(f, bi, IR_BR);
        in->a = c;
        in->line = line;
        int ii = f->blocks[bi].n - 1;
        return take_label_ref(p, fi, bi, ii, 0) && expect_punct(p, ',') && take_label_ref(p, fi, bi, ii, 1);
    }
    if (is_ident(p, "ret")) {
        next(p);
        int v;
        if (!take_vreg(p, f, &v)) return false;
        IrInstr *in = ir_emit(f, bi, IR_RET);
        in->a = v;
        in->line = line;
        return true;
    }
    perr(p, line, "expected an instruction, found %s%s%s", tok_desc(&p->tok),
         p->tok.kind == T_IDENT ? " " : "", p->tok.kind == T_IDENT ? p->tok.text : "");
    return false;
}

/* Is the current token the start of a "label:" line? Peeks one char. */
static bool at_label(P *p) {
    if (p->tok.kind != T_IDENT) return false;
    size_t q = p->pos;
    while (q < p->len && (p->src[q] == ' ' || p->src[q] == '\t')) q++;
    return q < p->len && p->src[q] == ':';
}

static bool parse_function(P *p) {
    int line = p->tok.line;
    next(p); /* 'fn' */
    if (p->tok.kind != T_GLOBAL) {
        perr(p, p->tok.line, "expected function name, found %s", tok_desc(&p->tok));
        return false;
    }
    if (ir_find_global(p->m, p->tok.text) >= 0) {
        perr(p, p->tok.line, "duplicate global '@%s'", p->tok.text);
        return false;
    }
    char name[256];
    snprintf(name, sizeof name, "%s", p->tok.text);
    next(p);
    if (!expect_punct(p, '(')) return false;
    char params[64][256];
    IrTy ptys[64];
    int np = 0;
    while (!is_punct(p, ')')) {
        if (np > 0 && !expect_punct(p, ',')) return false;
        if (p->tok.kind != T_VREG) {
            perr(p, p->tok.line, "expected parameter, found %s", tok_desc(&p->tok));
            return false;
        }
        if (np == 64) { perr(p, p->tok.line, "too many parameters"); return false; }
        for (int i = 0; i < np; i++)
            if (strcmp(params[i], p->tok.text) == 0) {
                perr(p, p->tok.line, "duplicate parameter '%%%s'", p->tok.text);
                return false;
            }
        snprintf(params[np], 256, "%s", p->tok.text);
        ptys[np] = TY_ANY;
        next(p);
        if (is_punct(p, ':')) {
            next(p);
            if (!take_type(p, &ptys[np])) return false;
        }
        np++;
    }
    next(p);
    IrTy rty = TY_ANY;
    if (is_punct(p, ':')) {
        next(p);
        if (!take_type(p, &rty)) return false;
    }
    if (!expect_punct(p, '{') || !expect_nl(p)) return false;
    int fi = ir_add_func(p->m, name, np);
    IrFunc *f = &p->m->funcs[fi];
    f->line = line;
    IrGlobal *g = &p->m->globals[f->global];
    g->ty = rty;
    bool typed = false;
    for (int i = 0; i < np; i++) typed |= ptys[i] != TY_ANY;
    if (typed) {
        g->ptys = xmalloc((size_t)np * sizeof *g->ptys);
        memcpy(g->ptys, ptys, (size_t)np * sizeof *g->ptys);
    }
    for (int i = 0; i < np; i++) ir_func_vreg(f, params[i]);
    size_t first_pending = p->npend;

    skip_nl(p);
    while (!is_punct(p, '}')) {
        if (p->tok.kind == T_EOF) { perr(p, p->tok.line, "unexpected end of file in function '@%s'", name); return false; }
        if (!at_label(p)) {
            perr(p, p->tok.line, "expected a block label ('name:')%s", f->nblocks == 0 ? " at start of function" : "");
            return false;
        }
        if (ir_func_find_block(f, p->tok.text) >= 0) {
            perr(p, p->tok.line, "duplicate block label '%s'", p->tok.text);
            return false;
        }
        int bi = ir_func_block(f, p->tok.text);
        int bline = p->tok.line;
        next(p);
        if (!expect_punct(p, ':') || !expect_nl(p)) return false;
        for (;;) {
            skip_nl(p);
            if (is_punct(p, '}') || at_label(p) || p->tok.kind == T_EOF) {
                perr(p, bline, "block '%s' does not end with a terminator (jmp, br or ret)", f->blocks[bi].label);
                return false;
            }
            if (!parse_instr(p, fi, bi) || !expect_nl(p)) return false;
            f = &p->m->funcs[fi];
            if (ir_block_terminated(&f->blocks[bi])) break;
        }
        skip_nl(p);
    }
    next(p); /* '}' */
    if (!expect_nl(p)) return false;
    if (f->nblocks == 0) {
        perr(p, line, "function '@%s' has no blocks", name);
        return false;
    }
    /* resolve this function's label references */
    for (size_t i = first_pending; i < p->npend; i++) {
        Pending *q = &p->pend[i];
        if (q->which < 0) continue;
        int b = ir_func_find_block(f, q->name);
        if (b < 0) {
            perr(p, q->line, "unknown block label '%s'", q->name);
            return false;
        }
        f->blocks[q->block].instrs[q->instr].target[q->which] = b;
    }
    return true;
}

/* Registers every "struct NAME" line first, so that types may name structs
 * declared later (a field of type Node?, a function parameter of type Point). */
static bool prescan_structs(P *p) {
    size_t i = 0;
    int line = 1;
    while (i < p->len) {
        size_t j = i;
        while (j < p->len && (p->src[j] == ' ' || p->src[j] == '\t')) j++;
        if (p->len - j > 7 && memcmp(p->src + j, "struct", 6) == 0 && (p->src[j + 6] == ' ' || p->src[j + 6] == '\t')) {
            j += 6;
            while (j < p->len && (p->src[j] == ' ' || p->src[j] == '\t')) j++;
            size_t k = j;
            while (k < p->len && name_char((unsigned char)p->src[k], k == j) && p->src[k] != '.') k++;
            char name[256];
            if (k == j || k - j >= sizeof name) {
                perr(p, line, "expected a struct name");
                return false;
            }
            memcpy(name, p->src + j, k - j);
            name[k - j] = '\0';
            if (ir_find_struct(p->m, name) >= 0) {
                perr(p, line, "duplicate struct '%s'", name);
                return false;
            }
            static const char *const reserved[] = {"int", "str", "bool", "nil", "any", "struct", "never"};
            for (size_t r = 0; r < sizeof reserved / sizeof *reserved; r++)
                if (strcmp(name, reserved[r]) == 0) {
                    perr(p, line, "'%s' is a builtin type and cannot name a struct", name);
                    return false;
                }
            int sid = ir_add_struct(p->m, name);
            p->m->structs[sid].line = line;
        }
        while (i < p->len && p->src[i] != '\n') i++;
        i++;
        line++;
    }
    return true;
}

/* struct NAME {field: type, field, ...} [methods {name = @fn, ...}] */
static bool parse_struct(P *p) {
    next(p); /* 'struct' */
    int sid = p->tok.kind == T_IDENT ? ir_find_struct(p->m, p->tok.text) : -1;
    if (sid < 0) {
        perr(p, p->tok.line, "expected a struct name, found %s", tok_desc(&p->tok));
        return false;
    }
    next(p);
    if (!expect_punct(p, '{')) return false;
    char names[256][256];
    const char *np[256];
    IrTy tys[256];
    int n = 0;
    while (!is_punct(p, '}')) {
        if (n > 0 && !expect_punct(p, ',')) return false;
        if (n == 256) { perr(p, p->tok.line, "too many fields"); return false; }
        if (p->tok.kind != T_IDENT || strchr(p->tok.text, '.')) {
            perr(p, p->tok.line, "expected a field name, found %s", tok_desc(&p->tok));
            return false;
        }
        for (int i = 0; i < n; i++)
            if (strcmp(names[i], p->tok.text) == 0) {
                perr(p, p->tok.line, "duplicate field '%s'", p->tok.text);
                return false;
            }
        snprintf(names[n], 256, "%s", p->tok.text);
        np[n] = names[n];
        tys[n] = TY_ANY;
        next(p);
        if (is_punct(p, ':')) {
            next(p);
            if (!take_type(p, &tys[n])) return false;
        }
        n++;
    }
    next(p);
    ir_struct_set_fields(p->m, sid, n, np, tys);
    if (is_ident(p, "methods")) {
        next(p);
        if (!expect_punct(p, '{')) return false;
        int k = 0;
        while (!is_punct(p, '}')) {
            if (k > 0 && !expect_punct(p, ',')) return false;
            if (p->tok.kind != T_IDENT) { perr(p, p->tok.line, "expected a method name, found %s", tok_desc(&p->tok)); return false; }
            p->pmeth = xrealloc(p->pmeth, (p->npmeth + 1) * sizeof *p->pmeth);
            PendingMethod *q = &p->pmeth[p->npmeth];
            q->sid = sid;
            q->line = p->tok.line;
            snprintf(q->method, sizeof q->method, "%s", p->tok.text);
            for (size_t i = 0; i < p->npmeth; i++)
                if (p->pmeth[i].sid == sid && strcmp(p->pmeth[i].method, q->method) == 0) {
                    perr(p, q->line, "duplicate method '%s'", q->method);
                    return false;
                }
            next(p);
            if (!expect_punct(p, '=')) return false;
            if (p->tok.kind != T_GLOBAL) { perr(p, p->tok.line, "expected a function name, found %s", tok_desc(&p->tok)); return false; }
            snprintf(q->global, sizeof q->global, "%s", p->tok.text);
            p->npmeth++;
            next(p);
            k++;
        }
        next(p);
    }
    return expect_nl(p);
}

bool ir_parse(const char *path, const char *src, size_t len, IrModule *out) {
    P p = {0};
    p.path = path;
    p.src = src;
    p.len = len;
    p.line = 1;
    if (memchr(src, '\0', len)) {
        fprintf(stderr, "%s: error: NUL byte in IR source\n", path);
        return false;
    }
    ir_module_init(out, "");
    p.m = out;
    if (!prescan_structs(&p)) goto fail;
    next(&p);
    skip_nl(&p);
    if (!expect_ident(&p, "module")) goto fail;
    if (p.tok.kind != T_STRING) { perr(&p, p.tok.line, "expected module name string"); goto fail; }
    buf_byte(&p.tok.str, 0);
    free(out->source);
    out->source = xstrdup((char *)p.tok.str.data);
    next(&p);
    if (!expect_nl(&p)) goto fail;
    for (;;) {
        skip_nl(&p);
        if (p.failed) goto fail;
        if (p.tok.kind == T_EOF) break;
        int line = p.tok.line;
        if (is_ident(&p, "extern")) {
            next(&p);
            if (is_ident(&p, "c")) { /* extern c fn @name(p: ctype, ...): ctype */
                next(&p);
                if (!expect_ident(&p, "fn")) goto fail;
                if (p.tok.kind != T_GLOBAL) { perr(&p, p.tok.line, "expected global name"); goto fail; }
                if (ir_find_global(out, p.tok.text) >= 0) { perr(&p, p.tok.line, "duplicate global '@%s'", p.tok.text); goto fail; }
                char name[256];
                snprintf(name, sizeof name, "%s", p.tok.text);
                next(&p);
                if (!expect_punct(&p, '(')) goto fail;
                CType types[64];
                char pnames[64][256];
                const char *pn[64];
                int np = 0;
                while (!is_punct(&p, ')')) {
                    if (np > 0 && !expect_punct(&p, ',')) goto fail;
                    if (np == 64) { perr(&p, p.tok.line, "too many parameters"); goto fail; }
                    if (p.tok.kind != T_IDENT) { perr(&p, p.tok.line, "expected parameter name"); goto fail; }
                    snprintf(pnames[np], 256, "%s", p.tok.text);
                    pn[np] = pnames[np];
                    next(&p);
                    if (!expect_punct(&p, ':')) goto fail;
                    if (p.tok.kind != T_IDENT) { perr(&p, p.tok.line, "expected a C type"); goto fail; }
                    char tn[256];
                    snprintf(tn, sizeof tn, "%s", p.tok.text);
                    int tline = p.tok.line;
                    next(&p);
                    bool opt = is_punct(&p, '?');
                    if (opt) next(&p);
                    if (!ctype_from_name(tn, opt, &types[np])) { perr(&p, tline, "unknown C type '%s%s'", tn, opt ? "?" : ""); goto fail; }
                    np++;
                }
                next(&p);
                if (!expect_punct(&p, ':')) goto fail;
                if (p.tok.kind != T_IDENT) { perr(&p, p.tok.line, "expected a C return type"); goto fail; }
                char rn[256];
                snprintf(rn, sizeof rn, "%s", p.tok.text);
                int rline = p.tok.line;
                next(&p);
                bool ropt = is_punct(&p, '?');
                if (ropt) next(&p);
                CType ret;
                if (!ctype_from_name(rn, ropt, &ret)) { perr(&p, rline, "unknown C type '%s%s'", rn, ropt ? "?" : ""); goto fail; }
                if (!expect_nl(&p)) goto fail;
                int g = ir_add_cextern(out, name, np, types, pn, ret);
                out->globals[g].line = line;
                continue;
            }
            if (!expect_ident(&p, "fn")) goto fail;
            if (p.tok.kind != T_GLOBAL) { perr(&p, p.tok.line, "expected global name"); goto fail; }
            if (ir_find_global(out, p.tok.text) >= 0) { perr(&p, p.tok.line, "duplicate global '@%s'", p.tok.text); goto fail; }
            char name[256];
            snprintf(name, sizeof name, "%s", p.tok.text);
            next(&p);
            if (!expect_punct(&p, '(')) goto fail;
            if (p.tok.kind != T_INT || p.tok.ival < 0 || p.tok.ival > 64) { perr(&p, p.tok.line, "expected arity (0..64)"); goto fail; }
            int arity = (int)p.tok.ival;
            next(&p);
            if (!expect_punct(&p, ')') || !expect_nl(&p)) goto fail;
            int g = ir_add_extern(out, name, arity); /* may realloc out->globals */
            out->globals[g].line = line;
        } else if (is_ident(&p, "data")) {
            next(&p);
            if (p.tok.kind != T_GLOBAL) { perr(&p, p.tok.line, "expected global name"); goto fail; }
            if (ir_find_global(out, p.tok.text) >= 0) { perr(&p, p.tok.line, "duplicate global '@%s'", p.tok.text); goto fail; }
            char name[256];
            snprintf(name, sizeof name, "%s", p.tok.text);
            next(&p);
            if (!expect_punct(&p, '=') || !expect_ident(&p, "str")) goto fail;
            if (p.tok.kind != T_STRING) { perr(&p, p.tok.line, "expected string literal"); goto fail; }
            int g = ir_add_data(out, name, p.tok.str.data ? (char *)p.tok.str.data : "", p.tok.str.len);
            out->globals[g].line = line;
            next(&p);
            if (!expect_nl(&p)) goto fail;
        } else if (is_ident(&p, "global")) {
            next(&p);
            if (p.tok.kind != T_GLOBAL) { perr(&p, p.tok.line, "expected global name"); goto fail; }
            if (ir_find_global(out, p.tok.text) >= 0) { perr(&p, p.tok.line, "duplicate global '@%s'", p.tok.text); goto fail; }
            int g = ir_add_var(out, p.tok.text);
            out->globals[g].line = line;
            next(&p);
            if (is_punct(&p, ':')) {
                next(&p);
                IrTy ty;
                if (!take_type(&p, &ty)) goto fail;
                out->globals[g].ty = ty;
            }
            if (!expect_nl(&p)) goto fail;
        } else if (is_ident(&p, "struct")) {
            if (!parse_struct(&p)) goto fail;
        } else if (is_ident(&p, "fn")) {
            if (!parse_function(&p)) goto fail;
        } else {
            perr(&p, line, "expected 'struct', 'extern', 'data', 'global' or 'fn', found %s", tok_desc(&p.tok));
            goto fail;
        }
    }
    /* resolve global references */
    for (size_t i = 0; i < p.npend; i++) {
        Pending *q = &p.pend[i];
        if (q->which >= 0) continue;
        int g = ir_find_global(out, q->name);
        if (g < 0) {
            perr(&p, q->line, "undefined global '@%s'", q->name);
            goto fail;
        }
        out->funcs[q->func].blocks[q->block].instrs[q->instr].global = g;
    }
    for (size_t i = 0; i < p.npmeth; i++) {
        PendingMethod *q = &p.pmeth[i];
        int g = ir_find_global(out, q->global);
        if (g < 0 || out->globals[g].kind != IRG_FUNC) {
            perr(&p, q->line, "method '%s' of struct '%s' must name a function, not '@%s'", q->method,
                 out->structs[q->sid].name, q->global);
            goto fail;
        }
        ir_struct_add_method(out, q->sid, q->method, g);
    }
    free(p.pmeth);
    free(p.pend);
    buf_free(&p.tok.str);
    return true;
fail:
    if (!p.failed) fprintf(stderr, "%s: error: malformed IR\n", path);
    free(p.pmeth);
    free(p.pend);
    buf_free(&p.tok.str);
    ir_module_free(out);
    return false;
}
