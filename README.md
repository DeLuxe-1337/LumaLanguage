# Luma

Luma is a dynamically typed, natively compiled programming language. The goal is for it to compile itself.
This repository holds the **bootstrap compiler**, which is written in C. It
includes its own intermediate representation (LIR), an SSA optimizer, a
register-allocating x86-64 backend, an assembler and an ELF64 object writer.

The syntax started from Lox and has grown its own features:

- `print` is a builtin function
- types are optional and gradual
- structs with `impl` blocks (methods and associated functions, Rust-style),
  generic structs, and hooks (`drop`, `deref`, `show`, `enter`/`leave`) that
  let a type define how it lives, forwards and prints
- manual memory management (`free`), with the compiler freeing what it can
  prove dead; `Rc<T>` and arenas are written in Luma itself, in the prelude
- C functions are called directly through `extern fun`, and C structs are
  declared with `extern struct` (passed by pointer or by value)
- a standard library written in Luma: `Math`, `Str`, `List<T>`, `File`,
  `Sys`, `Date`, `Random`, `C`

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
| `OUT.mem.lir` | The memory pass (`mem.c`, with `--mm=manual`): which allocations are reference counted, and every `retain` and `release` the compiler inserted. |
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
--mm=MODE       memory management: manual (default) or none (see below)
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
| Structs | `struct P { x: int, y }` declares fields (typed or untyped). `P { x: 1, y }` builds one (every field, any order, `y` is shorthand for `y: y`). Instances are references: assignment shares, `==` is identity, and they are always truthy. `print` shows `P { x: 1, y: "s" }`. `shared struct` makes every instance reference counted. |
| Generics | `struct Box<T> { value: T }` and `impl Box<T> { … }`: one struct per list of type arguments (`Box<int>`, `Pair<str, Point?>`), inferred in expressions, written in annotations (below). |
| Hooks | Methods with special meaning: `drop(self)` runs when an instance dies, `deref(self)` answers fields and methods the struct lacks, `show(self): str` is how `print` shows it, `enter(self)`/`leave(self)` are called by `with`. |
| Prelude | `Rc<T>` and `Arena` are Luma code in [std/prelude.luma](std/prelude.luma), built into `luma` and compiled only when a program uses them. |
| `impl` | `impl P { fun m(self, a) { … } fun new(…) { … } }` adds methods (first parameter `self`, untyped) and associated functions (no `self`). Call them as `p.m(1)`, `P::new(…)` or `P::m(p, 1)`. Several `impl` blocks per struct are allowed. When the object's static type is exactly `P`, field access and method calls are compiled statically; otherwise they are looked up by name at runtime, with the same error messages. |
| Functions | `fun name(a, b) { … return v; }` at top level, called as `name(x, y)`. Recursion and mutual recursion work, and functions can be called before their definition. Up to 6 parameters. Arity is checked at compile time. A missing `return` returns `nil`. `fun f(move p)`: the call takes over the caller's variable (using it afterwards is a compile error). |
| Builtins | `print(a, b, …)` writes its arguments separated by spaces, then a newline, and returns `nil`. All arguments are evaluated before anything is written. `free(x)`, `dispose(x)` and `refcount(x)`: see Memory. |
| Types | Optional annotations: `var n: int`, `fun f(a: str, b): bool?`. The types are `int str bool nil any`, struct names, and `T?` (T or nil). Mismatches the compiler can prove are compile errors; values whose type is only known at runtime are checked by a guard at the annotated boundary. Unannotated code stays fully dynamic. |
| C FFI | `extern fun name(p: ctype, …): ctype;` with `i8`…`i64`, `u8`…`u64`, `bool`, `cstr`, `cstr?`, `ptr` and `void`, plus C structs (below). Values are converted and range-checked at the call. `extern "sym" fun name(…)` calls the C symbol `sym`. Link libraries with `-l NAME -L DIR`, or pass `.o`/`.a`/`.so` files. |
| Standard library | `Math`, `Str`, `List<T>`, `File`, `Sys`, `Date`, `Random`, `C`, written in Luma in [std/](std); see below. |
| Statements | `var`, expression statements, `{ }` blocks, `if`/`else`, `while`, `for`, `return`, `with x { … }`, `arena { … }` |
| Operators | `= or and == != < <= > >= + - * / ! -` (unary), and `+` on two strings concatenates them |
| Variables | Top-level variables are globals that functions can read and write. Variables in blocks and functions are local, with lexical scoping checked at compile time. |
| Semantics | Lox truthiness (only `nil` and `false` are falsy). `and`/`or` return an operand. Integer overflow, type errors, reading a global before it is assigned, and stack overflow are runtime errors (exit status 1). `/` is floor division. |
| Memory | Manual by default (below): `free(x)` for struct instances, `Rc::new(x)` for shared ownership, `arena { … }` for bulk lifetimes; strings are automatic. `--mm=none` never frees, and `free` then does nothing, so a program compiles in both modes. Using a variable after `free`, or after a `move` parameter took it, on every path, is a compile error. |
| Not yet | first-class functions and closures, generic functions, traits/interfaces, floats (including C `f32`/`f64`), C callbacks, unions, arrays and nested structs in C structs, `break`, collecting reference cycles |

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

