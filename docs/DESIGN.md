# Luma bootstrap compiler: design notes (milestone 7)

This document describes how the Luma toolchain is built. Its sections cover:

- the pipeline and its modules
- the language subset accepted today
- optional types, structs and `impl` blocks, and the C FFI
- memory management: manual, reference counting, and the analyses behind them
- the C runtime
- the optimizer and the two code generators
- the x86-64 assembler (encodings, relaxation, relocations)
- the ELF64 object writer
- linking

The intermediate representation (LIR) has its own specification in
[IR.md](IR.md), and the assembler in [LASM.md](LASM.md).

## Pipeline and modules

```
INPUT.luma
   │  src/lexer.c        bytes → tokens (Lox token set)
   │  src/parser.c       tokens → AST (src/ast.h); `for` is desugared to `while`
   ▼
   │  src/lower.c        AST → LIR: static scoping, short-circuit logic   ──► OUT.lir
   ▼           (INPUT.lir enters here: src/ir_parse.c)
   │  src/ir_verify.c    structural, definite-assignment and type checks
   ▼           (src/ir_types.c: type inference, shared with the stages below)
   │  src/opt*.c         -O2: SSA optimizer (src/cfg.c analyses)          ──► OUT.opt.lir
   │  src/ir_verify.c    the optimized IR must verify too
   ▼
   │  src/mem.c          --mm=manual|rc: escape analysis, retain/release      ──► OUT.mem.lir
   ▼
   │  src/x86_gen.c      -O1/-O2: LIR → x86-64, register allocated        ──► OUT.s
   │  src/x86_isel.c     -O0: LIR → x86-64, one stack slot per vreg
   ▼
   │  src/asm.c          assembly text → ObjFile (src/obj.h)    (reads OUT.s back from disk)
   │  src/elf_writer.c   ObjFile → ELF64 ET_REL bytes                     ──► OUT.o
   ▼
   │  src/main.c         cc -o OUT OUT.o build/libluma_rt.a
   ▼
OUT  (native executable; runtime/luma_rt.c provides main → luma_main)
```

Each pair of neighbouring stages shares exactly one data structure:

| Boundary | Data structure |
|---|---|
| lexer → parser | `TokenList` |
| parser → lowering | `Program` (the AST) |
| lowering → isel | `IrModule` |
| assembler → ELF writer | `ObjFile` |

Every stage can be inspected or tested on its own:

| Stage | How to inspect or test it |
|---|---|
| lexer, parser | `--dump-tokens` / `--dump-ast` |
| lowering | the `.lir` file |
| optimizer | `OUT.opt.lir` (`--dump-opt-ir`); `-O1` skips it, `-O0` also uses the naive backend |
| memory pass | `OUT.mem.lir` (`--dump-mem-ir`): which allocations are counted, every retain and release |
| backend | `.lir` files are accepted as input |
| assembler | the `lasm` command and `tests/unit/test_asm.c` |
| ELF writer | `tests/unit/test_elf.c`, which builds an `ObjFile` by hand |

`src/value.h` defines the value representation. It is the single definition
shared by the compiler, which emits tagged constants, and the runtime, which
interprets them.

## Language (milestone 4)

The grammar is in `src/ast.h`. It started from Lox, and milestone 3 begins
to diverge: `print` is a builtin function, and functions are compiled
statically.

- **Declarations:**
  - `fun name(a, b) { … }`, at top level only for now
  - `var name;`, `var name = expr;`
- **Statements:** expression statements, blocks `{ … }`, `if (…) … else …`
  (`else` binds to the nearest `if`), `while (…) …`,
  `for (init; cond; incr) …` with every clause optional, and `return expr;` or
  `return;` (inside functions only).
- **Expressions**, from lowest to highest precedence:
  1. assignment (right-associative; the target is an identifier or a field `e.name`)
  2. `or`
  3. `and`
  4. `==` and `!=`
  5. `<`, `<=`, `>` and `>=`
  6. `+` and `-`
  7. `*` and `/`
  8. unary `!` and `-`
  9. calls `name(args…)`, field access `e.name`, method calls `e.name(args…)`
  10. literals, variables, parentheses, struct literals `P { x: 1, y }` and
      associated calls `P::name(args…)`
- **Values:** integers, strings, `true`, `false`, `nil` and struct instances. Truthiness, equality
  and the values returned by `and`/`or` follow Lox.
- **Functions:**
  - They are hoisted: any function can call any other, before or after its definition.
  - Arity is checked at compile time, with at most 6 parameters for now.
  - A missing `return` returns `nil`.
  - Parameters and the body's top-level declarations share one scope, so
    redeclaring a parameter is an error.
  - Functions are **not first-class values yet**. A function name may only
    be used as a callee, and nested functions (closures) are rejected with a
    specific error.
- **`print(a, b, …)`** is a builtin function:
  - It evaluates all of its arguments first, then writes them separated by single spaces, followed by a newline.
  - `print()` writes just a newline.
  - It returns `nil`.
  - Using `print` without calling it, or redefining it, is an error. The old
    statement form `print x;` gets the hint "print is a function now".
- **Variables:**
  - **Top-level variables** are module globals.
    - Top-level code sees those declared earlier.
    - Function bodies see all of them.
    - A function reading a global whose declaration hasn't executed yet gets
      the runtime error `Undefined variable 'x'.`
    - Redeclaring a global at top level reuses its slot, and the initializer sees the previous value.
  - **Block and function variables** are lexically scoped locals, resolved at compile time.
    - Shadowing is allowed in nested blocks.
    - Redeclaring a name in the same block is an error.
    - A local cannot be read in its own initializer.
- **Comments:** `//` to end of line.

Differences from reference Lox:

