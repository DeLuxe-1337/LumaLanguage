# Luma bootstrap compiler: design notes (milestone 3)

This document describes how the Luma toolchain is built. Its sections cover:

- the pipeline and its modules
- the language subset accepted today
- the C runtime
- the x86-64 assembler (encodings, relaxation, relocations)
- the ELF64 object writer
- linking

The intermediate representation (LIR) has its own specification in
[IR.md](IR.md).

## Pipeline and modules

```
INPUT.luma
   │  src/lexer.c        bytes → tokens (Lox token set)
   │  src/parser.c       tokens → AST (src/ast.h); `for` is desugared to `while`
   ▼
   │  src/lower.c        AST → LIR: static scoping, short-circuit logic   ──► OUT.lir
   ▼           (INPUT.lir enters here: src/ir_parse.c)
   │  src/ir_verify.c    structural + definite-assignment checks
   ▼
   │  src/x86_isel.c     LIR → x86-64 assembly text                       ──► OUT.s
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
| backend | `.lir` files are accepted as input |
| assembler | the `lasm` command and `tests/unit/test_asm.c` |
| ELF writer | `tests/unit/test_elf.c`, which builds an `ObjFile` by hand |

`src/value.h` defines the value representation. It is the single definition
shared by the compiler, which emits tagged constants, and the runtime, which
interprets them.

## Language (milestone 3)

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
  1. assignment (right-associative, target must be a bare identifier)
  2. `or`
  3. `and`
  4. `==` and `!=`
  5. `<`, `<=`, `>` and `>=`
  6. `+` and `-`
  7. `*` and `/`
  8. unary `!` and `-`
  9. calls `name(args…)`
  10. literals, variables and parentheses
- **Values:** integers, strings, `true`, `false` and `nil`. Truthiness, equality
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
| Undefined names | runtime errors | compile errors; a global read before its declaration executes is a runtime error |
| Strings | multi-line, no escapes | single line; `\"` `\\` `\n` `\t` |
| `class`, `this`, `super`, float literals | yes | rejected ("not supported yet") |

Compile errors have the form `path:line:col: error: message`, and compilation
stops at the first one. When it does, no `.lir`, `.s`, `.o` or executable is
written. Nesting depth, which includes long `a+b+c+…` chains, is bounded at
1000, so malformed input cannot overflow the C stack.

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

The generic operations behave as follows:

- Arithmetic on fixnums is checked for overflow.
- `/` and `mod` floor their results.
- `+` concatenates two strings.
- Strings created at run time are allocated with `malloc` and never freed,
  because v0.1 has no GC.

A runtime error flushes stdout, prints `luma: runtime error: <Lox-style
message>` to stderr, and exits with status 1.

**Stack overflow.** `main` installs a `SIGSEGV` handler on an alternate signal
stack. Unbounded recursion therefore reports `luma: runtime error: Stack
overflow.` (exit 1) instead of crashing with a bare segmentation fault.
Generated code only touches its own frame, the globals and runtime objects, so
a fault is almost always the stack. Any other memory fault would be reported
the same way.

Frames are large for now, because every virtual register has its own stack
slot. A small recursive function uses about 100 bytes per call, so with the
default 8 MB stack recursion overflows somewhere between 80,000 and 100,000
calls deep. The runtime is compiled with
`RT_CFLAGS`, separate from the compiler's `CFLAGS`, so a sanitizer build of
the compiler doesn't leak instrumentation into user programs.

## Code generation (`src/x86_isel.c`)

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

### Syntax accepted

- `label:` (several per line allowed), then `mnemonic op, op` or `.directive args`.
- `#` starts a comment. A `#` inside a string literal does not.
- Registers: the 64-bit GPRs `rax`…`r15` and the 32-bit GPRs `eax`…`r15d`.
- Immediates: decimal, `0x` hex, leading-`0` octal, and negative numbers.
- Memory operands come in two forms:
  - `[rip + sym ± n]` or `[rip ± n]`
  - `[rbp ± n]`

  Any other base register, and any index register, is rejected. This keeps
  SIB encoding out, because `rsp` as a base would need it.
- Branch and call targets: `sym` or `sym@PLT`.

### Instructions and encodings

| Form | Encoding | Notes |
|---|---|---|
| `ret` / `nop` | `C3` / `90` | |
| `push r64` / `pop r64` | `[41] 50+r` / `[41] 58+r` | REX.B for r8–r15 |
| `mov r, r` (32/64) | `[REX] 89 /r` | ModRM mod=11, reg=src, rm=dst (same choice as GAS) |
| `mov r64, [mem]` / `mov [mem], r64` | `REX.W 8B /r` / `REX.W 89 /r` | |
| `lea r64, [mem]` | `REX.W 8D /r` | |
| `mov r32, imm32` | `[41] B8+r id` | zero-extends |
| `mov r64, imm` | `REX.W C7 /0 id` when the value fits a sign-extended imm32; otherwise `REX.W B8+r io` | the same choices GAS makes |
| `add/or/and/sub/xor/cmp r, r` | `[REX] 01/09/21/29/31/39 /r` | |
| `add/or/and/sub/xor/cmp r, imm` | `83 /n ib` if the value fits imm8; else `05/0D/25/2D/35/3D id` when the register is `rax`/`eax` (accumulator forms, as GAS chooses); else `81 /n id` | |
| `test r, r` | `[REX] 85 /r` | |
| `test r, imm32` | `A9 id` (rax/eax) or `F7 /0 id` | no imm8 form exists |
| `call sym` | `E8 cd` | |
| `jmp sym` | `EB cb` or `E9 cd` | relaxed, see below |
| `jcc sym` (all 30 aliases: `je jz jne jnz jl jge jo …`) | `7x cb` or `0F 8x cd` | relaxed |