// Rc: shared ownership. The last handle frees the box and the Point in it.
var shared: Rc<Point>? = Rc::new(Point { x: 1, y: 2 });
var other = shared;                   // a second handle to the same Point
other.x = 10;                         // fields and methods go through the box
print(shared.x, shared.len2(), Rc::count(shared));   // 2 handles + the call's
other = nil;
shared = nil;                         // count reaches 0: both are freed

// Arenas: everything allocated inside is freed together at the end.
fun chain(n: int): Node? {
  var head: Node? = nil;
  for (var i = 0; i < n; i = i + 1) head = Node { value: i, label: "n", next: head };
  return head;
}
arena {
  var c = chain(1000);                // 1000 Nodes, no free() needed
  print(c.value);
}
var a = arena { };                    // a named arena, kept by its handle
arena a { list = chain(10); }         // re-enter it later
print(list.value);
list = nil;
a = nil;                              // the last handle frees the 10 Nodes
```

Output, with `LUMA_MEMCHECK=1`:

```
hello, luma! 25
10 104 3
999
9
luma: memcheck: --mm=manual, 1023 allocated, 1023 freed, 0 leaked, 0 reachable from globals
```

Memory is manual: what you allocate you free, and the compiler takes care of
the rest.

| | Freed |
|---|---|
| Strings and temporaries | automatically, at their last use |
| Struct instances | by `free(x)`; instances that provably never leave their function are freed for you |
| `Rc::new(x)` | when the last handle goes away; the box owns `x` (`x` is a `move` parameter: using it afterwards is a compile error) |
| Inside `arena { … }` | every struct allocated in the block, including in functions it calls, at the end of the block (or when the last handle of a named arena goes away). `free()` on them does nothing |

- `Rc<T>` handles are counted references: copying one shares the box, and
  field access and method calls go through it (`r.x`, `r.len2()`, by its
  `deref`). Rc's own methods win over the value's: `r.get()` is
  `Rc::get(r)`, the value. `Rc::count(r)` is the number of handles (a
  debugging aid: the call's own, and temporaries the optimizer keeps alive,
  count too). Cycles of Rc boxes leak. `Rc::new` of an untyped value makes
  an `Rc<any>`.
- `arena { … }` is `with Arena::new() { … }`. `var a = arena { … };` keeps
  the arena; `arena a { … }` allocates into it again. Strings and shared
  structs (Rc boxes) are never placed in an arena. A reference that
  outlives its arena is a use after free (`LUMA_MEMCHECK=1` reports it).
- `dispose(x)` frees `x` if it is an instance managed by hand and does
  nothing for anything else (for generic code that owns a `T`);
  `refcount(x)` reads a counted object's count.
- `--mm=none` frees nothing, which is useful to compare against.

The compiler inserts the retain/release operations itself, after
optimization, using liveness and ownership:
- Parameters are borrowed, so passing an object costs nothing.
- A last use moves a reference instead of copying it.
- Reading a field or global that nothing can free before the value is used
  borrows it, with no retain/release.
- Values whose type can't be a heap object get no operations at all: typed
  integer code compiles exactly as before.

`OUT.mem.lir` shows every decision. Run any program with `LUMA_MEMCHECK=1` to
see what it leaves behind:

```
$ luma tests/mem/graphs.luma -o graphs && LUMA_MEMCHECK=1 ./graphs
124750
9 7
luma: memcheck: --mm=manual, 2032 allocated, 2018 freed, 6 leaked, 8 reachable from globals
luma: memcheck: leaked: 3 Pair, 3 Rc<Pair>
luma: memcheck: reachable from globals: 1 Node, 3 Pair, 1 Rc<Node>, 3 Rc<Pair>
```

The 6 leaked objects are two Rc cycles (a box that refers to itself, and a ring of two) with their values; see `tests/mem/graphs.luma`.

The design, the analyses and the runtime are described in
[docs/DESIGN.md](docs/DESIGN.md) ("Memory").

## Generics, hooks and the prelude

`Rc<T>` and `Arena` are not built into the compiler: they are ordinary Luma
in [std/prelude.luma](std/prelude.luma), using features any program can use.
This program defines its own owner type the same way:

```js
struct Node { label: str, next: Node? }
impl Node { fun shout(self): str { return self.label + "!"; } }

