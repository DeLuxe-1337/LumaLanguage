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
OUT=build/test
rm -rf "$OUT"
mkdir -p "$OUT"

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; }
check() { # check NAME COMMAND...
    name=$1; shift
    if "$@" >"$OUT/last.log" 2>&1; then ok "$name"; else bad "$name"; sed 's/^/      /' "$OUT/last.log"; fi
}

# ---------------------------------------------------------------- hello world
printf '== hello world pipeline ==\n'
rm -f build/hello build/hello.s build/hello.o
check "luma compiles examples/hello.luma" "$LUMA" examples/hello.luma -o build/hello
check "assembly file build/hello.s exists" test -s build/hello.s
check "assembly generated from source (string + call)" \
    sh -c 'grep -q "\.asciz \"Hello, world!\"" build/hello.s && grep -q "call puts@PLT" build/hello.s'
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
check "R_X86_64_PC32 against .rodata - 4" grep -Eq 'R_X86_64_PC32 +0+ \.rodata - 4' "$OUT/rel.txt"
check "R_X86_64_PLT32 against puts - 4"   grep -Eq 'R_X86_64_PLT32 +0+ puts - 4' "$OUT/rel.txt"
readelf -s -W build/hello.o >"$OUT/sym.txt"
check "main is GLOBAL FUNC in .text"  grep -Eq 'FUNC +GLOBAL +DEFAULT +[0-9]+ main$' "$OUT/sym.txt"
check "puts is GLOBAL UND"            grep -Eq 'NOTYPE +GLOBAL +DEFAULT +UND puts$' "$OUT/sym.txt"
check "FILE symbol names the .luma source" grep -q 'FILE.*examples/hello.luma' "$OUT/sym.txt"
check "no .comment section (not produced by GNU as)" sh -c '! readelf -S build/hello.o | grep -q "\.comment"'
check "objdump -d -r accepts hello.o" objdump -d -r build/hello.o
objdump -d -r -M intel build/hello.o >"$OUT/dis.txt"
check "objdump decodes push/mov/lea/call/xor/pop/ret" sh -c '
    for i in "push +rbp" "mov +rbp,rsp" "lea +rdi,\[rip\+0x0\]" "call" "xor +eax,eax" "pop +rbp" "ret"; do
        grep -Eq "$i" '"$OUT"'/dis.txt || { echo "missing: $i"; exit 1; }; done'
check "objdump shows relocations inline" sh -c 'grep -q "R_X86_64_PC32" '"$OUT"'/dis.txt && grep -q "R_X86_64_PLT32.*puts" '"$OUT"'/dis.txt'
check "final executable is native x86-64 ELF" sh -c 'readelf -h build/hello | grep -q "Machine:.*X86-64"'

# The link step: cc receives only an object file, so it must run the linker
# (collect2/ld) and never an assembler.
cc -v -o "$OUT/relink" build/hello.o >"$OUT/link.log" 2>&1
check "system linker runs (collect2/ld)" grep -Eq 'collect2|/ld ' "$OUT/link.log"
check "link step invokes no assembler" sh -c '! grep -Eq "^ *([^ ]*/)?as( |$)" '"$OUT"'/link.log'

# --------------------------------------------------------- stage control
printf '== stage flags ==\n'
rm -f "$OUT/s1.s" "$OUT/s1.o" "$OUT/s1"
check "-S stops after assembly" sh -c "$LUMA examples/hello.luma -S -o $OUT/s1 && test -s $OUT/s1.s && test ! -e $OUT/s1.o"
check "-c stops after object"   sh -c "$LUMA examples/hello.luma -c -o $OUT/s1 && test -s $OUT/s1.o && test ! -e $OUT/s1"
check "--dump-tokens lists PRINT STRING SEMICOLON EOF" sh -c \
    "$LUMA examples/hello.luma -S -o $OUT/s2 --dump-tokens | awk '{print \$2}' | tr '\n' ' ' | grep -q 'PRINT STRING SEMICOLON EOF'"
check "--dump-ast shows the print node" sh -c \
    "$LUMA examples/hello.luma -S -o $OUT/s2 --dump-ast | grep -q '(print @1:1 (string \"Hello, world!\"))'"
check "lasm on build/hello.s gives the same .text as luma" sh -c \
    "$LASM build/hello.s -o $OUT/hello_lasm.o && objcopy -O binary -j .text build/hello.o $OUT/a.bin && objcopy -O binary -j .text $OUT/hello_lasm.o $OUT/b.bin && cmp $OUT/a.bin $OUT/b.bin"