**Memory ModRM.** The memory forms use rm=101 in every case:

| Operand | ModRM | Displacement |
|---|---|---|
| `[rip+disp32]` | mod=00 | 32-bit |
| `[rbp+disp8]` | mod=01 | 8-bit |
| `[rbp+disp32]` | mod=10 | 32-bit |

`[rbp]` must be encoded with a zero disp8, because mod=00 with rm=101 means RIP-relative.

### Directives

`.intel_syntax noprefix`, `.text`, `.data`, `.section NAME`, `.globl`/`.global`,
`.extern`, `.type sym, @function|@object`, `.size sym, .-sym | N`,
`.byte`/`.quad` (up to 16 values per line), `.ascii`, `.asciz`/`.string`,
`.p2align N`.

`.p2align` is accepted only in data sections, where it pads with zeros. GNU
`as` pads code with specific multi-byte NOP sequences, so supporting `.p2align`
in code would mean copying GNU's NOP table.

String escapes in directives: `\n \t \r \" \\` and 1–3 digit octal.

### Sections

| Name | sh_type | Flags | Align |
|---|---|---|---|
| `.text` | PROGBITS | AX | 16 |
| `.rodata` | PROGBITS | A | 1, raised by `.p2align` |
| `.data` | PROGBITS | WA | 1, raised by `.p2align` |
| `.note.GNU-stack` | PROGBITS | none | 1 |

The empty `.note.GNU-stack` section marks the object as not needing an
executable stack. Code placed before any section directive goes into `.text`.
An instruction in a section without the execute flag is an error.

### Branch relaxation

GNU `as` emits a short `rel8` jump whenever its target is within range. To stay
byte-identical, lasm does the same by assembling the source in full passes:

1. **First pass:** every jump to a label is emitted in its 2-byte short form.
2. **Resolve:** a short jump is marked long for the next pass when its target is
   any of the following:
   - out of rel8 range
   - in another section
   - global
   - undefined
3. **Repeat:** the source is assembled again until a pass marks nothing new.

Jumps only ever grow, so the loop reaches a fixed point within (number of
jumps + 1) passes. Unit tests cover:

- the ±127/128 boundaries in both directions
- jcc forms
- external targets
- a cascade, where growing one jump pushes another out of range

`tests/asm/coverage.s` includes the same cases and is compared byte for byte
against GNU `as`.

### Symbols and fixup resolution

Every PC-relative field (rel8 or rel32) is the last field of its instruction,
so `P + size` is the address of the next instruction. The stored addend is
therefore `(operand constant) − size`. Each fixup is resolved as follows:

| Target | Result |
|---|---|
| Local (not `.globl`), same section | Patched in place with `S + A − P`, with no relocation. Short jumps must also fit in rel8. |
| Local, different section | Relocation against that section's `STT_SECTION` symbol, with `addend += symbol offset`. This is how `.L` labels, which never appear in `.symtab`, are referenced across sections. |
| Global and defined | Relocation against the symbol itself, which keeps it preemptible (as GAS does). |
| Undefined | Marked global (external) and relocated against the symbol. |
| An undefined `.L` label | An error. |

Relocation types:

- `R_X86_64_PC32` for `[rip + sym]` memory operands.
- `R_X86_64_PLT32` for `call` and long `jmp`/`jcc` to symbols that aren't resolved locally.

The ELF writer can also emit `R_X86_64_64`, but the assembler doesn't produce it yet.

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
- **Output:** the result is a default PIE. Both relocation types are valid in a
  PIE: PC32 points at our own `.rodata`, and PLT32 resolves the runtime functions.

External dependencies:

- a C11 compiler, to build `luma` and the runtime
- `cc`/`ld` with glibc, for the final link

`readelf`, `objdump`, `objcopy`, GNU `as` and Python 3 are used **only** by the
tests.

## Testing

| Suite | What it covers |
|---|---|
| `make unit` | lexer, parser, lowering, IR parse/print/verify, assembler encodings and relaxation, ELF layout (built by hand) |
| `make e2e` (`tests/run_e2e.sh`) | hello world pipeline and object inspection; for every `tests/pos` program: exact output, GNU `as` byte equivalence, `.lir` recompilation identity and IR round trip; compile errors with locations; runtime errors (exact stdout, stderr and exit status); hand-written `.lir` programs; lasm on its own |
| `make difftest` (`tests/difftest.py`) | random, mostly well-typed programs with functions (an acyclic call graph, early returns, globals) and multi-argument `print`, with injected type errors, overflow and division by zero. Each program is compiled natively and compared with an independent Python reference interpreter, plus the IR round-trip and GNU `as` checks |
| `make selftest` | `examples/selftest.luma`: 91 checks written in Luma itself, built on a `check()` function |