| | Reference Lox | Luma (milestone 3) |
|---|---|---|
| Numbers | doubles | 63-bit integers; literals 0 … 2⁶²−1; overflow is a runtime error; `/` is floor division |
| `print` | statement: `print x;` | builtin function: `print(a, b, …)` |
| Functions | first-class closures | top-level, hoisted, statically resolved; not first-class (yet) |
| Types | none | optional gradual annotations (see below); doomed operations such as `1 + "a"` are compile errors |
| C interop | none | `extern fun` with C types (see below) |
| Undefined names | runtime errors | compile errors; a global read before its declaration executes is a runtime error |
| Strings | multi-line, no escapes | single line; `\"` `\\` `\n` `\t` |
| Objects | `class`, `this`, `super`, inheritance | `struct` + `impl` (see below); `class` is rejected with a hint to use them; no inheritance |
| Float literals | yes | rejected ("not supported yet") |

Compile errors have the form `path:line:col: error: message`, and compilation
stops at the first one. When it does, no `.lir`, `.s`, `.o` or executable is
written. Nesting depth, which includes long `a+b+c+…` chains, is bounded at
1000, so malformed input cannot overflow the C stack.

## Types (gradual, optional)

Annotations are optional and can go on variables, parameters and return
values:

```js
var count: int = 0;
fun greet(name: str?): str { … }
```

| Type | Accepts |
|---|---|
| `int` | integers |
| `str` | strings |
| `bool` | `true` and `false` |
| `nil` | `nil` |
| `any` | anything; also the meaning of no annotation |
| `P` (a struct name) | instances of struct `P` |
| `T?` | `T` or `nil` |

The compiler gives every expression a **static type**: the set of runtime
types it might have.

- Literals have exact types.
- Unannotated variables, parameters and returns are `any`.
- Operators compute result types: `int + int` is `int`; `x + 1` with `x: any`
  is `int`, since a string plus 1 can never succeed; comparisons are `bool`.
- A call has its callee's declared return type.
- `and`/`or` have the union of their operand types.

Values are checked at every **typed boundary**: initializing or assigning an
annotated variable, passing an argument to an annotated parameter, and
returning from a function with an annotated return type.

| The value's static type… | Result |
|---|---|
| fits the slot (subset) | accepted, no code emitted |
| can't fit (disjoint) | compile error: `variable 'n' expects int, got str` |
| might fit (overlap, e.g. `any` into `int`) | runtime guard: `luma_check_type(v, mask, "variable 'n'")` raises `variable 'n' expects int, got str.` |

Further rules:

- **Doomed operators:** an operator whose operand types make it fail on every
  execution is a compile error, for example `1 + "a"`, `-"x"` or `nil < 1`.
- **Untyped code is unchanged:** code without annotations or literal type
  mistakes behaves exactly as dynamic code, and its errors stay at runtime.
- **Missing returns:** a function with a return type that doesn't include
  `nil` must not be able to reach the end of its body. `while (true)` and
  `for (;;)` count as never exiting.
- **Initializers:** `var x: int;` needs an initializer, because it would
  otherwise be `nil`.
- **Global redeclarations** must keep their annotation.

Lowering turns the runtime guards into LIR `check` instructions and records
the declared types in the IR (IR.md section 17). From there, types make code
faster: the optimizer deletes `check`s that inference proves redundant, and
the backend skips tag checks on values known to be `int` (for example,
`fun fib(n: int): int` compiles to plain machine arithmetic plus overflow
checks).

## Structs and `impl` blocks (milestone 6)

```js
struct Node { value: int, next: Node? }   // fields, optionally typed

impl Node {
  fun new(v: int): Node { return Node { value: v, next: nil }; }  // associated function
  fun sum(self): int {                                             // method
    var total = 0;
    var n: Node? = self;
    while (n != nil) { total = total + n.value; n = n.next; }
    return total;
  }
}

var list = Node::new(1);
list.next = Node { value: 2, next: nil };
print(list.sum(), list);   // 3 Node { value: 1, next: Node { value: 2, next: nil } }
```

**Declarations.** `struct` and `impl` are top-level only. Struct names share
the namespace of functions, globals and builtins, and all structs are hoisted
like functions, so they may refer to each other in any order (`next: Node?`).
A struct may have several `impl` blocks; a method name must be unique across
them. In an `impl`, a function whose first parameter is `self` is a
**method**; one without is an **associated function**. `self` must come first
and cannot be annotated (it is always the struct). `impl` for an unknown
struct, `self` elsewhere, and duplicate fields or methods are compile errors.

**Literals.** `P { x: 1, y }` must initialize every field exactly once, in any
order; `y` alone means `y: y`. Initializers are evaluated left to right as
written, then the object is allocated and filled, so a runtime error in an
initializer leaves no half-built object. Each initializer flows into its
field's type like any typed boundary (compile error, nothing, or a guard).

**Semantics.**

- Instances are references. Assignment and argument passing share the object,
  `==` is identity, and every instance is truthy.
- `print` shows `P { x: 1, s: "q" }`: strings inside a struct are quoted, and
  an object already being printed (a cycle) shows as `P { ... }`.
- `P::f(args)` calls an associated function or a method with `self` passed
  explicitly. `p.m(args)` calls a method; calling an associated function this
  way is a compile error with a hint.

**Static and dynamic access (gradual).** What an access compiles to depends
on the object's static type:

| Static type of `e` | `e.x`, `e.x = v` | `e.m(args)` |
|---|---|---|
| exactly `P` | a load/store at a fixed offset; unknown field is a compile error; `v` is checked against the field type at compile time or by a guard | a direct call to `P`'s method; arity checked at compile time |
| can't be a struct (`int`, `str?`, …) | compile error: `only struct instances have fields, got int` | compile error |
| anything else (`any`, `P?`, `P or int`) | `luma_getfield` / `luma_setfield` by name at runtime | `luma_method` finds the code by name, checks the arity, and the call goes through the returned pointer |

The dynamic paths give the same results and the same error messages as the
static ones, just at run time: `Struct 'P' has no field 'y'.`,
`field 'x' of 'P' expects int, got str.`, `'P::m' expects 1 argument, got 2.`
A dynamic call cannot see the method's parameter types, so a method with
typed parameters (besides `self`) gets a second entry, `@m.P.name.dyn`, with
untyped parameters, that checks the arguments and calls the real method; the
method table points at that entry. Methods without typed parameters need no
wrapper.

