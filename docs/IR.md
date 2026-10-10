# LIR: the Luma intermediate representation (spec v0.2)

**Status:** implemented in milestone 2. The decisions in section 13 are resolved,
and the places where the implementation refined the original draft are listed in section 14.
**Scope:** this spec covers:

- what LIR is
- how values are represented
- the text syntax
- the instruction set
- the verifier rules
- the runtime interface
- the lowering to x86-64
- the assembler extensions this requires

## 1. Where LIR sits

```
.luma ──lex/parse──► AST ──lower.c──► LIR ──x86_isel.c──► .s ──lasm──► .o ──ld──► exe
                                       │                    │            │
                                  OUT.lir (inspectable)  OUT.s        OUT.o
```

- **LIR describes Luma semantics, not x86.** It talks about tagged values, generic
  arithmetic, truthiness and runtime calls. It contains no registers, addressing
  modes or encodings.
- **lasm stays a pure assembler**, with GNU `as`-compatible syntax and byte-for-byte
  differential testing. It never learns about Luma semantics.
- **`codegen.c` is replaced** by two passes: `lower.c` (AST → LIR) and
  `x86_isel.c` (LIR → assembly text). A second target later means writing a
  new `*_isel.c`. Nothing upstream changes.

## 2. Design decisions at a glance

| Decision | Choice | Why |
|---|---|---|
| Form | Linear three-address code in basic blocks, with explicit terminators | Simple to build, print, parse and lower. Control flow stays explicit. |
| SSA? | **No.** Virtual registers are mutable slots, so there is no φ. | Lowering stays trivial (one stack slot per vreg), and front-end variables map 1:1 onto vregs. SSA construction can be added later as an optimization pass. |
| Types | **One type: `val`**, a 64-bit tagged Luma value | Every milestone-2 operation consumes and produces Luma values. Raw `i64`/`ptr` types will be added only when an operation needs them. |
| Generic ops | `add`, `lt`, … mean *Luma* `+`, `<`, … with dynamic checks | The backend decides between inline fast paths and runtime calls. The front end never does. |
| Text form | Required, and it round-trips (print → parse → print gives identical text) | Every stage stays inspectable, and LIR can be hand-written and tested on its own, the way lasm is. |
| Register allocation | None in v0.1: every vreg lives in a stack slot | Correct first. A real allocator later changes only `x86_isel.c`. |
| Short-circuit `and`/`or` | Not IR ops. The front end lowers them to branches. | Keeps the IR minimal. |

## 3. Value representation

Every Luma value is one 64-bit word. The low bits are a tag:

```
 63                                        3   2   1   0
┌──────────────────────────────────────────┬───┬───┬───┐
│ signed 63-bit integer n                  │ … │ … │ 1 │  fixnum: word = (n << 1) | 1
├──────────────────────────────────────────┼───┼───┼───┤
│ address of heap/static object (8-aligned)│ 0 │ 0 │ 0 │  pointer (never 0)
├──────────────────────────────────────────┼───┼───┼───┤
│ immediate payload                        │ x │ 1 │ 0 │  immediates
└──────────────────────────────────────────┴───┴───┴───┘
```

| Value | Word | Notes |
|---|---|---|
| fixnum *n* | `(n << 1) \| 1` | range −2⁶² … 2⁶²−1 |
| `false` | `0x02` | |
| `nil`   | `0x06` | |
| `true`  | `0x0A` | |
| object  | address | 8-aligned and non-zero, pointing at an object header |
| `0`     | — | **invalid**. Never a valid value, so it can catch uninitialized slots during debugging. |

Rationale:

- **Pointers are untagged**, so the C runtime and any future GC can dereference
  them directly.
- **Fixnum arithmetic stays cheap:** `a + b` is computed as `a + (b − 1)`, followed
  by an overflow check.
- **Truthiness is a single test:** `nil` and `false` are the only falsy values, and
  `(w & ~4) == 2` holds for exactly those two words.

### Object layout (v0.1)

```
offset 0   u64 header   bits 0–7: type id; bits 8–63: reserved (GC, flags)
offset 8   …            type-specific
```

| Type id | Type   | Payload |
|---|---|---|
| 1 | string | `u64 len; u8 bytes[len]; u8 0` (the NUL is for C interop and is not counted in `len`) |

- **String literals** are emitted as static objects in `.rodata`, so they have no
  allocation cost and are immutable.
- **Runtime-created strings** (milestone 3 and later) are allocated by the runtime.
  v0.1 has no GC: allocations are never freed, which is acceptable for the
  bootstrap.

