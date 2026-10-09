# Luma bootstrap compiler.
#
#   make            build build/luma and build/lasm
#   make hello      compile and run examples/hello.luma
#   make inspect    show readelf/objdump of build/hello.o
#   make test       run unit + end-to-end + negative tests
#   make clean      remove build/

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
CFLAGS  += -D_POSIX_C_SOURCE=200809L
BUILD   ?= build

CORE_SRC  := src/util.c src/lexer.c src/parser.c src/codegen.c src/obj.c src/asm.c src/elf_writer.c
CORE_OBJ  := $(CORE_SRC:src/%.c=$(BUILD)/obj/%.o)
HEADERS   := $(wildcard src/*.h)

.PHONY: all clean test unit e2e hello inspect

all: $(BUILD)/luma $(BUILD)/lasm

$(BUILD)/obj/%.o: src/%.c $(HEADERS) | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/luma: $(BUILD)/obj/main.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/lasm: $(BUILD)/obj/lasm.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_asm: tests/unit/test_asm.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) -Isrc tests/unit/test_asm.c $(CORE_OBJ) -o $@

$(BUILD)/test_elf: tests/unit/test_elf.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) -Isrc tests/unit/test_elf.c $(CORE_OBJ) -o $@

$(BUILD)/test_frontend: tests/unit/test_frontend.c $(CORE_OBJ) tests/unit/check.h
	$(CC) $(CFLAGS) -Isrc tests/unit/test_frontend.c $(CORE_OBJ) -o $@

$(BUILD)/obj:
	mkdir -p $@

# The example is always rebuilt from source (no prebuilt .o or executable).
hello: all
	./$(BUILD)/luma examples/hello.luma -o $(BUILD)/hello -v
	./$(BUILD)/hello

inspect: hello
	@echo "== $(BUILD)/hello.s =="; cat $(BUILD)/hello.s
	readelf -h -S -s -r $(BUILD)/hello.o
	objdump -d -r -M intel $(BUILD)/hello.o

unit: $(BUILD)/test_asm $(BUILD)/test_elf $(BUILD)/test_frontend
	./$(BUILD)/test_frontend
	./$(BUILD)/test_asm
	./$(BUILD)/test_elf

e2e: all
	./tests/run_e2e.sh

test: unit e2e

clean:
	rm -rf $(BUILD)
