# Luma

Luma is a dynamically typed, natively compiled programming language. The goal is for it to compile itself.
This repository holds the **bootstrap compiler**, which is written in C. It
includes its own intermediate representation (LIR), an SSA optimizer, a
register-allocating x86-64 backend, an assembler and an ELF64 object writer.

The syntax started from Lox and has grown its own features:

- `print` is a builtin function
- types are optional and gradual
- structs with `impl` blocks (methods and associated functions, Rust-style)
- memory management you choose per build: manual (`free`), reference
  counting, or none, with the compiler freeing what it can prove dead
- C functions are called directly through `extern fun`

Nothing is final before Luma bootstraps.

```js
extern fun strlen(s: cstr): u64;          // a C function, linked from libc
extern fun getenv(name: cstr): cstr?;     // NULL comes back as nil

fun fib(n: int): int {                    // typed: checked at compile time
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

fun shout(x) { return x + "!"; }          // untyped: fully dynamic

var name: str = "luma";
print(shout(name), strlen(name), fib(20));
print("HOME is", getenv("HOME") or "unset");

struct Point { x: int, y: int }

impl Point {
  fun new(x: int, y: int): Point { return Point { x, y }; }   // associated function
  fun len2(self): int { return self.x * self.x + self.y * self.y; }
}

var p = Point::new(3, 4);
p.x = p.x + 1;
print(p, p.len2());                       // Point { x: 4, y: 4 } 32
```

## Quick start

```bash
make clean
make
./build/luma examples/hello.luma -o build/hello
./build/hello          # → Hello, world!
```

Requirements:

- Linux x86-64
- a C11 compiler, to build `luma` and its runtime
- the system `cc` and `ld` with glibc, for the final link only

The tests also use `readelf`, `objdump`, `objcopy`, GNU `as` and Python 3.
None of these are part of the pipeline.

## What `luma` does

`luma IN.luma -o OUT` writes these files:

| File | Produced by |
|---|---|
| `OUT.lir` | Front end (`lexer`, `parser`) and lowering (`lower.c`). This is Luma's IR in its text form; see [docs/IR.md](docs/IR.md). |
| `OUT.opt.lir` | The optimizer (`opt*.c`, at `-O2`, the default): constant propagation and branch folding, CSE, loop-invariant code motion, dead-code elimination and CFG cleanup on SSA form. It must pass the IR verifier again. |
| `OUT.mem.lir` | The memory pass (`mem.c`, with `--mm=manual` or `rc`): which allocations are reference counted, and every `retain` and `release` the compiler inserted. |
| `OUT.s` | The backend, after the IR verifier passes. At `-O1`/`-O2` this is `x86_gen.c`: linear-scan register allocation, inline integer fast paths with out-of-line runtime calls, compare/branch fusion and tail calls. At `-O0` it is the naive `x86_isel.c`, with one stack slot per value. The output is readable Intel-syntax assembly, with each LIR instruction shown as a comment. |
| `OUT.o` | Luma's assembler (`asm.c`) and ELF writer (`elf_writer.c`). The assembler reads `OUT.s` back from disk. |
| `OUT` | The system linker: `cc -o OUT OUT.o build/libluma_rt.a`. |

`luma` also accepts hand-written IR, for example `luma prog.lir -o prog`, which
lets you test the backend without the front end.

Options:

```
--emit-ir       stop after writing OUT.lir
-S              stop after writing OUT.s
-c              stop after writing OUT.o
--dump-tokens   print the token stream
--dump-ast      print the AST
--dump-ir       print the LIR module
--dump-opt-ir   print the optimized LIR module
-O0 / -O1 / -O2 naive backend / optimizing backend / optimizer + optimizing backend (default)
--mm=MODE       memory management: manual (default), rc or none (see below)
--dump-mem-ir   print the IR after the memory pass
-v              print each stage and the exact link command
-l LIB, -L DIR  link a C library / add a library search directory
EXTRA.o|.a|.so  extra objects or libraries for the linker
```

Environment variables:

- `LUMA_CC` replaces the `cc` command used to link.
- `LUMA_RUNTIME` replaces the path to `libluma_rt.a`.
- `LUMA_MEMCHECK=1`, set when running a compiled program, reports leaks at
  exit and turns use-after-free and double free into runtime errors.

The standalone assembler takes assembly directly:

```bash
./build/lasm file.s -o file.o
```

Its input language, encodings and relocation rules are specified in
[docs/LASM.md](docs/LASM.md).

## The language today