## 4. Module structure and text syntax

```
module      := 'module' STRING NL { toplevel }
toplevel    := extern | data | global | function
extern      := 'extern' 'fn' GLOBAL '(' INT ')' NL          ; arity
data        := 'data' GLOBAL '=' 'str' STRING NL
global      := 'global' GLOBAL NL                            ; module variable (v0.2)
function    := 'fn' GLOBAL '(' [ VREG { ',' VREG } ] ')' '{' NL { block } '}' NL
block       := LABEL ':' NL { instr NL } terminator NL
instr       := VREG '=' op
             | 'call' GLOBAL '(' args ')'                    ; result discarded
             | 'store' GLOBAL ',' VREG                       ; v0.2
op          := 'const' ( INT | 'nil' | 'true' | 'false' | GLOBAL )
             | 'load' GLOBAL                                 ; v0.2
             | 'mov' VREG
             | BINOP VREG ',' VREG
             | UNOP VREG
             | 'call' GLOBAL '(' args ')'
terminator  := 'jmp' LABEL
             | 'br' VREG ',' LABEL ',' LABEL
             | 'ret' VREG
BINOP       := add | sub | mul | div | mod | eq | ne | lt | le | gt | ge
UNOP        := neg | not
GLOBAL      := '@' IDENT        VREG := '%' IDENT        LABEL := IDENT
IDENT       := [A-Za-z_][A-Za-z0-9_.]*  (digits allowed first for vregs: %0)
STRING      := C-style "…" with escapes \" \\ \n \t \xNN
comment     := ';' to end of line
```

Printing is canonical: externs and data first (in declaration order), then
functions. `ir_parse(ir_print(m))` prints identically, and the test suite
checks this for every program.

Vregs are numbered in text order: parameters first, then in order of first
appearance, with the destination before the operands. The lowering pass
renumbers to the same order (`ir_func_canonicalize`), so a lowered module and
its re-parsed `.lir` get identical stack slots and identical assembly.

The first block of a function is its entry block. Vregs are local to their
function. Globals (`@…`) are module-wide: functions, data and externs share
one namespace.

## 5. Instruction reference

All operands and results are `val`.

| Instruction | Semantics | Can fail at runtime? |
|---|---|---|
| `%d = const 42` | fixnum literal (must fit in 63 bits, checked by the verifier) | no |
| `%d = const nil` / `true` / `false` | immediate | no |
| `%d = const @s` | pointer to static data object `@s` | no |
| `%d = mov %a` | copy | no |
| `%d = add %a, %b` | Luma `+`: fixnum + fixnum, or string + string (concatenation; allocates) | type error, overflow |
| `sub`, `mul` | fixnum only | type error, overflow |
| `div`, `mod` | fixnum only; floor semantics (rounding toward −∞; the sign of `mod` follows the divisor). `mod` has no surface syntax, because Lox has no `%`; it is reachable from hand-written LIR. | type error, division by zero |
| `%d = neg %a` | fixnum negation | type error, overflow (−2⁶²) |
| `%d = not %a` | `true` if `%a` is falsy, else `false` | no |
| `%d = eq %a, %b` / `ne` | structural equality: fixnums and immediates by word; strings by content; values of different types are never equal | no |
| `lt le gt ge` | fixnum compare only. Strings are a type error, as in Lox. | type error |
| `%d = call @f(%a, …)` | call a LIR function or extern; at most 6 arguments in v0.1 | whatever `@f` does |
| `call @f(…)` | the same, result discarded | |
| `%d = load @g` | read global variable `@g` | `Undefined variable 'NAME'.` if `@g` was never stored (v0.2) |
| `store @g, %a` | write global variable `@g` | no (v0.2) |
| `jmp L` | unconditional branch | |
| `br %c, T, F` | go to `T` if `%c` is truthy, otherwise `F` | no |
| `ret %v` | return `%v` | |

Runtime failures print a message to stderr and exit with status 1. Overflow
is an error in v0.1. There is no silent wraparound, and bignums are deferred.

## 6. Verifier (`ir_verify.c`)

The verifier runs after `lower.c` and after `ir_parse.c`, and always runs before
`x86_isel.c`. It rejects a module whose:

