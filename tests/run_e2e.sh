#!/bin/sh
# End-to-end tests for the Luma toolchain. Run from the repository root
# after `make` (or via `make e2e`). Exits non-zero if any check fails.
#
# GNU as / readelf / objdump are used here ONLY to inspect and cross-check
# what Luma produced. They never produce an object that is linked into a
# Luma program.

set -u
LUMA=${LUMA:-build/luma}
LASM=${LASM:-build/lasm}
RTLIB=${RTLIB:-build/libluma_rt.a}
OUT=build/test
rm -rf "$OUT"
mkdir -p "$OUT"
HAVE_AS=0
command -v as >/dev/null 2>&1 && HAVE_AS=1

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; }
check() { # check LABEL COMMAND...   (sh has no locals: never reuse loop variable names here)
    _label=$1; shift
    if "$@" >"$OUT/last.log" 2>&1; then ok "$_label"; else bad "$_label"; sed 's/^/      /' "$OUT/last.log"; fi
}

# Relocations as "section offset type symbol+addend", for comparing two objects.
# Grouped by relocation section (in source order within each), since GNU as
# creates .text and .data before other sections while lasm creates sections
# in the order the source first uses them.
norm_relocs() {
    readelf -r -W "$1" | awk '/^Relocation section/{sec=$3} /^[0-9a-f]+ /{print sec, $1, $3, $5, $6, $7}' | sort -s -k1,1
}

# same_as_gas NAME FILE.s FILE.o : assemble FILE.s with GNU as and compare
# section bytes and relocations with Luma's FILE.o.
same_as_gas() {
    [ "$HAVE_AS" -eq 1 ] || return 0
    as --64 -o "$OUT/gas.o" "$2" || return 1
    for sec in .text .rodata .data; do
        objcopy -O binary -j "$sec" "$3" "$OUT/l.bin" 2>/dev/null || : >"$OUT/l.bin"
        objcopy -O binary -j "$sec" "$OUT/gas.o" "$OUT/g.bin" 2>/dev/null || : >"$OUT/g.bin"
        cmp -s "$OUT/l.bin" "$OUT/g.bin" || { echo "$sec differs from GNU as"; return 1; }
    done
    norm_relocs "$3" >"$OUT/rl.txt"
    norm_relocs "$OUT/gas.o" >"$OUT/rg.txt"
    diff "$OUT/rl.txt" "$OUT/rg.txt"
}

# ---------------------------------------------------------------- hello world
printf '== hello world pipeline ==\n'
rm -f build/hello build/hello.lir build/hello.s build/hello.o
check "luma compiles examples/hello.luma" "$LUMA" examples/hello.luma -o build/hello
check "IR file build/hello.lir exists" test -s build/hello.lir
check "IR generated from source (data + print call)" \
    sh -c 'grep -q "data @s0 = str \"Hello, world!\"" build/hello.lir && grep -q "call @luma_write(%0)" build/hello.lir'
check "assembly file build/hello.s exists" test -s build/hello.s
check "assembly lowered from IR (string object + runtime call)" \
    sh -c 'grep -q "\.ascii \"Hello, world!\"" build/hello.s && grep -q "call luma_write@PLT" build/hello.s'
check "object file build/hello.o exists" test -s build/hello.o

./build/hello >"$OUT/hello.out" 2>"$OUT/hello.err"
status=$?
if [ "$status" -eq 0 ]; then ok "build/hello exits with status 0"; else bad "build/hello exit status $status"; fi
printf 'Hello, world!\n' >"$OUT/hello.expected"
if cmp -s "$OUT/hello.out" "$OUT/hello.expected"; then ok "stdout is exactly 'Hello, world!\\n'"
else bad "stdout mismatch"; od -c "$OUT/hello.out" | sed 's/^/      /'; fi
if [ ! -s "$OUT/hello.err" ]; then ok "stderr is empty"; else bad "stderr not empty"; fi

