# lasm: the Luma assembler (specification)

**Status:** describes `src/asm.c` as of milestone 5.
**Scope:** this document is the reference for:

- what lasm accepts: the source format, sections, directives, operands and instructions
- the bytes it produces for each form
- how it resolves symbols and which relocations it emits
- its diagnostics and its limits

The ELF64 serialization of its output is described in
[DESIGN.md](DESIGN.md#elf64-writer-srcelf_writerc).

## 1. Overview

lasm turns x86-64 assembly text into a relocatable object. It is the assembler
of the Luma toolchain: `luma` writes `OUT.s` and then assembles it with lasm,
reading the file back from disk, so the object is built from exactly the
inspectable artifact. No external assembler is ever run.

```
OUT.s ──asm.c──► ObjFile (src/obj.h) ──elf_writer.c──► OUT.o (ELF64 ET_REL)
```

**Compatibility contract.** The dialect is a strict subset of GNU `as` Intel
syntax (`.intel_syntax noprefix`). For every input lasm accepts, it must
produce **the same section bytes and the same relocations as GNU `as`**
(binutils 2.42 was used for validation). Where x86-64 offers several
encodings for one instruction, lasm makes GNU's choice. The test suite checks
this byte for byte (section 14). An input lasm rejects may still be valid GNU
`as` input; lasm reports an error instead of guessing.

lasm knows nothing about Luma. Its only Luma-specific trait is the subset it
supports, which is what the code generators emit.

### 1.1 Command line

```
lasm INPUT.s -o OUTPUT.o
```

| Exit status | Meaning |
|---|---|
| 0 | `OUTPUT.o` written |
| 1 | assembly or write error (diagnostic on stderr; see section 12) |
| 2 | usage error: missing input or `-o`, an unknown option, or several inputs |

The input path is recorded in the object as its `STT_FILE` symbol.

### 1.2 Library interface

```c
#include "asm.h"          /* bool assemble(const char *path, const char *src, size_t len, ObjFile *out); */
#include "elf_writer.h"   /* serialize an ObjFile */
```

`assemble` fills an `ObjFile` (sections, symbols, relocations). It never
writes ELF itself. On error it prints the diagnostic, frees any partial
output and returns `false`. The ELF writer can be tested without the
assembler, and the assembler without ELF (`tests/unit/test_asm.c` inspects
the `ObjFile` directly).

## 2. Source format

The input is processed one line at a time.

| Rule | Detail |
|---|---|
| Encoding | bytes; a NUL byte anywhere is an error |
| Line length | at most 4095 bytes |
| Line endings | `\n`; a trailing `\r` is ignored |
| Comments | `#` to end of line, except inside a string literal |
| Labels | zero or more `name:` at the start of a line, before any instruction or directive: `a: b: ret` defines `a` and `b` |
| Statement | one instruction or one directive per line (or nothing) |
| Whitespace | spaces and tabs separate the mnemonic from operands; around commas and inside `[ ]` it is free |

### 2.1 Names

- **Symbols and labels:** the first character is a letter, `_`, `.` or `$`; the
  rest are letters, digits, `_`, `.` or `$`; at most 255 characters. Luma's
  generated names such as `fn.fib` and `.Lfn.fib.entry` are ordinary symbols.
- **`.L` names** (any name starting with `.L`) are **local labels**. They never
  appear in the object's symbol table and cannot be made global. Referring to
  one that is never defined is an error.
- **Mnemonics** are case-insensitive (`RET` is `ret`). **Register names** must be
  lowercase: `RAX` is not a register; it is taken as a symbol name.
- A symbol may be defined only once.

### 2.2 Numbers

Integers are written as in C: decimal (`42`), hexadecimal (`0x2a`), or octal
with a leading zero (`052`), with an optional `+` or `-` sign. Values up to
64 bits are accepted, including unsigned hex such as `0xffffffffffffffff`,
which means −1. Each use then checks its own range (sections 5.2 and 6).

### 2.3 String literals

They are delimited by `"` and support these escapes:

| Escape | Byte |
|---|---|
| `\n` `\t` `\r` | 0x0A, 0x09, 0x0D |
| `\"` `\\` | `"`, `\` |
| `\NNN` | one to three octal digits, at most 255 |

Any other escape is an error, and nothing may follow the closing quote.

## 3. Sections

Code and data go into the current section. Before the first section
directive, the current section is `.text`, as in GNU `as`.

| Section | Selected by | sh_type | Flags | Alignment |
|---|---|---|---|---|
| `.text` | `.text`, `.section .text` | PROGBITS | ALLOC, EXECINSTR (`AX`) | 16 |
| `.rodata` | `.section .rodata` | PROGBITS | ALLOC (`A`) | 1, raised by `.p2align` |
| `.data` | `.data`, `.section .data` | PROGBITS | ALLOC, WRITE (`WA`) | 1, raised by `.p2align` |
| `.note.GNU-stack` | `.section .note.GNU-stack` | PROGBITS | none | 1 |

- Selecting a section again continues where it left off.
- Sections appear in the object in the order they were first selected.
- The empty `.note.GNU-stack` marks the object as not needing an executable
  stack. lasm only emits it when the source asks for it, as GNU `as` does.
  Luma's code generators always do.
- Other section names are errors, and so is a flags argument to `.section`:
  each supported section has fixed flags.
- **Instructions** are allowed only in `.text`. **Data directives** are allowed
  anywhere, including `.text`.

## 4. Directives

| Directive | Effect |
|---|---|
| `.intel_syntax noprefix` | Accepted (it's the only syntax). Any other argument is an error. |
| `.text`, `.data` | Select the section. |
| `.section NAME` | Select one of the sections in section 3. |
| `.globl A, B, …`, `.global …`, `.extern …` | Make each symbol global, whether it's defined here or not. `.extern` is the same as `.globl` (an undefined symbol is external anyway). `.L` names are rejected. |
| `.type SYM, @function` / `@object` | Set the symbol type to `STT_FUNC` / `STT_OBJECT`. The default is `STT_NOTYPE`. |
| `.size SYM, .-SYM` | Size = current offset − SYM's offset. SYM must be defined in the current section. |
| `.size SYM, N` | Size = N (a non-negative integer). |
| `.byte V, …` | 1 to 16 values, each from −128 to 255. |
| `.quad V, …` | 1 to 16 64-bit values, little-endian. Each value is an integer or a symbol with an optional constant offset (`SYM`, `SYM+N`, `SYM-N`), which becomes an `R_X86_64_64` relocation (section 9). |
| `.ascii "s"` | The string's bytes. |
| `.asciz "s"`, `.string "s"` | The string's bytes plus a NUL. |
| `.p2align N` | Pad with zero bytes to a multiple of 2^N (0 ≤ N ≤ 12), and raise the section's alignment to at least 2^N. Data sections only. |

`.p2align` is rejected in `.text` because GNU `as` pads code with particular
multi-byte NOP sequences. Copying them would be needed to stay
byte-identical, and nothing needs code alignment yet.

Expressions are not supported. A value is a single integer, except for the
`.-SYM` form of `.size` and the `SYM±N` form of `.quad`.

## 5. Operands

An instruction has up to three operands, separated by commas.

### 5.1 Registers

| Width | Names |
|---|---|
| 64 | `rax rcx rdx rbx rsp rbp rsi rdi r8 … r15` |
| 32 | `eax ecx edx ebx esp ebp esi edi r8d … r15d` |
| 8 | `al cl dl bl spl bpl sil dil r8b … r15b` |

- **8-bit high halves** (`ah`, `bh`, `ch`, `dh`) and 16-bit registers are not
  supported.
- **REX prefix:** `spl`, `bpl`, `sil` and `dil` require one. When no other REX
  bit is set, lasm emits the empty prefix `40`.

### 5.2 Immediates

An immediate is an integer (section 2.2). Its allowed range depends on the
operand size:

| Operand size | Accepted values |
|---|---|
| 8 | −128 … 255 |
| 32 | −2³¹ … 2³²−1 |
| 64 | −2³¹ … 2³¹−1 (sign-extended imm32), except `mov r64, imm`, which takes any 64-bit value |

### 5.3 Memory

```
[ term ± term ± … ]
term := rip | REG64 | REG64*SCALE | SCALE*REG64 | INTEGER | SYMBOL
```

A memory operand has these parts:

| Part | Rules |
|---|---|
| **Base** | Any 64-bit register, or `rip`. `rip` must be the first term, and then only one symbol and displacements may follow. |
| **Index** | Any 64-bit register except `rsp`, with scale 1, 2, 4 or 8 (`rcx*8`). A second plain register is the index with scale 1. Registers can't be subtracted. |
| **Displacement** | The sum of all integer terms (`[rbp - 8]`, `[-8 + rbp]`, `[rax + 16 - 4]`). It must fit in a signed 32-bit value. |
| **Symbol** | Only with `rip` (`[rip + sym]`, `[rip + sym + 8]`). There is at most one symbol, and it can't be subtracted. |

Forms that are rejected:
- 32-bit address registers;
- absolute addresses with no register (`[0x1000]`);
- three registers;
- symbols without `rip`.

**Operand size.** A memory operand normally takes its size from the register
operand. Where there is none (`mov [rbp - 8], 5`, `test [rax], 1`), it needs a
size prefix: `qword ptr`, `dword ptr` or `byte ptr`. `word ptr` is not
supported.

### 5.4 Branch and call targets

The operand is a symbol name, optionally written `name@PLT`. `@PLT` is
accepted for compatibility and changes nothing, because lasm always emits
`R_X86_64_PLT32` for calls and jumps that need a relocation (section 9).
Every other `@` modifier is an error. `call` also accepts a 64-bit register
(`call r11`, an indirect call; section 6.4). Indirect jumps, and calls
through memory, are not supported.

## 6. Instruction reference

Notation:

| Symbol | Meaning |
|---|---|
| `r` | a register |
| `m` | a memory operand |
| `r/m` | either a register or a memory operand |
| `imm` | an immediate |
| `/n` | the ModRM.reg field holds the opcode extension *n* |
| `/r` | the ModRM.reg field holds a register |
| `ib`, `id`, `io` | 1-, 4- and 8-byte immediates |
| `cb`, `cd` | 1- and 4-byte branch displacements |

Unless noted otherwise, an instruction takes 32- or 64-bit operands, and 64-bit
operands get `REX.W`.

### 6.1 Data movement

| Form | Encoding | Notes |
|---|---|---|
| `mov r/m, r` | `89 /r` (`88` for 8-bit) | register to register uses this form: ModRM.reg = source, as GNU `as` does |
| `mov r, m` | `8B /r` (`8A` for 8-bit) | |
| `mov r8, imm` | `B0+r ib` | |
| `mov r32, imm` | `B8+r id` | zero-extends into the 64-bit register |
| `mov r64, imm` | `REX.W C7 /0 id` if the value fits a sign-extended imm32, else `REX.W B8+r io` (`movabs`) | `mov rax, 0xffffffff` is therefore a 10-byte `movabs` |
| `mov m, imm` | `C6 /0 ib` (byte), `C7 /0 id` (dword, qword: sign-extended) | needs a size prefix |
| `movzx r32/r64, r/m8` | `0F B6 /r` | 8-bit source only |
| `lea r64, m` | `REX.W 8D /r` | |
| `push r64`, `pop r64` | `50+r`, `58+r` (`41` prefix for r8–r15) | 64-bit registers only |
| `cqo` | `48 99` | sign-extends rax into rdx:rax |

### 6.2 Arithmetic and logic

| Form | Encoding | Notes |
|---|---|---|
| `add/or/and/sub/xor/cmp r/m, r` | `01/09/21/29/31/39 /r` | |
| `add/… r, m` | `03/0B/23/2B/33/3B /r` | |
| `add/… r/m, imm` | `83 /n ib` if −128 ≤ imm ≤ 127; else `05/0D/25/2D/35/3D id` for `rax`/`eax` (accumulator form); else `81 /n id` | /n = 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp; no 8-bit forms |
| `test r/m, r` | `85 /r` (`84` for 8-bit) | `test r, m` is accepted and encoded as `test m, r` |
| `test r/m, imm` | `A8 ib` / `A9 id` for `al`/`eax`/`rax`; else `F6 /0 ib` (8-bit) / `F7 /0 id` | there is no imm8 form for 32/64-bit |
| `imul r, r/m` | `0F AF /r` | |
| `imul r, r/m, imm` | `6B /r ib` if imm fits 8 bits, else `69 /r id` | |
| `neg r/m`, `not r/m`, `idiv r/m` | `F7 /3`, `F7 /2`, `F7 /7` | a memory operand needs a size prefix |
| `shl/shr/sar r/m, imm` | `D1 /n` when the count is 1, else `C1 /n ib` | /n = 4, 5, 7; count 0 … size−1; no shifts by `cl` |

### 6.3 Conditions

| Form | Encoding |
|---|---|
| `setCC r/m8` | `0F 90+cc /0` |
| `cmovCC r, r/m` | `0F 40+cc /r` (32/64-bit) |
| `jCC sym` | `70+cc cb` or `0F 80+cc cd` (section 8) |

Condition codes, with all the aliases GNU `as` accepts:

| cc | Names | cc | Names |
|---|---|---|---|
| 0 | `o` | 8 | `s` |
| 1 | `no` | 9 | `ns` |
| 2 | `b c nae` | A | `p pe` |
| 3 | `ae nb nc` | B | `np po` |
| 4 | `e z` | C | `l nge` |
| 5 | `ne nz` | D | `ge nl` |
| 6 | `be na` | E | `le ng` |
| 7 | `a nbe` | F | `g nle` |

### 6.4 Control flow

| Form | Encoding | Notes |
|---|---|---|
| `jmp sym` | `EB cb` or `E9 cd` | relaxed (section 8) |
| `call sym` | `E8 cd` | always rel32 |
| `call r64` | `[REX.B] FF /2` (ModRM mod=11) | indirect call through a register, e.g. `call r11` = `41 FF D3` |
| `ret` | `C3` | |
| `nop` | `90` | |

### 6.5 Not supported

- 16-bit operands;
- 8-bit ALU/`neg`/`not`/`idiv`/shift forms;
- `mul`, `div`, `inc`, `dec`, `xchg`, `movsx`/`movsxd`;
- string instructions;
- SSE/AVX;
- indirect `jmp`, and `call` through memory (`call r64` is supported);
- shifts by `cl`;
- segment and `lock`/`rep` prefixes.

Each of these is an "unknown or unsupported instruction" or "unsupported
operand combination" error. New forms are added when a code generator needs
them, together with their differential tests.

## 7. Encoding rules

Every instruction is encoded as:

```
[REX] opcode [ModRM [SIB] [disp8 | disp32]] [imm]
```

**REX** = `0100WRXB`.
- **W:** set for 64-bit operand size.
- **R, X, B:** the high bits of ModRM.reg, SIB.index and ModRM.rm (or SIB.base).
- **When emitted:** whenever any bit is set, or when a `spl`/`bpl`/`sil`/`dil`
  operand requires the empty `40`.

**ModRM and SIB** follow GNU `as`:

| Operand | Encoding |
|---|---|
| register | mod=11 |
| `[base]` | mod=00, except `rbp`/`r13` as base: mod=01 with disp8 0 (mod=00 rm=101 means RIP-relative) |
| `[base + d]`, −128 ≤ d ≤ 127 | mod=01, disp8 |
| `[base + d]`, other d | mod=10, disp32 |
| base `rsp` or `r12` | always a SIB byte (rm=100), index=100 (none) |
| `[base + index*s + d]` | SIB: scale, index, base; mod chosen from d as above |
| `[index*s + d]` (no base) | mod=00, SIB base=101, always disp32 |
| `[rip + d]`, `[rip + sym + d]` | mod=00, rm=101, disp32 (a fixup when there is a symbol) |

**Immediates.** The shortest form GNU `as` would pick is used: sign-extended
imm8 when the value fits (`83`, `6B`), the accumulator form for `rax`/`eax`
with imm32, and `movabs` only for values beyond imm32. No other size
optimizations are made: for example, `mov rax, 1` stays the 7-byte
`48 C7 C0 01 00 00 00`, as in GNU `as` without `-O`.

## 8. Branch relaxation

Every `jmp`/`jCC` to a symbol starts in its 2-byte short form (rel8). lasm
assembles the whole source in passes:

1. **Assemble:** emit each jump in its current form (short, or long if an
   earlier pass marked it).
2. **Resolve:** a short jump is marked long if its target is undefined, in
   another section, or more than −128 … +127 bytes away from the end of the
   jump.
3. **Repeat** until a pass marks nothing new.

Jumps only ever grow, so this terminates within (number of jumps + 1)
passes. The result is the same as GNU `as`, including cascades, where
growing one jump pushes another out of range.

A jump to a symbol defined in the **same section** binds to it directly even
when the symbol is global. This matches GNU `as`, and Luma's tail calls
(`jmp fn.count`) rely on it. `call` is never relaxed.

## 9. Symbols, fixups and relocations

Every PC-relative field (rel8, rel32, or a RIP-relative disp32) is the last
field of its instruction, except that a RIP-relative operand may be followed
by an immediate. A field is recorded as a **fixup** with addend
`(constant) − (bytes from the field to the end of the instruction)`, so the
value is relative to the next instruction, as the CPU computes it. For
example, `mov qword ptr [rip + x], 5` has a 4-byte immediate after its
displacement, so its addend is −8.

After all labels are known, each fixup is resolved:

| Target symbol | `[rip + sym]` | `call sym` | `jmp`/`jCC sym` |
|---|---|---|---|
| Local, same section | patched; no relocation | patched | patched (relaxed) |
| Global, same section | `R_X86_64_PC32` against the symbol | `R_X86_64_PLT32` against the symbol | patched (relaxed) |
| Local, other section | `R_X86_64_PC32` against the section symbol, addend + offset | `R_X86_64_PC32` against the section symbol (no PLT for a local target) | same as `call` |
| Global, other section | `R_X86_64_PC32` against the symbol | `R_X86_64_PLT32` against the symbol | `R_X86_64_PLT32` against the symbol |
| Undefined | becomes a global undefined symbol; relocation as for a global | same | same |
| Undefined `.L` label | error | error | error |

Notes:
- **Cross-section references** to `.L` labels go through the target section's
  `STT_SECTION` symbol, because `.L` names are never in the symbol table. This
  is how `lea rdi, [rip + .Ldata.s0]` reaches `.rodata`.
- **Globals stay preemptible:** references to globals keep their relocation,
  which leaves the symbol interposable. Jumps are the exception, as in GNU
  `as`.
- **Addends** are stored in the relocation (RELA); the field bytes stay zero.
- **Patched displacements** must fit their field: rel8 is handled by
  relaxation, and a rel32 that doesn't fit is an error.
- **Relocation order** is part of the GNU `as` contract. Relocations come in
  source order, except that those of long (relaxed) jumps come after all the
  others, still in source order. GNU `as` creates them only when it finishes
  relaxation, and lasm emits them in the same order.

  Example: `jmp ext_a; call ext_b` gives the relocation for `ext_b` first.
  `tests/asm/coverage.s` pins this ordering case and the PC32 rule above.

**Absolute data** (`.quad SYM±N`) is never patched by lasm, since the address
is only known at link time. It is always an `R_X86_64_64` relocation:

| Target symbol | Relocation |
|---|---|
| Local (including `.L` labels), any section | against the target section's `STT_SECTION` symbol, addend = N + the label's offset |
| Global, defined or undefined | against the symbol, addend = N |

This matches GNU `as`. The code generators use it for struct descriptors
(tables of name and function addresses). In a PIE the linker turns each one
into an `R_X86_64_RELATIVE` dynamic relocation.

Relocation types emitted: `R_X86_64_64` (1, `.quad SYM` only),
`R_X86_64_PC32` (2) and `R_X86_64_PLT32` (4). `R_X86_64_PLT32` is used only
for calls and jumps to global or undefined symbols.

When an object has relocations in several sections, each `.rela.X` lists its
own section's relocations in the order above. lasm creates sections in order
of first use, while GNU `as` always creates `.text`, `.data` and `.bss` first,
so the *order of the `.rela` sections* may differ; the conformance tests
compare relocations grouped by section.

## 10. Object output

The assembler fills an `ObjFile`, and `elf_writer.c` serializes it:

- **Sections:** the sections in first-use order, each with its bytes, and a
  `.rela.NAME` for each section that has relocations.
- **Symbol table**, in this order:
  1. the null symbol;
  2. `STT_FILE` (the input path);
  3. one `STT_SECTION` symbol per section;
  4. named local symbols;
  5. globals, defined ones first.

  `.L` labels are omitted.
- **Symbol attributes:** a symbol's type (`.type`) and size (`.size`) are
  recorded; the defaults are `STT_NOTYPE` and 0.
- **Not emitted:** no `.comment`, debug sections or `.bss`.

The ELF layout, header fields and validation rules are in DESIGN.md.

## 11. Example

```asm
    .intel_syntax noprefix
    .section .rodata
    .p2align 3
.Lmsg:
    .asciz "from lasm"

    .text
    .globl answer
    .type answer, @function
answer:
    push rbp
    mov rbp, rsp
    call .Lforty            # local, same section: E8 rel32 patched, no relocation
    add eax, 2              # 83 C0 02
    pop rbp
    ret
    .size answer, .-answer
.Lforty:
    mov eax, 40             # B8 28 00 00 00
    ret

    .globl answer_msg
answer_msg:
    lea rax, [rip + .Lmsg]  # 48 8D 05 + R_X86_64_PC32 .rodata - 4
    ret
    .section .note.GNU-stack
```

`lasm answer.s -o answer.o` produces an object a C program can link against
(`tests/asm/answer.s` is this test).

## 12. Diagnostics

Errors are printed to stderr as:

```
PATH:LINE: error: MESSAGE
```

Assembly stops at the **first** error, and no object is written. Errors found
after all lines are read, such as an undefined `.L` label, report the line of
the reference, or line 0 when there is no single line.

Representative messages:

| Situation | Message |
|---|---|
| unknown mnemonic | `unknown or unsupported instruction 'frobnicate'` |
| form not in section 6 | `unsupported operand combination for 'jmp'` |
| operand count | `'ret' expects 0 operands, got 1` |
| sizes disagree | `operand size mismatch for 'mov'` |
| memory with no size | `operand size of 'mov' is ambiguous (use qword ptr / dword ptr / byte ptr)` |
| immediate range | `immediate 4294967296 out of range for a 32-bit operand` |
| bad memory operand | `rsp cannot be an index register`, `symbols are only allowed in [rip + symbol] operands`, `memory operand has more than two registers` |
| shift count | `shift count 64 out of range` |
| duplicate label | `symbol 'x' is already defined` |
| missing local label | `undefined local label '.Lnowhere'` |
| section | `unsupported section '.weird' (supported: .text .rodata .data .note.GNU-stack)` |
| code outside `.text` | `instruction 'ret' in non-executable section '.rodata'` |
| directive | `unknown or unsupported directive '.bogus'`, `usage: .p2align N (0 <= N <= 12)` |
| symbol modifier | `unsupported symbol modifier '@GOT' (only @PLT)` |
| input | `NUL byte in assembly source`, `line too long (max 4095 bytes)` |

`tests/unit/test_asm.c` checks the error cases.

## 13. Limits

| Item | Limit |
|---|---|
| line length | 4095 bytes |
| operands per instruction | 3 |
| values per `.byte` / `.quad` line | 16 |
| symbol name | 255 characters |
| displacement | signed 32-bit |
| `.p2align` | N ≤ 12, data sections only |

## 14. Conformance testing

| Test | What it checks |
|---|---|
| `tests/unit/test_asm.c` | Encodings of individual instructions against expected bytes. It also covers relaxation: the ±127/128 boundaries both ways, jcc, cascades, external targets and same-section globals. Plus data directives, relocations and error messages. |
| `tests/asm/coverage.s` | Every directive and instruction family (including `call r64` and a `.quad` table of local, global and undefined symbols with offsets), assembled by lasm and by GNU `as`. Section bytes and relocations must be identical. |
| `tests/asm/gen_forms.py` | About 5,700 generated forms, compared the same way:<br>• every base register with every displacement class;<br>• base + index × scale, index-only;<br>• RIP-relative with trailing immediates;<br>• byte registers;<br>• the setcc, movzx, test, imul, shift, neg, idiv and memory-immediate forms. |
| `tests/asm/answer.s` | A lasm object linked into a C program and run. |
| every compiled program | `tests/run_e2e.sh` and `tests/difftest.py` assemble each generated `.s` with GNU `as` too and require identical bytes (validation only). |

To add an instruction form:
1. implement it in `assemble_insn`;
2. add unit expectations;
3. extend `gen_forms.py` or `coverage.s`;
4. confirm the GNU `as` comparison still passes.