1. global names are duplicated, or that references an undefined global;
2. blocks don't end in exactly one terminator, or that has an instruction after a terminator;
3. functions have no blocks, or that has duplicate labels within a function or a branch to an unknown label;
4. calls use the wrong number of arguments for the callee's declared arity, or use more than 6 arguments;
5. vregs may be read before being written on some path from entry. This uses a simple forward data-flow analysis, so the front end's definite-assignment rule is enforced again here;
6. `const` integers are outside the 63-bit fixnum range;
7. `const @x` references something other than a `data` global.
8. `load`/`store` reference something other than a `global` variable, or a
   call targets a `data` or `global` (v0.2).

Error format: `file.lir:LINE: error: …` for parsed IR, and
`<function>/<block>: error: …` for IR built in memory.

## 7. Runtime interface (`runtime/luma_rt.c`)

These are C functions following the System V ABI. Every argument and return value is a `val` (`uint64_t`).

| Symbol | Purpose |
|---|---|
| `main` | Owned by the runtime. Calls `luma_main()`, flushes stdout, and returns 0 (or 1 if the flush fails). Future runtime initialization (GC, argv) lives here. |
| `luma_write(v)` | Writes `v`'s display form: strings raw, fixnums in decimal, `nil`/`true`/`false` as words. Returns `nil`. (v0.2) |
| `luma_write_space()`, `luma_write_newline()` | Write `' '` / `'\n'`. Together with `luma_write` they implement the variadic builtin `print`. Return `nil`. (v0.2) |
| `luma_print(v)` | `luma_write(v)` followed by a newline. Kept for hand-written LIR. Returns `nil`. |
| `luma_undefined_variable(name)` | Called by a checked `load`. Reports `Undefined variable 'name'.` and exits with status 1. (v0.2) |
| `luma_add/sub/mul/div/mod(a,b)` | Generic arithmetic slow paths, including all error checks. |
| `luma_neg(a)`, `luma_not(a)` | |
| `luma_eq/ne/lt/le/gt/ge(a,b)` | |
| *(internal)* `luma_panic` | Flushes stdout, prints `luma: runtime error: msg` (Lox wording, e.g. `Operands must be numbers.`), and exits with status 1. Not exported. |

The program body compiles to `fn @luma_main()`, which returns `nil`.

`print "Hello, world!";` keeps its exact milestone-1 output, but now goes
through `luma_print` instead of calling `puts` directly.

## 8. Lowering LIR → x86-64 (`x86_isel.c`, v0.1 naive)

**Frame.** Slots are numbered in this order:

1. parameters, in order;
2. every other vreg, in order of first appearance.

Slot *k* lives at `[rbp − 8(k+1)]`. The frame size is F = 8·nslots rounded up to 16,
so `rsp` stays 16-byte aligned at every call.

```asm
fn:   push rbp
      mov rbp, rsp
      sub rsp, F                 ; omitted if F = 0
      mov [rbp - 8], rdi         ; spill params: rdi rsi rdx rcx r8 r9
      …
```

**Instruction templates.** `rax`, `rcx`, `rdi`, `rsi`, `rdx`, `r8` and `r9` are
scratch registers. No value lives in a register across LIR instructions.

| LIR | x86-64 |
|---|---|
| `%d = const n` | `mov rax, (n<<1)\|1` → `mov [d], rax` |
| `%d = const nil` | `mov rax, 6` → `mov [d], rax` |
| `%d = const @s` | `lea rax, [rip + .Ldata.s]` → `mov [d], rax` |
| `%d = mov %a` | `mov rax, [a]` → `mov [d], rax` |
| `%d = OP %a, %b` (v0.1) | `mov rdi, [a]` · `mov rsi, [b]` · `call luma_OP@PLT` · `mov [d], rax` |
| `%d = call @f(%a…)` | load args into `rdi…r9` · `call f` (`@PLT` for externs) · `mov [d], rax` |
| `jmp L` | `jmp .L<fn>.<L>`, omitted when `L` is the next block |
| `br %c, T, F` | `mov rax, [c]` · `and rax, -5` · `cmp rax, 2` · `je .LF` · `jmp .LT` (the last jump is omitted when `T` is next) |
| `ret %v` | `mov rax, [v]` · `mov rsp, rbp` · `pop rbp` · `ret` |

Data:

```asm
    .section .rodata
    .p2align 3
.Ldata.s0:
    .quad 1          # header: type id 1 = string
    .quad 13         # len
    .ascii "Hello, world!"
    .byte 0
```

Symbol names: LIR `@foo` becomes the symbol `foo`. Data becomes the local label
`.Ldata.foo`, and blocks become `.L<fn>.<label>`. User-defined Luma functions
will get a prefix (e.g. `lm.foo`) to avoid colliding with libc when they arrive.