# ------------------------------------------------------------ object checks
printf '== object file inspection ==\n'
check "readelf -h -S -s -r accepts hello.o" readelf -h -S -s -r build/hello.o
readelf -h build/hello.o >"$OUT/eh.txt" 2>&1
check "ELF64 class"            grep -q 'Class:.*ELF64' "$OUT/eh.txt"
check "type REL (relocatable)" grep -q 'Type:.*REL (Relocatable file)' "$OUT/eh.txt"
check "machine x86-64"         grep -q 'Machine:.*X86-64' "$OUT/eh.txt"
check "readelf reports no warnings" sh -c '! readelf -a -W build/hello.o 2>&1 | grep -qi "warning\|error"'
readelf -r -W build/hello.o >"$OUT/rel.txt"
check "R_X86_64_PC32 against .rodata - 4"      grep -Eq 'R_X86_64_PC32 +0+ \.rodata - 4' "$OUT/rel.txt"
check "R_X86_64_PLT32 against luma_write - 4"  grep -Eq 'R_X86_64_PLT32 +0+ luma_write - 4' "$OUT/rel.txt"
readelf -s -W build/hello.o >"$OUT/sym.txt"
check "luma_main is GLOBAL FUNC in .text"  grep -Eq 'FUNC +GLOBAL +DEFAULT +[0-9]+ luma_main$' "$OUT/sym.txt"
check "luma_write is GLOBAL UND"           grep -Eq 'NOTYPE +GLOBAL +DEFAULT +UND luma_write$' "$OUT/sym.txt"
check "FILE symbol names the .luma source" grep -q 'FILE.*examples/hello.luma' "$OUT/sym.txt"
check "no .comment section (not produced by GNU as)" sh -c '! readelf -S build/hello.o | grep -q "\.comment"'
check "objdump -d -r accepts hello.o" objdump -d -r build/hello.o
objdump -d -r -M intel build/hello.o >"$OUT/dis.txt"
check "objdump decodes the generated instructions (-O2)" sh -c '
    for i in "push +rbp" "mov +rbp,rsp" "lea +rdi,\[rip\+0x0\]" "call" "mov +eax,0x6" "pop +rbp" "ret"; do
        grep -Eq "$i" '"$OUT"'/dis.txt || { echo "missing: $i"; exit 1; }; done'
"$LUMA" -O0 examples/hello.luma -c -o "$OUT/hello0" >/dev/null 2>&1
objdump -d -r -M intel "$OUT/hello0.o" >"$OUT/dis0.txt" 2>&1
check "objdump decodes the generated instructions (-O0, stack slots)" sh -c '
    for i in "push +rbp" "mov +rbp,rsp" "sub +rsp,0x10" "lea +rax,\[rip\+0x0\]" "mov +QWORD PTR \[rbp-0x8\],rax" \
             "mov +rdi,QWORD PTR \[rbp-0x8\]" "call" "mov +rsp,rbp" "pop +rbp" "ret"; do
        grep -Eq "$i" '"$OUT"'/dis0.txt || { echo "missing: $i"; exit 1; }; done'
check "objdump shows relocations inline" sh -c 'grep -q "R_X86_64_PC32" '"$OUT"'/dis.txt && grep -q "R_X86_64_PLT32.*luma_write" '"$OUT"'/dis.txt'
check "final executable is native x86-64 ELF" sh -c 'readelf -h build/hello | grep -q "Machine:.*X86-64"'

# The link step: cc receives only an object and an archive, so it must run
# the linker (collect2/ld) and never an assembler.
cc -v -o "$OUT/relink" build/hello.o "$RTLIB" >"$OUT/link.log" 2>&1
check "system linker runs (collect2/ld)" grep -Eq 'collect2|/ld ' "$OUT/link.log"
check "link step invokes no assembler" sh -c '! grep -Eq "^ *([^ ]*/)?as( |$)" '"$OUT"'/link.log'