The optimizer often turns dynamic accesses into static ones: after
`n != nil` is tested, a `Node?` is known to be a `Node` on that edge (see
"Optimizer" below).

**Naming.** Methods and associated functions are LIR functions named
`@m.P.name` (shown as `P::name` in messages). Struct descriptors are emitted
as `.Lstruct.K`, field and method names as `.Lname.NAME`.

**Memory.** How instances are freed depends on the memory mode: `free()`
in `--mm=manual` (or automatically for instances that never leave their
function), reference counting in `--mm=rc`. See "Memory" below.

## Memory (milestone 7)

Every compilation chooses one memory-management mode with `--mm`:

| Mode | Struct instances | Strings | `free(x)` |
|---|---|---|---|
| `manual` (default) | yours: freed by `free(x)`; the compiler also frees instances it can prove never leave their function | freed automatically at their last use | frees `x` and releases the strings in its fields |
| `rc` | reference counted: freed when the last reference goes away; cycles leak | reference counted | checks its argument, frees nothing |
| `none` | never freed | never freed | checks its argument, frees nothing |

The same program compiles in every mode, and its output never depends on
the mode. `--mm=none` is the old behaviour and the fastest choice for
short-lived programs. Programs written for `manual` run unchanged under `rc`,
which is a way to find out whether they free everything.

**`free(x)`** is a builtin, like `print`.
- `x` must be a struct instance or `nil`. `free(nil)` does nothing.
- `free("text")` and `free(1)` are compile errors when the type is known. At
  run time they are errors in every mode:
  `free() expects a struct instance, got str (strings are freed automatically).`
- After `free(x)`, using `x` (or freeing it again) is an error in every mode:
  - A compile error when it happens on every path, checked on the lowered IR
    before optimization: `'p' is used after free(p) on line 4`.
  - Through an alias it is undefined behaviour in `manual` mode, as in C.
    `LUMA_MEMCHECK=1` turns it into a runtime error (below).

### Ownership and reference counts

A counted object's count is the number of **references** that hold it. A
reference is held by:
- a vreg (a local value);
- a struct field;
- a global variable.

The compiler inserts the operations that keep the count exact, in a pass
over the final IR (`src/mem.c`, `mem_lower`, after the optimizer). It works
from three rules:

- **Operands are consumed or borrowed.**
  - Consumed: the instruction takes over the reference. This is the case for
    `mov`/`check` (the result holds it), `store` and the value of `setfield`
    (the slot holds it), the arguments of `new` (the fields hold them), and
    `ret`.
  - Borrowed: every other use, including call arguments. **Parameters are
    borrowed**: the caller keeps its reference alive for the call, so passing
    an object costs nothing.
- **Results are owned.** New objects, call results, `mov` and the like are
  owned. `load` and `getfield` are the exceptions: they return the slot's
  reference, so they are followed by a `retain`.
- **Every owned reference is released exactly once, where liveness says it
  dies.**
  - A consumed operand that dies at that use is *moved* (no operation); one
    that is still live is retained first.
  - A borrowed operand that dies is released right after the instruction.
  - An unused result is released immediately.
  - A value that is live out of a block but not into one of its successors
    is released on that edge. The release goes at the start of the
    successor, or in a new block when the edge is critical.
  - Overwriting a global or a field releases the old value: `store` and
    static `setfield` carry a flag the backends act on, and dynamic
    `setfield` is handled by the runtime.

Refinements:
- A parameter the function reassigns is retained on entry, so it owns its
  value from then on.
- A `mov` or `check` copy of a borrowed value that is never handed on stays
  borrowed (the `.dyn` method wrappers need no operations at all).
- A call whose result is discarded still gets its result released.
- The operands of a compare that the backend fuses with the following `br`
  are released on the branch's edges instead, so the fusion survives.
- An operand that dies at an operation that only succeeds on fixnums (`sub`,
  `mul`, `div`, `mod`, `neg`, ordered compares) needs no release: if
  execution gets past it, the value was a fixnum.

**Which vregs need operations at all.** Only vregs that may hold a counted
object get any. That excludes:
- ints, booleans and nil, by flow-insensitive type inference over every
  definition;
- vregs that only ever hold string literals (static, never counted);
- in `manual` mode, struct types that never have counted instances.

Typed integer code is therefore unchanged: `fib(n: int)` has no reference
counting at all.

**Escape analysis (`manual` mode).** An allocation site's instances are
counted, and so freed automatically, when they provably never leave the
local values of their function. The analysis is interprocedural and works as
follows:
- Vregs connected by `mov`/`check` form a class (union-find).
- A class escapes when one of its vregs is:
  - stored (`store`, a `setfield` value, a `new` argument);
  - returned;
  - passed to `free`;
  - passed to a parameter that escapes, or to an unknown external function.
- A parameter escapes when its class does. This is a least fixpoint over
  all functions.
- A dynamic `callm` can only reach a method of that name and arity in some
  struct's table, so it escapes only if one of those methods' parameters
  does.

Everything else stays manual: no count, never touched by retain/release, and
the program's to free.

```js
fun dist2(a: int, b: int): int {
  var p = Point { x: a, y: b };   // never stored, returned or freed:
  return p.len2();                // counted, freed after the call
}
```

### Runtime

- **Header.** Bits 32–63 of the header word are the count, at offset 4. A
  count of 0 means *not counted*: string literals, everything in `none`, and
  manual struct instances.
- **Allocation.** Strings created at run time are counted unless the mode is
  `none`. `luma_new_struct(desc, counted)` gets `counted` from the compiler.
- **Freeing.** At count 0 the object is destroyed: its fields are released,
  and objects whose count reaches 0 in turn go on a worklist. Destruction is
  iterative, so dropping the head of a million-node list frees the whole
  chain without deep C recursion.