**Planned first optimization (not v0.1):** inline fixnum fast paths for
`add`, `sub` and the comparisons, falling back to the runtime call:

```asm
mov rax, [a] ; mov rcx, [b]
mov rdx, rax ; and rdx, rcx ; test edx, 1 ; jz .slow   ; both fixnums?
sub rcx, 1   ; add rax, rcx ; jo .slow                 ; (2a+1)+(2b) = 2(a+b)+1
mov [d], rax ; jmp .done
.slow: … call luma_add …
```

## 9. Required lasm extensions

All encodings below were checked against GNU `as` 2.42. They will also be added
to the coverage file in the differential test suite.

| Form | Encoding |
|---|---|
| `mov r64, [rbp ± d]` | `REX.W 8B /r`: mod=01 + disp8 when −128 ≤ d ≤ 127, otherwise mod=10 + disp32; rm=101 |
| `mov [rbp ± d], r64` | `REX.W 89 /r`, same addressing |
| `cmp r64, imm` | `REX.W 83 /7 ib`; for an imm32: `REX.W 3D id` if the register is `rax`, otherwise `REX.W 81 /7 id` |
| `cmp r64, r64` | `REX.W 39 /r` |
| `and r64, imm` | `REX.W 83 /4 ib` / `25 id` (rax) / `81 /4 id` |
| `test r32, imm32` | `A9 id` (eax) / `F7 /0 id` |
| `jmp L` | `EB cb` / `E9 cd` |
| `je` / `jne` / `jo` / `jz` … `L` | `7x cb` / `0F 8x cd` |
| `.quad`, `.p2align N` | data directives. `.p2align` pads with zeros and is accepted only in data sections in v0.1. GNU `as` pads `.text` with specific multi-byte NOP sequences (`66 66 2e 0f 1f 84 …`, `0f 1f 00`), so supporting it in `.text` would mean copying GNU's NOP table to stay byte-identical. That is deferred until code alignment is actually needed. |

Memory operands are limited to `[rbp ± disp]` and `[rip + sym]`. Any base other
than `rbp`/`rip`, and any index register, is still rejected. That keeps SIB
encoding out of v0.1, because `rsp` as a base requires SIB.

**Branch relaxation (new).** GNU `as` emits short `rel8` jumps whenever the
target is in range. To keep byte-identical differential testing, lasm must do
the same:

1. Assume every jump to a label in the same section is short.
2. Compute the offsets.
3. Grow any jump whose displacement doesn't fit in `rel8` to its long form.
4. Repeat until nothing changes.

Jumps only ever grow, so the loop terminates. Jumps to other sections or to
undefined symbols are always long, with an `R_X86_64_PC32` relocation.

> **Existing gap found while writing this spec.** Milestone-1 lasm encodes
> `add/sub/xor rax, imm32` as `81 /n id`. GNU `as` uses the shorter accumulator
> forms (`05`/`2D`/`35 id`). This was confirmed by assembling
> `add rax, 0x1000` with both tools: lasm gives `48 81 c0 …` and GNU `as`
> gives `48 05 …`. Both are valid. Our differential test simply never
> exercised `rax` with an imm32. Milestone 2 should switch to the accumulator
> forms and add those cases to `tests/asm/coverage.s`.

## 10. Toolchain integration

| Item | Change |
|---|---|
| New source files | `src/ir.h`, `src/ir.c` (data structures, builder, printer), `src/ir_parse.c`, `src/ir_verify.c`, `src/lower.c`, `src/x86_isel.c`, `runtime/luma_rt.c` |
| Removed | `src/codegen.c` |
| `luma` inputs | `.luma` (full pipeline) or `.lir` (hand-written IR, starting from the verifier). This mirrors `lasm` and makes the backend testable on its own. |
| `luma` flags | `--emit-ir` (stop after writing `OUT.lir`), `--dump-ir`. `-S` and `-c` are unchanged. |
| Link line | `cc -o OUT OUT.o <libdir>/libluma_rt.a`, where `libluma_rt.a` is built by `make` |
| Tests | IR round-trip (parse → print is identity); verifier negative cases; golden `.lir` files for `tests/pos/*.luma`; hand-written `tests/ir/*.lir` programs run end to end; lasm unit and differential tests for every new encoding, including relaxation edge cases at the ±127/128 boundaries |

## 11. Worked examples

**hello.luma**