# --------------------------------------------------------- stage control
printf '== stage flags ==\n'
rm -f "$OUT"/s1*
check "--emit-ir stops after IR" sh -c "$LUMA examples/hello.luma --emit-ir -o $OUT/s1 && test -s $OUT/s1.lir && test ! -e $OUT/s1.s"
check "-S stops after assembly"  sh -c "$LUMA examples/hello.luma -S -o $OUT/s1 && test -s $OUT/s1.s && test ! -e $OUT/s1.o"
check "-c stops after object"    sh -c "$LUMA examples/hello.luma -c -o $OUT/s1 && test -s $OUT/s1.o && test ! -e $OUT/s1"
check "--dump-tokens lists IDENTIFIER LEFT_PAREN STRING RIGHT_PAREN SEMICOLON EOF" sh -c \
    "$LUMA examples/hello.luma -S -o $OUT/s2 --dump-tokens | awk '{print \$2}' | tr '\n' ' ' | grep -q 'IDENTIFIER LEFT_PAREN STRING RIGHT_PAREN SEMICOLON EOF'"
check "--dump-ast shows the print call" sh -c \
    "$LUMA examples/hello.luma -S -o $OUT/s2 --dump-ast | grep -q '(expr (call print \"Hello, world!\"))'"
check "--dump-ir prints the module" sh -c "$LUMA examples/hello.luma -S -o $OUT/s2 --dump-ir | grep -q '^fn @luma_main() {'"
check "lasm on build/hello.s gives the same .text as luma" sh -c \
    "$LASM build/hello.s -o $OUT/hello_lasm.o && objcopy -O binary -j .text build/hello.o $OUT/a.bin && objcopy -O binary -j .text $OUT/hello_lasm.o $OUT/b.bin && cmp $OUT/a.bin $OUT/b.bin"
check "missing runtime library is reported" sh -c \
    "! LUMA_RUNTIME=$OUT/nope.a $LUMA examples/hello.luma -o $OUT/s3 2>$OUT/e && grep -q 'runtime library' $OUT/e"

# ---------------------------------------------------------- positive programs
printf '== positive programs (output, GNU as equivalence, IR round trip) ==\n'
for src in tests/pos/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/pos_$name"
    if ! "$LUMA" "$src" -o "$exe" >"$OUT/last.log" 2>&1; then bad "pos/$name compiles"; sed 's/^/      /' "$OUT/last.log"; continue; fi
    "$exe" >"$exe.actual" 2>"$exe.stderr"
    st=$?
    if [ $st -eq 0 ] && cmp -s "$exe.actual" "tests/pos/$name.out" && [ ! -s "$exe.stderr" ]; then ok "pos/$name output + exit 0"
    else bad "pos/$name (exit $st)"; diff "tests/pos/$name.out" "$exe.actual" | head -20 | sed 's/^/      /'; cat "$exe.stderr"; fi
    check "pos/$name: object identical to GNU as" same_as_gas "$name" "$exe.s" "$exe.o"
    # the emitted IR, compiled on its own, gives identical assembly
    cp "$exe.lir" "$OUT/rt_$name.lir"
    check "pos/$name: .lir recompiles to identical .s" sh -c \
        "$LUMA $OUT/rt_$name.lir -S -o $OUT/rt_$name && sed 1d $exe.s > $OUT/x1 && sed 1d $OUT/rt_$name.s > $OUT/x2 && cmp $OUT/x1 $OUT/x2"
    check "pos/$name: IR parse/print round trip" sh -c \
        "$LUMA $OUT/rt_$name.lir -S -o $OUT/rt2_$name --dump-ir > $OUT/rt_$name.reprint && cmp $exe.lir $OUT/rt_$name.reprint"
done

