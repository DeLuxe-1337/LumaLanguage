# Luma bootstrap compiler.
#
#   make            build build/luma and build/lasm
#   make hello      compile and run examples/hello.luma
#   make inspect    show readelf/objdump of build/hello.o
#   make test       run unit + end-to-end + differential tests
#   make selftest   compile and run the self-checking program examples/selftest.luma
#   make difftest   random programs vs. a reference interpreter (DIFFTEST_COUNT=N)
#   make bench      benchmarks: -O0 (naive backend) vs. the default -O2
#   make clean      remove build/

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
# Required defines live outside CFLAGS so `make CFLAGS=...` (e.g. sanitizers) keeps them.
DEFS    := -D_POSIX_C_SOURCE=200809L
# The runtime is linked into user programs, so compiler-only flags such as
# sanitizers (passed via CFLAGS) must not leak into it.
RT_CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
BUILD   ?= build

CORE_SRC  := src/util.c src/lexer.c src/parser.c src/lower.c src/ir.c src/ir_types.c src/ir_parse.c src/ir_verify.c \
             src/cfg.c src/opt.c src/opt_util.c src/opt_cfg.c src/opt_ssa.c src/opt_passes.c \
             src/x86_isel.c src/x86_gen.c src/obj.c src/asm.c src/elf_writer.c
CORE_OBJ  := $(CORE_SRC:src/%.c=$(BUILD)/obj/%.o)
HEADERS   := $(wildcard src/*.h)

.PHONY: all clean test unit e2e difftest selftest hello inspect bench

all: $(BUILD)/luma $(BUILD)/lasm $(BUILD)/libluma_rt.a

# The runtime is part of the toolchain (like libc): built once here with the
# system C compiler, then linked into every Luma program by `luma`.
$(BUILD)/obj/luma_rt.o: runtime/luma_rt.c src/value.h | $(BUILD)/obj
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(BUILD)/libluma_rt.a: $(BUILD)/obj/luma_rt.o
	rm -f $@
	ar rcs $@ $^

$(BUILD)/obj/%.o: src/%.c $(HEADERS) | $(BUILD)/obj
	$(CC) $(CFLAGS) $(DEFS) -c $< -o $@

$(BUILD)/luma: $(BUILD)/obj/main.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $(DEFS) $^ -o $@

$(BUILD)/lasm: $(BUILD)/obj/lasm.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $(DEFS) $^ -o $@

$(BUILD)/test_asm: tests/unit/test_asm.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) $(DEFS) -Isrc tests/unit/test_asm.c $(CORE_OBJ) -o $@

$(BUILD)/test_elf: tests/unit/test_elf.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) $(DEFS) -Isrc tests/unit/test_elf.c $(CORE_OBJ) -o $@

$(BUILD)/test_frontend: tests/unit/test_frontend.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) $(DEFS) -Isrc tests/unit/test_frontend.c $(CORE_OBJ) -o $@

$(BUILD)/test_ir: tests/unit/test_ir.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) $(DEFS) -Isrc tests/unit/test_ir.c $(CORE_OBJ) -o $@

$(BUILD)/obj:
	mkdir -p $@

# The example is always rebuilt from source (no prebuilt .o or executable).
hello: all
	./$(BUILD)/luma examples/hello.luma -o $(BUILD)/hello -v
	./$(BUILD)/hello

inspect: hello
	@echo "== $(BUILD)/hello.lir =="; cat $(BUILD)/hello.lir
	@echo "== $(BUILD)/hello.s =="; cat $(BUILD)/hello.s
	readelf -h -S -s -r $(BUILD)/hello.o
	objdump -d -r -M intel $(BUILD)/hello.o

unit: $(BUILD)/test_asm $(BUILD)/test_elf $(BUILD)/test_frontend $(BUILD)/test_ir
	./$(BUILD)/test_frontend
	./$(BUILD)/test_ir
	./$(BUILD)/test_asm
	./$(BUILD)/test_elf

e2e: all
	./tests/run_e2e.sh

# Random programs vs. a reference interpreter (tests/difftest.py).
DIFFTEST_COUNT ?= 300
difftest: all
	python3 -I tests/difftest.py $(DIFFTEST_COUNT)

# Benchmarks (bench/*.luma): wall time at -O0 and at the default level.
bench: all
	python3 bench/run.py --compare

# A Luma program that checks the compiler from the inside (examples/selftest.luma).
selftest: all
	./$(BUILD)/luma examples/selftest.luma -o $(BUILD)/selftest
	./$(BUILD)/selftest

test: unit e2e difftest selftest

clean:
	rm -rf $(BUILD)