// A unique owner: freeing it frees what it holds. Like Rc<T>, minus sharing.
struct Owned<T> { value: T }
impl Owned<T> {
  fun new(move v: T): Owned<T> { return Owned { value: v }; }
  fun deref(self): T { return self.value; }       // o.label is o.value.label
  fun drop(self) { dispose(self.value); }         // free(o) frees the Node too
  fun show(self): str { return "Owned(" + self.value.label + ")"; }
}

var o = Owned::new(Node { label: "root", next: nil });
print(o, o.label, o.shout());
free(o);

// A scope: with calls enter() before the block and leave() after it.
var depth = 0;
struct Indent { by: int }
impl Indent {
  fun enter(self) { depth = depth + self.by; }
  fun leave(self) { depth = depth - self.by; }
}
with Indent { by: 2 } { print("depth", depth); }
print("depth", depth);
```

Output:

```
Owned(root) root root!
depth 2
depth 0
```

| Feature | Meaning |
|---|---|
| `struct Box<T> { … }`, `impl Box<T> { … }` | A struct per list of type arguments, inferred from field values and arguments (`Owned::new(node)` is an `Owned<Node>`) |
| `shared struct S` | Every instance is reference counted; `free()` of one is an error |
| `fun drop(self)` | Runs once when an instance dies: its count reaches 0, `free`/`dispose`, or its arena ends. For counted objects that is after their last use, so it can run earlier at `-O2` than at `-O0` |
| `fun deref(self)` | A field or method the struct lacks is looked up on what `deref` returns (statically when types are known, else at run time); `return self.f;` compiles to a plain field read |
| `fun show(self): str` | What `print` shows for an instance, also inside other values |
| `fun enter(self)`, `fun leave(self)` | Called around `with x { … }`, also when a `return` leaves the block |
| `fun f(move p)` | The call takes the caller's variable |

Temporaries live to the end of their full expression, so
`make().peek().n` can't read a value whose owner's `drop` already ran. Names
starting with `__` are reserved for the prelude's runtime intrinsics.
`LUMA_PRELUDE=path` replaces the prelude (for working on it).

## The standard library

The library is Luma code in [std/](std), built into `luma` and compiled only
as far as a program uses it. Its functions are associated functions of a
module struct (`Math::gcd(a, b)`, `Str::split(s, sep)`), and its types have
methods (`list.push(x)`, `file.read_line()`).

```js
var path = "notes.txt";
File::write_all(path, "the quick brown fox\njumps over the lazy dog\nThe End\n");

var words = Str::words(Str::lower(File::read(path)));
words.sort();
print(words.len(), "words:", words.first(), "...", words.last());

var f = File::open(path, "r");                 // closed by drop, or here:
var n = 1;
var line = f.read_line();
while (line != nil) {
  print(Str::pad_left(Str::from_int(n), 3, " "), line);
  line = f.read_line();
  n = n + 1;
}
f.close();
File::remove(path);

print(Math::gcd(84, 36), Math::pow(3, 4), Str::split("a,b,c", ","), Sys::date(0));
```

```
11 words: brown ... the
  1 the quick brown fox
  2 jumps over the lazy dog
  3 The End