- **The manual pool.** Manual struct instances come from per-size free
  lists, and their memory is never returned to `malloc`. So a stale
  reference still reads a header with count 0, and the compiler's own
  retain/release on it stays a no-op instead of corrupting another object.
- **Functions:** `luma_retain`, `luma_release` (-O0), `luma_rc_free` (the
  last reference; called by the inline fast paths), `luma_free`.
- **Double free.** A freed object's header type becomes `LUMA_TYPE_FREED`.
  `free` of such an object reports `Double free: this object was already
  freed.`, and any runtime operation on it reports `Use after free.`

**`LUMA_MEMCHECK=1`** (an environment variable for the compiled program):
- Freed memory is never reused.
- Fields of freed instances read as `LUMA_POISON`, a value with the unused
  tag `100`. The next runtime operation on it reports `Use after free.`, as
  does a fault at its address (when typed code dereferences it).
- At exit, the objects still allocated are reported. Those reachable from a
  global variable (via the `luma_globals` table the compiler emits) are
  listed separately from those lost:

```
luma: memcheck: --mm=rc, 1016 allocated, 1009 freed, 3 leaked, 4 reachable from globals
luma: memcheck: leaked: 3 Pair
luma: memcheck: reachable from globals: 1 Node, 3 Pair
```

### Code generation

| Operation | `-O1`/`-O2` |
|---|---|
| `retain %v` | `test v, 7; jnz done` (not an object); `cmp dword ptr [v + 4], 0; je done` (not counted); `add dword ptr [v + 4], 1` |
| `release %v` | `test v, 7; jnz done`; `cmp dword ptr [v + 4], 1; jb done` (0: not counted); `je last`; `sub dword ptr [v + 4], 1`. `last:` is an out-of-line stub calling `luma_rc_free` that keeps the live registers |
| `store` / static `setfield` releasing | the old value goes to `r10` before the write, then the release sequence (plus a zero test for a never-assigned global) |
| `new` | `mov esi, 0/1` (counted) before `call luma_new_struct` |

The backend skips the tag test when the value cannot be an immediate, and
skips the operation entirely when its flow-sensitive types prove the value
is not an object. One more trick: the releases of an `add`'s operands right
after it are emitted only on the add's slow path, because on the inline
fast path both operands were fixnums. So untyped `fib(n - 1) + fib(n - 2)`
runs exactly the old instructions. Struct type checks compare only the low
32 bits of the header (`cmp dword ptr [r], HDR`), since the count is in the
high half. At `-O0`, every operation is a call to `luma_retain` or
`luma_release`.

Benchmarks at `-O2` (wall time, best of 5; differences of about 10% are
within run-to-run noise on the build machine):

| Benchmark | `none` | `manual` | `rc` |
|---|---|---|---|
| collatz | 0.33 s | 0.32 s | 0.32 s |
| fib(37), untyped | 0.10 s | 0.12 s | 0.13 s |
| fib(37), typed | 0.12 s | 0.11 s | 0.11 s |
| loop_sum | 0.11 s | 0.11 s | 0.11 s |
| primes | 0.58 s | 0.53 s | 0.58 s |
| strings (3M concatenations) | 0.007 s | 0.008 s | 0.008 s |
| structs (20M method calls, list walks) | 0.10 s | 0.11 s | 0.11 s |

The structs benchmark got faster than before this milestone (0.14 s),
because devirtualized calls now skip the checking `.dyn` wrapper. The
runtime's generic operations now test for two fixnums before anything else,
which made the `-O0` column faster too (e.g. loop_sum 1.00 s → 0.69 s).

### Limits and next steps

- Reference cycles leak in `rc` mode; memcheck lists them. Planned options:
  weak fields, or a cycle collector over counted objects (trial deletion).
- The escape analysis only follows values through vregs. An instance stored
  in a field of another local instance counts as escaping.
- The ownership pass is per function and does not remove retain/release
  pairs across instructions (beyond moves on last use); there is no
  "borrow" of field values across code that cannot write them.
- Strings and counted objects are allocated with `malloc` directly; a size-
  class allocator would make the frequent small allocations cheaper.

## C FFI (`extern fun`)

```js
extern fun puts(s: cstr): i32;
extern fun getenv(name: cstr): cstr?;   // NULL <-> nil
puts("hi");
```

`luma prog.luma -o prog -l NAME -L DIR` links C libraries. Extra `.o`, `.a`
and `.so` arguments are passed to the linker, and libc needs no flags.
`extern` declarations go at top level, every parameter must be typed, and a
return type is required (`void` for none). The C symbol has the same name as
the Luma function. Names starting with `luma_` are reserved, and at most 6
parameters are allowed for now.

| C type | Luma type | Argument conversion | Result conversion |
|---|---|---|---|
| `i8 i16 i32 i64` | `int` | must be an `int` that fits, otherwise a runtime error (`5000000000 does not fit in i32`) | sign-extended from the declared width; `i64` must fit in 63 bits |
| `u8 u16 u32 u64` | `int` | must be ≥ 0 and fit | zero-extended; `u64` must fit in 63 bits |
| `bool` | `bool` | `true` → 1, `false` → 0 | low byte ≠ 0 |
| `cstr` | `str` | pointer to the string's NUL-terminated bytes, which C must not modify or keep | copied into a new Luma string; NULL is an error |
| `cstr?` | `str?` | `nil` → NULL | NULL → `nil` |
| `ptr` | `int` | an address | an address |
| `void` | `nil` | (not allowed as a parameter) | `nil` |

**What a call compiles to:**

1. Each argument is converted by `luma_ffi_arg_<ctype>(value, "argument N ('p') of f")`
   into a scratch stack slot.
2. The raw values are loaded into `rdi`…`r9`.
3. `eax` is zeroed, which tells a variadic callee that no vector registers are used.
4. The C function is called.
5. The result is converted with `luma_ffi_ret_<ctype>(rax, "return value of f")`.

Mismatches the compiler can see are compile errors, for example passing `42`
to a `cstr`. Everything else is checked by the converters at runtime. Values
are never truncated silently: narrowing happens only when C itself returns a
narrow type.