# ---------------------------------------------------------- positive programs
printf '== positive programs ==\n'
for src in tests/pos/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/pos_$name"
    if ! "$LUMA" "$src" -o "$exe" >"$OUT/last.log" 2>&1; then bad "pos/$name compiles"; sed 's/^/      /' "$OUT/last.log"; continue; fi
    "$exe" >"$exe.actual" 2>&1
    st=$?
    if [ $st -eq 0 ] && cmp -s "$exe.actual" "tests/pos/$name.out"; then ok "pos/$name output + exit 0"
    else bad "pos/$name (exit $st)"; diff "tests/pos/$name.out" "$exe.actual" | sed 's/^/      /'; fi
done

# ---------------------------------------------------------- negative programs
printf '== negative programs ==\n'
for src in tests/neg/*.luma; do
    name=$(basename "$src" .luma)
    exe="$OUT/neg_$name"
    "$LUMA" "$src" -o "$exe" >"$OUT/neg.out" 2>"$OUT/neg.err"
    st=$?
    want=$(cat "tests/neg/$name.err")
    if [ $st -ne 0 ] && grep -qF "$want" "$OUT/neg.err" && grep -q "^$src:[0-9]*:[0-9]*: error:" "$OUT/neg.err" \
        && [ ! -e "$exe.s" ] && [ ! -e "$exe" ]; then
        ok "neg/$name rejected: $(head -1 "$OUT/neg.err")"
    else
        bad "neg/$name (exit $st)"; sed 's/^/      /' "$OUT/neg.err"
    fi
done
check "missing input file is reported" sh -c "! $LUMA does/not/exist.luma -o $OUT/x 2>$OUT/e && grep -q 'cannot open' $OUT/e"
check "unknown option is reported"     sh -c "! $LUMA --frobnicate 2>$OUT/e && grep -q 'unknown option' $OUT/e"

# ------------------------------------------------ assembler, independently
printf '== standalone assembler (lasm) ==\n'
check "lasm assembles answer.s" "$LASM" tests/asm/answer.s -o "$OUT/answer.o"
check "C program links against lasm object" cc -o "$OUT/answer" tests/asm/answer_main.c "$OUT/answer.o"
check "answer() == 42 and answer_msg() from .rodata" "$OUT/answer"
check "lasm assembles coverage.s" "$LASM" tests/asm/coverage.s -o "$OUT/cov_luma.o"
check "readelf accepts coverage object" readelf -h -S -s -r "$OUT/cov_luma.o"
check "lasm rejects bad input" sh -c "printf 'mov [rax], rbx\n' > $OUT/bad.s && ! $LASM $OUT/bad.s -o $OUT/bad.o 2>$OUT/e && grep -q 'bad.s:1: error' $OUT/e"

# Differential test: same source through GNU as (validation only).
if command -v as >/dev/null 2>&1; then
    as --64 -o "$OUT/cov_gas.o" tests/asm/coverage.s
    for sec in .text .rodata .data; do
        objcopy -O binary -j "$sec" "$OUT/cov_luma.o" "$OUT/l$sec.bin"
        objcopy -O binary -j "$sec" "$OUT/cov_gas.o" "$OUT/g$sec.bin"
        check "differential: $sec bytes identical to GNU as" cmp "$OUT/l$sec.bin" "$OUT/g$sec.bin"
    done
    # Compare relocations: offset, type, symbol+addend.
    norm() { readelf -r -W "$1" | awk '/^[0-9a-f]+ /{print $1, $3, $5, $6, $7}'; }
    norm "$OUT/cov_luma.o" >"$OUT/rl.txt"
    norm "$OUT/cov_gas.o" >"$OUT/rg.txt"
    check "differential: relocations identical to GNU as" diff "$OUT/rl.txt" "$OUT/rg.txt"
    as --64 -o "$OUT/hello_gas.o" build/hello.s
    objcopy -O binary -j .text build/hello.o "$OUT/hl.bin"
    objcopy -O binary -j .text "$OUT/hello_gas.o" "$OUT/hg.bin"
    check "differential: hello .text identical to GNU as" cmp "$OUT/hl.bin" "$OUT/hg.bin"
    norm build/hello.o >"$OUT/rl.txt"; norm "$OUT/hello_gas.o" >"$OUT/rg.txt"
    check "differential: hello relocations identical to GNU as" diff "$OUT/rl.txt" "$OUT/rg.txt"
else
    printf 'SKIP  differential tests (GNU as not installed)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
