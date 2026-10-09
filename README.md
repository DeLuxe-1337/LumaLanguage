# Luma

Luma is a dynamically typed, natively compiled programming language. The goal is for it to compile itself.
This repository holds the **bootstrap compiler**, which is written in C. It
includes its own x86-64 assembler and ELF64 object writer.

Milestone 1 is a complete pipeline for a single statement form:

```
print "Hello, world!";
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
- a C11 compiler, to build `luma`
- the system `cc` and `ld` with glibc, for the final link only

`readelf`, `objdump`, `objcopy` and GNU `as` are needed only by `make test`.

## What `luma` does

`luma IN.luma -o OUT` writes three files:

| File    | Produced by |
|---------|-------------|
| `OUT.s` | Luma code generator (`src/codegen.c`). The output is readable Intel-syntax assembly. |
| `OUT.o` | Luma assembler (`src/asm.c`) and ELF writer (`src/elf_writer.c`). The assembler reads `OUT.s` back from disk. |
| `OUT`   | The system linker, run as `cc -o OUT OUT.o`. You can override the command with `$LUMA_CC`. |

Options:

```
-S              stop after writing OUT.s
-c              stop after writing OUT.o
--dump-tokens   print the token stream
--dump-ast      print the AST
-v              print each stage and the exact link command
```

The standalone assembler takes assembly directly:

```bash
./build/lasm file.s -o file.o
```

## Inspecting the intermediate files

```bash
make inspect                      # rebuilds hello, prints .s, readelf and objdump output
cat build/hello.s                 # generated assembly
readelf -h -S -s -r build/hello.o # ELF header, sections, symbols, relocations
objdump -d -r -M intel build/hello.o
readelf -h build/hello            # final native executable
```

## Tests

```bash
make test      # unit tests + end-to-end suite
make unit      # test_frontend, test_asm, test_elf
make e2e       # tests/run_e2e.sh
```

- **`tests/unit/`** has unit tests for the lexer, parser and code generator, for
  the assembler encodings and fixups, and for the ELF writer. The ELF writer test
  builds its object by hand and parses the output back with `<elf.h>`.
- **`tests/pos/`** holds `.luma` programs together with their exact expected stdout.
- **`tests/neg/`** holds malformed programs together with the expected error text:
  - an unterminated string
  - a missing `;`
  - a missing string
  - a missing `print`
  - a bad escape
  - and more
- **`tests/asm/`** checks the assembler on its own:
  - a C program links against an object built by `lasm`.
  - A coverage file is assembled by both `lasm` and GNU `as`. The suite checks
    that `.text`, `.rodata` and `.data` are byte-identical and that the
    relocations match. GNU `as` is used only for this comparison.

## Layout

```
src/        lexer, parser + ast, codegen, asm, obj (object model), elf_writer, main (luma), lasm
examples/   hello.luma
tests/      unit/, pos/, neg/, asm/, run_e2e.sh
docs/       DESIGN.md: encodings, relocations, section layout, symbol resolution
```

See [docs/DESIGN.md](docs/DESIGN.md) for the details of the language subset, the
supported instructions, and the ELF layout.