Not supported yet: `f32`/`f64` (Luma has no floats), structs, callbacks,
variadic declarations, and more than 6 parameters.

## Runtime (`runtime/luma_rt.c` → `build/libluma_rt.a`)

The runtime is part of the toolchain, like libc. `make` builds it with the
system C compiler, and `luma` links it into every program. It owns `main`,
which calls `luma_main()`, flushes stdout, and exits with status 0.

| Function | Purpose |
|---|---|
| `luma_write`, `luma_write_space`, `luma_write_newline` | back `print(...)`: each argument is written, separated by spaces, and the line is ended with a newline |
| `luma_print` | writes a value and a newline (kept for hand-written LIR) |
| `luma_add/sub/mul/div/mod/neg/not/eq/ne/lt/le/gt/ge` | generic operations |
| `luma_undefined_variable(name)` | called by a checked global `load` that finds an unassigned slot |
| `luma_check_type(v, mask, context)` | the runtime guard at typed boundaries (returns `v`) |
| `luma_int_overflow()`, `luma_div_zero()` | noreturn error exits of the inline fast paths |
| `luma_ffi_arg_<ctype>`, `luma_ffi_ret_<ctype>` | FFI converters, with range and type checks |
| `luma_new_struct(desc, counted)` | allocates an instance with every field `nil` (counted or not, see "Memory"); the compiled code then stores the initializers |
| `luma_getfield(o, name)`, `luma_setfield(o, name, v)` | dynamic field access; `setfield` checks `v` against the field's declared type and releases the old value |
| `luma_retain`, `luma_release`, `luma_rc_free`, `luma_free` | reference counting and `free()` (see "Memory") |
| `luma_method(o, name, nargs)` | returns the code of `o`'s method `name` after checking the arity (`LumaFn`, called with the real arguments) |

**Struct instances** (`src/value.h`):

```
+0   header   LUMA_TYPE_STRUCT (2) | (struct index + 1) << 8 | count << 32
+8   descriptor pointer
+16  field 0, field 1, …   (8 bytes each, declaration order)
```

The compiler emits one **descriptor** per struct into `.data`
(`{id, name, nfields, field names, field type masks, nmethods, methods}`,
where each method is `{name, code, arity including self}`), plus a global
table `luma_structs = [count, desc…]` that the runtime uses to name struct
types in error messages (`expects Point, got int`). Every program defines
`luma_structs` (count 0 if it declares no structs), `luma_mm` (its memory
mode) and `luma_globals` (`[count, &slot…]`, for memcheck). The descriptors hold
absolute addresses, so they use `R_X86_64_64` relocations (`.quad SYMBOL`),
which the linker turns into `R_X86_64_RELATIVE` dynamic relocations in the
PIE.

The generic operations behave as follows:

- Arithmetic on fixnums is checked for overflow.
- `/` and `mod` floor their results.
- `+` concatenates two strings.
- Strings and struct instances created at run time are allocated by the
  runtime's memory layer and freed according to the memory mode (see
  "Memory"). Out of memory is a runtime error.

A runtime error flushes stdout, prints `luma: runtime error: <Lox-style
message>` to stderr, and exits with status 1.

**Stack overflow.** `main` installs a `SIGSEGV` handler on an alternate signal
stack. Unbounded recursion therefore reports `luma: runtime error: Stack
overflow.` (exit 1) instead of crashing with a bare segmentation fault.
Generated code only touches its own frame, the globals and runtime objects, so
a fault is almost always the stack. Any other memory fault would be reported
the same way.

At `-O0` every virtual register has its own stack slot, so a small recursive
function uses about 100 bytes per call and overflows the default 8 MB stack
between 80,000 and 100,000 calls deep. The optimizing backend keeps values in
registers (a call to `fib` above takes 32 bytes) and turns tail calls into
jumps, which use no stack at all. The runtime is compiled with
`RT_CFLAGS`, separate from the compiler's `CFLAGS`, so a sanitizer build of
the compiler doesn't leak instrumentation into user programs.

## Optimizer (`src/opt*.c`, `src/cfg.c`)

`opt_module` (`-O2`) optimizes each function on a copy and keeps the original
if a pass gives up, so the optimizer can only make code faster, never break
compilation. The order of passes and what each one does is in IR.md
section 17. Implementation notes:

- **Analyses** (`cfg.c`): successors and predecessors, reverse postorder,
  dominators (Cooper–Harvey–Kennedy), dominance frontiers, phi-aware
  liveness as bitsets, and natural loops (innermost first). The backend
  reuses the CFG and liveness.
- **CFG simplification** (`opt_cfg.c`): merges straight-line blocks, threads
  jumps through empty blocks, removes unreachable code, and duplicates small
  loop headers into the loop's entry and latch ("loop rotation"), so a
  `while` loop tests its condition once per iteration at the bottom.
- **SSA** (`opt_ssa.c`): phis are placed only where the variable is live
  (pruned SSA) and renamed by a walk over the dominator tree. Leaving SSA uses
  a fresh temporary per phi, so the lost-copy and swap problems cannot occur.
  Copy coalescing (Chaitin-style, on the interference of the non-SSA result)
  then removes most of the copies again.
- **SCCP** propagates constants and types through phis and prunes branches whose
  condition is known, including from types alone (a `str` is always truthy).
  It folds arithmetic only when the runtime would not fail: overflow, division
  by zero and type errors are left in place to happen at run time.
