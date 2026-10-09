# Luma bootstrap compiler: design notes (milestone 1)

This document describes the first vertical slice of the Luma toolchain:
`print "…";` → x86-64 assembly → Luma assembler → ELF64 relocatable object →
system linker → native executable.

## Pipeline and modules

```
examples/hello.luma
   │  src/lexer.c        bytes → tokens (PRINT, STRING, SEMICOLON, EOF)
   ▼
   │  src/parser.c       tokens → AST (src/ast.h): Program{Stmt[]}, Stmt=Print(Expr=String)
   ▼
   │  src/codegen.c      AST → assembly text           ──► build/hello.s
   ▼
   │  src/asm.c          assembly text → ObjFile       (reads build/hello.s back from disk)
   ▼                     (src/obj.h: sections, symbols, relocations — no ELF knowledge)
   │  src/elf_writer.c   ObjFile → ELF64 ET_REL bytes  ──► build/hello.o
   ▼
   │  src/main.c         driver: runs `cc -o build/hello build/hello.o`
   ▼
build/hello  →  "Hello, world!\n"
```

Driver and build plumbing live in `src/main.c` (the `luma` command), `src/lasm.c`
(standalone assembler, `lasm in.s -o out.o`), and the `Makefile`.

The assembler and the ELF writer only meet through `ObjFile` (`src/obj.h`).
`tests/unit/test_asm.c` checks the assembler's bytes and relocations without
writing ELF. `tests/unit/test_elf.c` builds an `ObjFile` by hand and parses
the ELF output back with the host `<elf.h>`, without using the assembler.

## Language (milestone 1)

```
program    := statement* EOF
statement  := 'print' STRING ';'
```

- Whitespace: space, tab, CR, LF. There are no comments yet.
- Identifiers other than `print` are rejected (`unknown identifier`).
- Supported string escapes: `\"`, `\\`, `\n`, `\t`. **Not supported yet:**
  `\0`, `\r`, `\xNN`, `\u{…}`, and any other escape. Each is rejected
  with an error that points at it.
- A raw newline or EOF inside a string is reported as `unterminated string literal`.
- NUL bytes are rejected in source files and in strings, because `puts`
  stops at the first NUL.
- Non-ASCII bytes (such as UTF-8) pass through unchanged.
- Diagnostics use the form `path:line:col: error: message`. Compilation stops
  at the first error and no output files are written.

### Print semantics

`print "s";` calls `puts(s)`, which writes the bytes of `s` followed by
one `'\n'`. So `print "a\n";` prints two newlines, and `print "";` prints
an empty line. `puts`'s return value is ignored, and the program exits
with status 0.

## Code generation

```asm
    .intel_syntax noprefix
    .section .rodata
.Lstr0:    # "Hello, world!"
    .asciz "Hello, world!"
    .text
    .globl main
    .type main, @function
main:
    push rbp                 # entry: rsp ≡ 8 (mod 16); after push: ≡ 0
    mov rbp, rsp
    lea rdi, [rip + .Lstr0]  # SysV arg0
    call puts@PLT            # rsp is 16-byte aligned at the call, as the ABI requires
    xor eax, eax             # return 0
    pop rbp
    ret
    .size main, .-main
    .section .note.GNU-stack
```

