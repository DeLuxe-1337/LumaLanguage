# Luma bootstrap compiler: design notes (milestone 5)

This document describes how the Luma toolchain is built. Its sections cover:

- the pipeline and its modules
- the language subset accepted today
- optional types and the C FFI
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
| Types | none | optional gradual annotations (see below); doomed operations such as `1 + "a"` are compile errors |
| C interop | none | `extern fun` with C types (see below) |
| Undefined names | runtime errors | compile errors; a global read before its declaration executes is a runtime error |
| Strings | multi-line, no escapes | single line; `\"` `\\` `\n` `\t` |
| `class`, `this`, `super`, float literals | yes | rejected ("not supported yet") |

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
  `jmp`/`jcc` (relaxed in passes, as GNU `as` does), `ret` and `nop`.
- **Relocations:** `R_X86_64_PC32` for RIP-relative operands and local
  targets, and `R_X86_64_PLT32` for calls and jumps to global or undefined
  symbols. `.L` labels never reach the symbol table.

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
| `make e2e` (`tests/run_e2e.sh`) | hello world pipeline and object inspection (at `-O2` and `-O0`); for every `tests/pos` program: exact output at `-O0`, `-O1` and `-O2`, GNU `as` byte equivalence, `.lir` recompilation identity and IR round trip; properties of the optimized code (no runtime calls in typed `fib`, frameless leaf loops, shifts for division by 4, tail calls as jumps); compile errors with locations; runtime errors and FFI programs at every level (exact stdout, stderr and exit status); hand-written `.lir` programs; lasm on its own |
| `make difftest` (`tests/difftest.py`) | random programs with functions, globals, multi-argument `print` and random **type annotations**. Wrong-typed values are hidden behind an untyped `dyn()` so they reach runtime errors and the **runtime guards**, which the reference interpreter models with exact messages. Each program is compiled natively at `-O0`, `-O1` and `-O2` (`DIFFTEST_LEVELS`) and compared with an independent Python reference interpreter, plus the IR round-trip and GNU `as` checks |
| `make selftest` | `examples/selftest.luma`: 91 checks written in Luma itself, built on a `check()` function. It exits through `extern fun exit` |
| e2e FFI section | `tests/ffi`: the C helper library `ffi_helper.c` (built with the system cc, test-only), round trips for every C type and boundary value, 9 runtime-error cases, and the link modes |
| `make bench` (`bench/run.py`) | wall time of six programs (recursion, loops, division, strings) at `-O0` and `-O2`, with their output |

The optimizer and backend were also checked with ASan/UBSan builds of the
compiler (`make BUILD=build-asan CFLAGS="-fsanitize=address,undefined …"`,
then the suites with `LUMA=build-asan/luma`), and by planting bugs in the
backend (a dropped `jo`, a wrong swapped condition, no register saving
around slow paths, a missing floor fix-up, ignoring calls in allocation):
`make difftest` catches each of them.
