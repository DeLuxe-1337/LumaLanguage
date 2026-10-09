# Luma

Luma is a dynamically typed, natively compiled programming language. The goal is for it to compile itself.
This repository holds the **bootstrap compiler**, which is written in C. It
includes its own intermediate representation (LIR), x86-64 assembler and ELF64
object writer.

The current surface syntax follows Lox. It will change before Luma bootstraps.

```js
// examples/fizzbuzz.luma
for (var i = 1; i <= 15; i = i + 1) {
  if (i - i / 15 * 15 == 0) print "FizzBuzz";
  else if (i - i / 3 * 3 == 0) print "Fizz";
  else if (i - i / 5 * 5 == 0) print "Buzz";
  else print i;
}
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

`luma IN.luma -o OUT` writes four files:

| File | Produced by |
|---|---|
| `OUT.lir` | Front end (`lexer`, `parser`) and lowering (`lower.c`). This is Luma's IR in its text form; see [docs/IR.md](docs/IR.md). |
| `OUT.s` | Instruction selection (`x86_isel.c`), after the IR verifier passes. The output is readable Intel-syntax assembly, with each LIR instruction shown as a comment. |
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
-v              print each stage and the exact link command
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
| Statements | `var`, `print`, expression statements, `{ }` blocks, `if`/`else`, `while`, `for` |
| Operators | `= or and == != < <= > >= + - * / ! -` (unary), and `+` on two strings concatenates them |
| Semantics | Lox truthiness (only `nil` and `false` are falsy). `and`/`or` return an operand. Lexical block scoping is checked at compile time. Integer overflow and type errors are runtime errors (exit status 1). `/` is floor division. |
| Not yet | functions, classes, floats, `break`, a GC (runtime strings are never freed) |

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
```

`examples/selftest.luma` is a Luma program that checks the compiler from the
inside. It runs 77 checks covering:

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

Each failing check prints `FAIL: <description>`. The program ends with
`SELFTEST PASSED` (exit 0) or `SELFTEST FAILED` (exit 1). Until Luma has
functions, each check is written out inline:

```js
t = t + 1; if (!(-7 / 2 == -4)) { f = f + 1; print "FAIL: -7 / 2 floors toward -infinity"; }
```

| Location | Contents |
|---|---|
| `tests/unit/` | Unit tests for the lexer, parser, lowering, IR (round trip and verifier), assembler (encodings and branch relaxation) and ELF writer. |
| `tests/pos/` | Programs with their exact expected output. For each one, the suite also checks that GNU `as` builds a byte-identical object from our `.s`, and that the emitted `.lir` recompiles to identical assembly. |
| `tests/neg/` | Compile errors, with the expected message and source location. |
| `tests/rt/` | Runtime errors, with exact stdout, stderr and exit status. |
| `tests/ir/` | Hand-written LIR programs: loops, six-argument calls and recursion. |
| `tests/asm/` | lasm on its own: a C program linked against an object built by `lasm`, and a coverage file compared byte for byte against GNU `as`. |
| `tests/difftest.py` | Random programs compiled natively and compared with an independent Python interpreter. |

## Layout

```
src/        lexer, parser + ast, lower (AST→LIR), ir/ir_parse/ir_verify, x86_isel (LIR→asm),
            asm, obj (object model), elf_writer, value.h (shared value tags), main (luma), lasm
runtime/    luma_rt.c: the C runtime linked into every program
examples/   hello.luma, fizzbuzz.luma, selftest.luma
tests/      unit/, pos/, neg/, rt/, ir/, asm/, run_e2e.sh, difftest.py
docs/       DESIGN.md: pipeline, language, runtime, assembler, ELF, linking
            IR.md:     LIR specification
```