| | |
|---|---|
| Values | 63-bit integers, strings, `true`, `false`, `nil`, struct instances |
| Structs | `struct P { x: int, y }` declares fields (typed or untyped). `P { x: 1, y }` builds one (every field, any order, `y` is shorthand for `y: y`). Instances are references: assignment shares, `==` is identity, and they are always truthy. `print` shows `P { x: 1, y: "s" }`. |
| `impl` | `impl P { fun m(self, a) { … } fun new(…) { … } }` adds methods (first parameter `self`, untyped) and associated functions (no `self`). Call them as `p.m(1)`, `P::new(…)` or `P::m(p, 1)`. Several `impl` blocks per struct are allowed. When the object's static type is exactly `P`, field access and method calls are compiled statically; otherwise they are looked up by name at runtime, with the same error messages. |
| Functions | `fun name(a, b) { … return v; }` at top level, called as `name(x, y)`. Recursion and mutual recursion work, and functions can be called before their definition. Up to 6 parameters. Arity is checked at compile time. A missing `return` returns `nil`. |
| Builtins | `print(a, b, …)` writes its arguments separated by spaces, then a newline, and returns `nil`. All arguments are evaluated before anything is written. |
| Types | Optional annotations: `var n: int`, `fun f(a: str, b): bool?`. The types are `int str bool nil any`, struct names, and `T?` (T or nil). Mismatches the compiler can prove are compile errors; values whose type is only known at runtime are checked by a guard at the annotated boundary. Unannotated code stays fully dynamic. |
| C FFI | `extern fun name(p: ctype, …): ctype;` with `i8`…`i64`, `u8`…`u64`, `bool`, `cstr`, `cstr?`, `ptr` and `void`. Values are converted and range-checked at the call. Link libraries with `-l NAME -L DIR`, or pass `.o`/`.a`/`.so` files. |
| Statements | `var`, expression statements, `{ }` blocks, `if`/`else`, `while`, `for`, `return` |
| Operators | `= or and == != < <= > >= + - * / ! -` (unary), and `+` on two strings concatenates them |
| Variables | Top-level variables are globals that functions can read and write. Variables in blocks and functions are local, with lexical scoping checked at compile time. |
| Semantics | Lox truthiness (only `nil` and `false` are falsy). `and`/`or` return an operand. Integer overflow, type errors, reading a global before it is assigned, and stack overflow are runtime errors (exit status 1). `/` is floor division. |
| Memory | Chosen per build with `--mm` (below). `free(x)` frees a struct instance in `manual` mode and is accepted (and does nothing) in the others, so a program compiles in every mode. Using a variable after `free` on every path is a compile error. |
| Not yet | first-class functions and closures, traits/interfaces, floats (including C `f32`/`f64`), C structs and callbacks, `break`, collecting reference cycles |

## Memory management

```js
struct Point { x: int, y: int }
struct Node { value: int, label: str, next: Node? }

impl Point {
  fun len2(self): int { return self.x * self.x + self.y * self.y; }
}

fun destroy(list: Node?) {
  while (list != nil) {
    var next = list.next;
    free(list);                       // releases its label string too
    list = next;
  }
}

fun dist2(a: int, b: int): int {
  var p = Point { x: a, y: b };       // never leaves the function:
  return p.len2();                    // the compiler frees it
}

fun greet(name: str): str { return "hello, " + name; }

var list: Node? = nil;
for (var i = 0; i < 3; i = i + 1) list = Node { value: i, label: greet("node"), next: list };
destroy(list);
list = nil;
print(greet("luma") + "!", dist2(3, 4));   // both strings are freed after print
```

| `--mm=` | Struct instances | Strings and temporaries |
|---|---|---|
| `manual` (default) | yours to `free()`; instances that provably never leave their function are freed for you | freed automatically at their last use |
| `rc` | reference counted (ARC-style); cycles leak | reference counted |
| `none` | never freed | never freed |

The compiler inserts the retain/release operations itself, after
optimization, using liveness and ownership:
- Parameters are borrowed, so passing an object costs nothing.
- A last use moves a reference instead of copying it.
- Values whose type can't be a heap object get no operations at all: typed
  integer code compiles exactly as before.

`OUT.mem.lir` shows every decision. Run any program with `LUMA_MEMCHECK=1` to
see what it leaves behind:

```
$ luma --mm=rc tests/mem/graphs.luma -o graphs && LUMA_MEMCHECK=1 ./graphs
124750
9 7
luma: memcheck: --mm=rc, 1016 allocated, 1009 freed, 3 leaked, 4 reachable from globals
luma: memcheck: leaked: 3 Pair
luma: memcheck: reachable from globals: 1 Node, 3 Pair
```

The design, the analyses and the runtime are described in
[docs/DESIGN.md](docs/DESIGN.md) ("Memory").

## Inspecting the intermediate files