- **GVN** numbers expressions within the dominator tree. It merges:
  - equal expressions;
  - a `check` that the value's type already proves, or that repeats a
    dominating check of the same value and type;
  - within a block, a `load` of a global that was already loaded or stored,
    with no call to a Luma function in between.

  `add` is treated as commutative only when both operands are `int`
  (`"a" + "b"` isn't `"b" + "a"`).
- **LICM** creates loop preheaders. It hoists pure instructions freely, and
  instructions that may fail only from the loop header before any other
  effect, so a hoisted error still happens first and only if the loop is
  entered.
- **DCE** deletes unused instructions that cannot fail and have no effect.
  A `sub` whose result is unused stays if it might overflow or fail on a type.
- **Struct operations.** `new`, `getfield`, `setfield` and `callm` count as
  effects: they are never hoisted, merged or deleted, since a field may change
  between two reads and a dynamic access may fail. `callm` (which can run any
  method) also clears GVN's knowledge of globals, like a call.
- **Edge refinement** (`ir_types.c`). On the two edges out of `br %c`, `%c` is
  known truthy or falsy; when `%c` came from `ne %v, nil` / `eq %v, nil` (or
  `not` of one) in the same block, `%v` is known non-nil or nil on each edge.
  This is what makes `while (n != nil) { … n.value … }` see `n` as a `Node`.
- **Objects** (`opt_objects.c`, last, on non-SSA IR). Three rewrites, each of
  which the verifier re-checks:
  - *Devirtualization:* a dynamic `getfield`/`setfield`/`callm` whose object
    is proven to be exactly struct `P` becomes the static form (or a direct
    `call` of `P`'s method entry). It is kept dynamic when `P` has no such
    field or method, the arity is wrong, or a written value might not fit the
    field, so runtime errors are unchanged.
  - *Field load forwarding:* within a block, a static read of `%o.P.f` after
    an earlier read or write of the same `%o.P.f` reuses the known value,
    unless in between there is a write to field `P.f` of any object (it may
    alias), a dynamic write, a Luma call or `callm`, or a reassignment of
    `%o` or of the value's vreg.
  - A `check` that the sharper types now prove becomes a `mov`.

## Code generation: optimizing backend (`src/x86_gen.c`, `-O1`/`-O2`)

The backend runs per function:

1. **Numbering.** Instructions are numbered in layout order (block order,
   unreachable blocks dropped). Instruction *i* reads its operands at
   position 2*i* and writes its result at 2*i*+1.
2. **Intervals.** Each vreg gets one interval: the hull of its liveness
   (block live-in/live-out and every def and use). Constants with a single
   definition get no interval; they are **rematerialized** as immediates (or
   `lea` of a string object) wherever they are used.
3. **Linear scan.** Intervals are allocated in order of their start.
   - **Calls:** an interval that is live across a call may only use
     callee-saved registers (`rbx`, `r12`–`r15`). Others prefer caller-saved
     ones (`rax rcx rdx rsi rdi r8 r9`).
   - **Hints:** parameters prefer their argument register, an argument that
     dies at a call its argument register, results `rax`, and both sides of a
     `mov` the same register, so most copies vanish.
   - **Division:** values live across an `idiv` avoid `rax` and `rdx`.
   - **Spilling:** when no register is free, the interval that ends last
     is spilled to its own frame slot.
   - **Scratch:** `r10` and `r11` are never allocated; they serve as scratch.
4. **Frame.** Only functions with calls, spills, callee-saved registers or FFI
   scratch need one:

   ```
   push rbp; mov rbp, rsp; push <callee-saved used>…; sub rsp, N
   ```

   N keeps `rsp` 16-byte aligned. Slot *k* is at `[rbp - 8·ncs - 8(k+1)]`.
   Other functions are frameless.

**Fixnum fast paths.** With a = 2x+1 and b = 2y+1, each operation is a few
instructions. The 64-bit overflow flag is exactly "the result does not fit
in 63 bits", so `jo` jumps to a shared noreturn stub (`and rsp, -16; call
luma_int_overflow`).

| Operation | Code |
|---|---|
| `a + b` | `lea t, [a - 1]; add t, b; jo` (a constant becomes `add t, imm`) |
| `a - b` | `lea r11, [b - 1]; mov t, a; sub t, r11; jo` |
| `a * b` | `mov r11, b; sar r11, 1; lea t, [a - 1]; imul t, r11; jo; or t, 1` (`imul t, t, k` for a constant) |
| `a / 2^k`, `a mod 2^k` | `sar t, k+1; lea t, [t + t + 1]` and `and t, 2^(k+1) - 1` |
| `a / b`, `a mod b` | zero test; `idiv` on the untagged values with a floor fix-up; `rax`/`rdx` are pushed and popped only if they hold other live values |
| `-a` | `neg t; add t, 2; jo` |
| `a < b` … | `cmp a, b` (tagged fixnums order like integers), then `setcc` or a fused `jcc` |
| `a == b` | a word compare when either side can't be a string. Otherwise equal words are equal; any non-object operand means unequal; two objects call `luma_eq`. |
| `not a` | `xor t, 8` for a `bool`; otherwise the truthiness test and `setcc` |

**Tag checks.** Before a fast path, operands that might not be fixnums are
tested (`test r8, 1`, or `mov r11, a; and r11, b; test r11b, 1` for two
operands). On failure the code jumps to an **out-of-line slow path** after the
function body. The slow path:

1. pushes the caller-saved registers that hold values live across the
   instruction (and only those);
2. calls the generic runtime function (`luma_add`, …), which produces the
   result or the same runtime error as `-O0`;
3. restores the registers and jumps back.

Operands whose inferred type is `int` are not tested at all, and an operation
whose operands can never both be ints (string `+`, say) just calls the
runtime inline. The fast path never writes the destination before its checks
pass, so the slow path always sees the original operands.

**Other instructions:**

| Instruction | Code |
|---|---|
| `check` | Inline tests for its int/nil/bool members: `test r8, 1`, `cmp r, 6`, `and r10, -9; cmp r10, 2`. Strings and failures go to a slow path calling `luma_check_type`, which returns the value or raises the error. |
| `load` | `mov t, [rip + .Lvar.g]; test t, t; je <undefined-variable stub>` |
| `store` | `mov [rip + .Lvar.g], imm32 / reg` |
| `new P(…)` | `lea rdi, [rip + .Lstruct.K]; call luma_new_struct`, then one `mov [rax + 16 + 8i], arg` per field. The arguments are live across the call, so they sit in callee-saved registers or slots. |
| `getfield %o, P.x` / `setfield` | one `mov` at offset 16 + 8·index; the verifier has proven `%o` is a `P` |
| dynamic `getfield` / `setfield` | `lea rsi, [rip + .Lname.x]; call luma_getfield` (or `luma_setfield`) |
| `callm %o, .m(args)` | the arguments are parallel-moved into their final registers (self in `rdi`) and pushed; `luma_method(o, "m", n)` returns the code pointer in `rax`; the arguments are popped back, and the call is `mov r11, rax; call r11` |
| `check` with a struct type | `test r, 7; jnz slow; cmp qword ptr [r], HEADER; je ok` (an object with the right header word), else the slow path |
| `br` | Uses the operand's type: a `bool` compares with `false`, a value that can't be `bool` compares with `nil`, and an `int`/`str` is always truthy. |

**Branch fusion.** When a `br` tests the result of a comparison earlier in
its block, the comparison is emitted at the branch as `cmp` + `jcc`. This
applies when the result has no other use and only copies and constants that
leave the operands alone come in between, as out-of-SSA leaves them. The
slow path of a fused compare branches directly on the runtime's answer.

**Calls:**
- **Argument moves** into `rdi…r9` are a parallel move; cycles are broken
  through `r11`.
- **Tail calls:** a call to a Luma function immediately followed by `ret` of
  its result becomes `epilogue; jmp fn.X`. This is also why
  `tests/rt/stack_overflow.luma` recurses with `1 + forever(n + 1)`.
- **FFI calls** convert each argument through `luma_ffi_arg_<T>` into frame
  scratch slots before loading the argument registers, exactly as at `-O0`.

**Peephole.** A final pass over the emitted lines removes jumps to the
next line, inverts `jcc L1; jmp L2; L1:` into one branch, drops `mov r, r`,
and removes a re-tag immediately undone (`lea r, [r + 1]; lea r, [r - 1]`).

**Readability.** As at `-O0`, every LIR instruction appears as a `#` comment
above its code. Each function's header comment says how many vregs are in
registers or spilled, whether it has a frame, and which callee-saved
registers it saves.

## Code generation: naive backend (`src/x86_isel.c`, `-O0`)

See [IR.md](IR.md) section 8 for the per-instruction templates.

- **Stack slots:** every virtual register gets a stack slot at `[rbp - 8(k+1)]`.
  The frame is rounded up to 16 bytes, so `rsp` is aligned at every call.
- **Operations:** each LIR operation loads its operands into scratch
  registers, calls the runtime or moves the value, and stores the result.
- **Branches:**
  - `br` tests truthiness with `and rax, -5; cmp rax, 2`, since only `nil` (6)
    and `false` (2) map to 2.
  - Jumps to the next block are omitted.
- **Comments:** every LIR instruction appears as a `#` comment above its code,
  so `OUT.s` can be read side by side with `OUT.lir`.
- **Calls:** arguments are passed in `rdi`, `rsi`, `rdx`, `rcx`, `r8` and `r9`
  (System V), and the callee spills them into its parameter slots. The result
  comes back in `rax`.
- **Globals:** each one is an 8-byte slot in `.data` (`.Lvar.NAME`),
  initialized to 0, which is never a valid value. `store` writes the slot.
  `load` reads it, and calls `luma_undefined_variable` with the variable's
  name if it's still 0.
- **Symbols:**
  - Lowering names user functions `@fn.NAME`, globals `@var.NAME` and strings
    `@sN`. The dots keep them from ever colliding with C or runtime symbols
    such as `puts` or `luma_add`, because C identifiers can't contain dots.
  - LIR functions are emitted as global `FUNC` symbols.
  - String literals become static objects in `.rodata`, each 8-aligned with a header and length.
  - The data label `@sN` becomes `.Ldata.sN`, and the block `B` of function `F` becomes `.LF.B`.

The emitted dialect is a strict subset of GNU `as` Intel syntax. The test
suite checks that GNU `as` produces **byte-identical** sections and identical
relocations for every program it compiles. The pipeline itself never runs an
external assembler.

## Assembler (`src/asm.c`)

The assembler (lasm) has its own specification, [LASM.md](LASM.md). It
covers the accepted syntax, sections and directives, the operand forms, every
instruction with its encoding, branch relaxation, symbol resolution and
relocations, and the diagnostics. In short:

- **Dialect:** a strict subset of GNU `as` Intel syntax. lasm must produce the
  same bytes and relocations as GNU `as` for every input it accepts, and the
  tests check this for every compiled program and about 5,700 generated forms.
- **Operands:** registers of 8, 32 and 64 bits, and full ModRM/SIB memory
  operands (`[base + index*scale + disp]`, `[rip + sym]`).
- **Instructions:** `mov`, `movzx`, `lea`, the ALU group, `test`, `imul`,
  `neg`/`not`/`idiv`, shifts, `setcc`, `cmovcc`, `cqo`, `push`/`pop`, `call`,
  `jmp`/`jcc` (relaxed in passes, as GNU `as` does), indirect `call r64`,
  `ret` and `nop`.
- **Relocations:** `R_X86_64_PC32` for RIP-relative operands and local
  targets, `R_X86_64_PLT32` for calls and jumps to global or undefined
  symbols, and `R_X86_64_64` for `.quad SYMBOL[±N]` data (struct
  descriptors). `.L` labels never reach the symbol table; a relocation against
  one uses its section's symbol plus the label's offset.

## ELF64 writer (`src/elf_writer.c`)

The writer emits an ET_REL file for EM_X86_64 (ELFCLASS64, little-endian, SysV
OSABI). It has no program headers, and `e_entry` is 0. It defines its own ELF
constants and does not depend on the host `<elf.h>`.

```
0x00  ELF header (64 bytes)
      user sections in creation order, each aligned to its sh_addralign
      .rela.<sec> for every section with relocations      (align 8, entsize 24)
      .symtab                                             (align 8, entsize 24)
      .strtab
      .shstrtab
      section header table                                (align 8)
```

Section header indices are assigned in this order: `0` is the null section,
then the user sections (`1..N`), then the `.rela.*` sections, then `.symtab`,
`.strtab` and `.shstrtab`.

- **`.rela.X`:** `sh_link` = `.symtab`, `sh_info` = X, and flags include `SHF_INFO_LINK`.
- **`.symtab`:** `sh_info` = the index of the first non-local symbol. Symbols are written in this order:
  1. the null symbol
  2. an `STT_FILE` symbol for the source file
  3. one `STT_SECTION` symbol per user section
  4. named local symbols
  5. global symbols, defined first and then undefined

  All locals come before all globals, as the ELF specification requires.

Before writing anything, the writer checks the object model and reports an
error instead of producing a corrupt file. It checks that:

- every relocation type is known
- every relocation field lies inside its section
- every symbol and section index is in range
- no relocation names a `.L` label
- every symbol value lies inside its section

## Linking

`luma` runs `cc -o OUT OUT.o <libdir>/libluma_rt.a` through `posix_spawnp`, with
no shell involved. `<libdir>` is the directory containing the `luma` binary.

- **Overrides:** `$LUMA_CC` replaces the `cc` command, and `$LUMA_RUNTIME`
  replaces the archive path.
- **No assembler runs:** `cc` receives only an object file and an archive, so
  it runs nothing but the linker. The e2e suite checks this with `cc -v`.
- **Output:** the result is a default PIE. All three relocation types are
  valid in a PIE: PC32 points at our own sections, PLT32 resolves the runtime
  functions, and the linker turns each R_X86_64_64 in `.data` into a dynamic
  `R_X86_64_RELATIVE` that the loader applies at startup.

External dependencies:

- a C11 compiler, to build `luma` and the runtime
- `cc`/`ld` with glibc, for the final link

`readelf`, `objdump`, `objcopy`, GNU `as` and Python 3 are used **only** by the
tests.

## Testing

| Suite | What it covers |
|---|---|
| `make unit` | lexer, parser, lowering, IR parse/print/verify, assembler encodings and relaxation, ELF layout (built by hand) |
| `make e2e` (`tests/run_e2e.sh`) | hello world pipeline and object inspection (at `-O2` and `-O0`); for every `tests/pos` program: exact output at `-O0`, `-O1` and `-O2`, GNU `as` byte equivalence, `.lir` recompilation identity and IR round trip; properties of the optimized code (no runtime calls in typed `fib`, frameless leaf loops, shifts for division by 4, tail calls as jumps); compile errors with locations; runtime errors and FFI programs at every level (exact stdout, stderr and exit status); hand-written `.lir` programs; lasm on its own |
| `make difftest` (`tests/difftest.py`) | random programs with functions, globals, multi-argument `print`, random **type annotations**, and random **structs** with `impl` methods, literals, field reads and writes and method calls (typed and through `dyn()`), compared including printed structs and runtime errors. Wrong-typed values are hidden behind an untyped `dyn()` so they reach runtime errors and the **runtime guards**, which the reference interpreter models with exact messages. Each program is compiled natively at `-O0`, `-O1` and `-O2` (`DIFFTEST_LEVELS`) and compared with an independent Python reference interpreter, plus the IR round-trip and GNU `as` checks |
| `make selftest` | `examples/selftest.luma`: 91 checks written in Luma itself, built on a `check()` function. It exits through `extern fun exit` |
| e2e FFI section | `tests/ffi`: the C helper library `ffi_helper.c` (built with the system cc, test-only), round trips for every C type and boundary value, 9 runtime-error cases, and the link modes |
| `make bench` (`bench/run.py`) | wall time of seven programs (recursion, loops, division, strings, structs) at `-O0` and `-O2`, with their output; extra luma flags (e.g. `--mm=rc`) are passed through |

The optimizer and backend were also checked with ASan/UBSan builds of the
compiler (`make BUILD=build-asan CFLAGS="-fsanitize=address,undefined …"`,
then the suites with `LUMA=build-asan/luma`), and by planting bugs in the
backend (a dropped `jo`, a wrong swapped condition, no register saving
around slow paths, a missing floor fix-up, ignoring calls in allocation):
`make difftest` catches each of them. Field load forwarding was checked the
same way: forgetting to invalidate on an aliased write, a dynamic write, a
call, or a reassignment of the object each makes `tests/pos/struct_aliasing`
fail.

Memory management is covered by:
- `tests/mem`: exact `LUMA_MEMCHECK` reports for every mode at every level
  (strings, object graphs and cycles, a program that frees everything, a
  use after free through a typed alias, a million-node list);
- every positive program run again under `rc` and `none` with memcheck on;
- compile errors (`tests/neg/free_*`), runtime errors (`tests/rt/free_*`,
  `use_after_free`), and unit tests of the memory pass on hand-written LIR;
- the difftest, which runs each program under one mode with memcheck. In
  `rc` and `none` it compares the number of struct instances alive at exit
  with the reference interpreter's prediction (none: all of them; rc: those
  reachable from globals, plus garbage cycles and what they reference).
  Programs `free()` fresh local instances and store instances in untyped
  fields to build graphs and cycles. `DIFFTEST_VALGRIND=1` runs every
  executable under valgrind.

Planted bugs in the memory pass were each caught: no edge releases, no
release of unused results, no retain of `getfield` results, consuming
without retaining, no release when a global or a field is overwritten, no
retain of reassigned parameters. The tests also found two real bugs during
development:
- parameters were never considered live at entry, because the CFG liveness
  defines them there;
- a method wrapper's argument check retained a value whose copy was
  borrowed, which leaked one count per dynamic call.

Structs are covered by `tests/pos/structs.luma` and `struct_aliasing.luma`
(exact output at every level), 20 `tests/neg/struct_*` compile errors, 10
`tests/rt/struct_*` runtime errors (missing fields and methods, arity, field
type guards on dynamic writes and on refined values), `tests/ir/structs.lir`
(hand-written v0.5 IR), and unit tests for parsing, lowering, IR round trip and
the verifier's struct rules.
