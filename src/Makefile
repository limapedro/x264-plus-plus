# x264++ — out-of-tree-friendly build (increment I0)
#
# The source tree is located from this Makefile's own path, so `make` can be
# invoked from anywhere.  Outputs go to $(BUILD_DIR), which defaults to the
# repository's build/ directory (the ../build equivalent of src/Makefile) and
# can be overridden, e.g.
#
#   make -f src/Makefile BUILD_DIR=build
#   make -f /e/__PROJECTS__/x264++/src/Makefile BUILD_DIR=/tmp/x264pp-build
#
# Toolchain: MSYS2 MinGW-w64 GCC 16 (C11) and NASM (reserved for the SIMD
# increments; I0 has no assembly sources yet).

SRC_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT_DIR := $(patsubst %/,%,$(dir $(SRC_DIR)))
BUILD_DIR ?= $(ROOT_DIR)/build

CC       ?= gcc
NASM     ?= nasm
ASMFMT   ?= win64

CFLAGS   ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS ?=
LDFLAGS  ?=
ALL_CFLAGS := $(CFLAGS) $(CPPFLAGS) -I$(SRC_DIR)

SOURCES := \
	$(SRC_DIR)/cli/cli.c \
	$(SRC_DIR)/cli/input.c \
	$(SRC_DIR)/cli/output.c \
	$(SRC_DIR)/core/api.c \
	$(SRC_DIR)/core/params.c \
	$(SRC_DIR)/core/frame.c \
	$(SRC_DIR)/core/bs.c \
	$(SRC_DIR)/core/dct.c \
	$(SRC_DIR)/core/quant.c \
	$(SRC_DIR)/core/predict.c \
	$(SRC_DIR)/core/cavlc.c \
	$(SRC_DIR)/core/encode.c

# Future increments add hand-written SIMD here; the rule below is already in
# place so adding a .asm file does not require changing the build again.
ASM_SOURCES :=

OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES)) \
           $(patsubst $(SRC_DIR)/%.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

TARGET := $(BUILD_DIR)/x264pp.exe

.PHONY: all clean toolcheck

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p "$(dir $@)"
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@mkdir -p "$(dir $@)"
	$(NASM) -f $(ASMFMT) -I$(SRC_DIR)/ -o $@ $<

toolcheck:
	@$(CC) --version | head -n 1
	@$(NASM) -v

clean:
	rm -rf "$(BUILD_DIR)"
