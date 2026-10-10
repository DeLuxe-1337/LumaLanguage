#!/usr/bin/env python3
"""Differential tester: random Luma programs, compiled natively by `luma`,
compared against an independent reference interpreter written here.

Each program is generated as a small AST, rendered to Luma source, and
evaluated by `Interp` below with Luma semantics: Lox-style values, 63-bit
fixnums (overflow is a runtime error), floor division, functions, globals,
variadic print, and gradual types (annotated slots are guarded at runtime).
Stdout, the runtime error message (if any) and the exit status must match.

Programs are mostly well typed. Deliberate type errors are injected through
an untyped identity function `dyn(x)`, so the compiler's static checker
cannot see them and they surface as runtime errors (or runtime type guards
when they reach an annotated variable, parameter or return value).

For every program the tester also checks that the emitted .lir recompiles to
identical assembly and that GNU as (validation only) assembles our .s into
identical bytes.

usage: tests/difftest.py [COUNT] [SEED]      (run from the repository root)
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

FIXMAX = 2**62 - 1
FIXMIN = -2**62
LUMA = os.environ.get("LUMA", "build/luma")
HAVE_AS = shutil.which("as") is not None


class LumaError(Exception):
    pass


class TooBig(Exception):
    """Program builds strings too large to be a useful test (e.g. s = s + s in
    nested loops grows exponentially); it is skipped, not compared."""


MAX_STR = 100_000


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def check(n):
    if n < FIXMIN or n > FIXMAX:
        raise LumaError("Integer overflow.")
    return n


def truthy(v):
    return not (v is None or v is False)


def equal(a, b):
    if type(a) is not type(b):
        return False
    return a == b


def show(v):
    if v is None:
        return "nil"
    if v is True:
        return "true"
    if v is False:
        return "false"
    return str(v)


def type_of(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    return "str"


# ---------------------------------------------------------------- AST + render
# expressions: ('int', n) ('str', s) ('nil',) ('bool', b) ('var', name)
#              ('assign', name, e) ('un', op, e) ('bin', op, l, r) ('log', op, l, r)
#              ('call', fname, [args])
# statements:  ('print', [exprs]) ('expr', e) ('var', name, e, ann|None) ('block', [stmts])
#              ('if', c, then, else|None) ('loop', counter, n, [stmts]) ('return', e)
# functions:   ('fun', name, [params], [stmts], [param anns], ret ann)   -- rendered first


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")


def ann(a):
    return ": %s" % a if a else ""


def render_e(e):
    k = e[0]
    if k == "int":
        return str(e[1]) if e[1] >= 0 else "(-%d)" % -e[1]
    if k == "str":
        return '"%s"' % esc(e[1])
    if k == "nil":
        return "nil"
    if k == "bool":
        return "true" if e[1] else "false"
    if k == "var":
        return e[1]
    if k == "assign":
        return "(%s = %s)" % (e[1], render_e(e[2]))
    if k == "un":
        return "(%s%s)" % (e[1], render_e(e[2]))
    if k == "call":
        return "%s(%s)" % (e[1], ", ".join(render_e(a) for a in e[2]))
    return "(%s %s %s)" % (render_e(e[2]), e[1], render_e(e[3]))


def render_s(s, ind=0):
    p = "  " * ind
    k = s[0]
    if k == "print":
        return p + "print(%s);\n" % ", ".join(render_e(a) for a in s[1])
    if k == "return":
        return p + "return %s;\n" % render_e(s[1])
    if k == "fun":
        params = ", ".join(n + ann(a) for n, a in zip(s[2], s[4]))
        return (p + "fun %s(%s)%s {\n" % (s[1], params, ann(s[5])) + "".join(render_s(x, ind + 1) for x in s[3])
                + p + "}\n")
    if k == "expr":
        return p + "%s;\n" % render_e(s[1])
    if k == "var":
        return p + "var %s%s = %s;\n" % (s[1], ann(s[3]), render_e(s[2]))
    if k == "block":
        return p + "{\n" + "".join(render_s(x, ind + 1) for x in s[1]) + p + "}\n"
    if k == "if":
        out = p + "if (%s)\n" % render_e(s[1]) + render_s(s[2], ind + 1)
        if s[3] is not None:
            out += p + "else\n" + render_s(s[3], ind + 1)
        return out
    if k == "loop":
        c = s[1]
        body = "".join(render_s(x, ind + 1) for x in s[3])
        return (p + "var %s = %d;\n" % (c, s[2]) + p + "while (%s > 0) {\n" % c + body
                + p + "  %s = %s - 1;\n" % (c, c) + p + "}\n")
    raise ValueError(k)


# ---------------------------------------------------------------- interpreter
class Return(Exception):
    def __init__(self, value):
        self.value = value


DYN = ("fun", "dyn", ["x"], [("return", ("var", "x"))], [None], None)


class Interp:
    """Scopes map name -> [value, annotation]; scopes[0] holds the globals."""

    def __init__(self, funs=()):
        self.out = []
        self.scopes = [{}]
        self.funs = {f[1]: f for f in funs}
        self.depth = 0

    def guard(self, value, annotation, what):
        if annotation and type_of(value) != annotation:
            raise LumaError("%s expects %s, got %s." % (what, annotation, type_of(value)))

    def lookup(self, name):
        for sc in reversed(self.scopes):
            if name in sc:
                return sc[name]
        raise AssertionError("generator bug: undefined " + name)

    def ev(self, e):
        k = e[0]
        if k == "int":
            return e[1]
        if k == "str":
            return e[1]
        if k == "nil":
            return None
        if k == "bool":
            return e[1]
        if k == "var":
            return self.lookup(e[1])[0]
        if k == "assign":
            v = self.ev(e[2])
            cell = self.lookup(e[1])
            cell[0] = v
            self.guard(v, cell[1], "variable '%s'" % e[1])
            return v
        if k == "un":
            v = self.ev(e[2])
            if e[1] == "!":
                return not truthy(v)
            if not is_int(v):
                raise LumaError("Operand must be a number.")
            return check(-v)
        if k == "call":
            fn = self.funs[e[1]]
            args = [self.ev(a) for a in e[2]]      # all arguments, left to right
            for name, value, a in zip(fn[2], args, fn[4]):   # then each typed parameter's guard
                self.guard(value, a, "argument '%s' of '%s'" % (name, fn[1]))
            saved = self.scopes
            self.scopes = [saved[0], {n: [v, a] for n, v, a in zip(fn[2], args, fn[4])}]
            self.depth += 1
            if self.depth > 200:
                raise AssertionError("generator bug: runaway recursion")
            try:
                self.run(fn[3])
                result = None                       # implicit return nil
            except Return as r:
                result = r.value
            finally:
                self.scopes = saved
                self.depth -= 1
            return result
        if k == "log":
            left = self.ev(e[2])
            if e[1] == "or":
                return left if truthy(left) else self.ev(e[3])
            return self.ev(e[3]) if truthy(left) else left
        op, a, b = e[1], self.ev(e[2]), self.ev(e[3])
        if op == "==":
            return equal(a, b)
        if op == "!=":
            return not equal(a, b)
        if op == "+":
            if is_int(a) and is_int(b):
                return check(a + b)
            if isinstance(a, str) and isinstance(b, str):
                if len(a) + len(b) > MAX_STR:
                    raise TooBig()
                return a + b
            raise LumaError("Operands must be two numbers or two strings.")
        if not (is_int(a) and is_int(b)):
            raise LumaError("Operands must be numbers.")
        if op == "-":
            return check(a - b)
        if op == "*":
            return check(a * b)
        if op == "/":
            if b == 0:
                raise LumaError("Division by zero.")
            return check(a // b)
        return {"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]

    def run(self, stmts):
        for s in stmts:
            self.ex(s)

    def ex(self, s):
        k = s[0]
        if k == "print":
            vals = [self.ev(a) for a in s[1]]       # evaluated before anything is written
            self.out.append(" ".join(show(v) for v in vals))
        elif k == "return":
            v = self.ev(s[1])
            fn = self.current_fun
            raise Return(v) if fn is None else self.ret(v, fn)
        elif k == "expr":
            self.ev(s[1])
        elif k == "var":
            v = self.ev(s[2])
            self.scopes[-1][s[1]] = [v, s[3]]
            self.guard(v, s[3], "variable '%s'" % s[1])
        elif k == "block":
            self.scopes.append({})
            try:
                self.run(s[1])
            finally:
                self.scopes.pop()
        elif k == "if":
            if truthy(self.ev(s[1])):
                self.ex(s[2])
            elif s[3] is not None:
                self.ex(s[3])
        elif k == "loop":
            c = s[1]
            self.scopes[-1][c] = [s[2], None]
            while self.scopes[-1][c][0] > 0:
                self.scopes.append({})
                try:
                    self.run(s[3])
                finally:
                    self.scopes.pop()
                self.scopes[-1][c][0] -= 1

    current_fun = None

    def ret(self, v, fn):
        self.guard(v, fn[5], "return value of '%s'" % fn[1])
        raise Return(v)


class FunInterp(Interp):
    """Tracks the function whose body is executing, for return-value guards."""

    def ev(self, e):
        if e[0] != "call":
            return super().ev(e)
        saved = self.current_fun
        self.current_fun = self.funs[e[1]]
        try:
            return super().ev(e)
        finally:
            self.current_fun = saved


# ---------------------------------------------------------------- generator
# Programs are mostly well typed so they run long enough to exercise control
# flow, scoping, functions and arithmetic. With probability ERR_RATE an
# expression of the wrong type is generated and wrapped in dyn(...), which
# hides it from the static checker.
TYPES = ("int", "str", "bool", "nil")
ERR_RATE = 0.003  # per expression; programs are large, so most still contain none
ANNOTATE = 0.4    # chance that a declaration carries a type annotation
GUARD_RATE = 0.04 # chance that a value headed for an annotated slot is a hidden wrong type


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.n = 0
        self.scopes = [{}]   # per scope: name -> (static type, annotation or None)
        self.funs = []       # (name, [param types], return type, [param annotations]), in definition order
        self.callable = 0    # calls may target funs[:callable] (no recursion: always terminates)
        self.ret_type = None # inside a function body: its return type
        self.ret_ann = None  # ... and its annotation

    def fresh(self, prefix):
        self.n += 1
        return "%s%d" % (prefix, self.n)

    def visible(self):
        seen = {}
        for sc in self.scopes:
            seen.update(sc)   # inner declarations shadow outer ones
        return {k: v[0] for k, v in seen.items()}

    def literal(self, t):
        r = self.r
        if t == "int":
            if r.random() < 0.04:   # rare: values near the fixnum limits (overflow paths)
                return ("int", r.choice([FIXMAX, FIXMIN + 1, 2**31, 3037000499, 2**40]))
            return ("int", r.choice([0, 1, 2, 3, 7, 10, -1, -5, 100, r.randint(-1000, 1000), r.randint(-10**6, 10**6)]))
        if t == "str":
            return ("str", r.choice(["", "a", "ab", "x y", "q\"t", "tab\t", "nl\n", "é", "#"]))
        if t == "bool":
            return ("bool", r.random() < 0.5)
        return ("nil",)

    def expr(self, t, depth, exclude=()):
        """An expression of static type t (unless an error is injected)."""
        r = self.r
        if r.random() < ERR_RATE:
            wrong = r.choice(TYPES)   # type confusion, hidden from the checker
            return ("call", "dyn", [self.expr(wrong, depth - 1, exclude)])
        names = [v for v, vt in self.visible().items() if vt == t and v not in exclude]
        if depth <= 0 or r.random() < 0.2:
            if names and r.random() < 0.6:
                return ("var", r.choice(names))
            return self.literal(t)
        c = r.random()
        if c < 0.08 and names:
            return ("assign", r.choice(names), self.expr(t, depth - 1, exclude))
        cands = [f for f in self.funs[:self.callable] if f[2] == t]
        if cands and r.random() < 0.15:
            name, ptypes, _, panns = r.choice(cands)
            return ("call", name, [self.slot_expr(pt, pa, depth - 1, exclude) for pt, pa in zip(ptypes, panns)])
        if c < 0.2:   # same-typed short-circuit: result has the operands' type
            return ("log", r.choice(["and", "or"]), self.expr(t, depth - 1, exclude), self.expr(t, depth - 1, exclude))
        if t == "int":
            if c < 0.3:
                return ("un", "-", self.expr("int", depth - 1, exclude))
            op = r.choice(["+", "+", "-", "-", "*", "/"])
            right = self.expr("int", depth - 1, exclude)
            if op == "/" and right[0] == "int" and right[1] == 0 and r.random() > ERR_RATE:
                right = ("int", r.choice([1, 2, 3, -7]))
            elif op == "/" and right[0] != "int" and r.random() > ERR_RATE * 5:
                right = ("int", r.choice([1, 2, 3, 5, -2, -3]))   # avoid dividing by a value that may be 0
            return ("bin", op, self.expr("int", depth - 1, exclude), right)
        if t == "str":
            return ("bin", "+", self.expr("str", depth - 1, exclude), self.expr("str", depth - 1, exclude))
        if t == "bool":
            if c < 0.35:
                return ("un", "!", self.expr(r.choice(TYPES), depth - 1, exclude))
            if c < 0.65:
                return ("bin", r.choice(["==", "!="]), self.expr(r.choice(TYPES), depth - 1, exclude),
                        self.expr(r.choice(TYPES), depth - 1, exclude))
            return ("bin", r.choice(["<", "<=", ">", ">="]), self.expr("int", depth - 1, exclude),
                    self.expr("int", depth - 1, exclude))
        return self.literal("nil")

    def slot_expr(self, t, annotation, depth, exclude=()):
        """A value for a slot of type t; for annotated slots, sometimes a hidden
        wrong type, which the runtime guard must catch."""
        if annotation and self.r.random() < GUARD_RATE:
            wrong = self.r.choice([x for x in TYPES if x != t])
            return ("call", "dyn", [self.expr(wrong, depth - 1, exclude)])
        return self.expr(t, depth, exclude)

    def ann_of(self, name):
        for sc in reversed(self.scopes):
            if name in sc:
                return sc[name][1]
        return None

    def any_type(self):
        return self.r.choice(("int", "int", "int", "str", "str", "bool", "nil"))

    def annotation(self, t):
        return t if self.r.random() < ANNOTATE else None

    def stmts(self, n, depth):
        return [self.stmt(depth) for _ in range(n)]

    def scoped(self, n, depth):
        self.scopes.append({})
        body = self.stmts(n, depth)
        self.scopes.pop()
        return body

    def stmt(self, depth):
        r = self.r
        c = r.random()
        vis = self.visible()
        if self.ret_type and c < 0.05:
            return ("return", self.slot_expr(self.ret_type, self.ret_ann, 2))   # early return
        if c < 0.3:
            return ("print", [self.expr(self.any_type(), 3) for _ in range(r.choice((1, 1, 1, 2, 3, 0)))])
        if c < 0.45:
            top = len(self.scopes) == 1
            if top and vis and r.random() < 0.3:
                name = r.choice(list(vis))                      # top-level redeclaration
            elif not top and vis and r.random() < 0.3:
                name = r.choice([v for v in vis if v not in self.scopes[-1]] or [self.fresh("v")])  # shadowing
            else:
                name = self.fresh("v")
            if top and name in self.scopes[0]:
                t, a = self.scopes[0][name]   # a redeclaration keeps the type and the annotation
            else:
                t = self.any_type()
                a = self.annotation(t)
            init = self.slot_expr(t, a, 3, exclude=() if top else (name,))
            self.scopes[-1][name] = (t, a)
            return ("var", name, init, a)
        if c < 0.6 and vis:
            name = r.choice(list(vis))
            return ("expr", ("assign", name, self.slot_expr(vis[name], self.ann_of(name), 3)))
        if c < 0.67:
            return ("expr", self.expr(self.any_type(), 2))
        if c < 0.71 and self.funs[:self.callable]:
            name, ptypes, _, panns = r.choice(self.funs[:self.callable])   # call as a statement
            return ("expr", ("call", name, [self.slot_expr(pt, pa, 2) for pt, pa in zip(ptypes, panns)]))
        if depth <= 0:
            return ("print", [self.expr(self.any_type(), 2)])
        if c < 0.77:
            return ("block", self.scoped(r.randint(1, 4), depth - 1))
        if c < 0.88:
            cond = self.expr(r.choice(("bool", "bool", "int", "nil", "str")), 2)
            then = ("block", self.scoped(r.randint(1, 3), depth - 1))
            els = ("block", self.scoped(r.randint(1, 3), depth - 1)) if r.random() < 0.5 else None
            return ("if", cond, then, els)
        counter = self.fresh("c")   # never added to the assignable names
        return ("loop", counter, r.randint(0, 4), self.scoped(r.randint(1, 3), depth - 1))

    def function(self):
        """A top-level function. It sees the globals declared so far and may call
        functions defined before it (so the call graph is acyclic)."""
        r = self.r
        name = self.fresh("f")
        ptypes = [self.any_type() for _ in range(r.randint(0, 3))]
        params = [self.fresh("p") for _ in ptypes]
        panns = [self.annotation(t) for t in ptypes]
        ret = self.any_type()
        rann = self.annotation(ret)
        saved = (self.scopes, self.callable, self.ret_type, self.ret_ann)
        self.scopes = [dict(self.scopes[0]), {p: (t, a) for p, t, a in zip(params, ptypes, panns)}]
        self.callable = len(self.funs)
        self.ret_type, self.ret_ann = ret, rann
        body = self.stmts(r.randint(1, 4), 2) + [("return", self.slot_expr(ret, rann, 2))]
        self.scopes, self.callable, self.ret_type, self.ret_ann = saved
        self.funs.append((name, ptypes, ret, panns))
        return ("fun", name, params, body, panns, rann)


def generate(seed):
    """Returns (functions, main statements). Globals are declared first so the
    functions (generated next, rendered first because they are hoisted) can use them."""
    rng = random.Random(seed)
    g = Gen(rng)
    globals_ = [g.stmt(0) for _ in range(rng.randint(0, 3))]
    globals_ = [s for s in globals_ if s[0] == "var"]
    funs = [g.function() for _ in range(rng.randint(0, 4))]
    g.callable = len(g.funs)
    prog = globals_ + g.stmts(rng.randint(3, 14), 3)
    return [DYN] + funs, prog


def one(seed, workdir):
    funs, prog = generate(seed)
    src = "".join(render_s(f) for f in funs) + "".join(render_s(s) for s in prog)
    it = FunInterp(funs)
    err = None
    try:
        it.run(prog)
    except LumaError as e:
        err = str(e)
    except TooBig:
        return "skip"
    want_out = "".join(line + "\n" for line in it.out)
    want_err = "" if err is None else "luma: runtime error: %s\n" % err
    want_rc = 0 if err is None else 1

    path = os.path.join(workdir, "p.luma")
    with open(path, "w") as f:
        f.write(src)
    exe = os.path.join(workdir, "p")
    c = subprocess.run([LUMA, path, "-o", exe], capture_output=True, text=True)
    if c.returncode != 0:
        return src, "compile failed:\n" + c.stderr
    # The emitted IR, compiled on its own, must give identical assembly.
    with open(exe + ".lir") as f:
        lir = f.read()
    lir_copy = os.path.join(workdir, "copy.lir")
    with open(lir_copy, "w") as f:
        f.write(lir)
    c2 = subprocess.run([LUMA, lir_copy, "-S", "-o", os.path.join(workdir, "copy")], capture_output=True, text=True)
    with open(exe + ".s") as f1, open(os.path.join(workdir, "copy.s")) as f2:
        if c2.returncode != 0 or f1.read().split("\n", 1)[1] != f2.read().split("\n", 1)[1]:
            return src, "IR round trip: .lir did not recompile to identical assembly\n" + c2.stderr
    # GNU as (validation only) must produce the same bytes from our .s.
    if HAVE_AS:
        g = os.path.join(workdir, "gas.o")
        subprocess.run(["as", "--64", "-o", g, exe + ".s"], check=True)
        for sec in (".text", ".rodata"):
            subprocess.run(["objcopy", "-O", "binary", "-j", sec, exe + ".o", g + ".l"], check=True)
            subprocess.run(["objcopy", "-O", "binary", "-j", sec, g, g + ".g"], check=True)
            with open(g + ".l", "rb") as a, open(g + ".g", "rb") as b:
                if a.read() != b.read():
                    return src, "section %s differs from GNU as" % sec
    r = subprocess.run([exe], capture_output=True, timeout=30)
    got_out, got_err = r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")
    if (got_out, got_err, r.returncode) != (want_out, want_err, want_rc):
        return src, ("expected rc=%d out=%r err=%r\n     got rc=%d out=%r err=%r"
                     % (want_rc, want_out, want_err, r.returncode, got_out, got_err))
    return None


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    failures = 0
    skipped = 0
    with tempfile.TemporaryDirectory() as d:
        for i in range(count):
            res = one(base + i, d)
            if res == "skip":
                skipped += 1
            elif res:
                failures += 1
                src, why = res
                print("MISMATCH seed=%d\n%s\n--- program ---\n%s" % (base + i, why, src))
                if failures >= 3:
                    break
    print("difftest: %d programs (%d skipped: oversized strings), %d mismatch(es)" % (count, skipped, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