12 81 ["a", "b", "c"] 1970-01-01 00:00:00 GMT
```

| Module | What it has |
|---|---|
| `Math` | `abs min max clamp sign pow gcd lcm isqrt mod digits is_even is_odd max_int min_int` (integers: there are no floats yet) |
| `Str` | `len byte char slice find find_from contains starts_with ends_with count repeat upper lower trim trim_start trim_end split lines words join replace from_int to_int compare pad_left pad_right of is_space is_digit is_alpha`. Strings are byte strings; indexes count bytes |
| `List<T>` | `List::new()`; `len is_empty get set push pop first last insert remove index_of contains clear reverse sort slice copy`; prints as `[1, 2, 3]` |
| `File` | `File::open(path, mode)` (nil if it can't), `read_line read_all write write_line flush close`; `File::read write_all append lines exists remove` |
| `Sys` | `args env exit run fail eprint type_of time clock_ms clock_ns date local_date` |
| `Date` | `year month day hour minute second weekday zone`, `weekday_name()`, prints as `2023-11-14 22:13:20 GMT` |
| `Random` | `Random::new(seed)`, `next range bool shuffle` (reproducible; not for cryptography) |
| `C` | `alloc free string cstring write`: raw C memory for the FFI |

- The library's types (`List`, `File`, `Date`, `Random`) are shared
  structs: like strings they are freed when their last reference goes away.
  A `List` holds references to its items; it does not free them. A `File`
  closes itself in its `drop` (under `--mm=none`, which frees nothing, call
  `close()`).
- Misuse is a runtime error with a message, like the builtins' (`Index 5 is
  out of range (length 3).`, `File notes.txt is closed.`).
- A program may define its own `List`, `File`, and so on: its definition
  wins, unless library code the program uses needs the library's (then it
  is a compile error). `Rc` and `Arena` (std/prelude.luma) are reserved.
- The library is built from the same features programs use. It calls C
  through `extern fun` and `extern struct` (`Sys` reads the clock with
  `clock_gettime` into a `timespec`, dates with `gmtime_r` into a `tm`), and
  calls a few runtime primitives that only the prelude may call (`__name`:
  arrays, string slicing, `argv`).
- `LUMA_STD=dir` reads the library from `dir/NAME.luma` instead (for working
  on it).

## C structs

```js
extern struct timespec { tv_sec: i64, tv_nsec: i64 }
extern struct div_t { quot: i32, rem: i32 }
extern fun clock_gettime(clock: i32, ts: *timespec): i32;   // by pointer
extern fun div(n: i32, d: i32): div_t;                       // by value

var ts = timespec { };
clock_gettime(0, ts);                 // C writes into ts's memory
print(ts.tv_sec > 1700000000, div(17, 5), timespec::size());
```

prints `true div_t { quot: 3, rem: 2 } 16`.

- `extern struct S { field: ctype, … }` declares a C struct with the C
  layout (System V x86-64: each field aligned to its size, the size rounded
  to the largest). Field types are `i8`…`u64`, `bool`, `ptr`, `cstr` and
  `cstr?` (strings are read-only: their memory belongs to C).
- `S { … }` makes zeroed C memory (fields not given are 0), owned by the
  Luma value and freed with it. Field reads and writes convert and
  range-check like FFI arguments (`300 does not fit in i8`). Values print as
  `S { a: 1, b: 2 }` and work untyped too.
- In extern signatures, `*S` passes the struct's address (C may write
  through it) and `*S?` also accepts `nil` (NULL). A `*S` result is a *view*
  of memory C owns (not freed by Luma; NULL is an error, or `nil` for
  `*S?`). `S` passes or returns a struct by value, up to 16 bytes (in one or
  two registers). Larger structs must go by pointer (a compile error says
  so).
- `S::size()` is its size in bytes; `S::addr(s)` its address (valid while
  `s` is alive); `S::at(address)` a view of the struct at an address.

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

These are in the default `--mm=manual`. At `-O2` the two memory modes are
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
| `tests/ffi/` | C FFI: `ffi_helper.c` plus Luma programs that round-trip every C type and C structs (by value and by pointer), and runtime FFI errors. |
| `tests/asm/` | lasm on its own: a C program linked against an object built by `lasm`, and a coverage file compared byte for byte against GNU `as`. |
| `tests/difftest.py` | Random programs (including structs, methods, dynamic field access and `free`), each under one memory mode with memcheck on, compiled natively at every optimization level and compared with an independent Python interpreter. |
| `bench/` | Benchmark programs and `run.py`. |

## Layout

```
src/        lexer, parser + ast, lower (AST→LIR), ir/ir_parse/ir_verify/ir_types,
            cfg + opt* (optimizer), mem (memory analyses), x86_gen (optimizing LIR→asm), x86_isel (naive LIR→asm),
            asm, obj (object model), elf_writer, value.h (shared value tags), main (luma), lasm
runtime/    luma_rt.c: the C runtime linked into every program
std/        the prelude (Rc<T>, Arena) and the standard library, written in Luma (built into luma)
examples/   hello.luma, fizzbuzz.luma, ffi.luma, selftest.luma
tests/      unit/, pos/, neg/, rt/, mem/, ir/, ffi/, asm/, run_e2e.sh, difftest.py
bench/      benchmark programs and run.py
docs/       DESIGN.md: pipeline, language, runtime, optimizer, backends, assembler, ELF, linking
            IR.md:     LIR specification
            LASM.md:   assembler specification (syntax, encodings, relocations)
```