```bash
make inspect                         # rebuilds hello and prints .lir, .s, readelf and objdump output
./build/luma prog.luma --emit-ir -o build/prog && cat build/prog.lir
readelf -h -S -s -r build/hello.o    # ELF header, sections, symbols, relocations
objdump -d -r -M intel build/hello.o
```

## Tests

```bash
make test        # unit + end-to-end + differential
make unit        # test_frontend, test_ir, test_asm, test_elf
make e2e         # tests/run_e2e.sh
make difftest    # random programs vs. a reference interpreter (DIFFTEST_COUNT=300)
make selftest    # compile and run examples/selftest.luma
make bench       # benchmarks at -O0 and -O2
```

Every test program runs at `-O0`, `-O1` and `-O2`. Benchmarks on the build
machine (`make bench`, best of 5, wall time):

| Benchmark | `-O0` | `-O2` | Speedup |
|---|---|---|---|
| collatz (1M starts) | 1.74 s | 0.32 s | 5.5× |
| fib(37), untyped | 0.38 s | 0.12 s | 3.1× |
| fib(37), typed | 0.32 s | 0.11 s | 3.0× |
| loop_sum (100M iterations) | 0.69 s | 0.11 s | 6.2× |
| primes (to 2M) | 1.51 s | 0.53 s | 2.9× |
| strings (3M concatenations) | 0.09 s | 0.008 s | 11.7×* |
| structs (20M method calls, list walks) | 1.52 s | 0.11 s | 14.3× |

These are in the default `--mm=manual`. At `-O2` the three memory modes are
within a few percent of each other on every benchmark (`docs/DESIGN.md`,
"Memory").

\* The loop's `"x" + "y"` is loop-invariant, so `-O2` computes it once.
Strings are immutable and compared by content, so this is not observable.

`examples/selftest.luma` is a Luma program that checks the compiler from the
inside. It runs 91 checks covering:

- arithmetic and floor division
- fixnum limits
- precedence
- equality across types
- truthiness
- `and`/`or` values and short-circuiting
- strings and escapes
- scoping and shadowing
- loops (gcd, primes and fib(90))
- evaluation order
- `else` binding
- functions: recursion, mutual recursion, hoisting, early return, globals

Each failing check prints `FAIL: <description>`. The program ends with
`SELFTEST PASSED` (exit 0) or `SELFTEST FAILED` (exit 1). Each check is one
call to a `check` function defined in the file:

```js
check(-7 / 2 == -4, "-7 / 2 floors toward -infinity");
```

| Location | Contents |
|---|---|
| `tests/unit/` | Unit tests for the lexer, parser, lowering, IR (round trip and verifier), assembler (encodings and branch relaxation) and ELF writer. |
| `tests/pos/` | Programs with their exact expected output. For each one, the suite also checks that GNU `as` builds a byte-identical object from our `.s`, and that the emitted `.lir` recompiles to identical assembly. |
| `tests/neg/` | Compile errors, with the expected message and source location. |
| `tests/rt/` | Runtime errors, with exact stdout, stderr and exit status. |
| `tests/mem/` | Memory management: exact `LUMA_MEMCHECK` reports for each mode at every level. |
| `tests/ir/` | Hand-written LIR programs: loops, six-argument calls and recursion. |
| `tests/ffi/` | C FFI: `ffi_helper.c` plus Luma programs that round-trip every C type, and runtime FFI errors. |
| `tests/asm/` | lasm on its own: a C program linked against an object built by `lasm`, and a coverage file compared byte for byte against GNU `as`. |
| `tests/difftest.py` | Random programs (including structs, methods, dynamic field access and `free`), each under one memory mode with memcheck on, compiled natively at every optimization level and compared with an independent Python interpreter. |
| `bench/` | Benchmark programs and `run.py`. |

## Layout

```
src/        lexer, parser + ast, lower (AST→LIR), ir/ir_parse/ir_verify/ir_types,
            cfg + opt* (optimizer), mem (memory analyses), x86_gen (optimizing LIR→asm), x86_isel (naive LIR→asm),
            asm, obj (object model), elf_writer, value.h (shared value tags), main (luma), lasm
runtime/    luma_rt.c: the C runtime linked into every program
examples/   hello.luma, fizzbuzz.luma, ffi.luma, selftest.luma
tests/      unit/, pos/, neg/, rt/, mem/, ir/, ffi/, asm/, run_e2e.sh, difftest.py
bench/      benchmark programs and run.py
docs/       DESIGN.md: pipeline, language, runtime, optimizer, backends, assembler, ELF, linking
            IR.md:     LIR specification
            LASM.md:   assembler specification (syntax, encodings, relocations)
```
