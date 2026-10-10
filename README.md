# Luma

Luma is a dynamically typed, natively compiled programming language. The goal is for it to compile itself.
This repository holds the **bootstrap compiler**, which is written in C. It
includes its own intermediate representation (LIR), an SSA optimizer, a
register-allocating x86-64 backend, an assembler and an ELF64 object writer.

The syntax started from Lox and has grown its own features:

- `print` is a builtin function
- types are optional and gradual
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
-v              print each stage and the exact link command
-l LIB, -L DIR  link a C library / add a library search directory
EXTRA.o|.a|.so  extra objects or libraries for the linker
```

Environment variables:

- `LUMA_CC` replaces the `cc` command used to link.
- `LUMA_RUNTIME` replaces the path to `libluma_rt.a`.

The standalone assembler takes assembly directly:

```bash
./build/lasm file.s -o file.o
```

## The language today

| | |
|---|---|
| Values | 63-bit integers, strings, `true`, `false`, `nil` |
| Functions | `fun name(a, b) { … return v; }` at top level, called as `name(x, y)`. Recursion and mutual recursion work, and functions can be called before their definition. Up to 6 parameters. Arity is checked at compile time. A missing `return` returns `nil`. |
| Builtins | `print(a, b, …)` writes its arguments separated by spaces, then a newline, and returns `nil`. All arguments are evaluated before anything is written. |
| Types | Optional annotations: `var n: int`, `fun f(a: str, b): bool?`. The types are `int str bool nil any` and `T?` (T or nil). Mismatches the compiler can prove are compile errors; values whose type is only known at runtime are checked by a guard at the annotated boundary. Unannotated code stays fully dynamic. |
| C FFI | `extern fun name(p: ctype, …): ctype;` with `i8`…`i64`, `u8`…`u64`, `bool`, `cstr`, `cstr?`, `ptr` and `void`. Values are converted and range-checked at the call. Link libraries with `-l NAME -L DIR`, or pass `.o`/`.a`/`.so` files. |
| Statements | `var`, expression statements, `{ }` blocks, `if`/`else`, `while`, `for`, `return` |
| Operators | `= or and == != < <= > >= + - * / ! -` (unary), and `+` on two strings concatenates them |
| Variables | Top-level variables are globals that functions can read and write. Variables in blocks and functions are local, with lexical scoping checked at compile time. |
| Semantics | Lox truthiness (only `nil` and `false` are falsy). `and`/`or` return an operand. Integer overflow, type errors, reading a global before it is assigned, and stack overflow are runtime errors (exit status 1). `/` is floor division. |
| Not yet | first-class functions and closures, classes, floats (including C `f32`/`f64`), C structs and callbacks, `break`, a GC (runtime strings are never freed) |

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
machine (`make bench`, wall time):

| Benchmark | `-O0` | `-O2` | Speedup |
|---|---|---|---|
| collatz (1M starts) | 2.41 s | 0.38 s | 6.4× |
| fib(37), untyped | 0.35 s | 0.11 s | 3.1× |
| fib(37), typed | 0.34 s | 0.12 s | 2.9× |
| loop_sum (100M iterations) | 1.02 s | 0.12 s | 8.8× |
| primes (to 2M) | 1.97 s | 0.61 s | 3.2× |
| strings (3M concatenations) | 0.15 s | 0.007 s | 20.6×* |

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
| `tests/ir/` | Hand-written LIR programs: loops, six-argument calls and recursion. |
| `tests/ffi/` | C FFI: `ffi_helper.c` plus Luma programs that round-trip every C type, and runtime FFI errors. |
| `tests/asm/` | lasm on its own: a C program linked against an object built by `lasm`, and a coverage file compared byte for byte against GNU `as`. |
| `tests/difftest.py` | Random programs compiled natively at every optimization level and compared with an independent Python interpreter. |
| `bench/` | Benchmark programs and `run.py`. |

## Layout

```
src/        lexer, parser + ast, lower (AST→LIR), ir/ir_parse/ir_verify/ir_types,
            cfg + opt* (optimizer), x86_gen (optimizing LIR→asm), x86_isel (naive LIR→asm),
            asm, obj (object model), elf_writer, value.h (shared value tags), main (luma), lasm
runtime/    luma_rt.c: the C runtime linked into every program
examples/   hello.luma, fizzbuzz.luma, ffi.luma, selftest.luma
tests/      unit/, pos/, neg/, rt/, ir/, ffi/, asm/, run_e2e.sh, difftest.py
bench/      benchmark programs and run.py
docs/       DESIGN.md: pipeline, language, runtime, optimizer, backends, assembler, ELF, linking
            IR.md:     LIR specification
```