# The same programs through the other optimization levels: -O0 (naive
# stack-slot backend) and -O1 (optimizing backend without IR passes).
printf '== positive programs at -O0 and -O1 ==\n'
for lvl in 0 1; do
    for src in tests/pos/*.luma; do
        name=$(basename "$src" .luma)
        # deep tail recursion needs the tail calls of -O1/-O2
        [ "$lvl" = 0 ] && [ "$name" = tail_calls ] && continue
        exe="$OUT/pos${lvl}_$name"
        if ! "$LUMA" -O$lvl "$src" -o "$exe" >"$OUT/last.log" 2>&1; then bad "pos/$name -O$lvl compiles"; sed 's/^/      /' "$OUT/last.log"; continue; fi
        "$exe" >"$exe.actual" 2>"$exe.stderr"
        st=$?
        if [ $st -eq 0 ] && cmp -s "$exe.actual" "tests/pos/$name.out" && [ ! -s "$exe.stderr" ]; then ok "pos/$name -O$lvl output + exit 0"
        else bad "pos/$name -O$lvl (exit $st)"; diff "tests/pos/$name.out" "$exe.actual" | head -20 | sed 's/^/      /'; cat "$exe.stderr"; fi
        check "pos/$name -O$lvl: object identical to GNU as" same_as_gas "$name" "$exe.s" "$exe.o"
    done
done

# Properties of the optimizing backend's output (default -O2).
printf '== optimizing backend ==\n'
"$LUMA" bench/fib_typed.luma -S -o "$OUT/cg_fib" >/dev/null 2>&1
sed -n '/^fn.fib:/,/\.size fn.fib/p' "$OUT/cg_fib.s" | grep -v '^ *#' >"$OUT/cg_fib.body"
check "typed fib: inline fixnum arithmetic, no runtime calls" sh -c \
    "grep -q 'jo ' $OUT/cg_fib.body && test -z \"\$(grep 'call luma_' $OUT/cg_fib.body | grep -v luma_int_overflow)\""
check "typed fib: n and fib(n-1) live in callee-saved registers across the calls" sh -c \
    "grep -q 'push rbx' $OUT/cg_fib.body && grep -q 'push r12' $OUT/cg_fib.body && ! grep -q 'rbp - ' $OUT/cg_fib.body"
check "typed fib: compare fused into cmp + jcc" grep -Eq '^ +cmp rbx, 5$' "$OUT/cg_fib.body"
"$LUMA" bench/loop_sum.luma -S -o "$OUT/cg_loop" >/dev/null 2>&1
sed -n '/^fn.run:/,/\.size fn.run/p' "$OUT/cg_loop.s" | grep -v '^ *#' >"$OUT/cg_loop.body"
check "loop_sum: leaf function needs no frame and calls nothing" sh -c \
    "! grep -q 'push rbp' $OUT/cg_loop.body && ! grep -Eq 'call (luma_add|luma_sub|luma_mul|luma_div|luma_lt)' $OUT/cg_loop.body"
check "loop_sum: division by 4 is a shift" grep -q 'sar ' "$OUT/cg_loop.body"
"$LUMA" tests/pos/tail_calls.luma -S -o "$OUT/cg_tail" >/dev/null 2>&1
check "tail calls become jumps" sh -c "grep -q 'jmp fn.count    # tail call' $OUT/cg_tail.s && grep -q 'jmp fn.is_odd    # tail call' $OUT/cg_tail.s"
"$LUMA" -O1 bench/fib.luma -S -o "$OUT/cg_o1" >/dev/null 2>&1
check "-O1 uses the optimizing backend (registers), -O0 the stack-slot one" sh -c \
    "grep -q 'Optimizing backend' $OUT/cg_o1.s && $LUMA -O0 bench/fib.luma -S -o $OUT/cg_o0 && ! grep -q 'Optimizing backend' $OUT/cg_o0.s"

# The optimizer on tests/pos/optimizations.luma: one function per pass,
# checked in the optimized IR. (Its output at every level is checked above.)
printf '== optimizer (tests/pos/optimizations.luma) ==\n'
"$LUMA" tests/pos/optimizations.luma -o "$OUT/opt" >/dev/null 2>&1
# fn_body FILE NAME: the instructions of LIR function @fn.NAME
fn_body() { sed -n "/^fn @fn.$2(/,/^}/p" "$1" | sed '1d;$d'; }
for f in folded branch dead cse hoist; do
    fn_body "$OUT/opt.lir" "$f" >"$OUT/opt_$f.lir"
    fn_body "$OUT/opt.opt.lir" "$f" >"$OUT/opt_$f.opt"
done
check "the unoptimized IR still has the work (test is not vacuous)" sh -c \
    "grep -q ' mul ' $OUT/opt_folded.lir && grep -q ' br ' $OUT/opt_branch.lir && grep -q ' eq ' $OUT/opt_dead.lir && test \$(grep -c ' add ' $OUT/opt_cse.lir) -eq 2"
check "constant folding: 6 * 7 becomes const 42" sh -c \
    "grep -q 'const 42' $OUT/opt_folded.opt && ! grep -q ' mul ' $OUT/opt_folded.opt"
check "branch simplification: if (1 < 2) leaves no compare or branch" sh -c \
    "! grep -Eq ' (lt|br) ' $OUT/opt_branch.opt"
check "dead-code elimination: the unused == is gone" sh -c "! grep -q ' eq ' $OUT/opt_dead.opt"
check "CSE: (a + b) * (a + b) computes a + b once" sh -c "test \$(grep -c ' add ' $OUT/opt_cse.opt) -eq 1"
# LICM: n * 3 must be computed once, outside the block that branches back to itself
awk '/^[A-Za-z0-9_.]+:$/ { lbl = substr($0, 1, length($0) - 1); body = ""; next }
     { body = body $0 "\n" }
     /^ +br / && index($0, " " lbl ",") { printf "%s", body }' "$OUT/opt_hoist.opt" >"$OUT/opt_hoist.loop"
check "LICM: n * 3 is hoisted out of the loop" sh -c \
    "test -s $OUT/opt_hoist.loop && ! grep -q ' mul ' $OUT/opt_hoist.loop && test \$(grep -c ' mul ' $OUT/opt_hoist.opt) -eq 1"
check "the optimized IR is accepted as input and gives the same output" sh -c \
    "cp $OUT/opt.opt.lir $OUT/opt_re.lir && $LUMA $OUT/opt_re.lir -o $OUT/opt_re && $OUT/opt_re | cmp -s - tests/pos/optimizations.out"

# ---------------------------------------------------------- self-checking program
printf '== self-test (examples/selftest.luma) ==\n'
st="$OUT/selftest"
if "$LUMA" examples/selftest.luma -o "$st" >"$OUT/last.log" 2>&1; then
    "$st" >"$st.actual" 2>"$st.stderr"
    stst=$?
    # exact match pins the number of checks run, so silently skipped checks also fail
    if [ $stst -eq 0 ] && cmp -s "$st.actual" tests/selftest.out && [ ! -s "$st.stderr" ]; then
        ok "$(sed -n 1p "$st.actual"), exit 0"
    else
        bad "selftest (exit $stst)"; cat "$st.actual" "$st.stderr" | sed 's/^/      /'
    fi
    check "selftest: object identical to GNU as" same_as_gas selftest "$st.s" "$st.o"
    cp "$st.lir" "$OUT/st_copy.lir"
    check "selftest: .lir recompiles to identical .s" sh -c \
        "$LUMA $OUT/st_copy.lir -S -o $OUT/st_copy && sed 1d $st.s > $OUT/x1 && sed 1d $OUT/st_copy.s > $OUT/x2 && cmp $OUT/x1 $OUT/x2"
else
    bad "selftest compiles"; sed 's/^/      /' "$OUT/last.log"
fi
sed 's/sum == 5050/sum == 5051/' examples/selftest.luma > "$OUT/selftest_broken.luma"
check "selftest detects a failing check (exit 1, FAIL line)" sh -c \
    "$LUMA $OUT/selftest_broken.luma -o $OUT/stb && ! $OUT/stb > $OUT/stb.out 2>&1 && grep -q '^FAIL: while sums 1..100$' $OUT/stb.out && grep -q '^SELFTEST FAILED$' $OUT/stb.out"

# ---------------------------------------------------------- negative programs
printf '== compile errors ==\n'
for src in tests/neg/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/neg_$name"
    "$LUMA" "$src" -o "$exe" >"$OUT/neg.out" 2>"$OUT/neg.err"
    st=$?
    want=$(cat "tests/neg/$name.err")
    if [ $st -ne 0 ] && grep -qF "$want" "$OUT/neg.err" && grep -q "^$src:[0-9]*:[0-9]*: error:" "$OUT/neg.err" \
        && [ ! -e "$exe.lir" ] && [ ! -e "$exe.s" ] && [ ! -e "$exe" ]; then
        ok "neg/$name: $(head -1 "$OUT/neg.err" | sed "s|^$src:||")"
    else
        bad "neg/$name (exit $st)"; sed 's/^/      /' "$OUT/neg.err"
    fi
done
check "missing input file is reported" sh -c "! $LUMA does/not/exist.luma -o $OUT/x 2>$OUT/e && grep -q 'cannot open' $OUT/e"
check "unknown option is reported"     sh -c "! $LUMA --frobnicate 2>$OUT/e && grep -q 'unknown option' $OUT/e"

# ---------------------------------------------------------- runtime errors
printf '== runtime errors ==\n'
for lvl in 0 1 2; do
for src in tests/rt/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/rt_err${lvl}_$name"
    if ! "$LUMA" -O$lvl "$src" -o "$exe" >"$OUT/last.log" 2>&1; then bad "rt/$name -O$lvl compiles"; sed 's/^/      /' "$OUT/last.log"; continue; fi
    "$exe" >"$exe.stdout" 2>"$exe.stderr"
    st=$?
    if [ $st -eq 1 ] && cmp -s "$exe.stdout" "tests/rt/$name.out" && cmp -s "$exe.stderr" "tests/rt/$name.err"; then
        ok "rt/$name -O$lvl: exit 1, $(cat "$exe.stderr")"
    else
        bad "rt/$name -O$lvl (exit $st)"; cat "$exe.stdout" "$exe.stderr" | sed 's/^/      /'
    fi
done
done

# ---------------------------------------------------------- C FFI
printf '== C FFI (extern fun) ==\n'
FFILIB="$OUT/ffilib"
mkdir -p "$FFILIB"
check "build C helper library (test-only, system cc)" sh -c \
    "cc -c -O2 -o $FFILIB/ffi_helper.o tests/ffi/ffi_helper.c && ar rcs $FFILIB/libffihelper.a $FFILIB/ffi_helper.o"
for lvl in 0 1 2; do
for src in tests/ffi/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/ffi${lvl}_$name"
    if ! "$LUMA" -O$lvl "$src" -o "$exe" -L "$FFILIB" -l ffihelper >"$OUT/last.log" 2>&1; then
        bad "ffi/$name -O$lvl compiles + links"; sed 's/^/      /' "$OUT/last.log"; continue
    fi
    "$exe" >"$exe.stdout" 2>"$exe.stderr"
    st=$?
    if [ -f "tests/ffi/$name.err" ]; then
        if [ $st -eq 1 ] && cmp -s "$exe.stdout" "tests/ffi/$name.out" && cmp -s "$exe.stderr" "tests/ffi/$name.err"; then
            ok "ffi/$name -O$lvl: exit 1, $(sed 's/^luma: runtime error: //' "$exe.stderr")"
        else bad "ffi/$name -O$lvl (exit $st)"; cat "$exe.stdout" "$exe.stderr" | sed 's/^/      /'; fi
    else
        if [ $st -eq 0 ] && cmp -s "$exe.stdout" "tests/ffi/$name.out" && [ ! -s "$exe.stderr" ]; then ok "ffi/$name -O$lvl output + exit 0"
        else bad "ffi/$name -O$lvl (exit $st)"; diff "tests/ffi/$name.out" "$exe.stdout" | sed 's/^/      /'; cat "$exe.stderr"; fi
        check "ffi/$name -O$lvl: object identical to GNU as" same_as_gas "$name" "$exe.s" "$exe.o"
    fi
done
done
check "ffi: link a C object file given on the command line" sh -c \
    "$LUMA tests/ffi/types.luma -o $OUT/ffi_obj $FFILIB/ffi_helper.o && $OUT/ffi_obj | cmp -s - tests/ffi/types.out"
check "ffi: -lNAME / -LDIR attached forms" sh -c \
    "$LUMA tests/ffi/types.luma -o $OUT/ffi_attached -L$FFILIB -lffihelper && $OUT/ffi_attached | cmp -s - tests/ffi/types.out"
check "ffi: libc functions need no flags (examples/ffi.luma)" sh -c \
    "$LUMA examples/ffi.luma -o $OUT/ffi_example && $OUT/ffi_example > $OUT/ffi_example.out && grep -q 'luma via C' $OUT/ffi_example.out"
check "ffi: an unresolved C symbol is a link error" sh -c \
    "printf 'extern fun no_such_function_xyz(): void;\nno_such_function_xyz();\n' > $OUT/unres.luma && ! $LUMA $OUT/unres.luma -o $OUT/unres 2>$OUT/e && grep -q 'no_such_function_xyz' $OUT/e && grep -q 'linker' $OUT/e"
check "ffi: the call zeroes eax (variadic-safe) and converts via the runtime" sh -c \
    "grep -q 'xor eax, eax' $OUT/ffi2_types.s && grep -q 'call luma_ffi_arg_i8@PLT' $OUT/ffi2_types.s && grep -q 'call luma_ffi_ret_u64@PLT' $OUT/ffi2_types.s"

# ---------------------------------------------------------- hand-written IR
printf '== hand-written LIR ==\n'
for src in tests/ir/*.lir; do
    name=$(basename "$src" .lir)
    exe="$OUT/ir_$name"
    if ! "$LUMA" "$src" -o "$exe" >"$OUT/last.log" 2>&1; then bad "ir/$name compiles"; sed 's/^/      /' "$OUT/last.log"; continue; fi
    "$exe" >"$exe.actual" 2>&1
    st=$?
    if [ $st -eq 0 ] && cmp -s "$exe.actual" "tests/ir/$name.out"; then ok "ir/$name output + exit 0"
    else bad "ir/$name (exit $st)"; diff "tests/ir/$name.out" "$exe.actual" | sed 's/^/      /'; fi
    check "ir/$name: object identical to GNU as" same_as_gas "$name" "$exe.s" "$exe.o"
done
check "invalid LIR is rejected by the verifier" sh -c \
    "printf 'module \"m\"\nfn @luma_main() {\ne:\n  ret %%x\n}\n' > $OUT/bad.lir && ! $LUMA $OUT/bad.lir -o $OUT/bad 2>$OUT/e && grep -q 'may be read before it is assigned' $OUT/e"

# ------------------------------------------------ assembler, independently
printf '== standalone assembler (lasm) ==\n'
check "lasm assembles answer.s" "$LASM" tests/asm/answer.s -o "$OUT/answer.o"
check "C program links against lasm object" cc -o "$OUT/answer" tests/asm/answer_main.c "$OUT/answer.o"
check "answer() == 42 and answer_msg() from .rodata" "$OUT/answer"
check "lasm assembles coverage.s" "$LASM" tests/asm/coverage.s -o "$OUT/cov_luma.o"
check "readelf accepts coverage object" readelf -h -S -s -r "$OUT/cov_luma.o"
check "lasm rejects bad input" sh -c "printf 'mov [rax], [rbx]\n' > $OUT/bad.s && ! $LASM $OUT/bad.s -o $OUT/bad.o 2>$OUT/e && grep -q 'bad.s:1: error' $OUT/e"
if [ "$HAVE_AS" -eq 1 ]; then
    check "differential: coverage.s identical to GNU as (bytes + relocations)" same_as_gas cov tests/asm/coverage.s "$OUT/cov_luma.o"
    python3 -I tests/asm/gen_forms.py > "$OUT/forms.s"
    check "differential: $(grep -c '^    [a-z]' "$OUT/forms.s") generated instruction forms identical to GNU as" sh -c \
        "$LASM $OUT/forms.s -o $OUT/forms.o"
    check "  (generated forms: bytes + relocations)" same_as_gas forms "$OUT/forms.s" "$OUT/forms.o"
else
    printf 'SKIP  differential tests (GNU as not installed)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