- Each print statement adds one `lea`/`call` pair and one `.LstrN` literal.
- Only caller-saved registers (`rdi`, `rax`) are used, so nothing else is saved.
- In `.asciz`, printable ASCII is written literally (with `"` and `\` escaped).
  Every other byte becomes a 3-digit octal escape, e.g. `\012` for newline.
- The dialect is a strict subset of GNU `as` Intel syntax, on purpose. This
  lets the test suite assemble the same file with GNU `as` and compare the
  output byte for byte. The pipeline itself **never** runs an external assembler.

## Assembler (`src/asm.c`)

It works in one pass over the lines. A fixup list is resolved at the end, so
forward references to labels work. Every PC-relative field is a fixed 32-bit
field, so instruction sizes never depend on where labels end up, and no
relaxation pass is needed.

### Syntax accepted

- `label:` (several per line allowed), then `mnemonic op, op` or `.directive args`.
- `#` starts a comment. A `#` inside a string literal does not.
- Registers: the 64-bit GPRs `rax`…`r15` and the 32-bit GPRs `eax`…`r15d`.
- Immediates: decimal, `0x` hex, leading-`0` octal, and negative numbers.
- Memory operands: **RIP-relative only**, written `[rip + sym]`, `[rip + sym ± n]` or `[rip ± n]`.
- Call targets: `sym` or `sym@PLT`.

### Supported instructions and encodings

| Form                    | Encoding                         | Notes |
|-------------------------|----------------------------------|-------|
| `ret`                   | `C3`                             | |
| `nop`                   | `90`                             | |
| `push r64` / `pop r64`  | `[41] 50+r` / `[41] 58+r`        | REX.B for r8–r15 |
| `mov r64, r64`          | `REX.W 89 /r`                    | ModRM mod=11, reg=src, rm=dst (same choice as GAS) |
| `mov r32, r32`          | `[REX] 89 /r`                    | |
| `mov r32, imm32`        | `[41] B8+r id`                   | zero-extends to 64 bits |
| `mov r64, imm`          | `REX.W C7 /0 id` if the value fits in a sign-extended 32-bit immediate, otherwise `REX.W B8+r io` | the shortest form, as GAS chooses |
| `xor/add/sub r, r`      | `[REX] 31/01/29 /r`              | 32- or 64-bit |
| `add/sub r, imm`        | `[REX] 83 /0,/5 ib` (imm8) or `81 /0,/5 id` | imm8 when it fits |
| `xor r, imm`            | `[REX] 83 /6 ib` or `81 /6 id`   | |
| `lea r64, [rip+…]`      | `REX.W[+R] 8D /r`, ModRM `00 reg 101`, disp32 | relocated or constant |
| `call sym`              | `E8 rel32`                       | |

A REX prefix is emitted only when W, R, X or B is set. Size-qualified memory
(`qword ptr …`), SIB/base+index addressing, jumps and conditional branches are
**not** supported yet. Each of these is rejected with an error message.

### Directives

`.intel_syntax noprefix`, `.text`, `.data`, `.section NAME`, `.globl`/`.global`,
`.extern`, `.type sym, @function|@object`, `.size sym, .-sym | N`,
`.byte` (up to 16 values per line), `.ascii`, `.asciz`/`.string`.
String escapes in directives: `\n \t \r \" \\` and 1–3 digit octal.

### Sections

| Name              | sh_type   | Flags | Align |
|-------------------|-----------|-------|-------|
| `.text`           | PROGBITS  | AX    | 16    |
| `.rodata`         | PROGBITS  | A     | 1     |
| `.data`           | PROGBITS  | WA    | 1     |
| `.note.GNU-stack` | PROGBITS  | none  | 1     |

The empty `.note.GNU-stack` section marks the object as not needing an
executable stack, so the linker doesn't warn about it. Code placed before
any section directive goes into `.text`, as with GAS. An instruction in a
section without the execute flag is an error.

### Symbols and fixup resolution

Every PC-relative field is the last field of its instruction, so
`P + 4` is the address of the next instruction, which is what the CPU adds the
displacement to. The fixup therefore stores `addend = (operand constant) − 4`.
Each fixup is then resolved as follows:

| Target symbol                                | Result |
|----------------------------------------------|--------|
| Local (not `.globl`), same section            | Patched in place with `S + A − P`, and no relocation is emitted. A range check is applied. |
| Local, different section                      | Relocation against that section's **STT_SECTION** symbol, with `addend += symbol offset`. This is how `.L` labels refer across sections, because `.L` labels never appear in `.symtab`. |
| Global and defined                            | Relocation against the symbol itself, which keeps it preemptible. This matches GAS. |
| Undefined                                     | Marked global (external) and relocated against the symbol. |
| An undefined `.L` label                       | An error. |

Relocation types:

- `lea … [rip + sym]` emits **R_X86_64_PC32** (`S + A − P`).
- `call sym` emits **R_X86_64_PLT32** (`L + A − P`), with or without `@PLT`.
  This lets the linker route calls to shared-library functions such as
  `puts` through the PLT, and it is also correct for local functions.
  GAS behaves the same way.

The ELF writer can also emit `R_X86_64_64`, but the assembler doesn't produce it yet.

For hello world this gives `R_X86_64_PC32 .rodata − 4` at `.text+7` and
`R_X86_64_PLT32 puts − 4` at `.text+0xc`. GNU `as` produces the identical
result, and the e2e suite checks this.

## ELF64 writer (`src/elf_writer.c`)

The writer emits an ET_REL file for EM_X86_64 (ELFCLASS64, little-endian,
SysV OSABI). It has no program headers, and `e_entry` is 0. It defines its own
ELF constants and does not depend on the host `<elf.h>`.

File layout:

```
0x00  ELF header (64 bytes)
      user sections in creation order, each aligned to its sh_addralign
      .rela.<sec> for every section that has relocations   (align 8, entsize 24)
      .symtab                                              (align 8, entsize 24)
      .strtab
      .shstrtab
      section header table                                 (align 8)
```

Section header indices are assigned in this order: `0` is the null section,
then the user sections (`1..N`), then the `.rela.*` sections, then `.symtab`,
`.strtab` and `.shstrtab`. `e_shstrndx` points at `.shstrtab`.

- **`.rela.X`**: `sh_link` = `.symtab`, `sh_info` = the index of X, and flags include `SHF_INFO_LINK`.
- **`.symtab`**: `sh_link` = `.strtab`, `sh_info` = the index of the first non-local symbol.
  Symbols are written in this order:
  1. the null symbol
  2. an `STT_FILE` symbol for the source file (SHN_ABS)
  3. one `STT_SECTION` symbol per user section
  4. named local symbols
  5. global symbols, defined first and then undefined (`SHN_UNDEF`)

  All locals come before all globals, as the ELF specification requires.

Before writing anything, the writer checks the object model and reports an
error instead of producing a corrupt file. It checks that:

- every relocation type is known
- every relocation field lies inside its section
- every symbol and section index is in range
- no relocation names a `.L` label
- every symbol value lies inside its section

## Linking

`luma` runs `cc -o OUT OUT.o` through `posix_spawnp`. No shell is involved.
You can override the command with `$LUMA_CC`. Because `cc` receives only an
object file, it runs nothing but the linker (`collect2` → `ld`), which adds
the C runtime start files and libc. The e2e suite checks with `cc -v` that no
assembler runs during this step. The executable is a default PIE. Both
relocation types are valid in PIE: PC32 points at our own `.rodata`, and
PLT32 resolves `puts` through the PLT.

External dependencies:

- a C11 compiler to build `luma` itself
- a system C toolchain driver (`cc`, with binutils `ld` and glibc) for the final link

`readelf`, `objdump`, `objcopy` and GNU `as` are used **only** by the tests,
to inspect and cross-check the output.