```
module "examples/hello.luma"

data @s0 = str "Hello, world!"
extern fn @luma_write(1)
extern fn @luma_write_newline(0)

fn @luma_main() {
entry:
  %0 = const @s0
  call @luma_write(%0)
  call @luma_write_newline()
  %1 = const nil
  ret %1
}
```

**A loop.** The surface syntax shown is illustrative; milestone-2 syntax is still to be decided.

```
let i = 0;
while (i < 3) { print i; i = i + 1; }
```

```
fn @luma_main() {
entry:
  %i = const 0
  jmp cond
cond:
  %t0 = const 3
  %t1 = lt %i, %t0
  br %t1, body, exit
body:
  call @luma_print(%i)
  %t2 = const 1
  %i = add %i, %t2
  jmp cond
exit:
  %r = const nil
  ret %r
}
```

This shows the non-SSA rule: `%i` is assigned in `entry` and again in `body`.
It is still one slot.

## 12. Explicitly out of scope for v0.1

These are deferred, and each is listed with the milestone where it belongs:

- SSA form, optimization passes, register allocation (milestone 4+)
- garbage collection and runtime allocation (milestone 3+)
- floats, bignums, closures, more than 6 arguments, varargs
- debug info (DWARF), and source locations in runtime errors
- a second target

## 13. Decisions (resolved)

1. **Runtime language:** C (`runtime/luma_rt.c`), built by `make` into
   `build/libluma_rt.a` and treated as part of the toolchain. A runtime written
   in Luma is deferred until bootstrapping.
2. **Integer overflow:** a runtime error (`Integer overflow.`). There is no
   wraparound, and bignums are deferred.
3. **Surface syntax:** Lox-style. It is not final before bootstrap. See
   `docs/DESIGN.md` for the accepted subset and the differences from reference Lox.

## 14. Implementation notes (changes from the draft)

- **String concatenation** with `+` landed in v0.1 rather than milestone 3,
  because it is part of Lox's `+`. String ordering comparisons are a type error,
  as in Lox.
- **Function symbols:** every LIR function is emitted as a global `FUNC`
  symbol. Calls to module functions use direct `call f`. Externs and runtime
  operations use `call f@PLT`.
- **Constants:** immediates are materialized with `mov rax, imm`. The
  assembler picks the sign-extended imm32 form or `movabs`, as GNU `as` does.
- **lasm:** every extension in section 9 is implemented, and the
  accumulator-form gap is fixed. Branch relaxation is implemented as described.
  Each new form, including relaxation boundaries and cascades, is checked byte
  for byte against GNU `as`.
- **Verifier diagnostics** use the input's line numbers: `.lir` lines for
  parsed IR, and `.luma` lines for lowered IR.

## 15. v0.2 additions (milestone 3)

- **Module globals.** `global @g` declares an 8-byte slot in `.data`,
  initialized to 0, which is never a valid value.
  - `%d = load @g` reads the slot. The backend checks it, so a slot that was
    never stored raises `Undefined variable 'NAME'.` at runtime.
  - `store @g, %v` writes the slot.
  - `NAME` in that message is the global's name after its first `.`, so
    lowering's `@var.count` reports `count`.
  - Lowering:

    ```asm
    # %d = load @g
        mov rax, [rip + .Lvar.g]
        test rax, rax
        jne .Lload_ok.N
        lea rdi, [rip + .Lvarname.g]     # NUL-terminated display name in .rodata
        call luma_undefined_variable@PLT
    .Lload_ok.N:
        mov [rbp - d], rax
    # store @g, %a
        mov rax, [rbp - a]
        mov [rip + .Lvar.g], rax
    ```
- **Naming convention used by lowering.** The dots keep these names from ever
  colliding with C or runtime symbols:

  | LIR name | Meaning |
  |---|---|
  | `@luma_main` | top-level code |
  | `@fn.NAME` | user functions |
  | `@var.NAME` | top-level variables |
  | `@sN` | string literals |
  | `@luma_*` | runtime externs |

- **`print(a, b, …)`** lowers to `call @luma_write(%a)`, then
  `call @luma_write_space()` and `call @luma_write(%b)` for each further
  argument, then `call @luma_write_newline()`. All argument values are
  computed before the first write.
- **Canonicalization** (`ir_func_canonicalize`) now also drops vregs that no
  instruction references, such as those left behind when lowering prunes code
  after a `return`. A lowered module and its re-parsed `.lir` still produce
  identical assembly, and the test suite checks this for every program.
- **Unreachable blocks** are removed by lowering, so `ret` can be followed by
  dead source code without the IR containing it.
