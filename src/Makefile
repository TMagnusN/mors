# MORS - a modern C++23 chess engine
# Copyright (C) 2026 Theodore Magnus Øen
# SPDX-License-Identifier: AGPL-3.0-or-later

SHELL := /usr/bin/bash
.SHELLFLAGS := -eu -o pipefail -c

CXX ?= g++
WINDRES ?= windres

ARCH ?= generic
CONFIG ?= release
LTO ?= 1

ifeq ($(LTO),1)
else ifneq ($(LTO),0)
    $(error Unsupported LTO '$(LTO)'; expected 0 or 1)
endif

PROJECT_ROOT := ..
BUILD_ROOT := $(PROJECT_ROOT)/build
BUILD_DIR := $(BUILD_ROOT)/$(CONFIG)-$(ARCH)

OBJ_VARIANT := default

ifeq ($(CONFIG),release)
    ifeq ($(LTO),1)
        OBJ_VARIANT := lto
    endif
endif

OBJ_DIR := $(BUILD_DIR)/obj/$(OBJ_VARIANT)

EXE_EXT := .exe

PERFT_TARGET := $(BUILD_DIR)/mors-perft-$(ARCH)$(EXE_EXT)
TEST_TARGET := $(BUILD_DIR)/mors-tests-$(ARCH)$(EXE_EXT)
TT_BENCH_TARGET := $(BUILD_DIR)/mors-ttbench-$(ARCH)$(EXE_EXT)

ENGINE_ARCH := $(ARCH)

ENGINE_TARGET := $(BUILD_DIR)/mors-$(ENGINE_ARCH)$(EXE_EXT)
DATAGEN_TARGET := $(BUILD_DIR)/mors-datagen-$(ENGINE_ARCH)$(EXE_EXT)

CPPFLAGS := -I.

COMMON_FLAGS := \
    -std=c++23 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Wconversion \
    -Wshadow \
    -finput-charset=UTF-8 \
    -fexec-charset=UTF-8 \
    -ffunction-sections \
    -fdata-sections

# ---------------------------------------------------------------------------
# CPU architecture variants
#
# generic:
#   No optional ISA requirement.
#   Sliding attacks use the MAGIC implementation.
#
# avx2:
#   Requires AVX2.
#   Uses the AVX2-capable path while retaining non-PEXT sliding attacks.
#
# bmi2:
#   Requires BMI2.
#   PEXT is part of BMI2, so MORS_PEXT is the internal implementation flag.
#
# avx2+bmi2:
#   Requires both AVX2 and BMI2.
#   PEXT is used for sliding attacks and AVX2 is available to optimized code.
# ---------------------------------------------------------------------------

ifeq ($(ARCH),avx2)

    CPPFLAGS += -DMORS_DUAL_HQ
    ARCH_FLAGS := -mavx2

else ifeq ($(ARCH),bmi2)

    CPPFLAGS += -DMORS_PEXT
    ARCH_FLAGS := -mbmi2

else ifeq ($(ARCH),avx2+bmi2)

    CPPFLAGS += -DMORS_PEXT
    ARCH_FLAGS := -mavx2 -mbmi2

else ifeq ($(ARCH),generic)

    CPPFLAGS += -DMORS_MAGIC
    ARCH_FLAGS :=

else

    $(error Unsupported ARCH '$(ARCH)'; expected generic, avx2, bmi2 or avx2+bmi2)

endif

# ---------------------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------------------

ifeq ($(CONFIG),debug)

    CONFIG_FLAGS := \
        -Og \
        -g3

else ifeq ($(CONFIG),sanitize)

    CONFIG_FLAGS := \
        -O1 \
        -g3 \
        -fsanitize=address \
        -fsanitize=undefined \
        -fno-omit-frame-pointer

    LINK_FLAGS := \
        -fsanitize=address \
        -fsanitize=undefined

else ifeq ($(CONFIG),release)

    CONFIG_FLAGS := \
        -O3 \
        -DNDEBUG

    # Build self-contained Windows executables.
    #
    # GCC / MinGW runtimes are linked statically so release binaries do not
    # depend on libstdc++-6.dll, libgcc_s_seh-1.dll or libwinpthread-1.dll.
    #
    # Normal Windows system DLL dependencies remain.
    LINK_FLAGS := \
        -static \
        -static-libgcc \
        -static-libstdc++ \
        -Wl,--gc-sections

    ifeq ($(LTO),1)
        CONFIG_FLAGS += -flto=auto
        LINK_FLAGS += -flto=auto
    endif

else

    $(error Unsupported CONFIG '$(CONFIG)'; expected debug, sanitize or release)

endif

CXXFLAGS := \
    $(COMMON_FLAGS) \
    $(CONFIG_FLAGS) \
    $(ARCH_FLAGS) \
    -MMD \
    -MP

LDFLAGS := $(LINK_FLAGS)

WINDOWS_RESOURCE_SOURCE := $(PROJECT_ROOT)/resources/windows/mors.rc
WINDOWS_ICON_SOURCE := $(PROJECT_ROOT)/resources/windows/mors.ico
WINDOWS_RESOURCE_OBJECT := $(OBJ_DIR)/resources/mors-icon.o

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

CORE_SOURCES := \
    chess/bitboard.cpp \
    chess/attacks.cpp \
    chess/zobrist.cpp \
    chess/position.cpp \
    chess/movegen.cpp \
    chess/perft.cpp \
    eval/nnue/network.cpp \
    eval/nnue/wdl.cpp \
    eval/nnue/worker.cpp \
    search/search.cpp \
    search/see.cpp \
    search/time.cpp \
    search/tt.cpp

CORE_OBJECTS := \
    $(addprefix $(OBJ_DIR)/,$(CORE_SOURCES:.cpp=.o))

PERFT_OBJECT := \
    $(OBJ_DIR)/tools/perft_main.o

TT_BENCH_OBJECT := \
    $(OBJ_DIR)/tools/tt_bench.o

DATAGEN_OBJECT := \
    $(OBJ_DIR)/tools/datagen_main.o

UCI_OBJECT := \
    $(OBJ_DIR)/uci.o

ENGINE_OBJECTS := \
    $(OBJ_DIR)/main.o \
    $(UCI_OBJECT)

TEST_OBJECTS := \
    $(OBJ_DIR)/tests/attacks_tests.o \
    $(OBJ_DIR)/tests/zobrist_tests.o \
    $(OBJ_DIR)/tests/score_tests.o \
    $(OBJ_DIR)/tests/see_tests.o \
    $(OBJ_DIR)/tests/tt_tests.o \
    $(OBJ_DIR)/tests/nnue_tests.o \
    $(OBJ_DIR)/tests/search_tests.o \
    $(OBJ_DIR)/tests/time_tests.o \
    $(OBJ_DIR)/tests/uci_tests.o \
    $(OBJ_DIR)/tests/datagen_tests.o \
    $(OBJ_DIR)/tests/movegen_tests.o

DEPENDENCIES := \
    $(CORE_OBJECTS:.o=.d) \
    $(ENGINE_OBJECTS:.o=.d) \
    $(PERFT_OBJECT:.o=.d) \
    $(TT_BENCH_OBJECT:.o=.d) \
    $(DATAGEN_OBJECT:.o=.d) \
    $(TEST_OBJECTS:.o=.d)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.DEFAULT_GOAL := all

.PHONY: \
    all \
    all-versions \
    build \
    release \
    debug \
    sanitize \
    test \
    test-run \
    uci \
    perft \
    ttbench \
    ttbench-run \
    datagen \
    clean

# Normal build:
#
#   make
#   make -j4
#   make -j4 ARCH=avx2
#   make -j4 ARCH=bmi2
#   make -j4 ARCH=avx2+bmi2

all: build

build: $(ENGINE_TARGET) $(PERFT_TARGET)

release: build

# Build all four distribution variants.
#
# The variants are intentionally built sequentially. Each recursive make still
# inherits GNU Make's jobserver, so:
#
#   make -j4 all-versions
#
# compiles each individual variant with up to four parallel jobs without
# trying to build all four variants simultaneously.
all-versions:
	+$(MAKE) CONFIG=release ARCH=generic LTO=$(LTO) build
	+$(MAKE) CONFIG=release ARCH=avx2 LTO=$(LTO) build
	+$(MAKE) CONFIG=release ARCH=bmi2 LTO=$(LTO) build
	+$(MAKE) CONFIG=release ARCH=avx2+bmi2 LTO=$(LTO) build

debug:
	+$(MAKE) CONFIG=debug ARCH=$(ARCH) LTO=0 build

sanitize:
	+$(MAKE) CONFIG=sanitize ARCH=$(ARCH) LTO=0 test-run

test:
	+$(MAKE) CONFIG=debug ARCH=$(ARCH) LTO=0 test-run

test-run: $(TEST_TARGET)
	$(TEST_TARGET)

uci: $(ENGINE_TARGET)
	$(ENGINE_TARGET)

perft:
	+$(MAKE) CONFIG=release ARCH=$(ARCH) LTO=$(LTO) build
	$(BUILD_ROOT)/release-$(ARCH)/mors-perft-$(ARCH)$(EXE_EXT) 5

ttbench:
	+$(MAKE) CONFIG=release ARCH=$(ARCH) LTO=$(LTO) ttbench-run

ttbench-run: $(TT_BENCH_TARGET)
	$(TT_BENCH_TARGET)

datagen: $(DATAGEN_TARGET)

# ---------------------------------------------------------------------------
# Linking
# ---------------------------------------------------------------------------

$(PERFT_TARGET): $(CORE_OBJECTS) $(PERFT_OBJECT)
	mkdir -p "$(@D)"
	$(CXX) $^ $(LDFLAGS) -o "$@"

$(ENGINE_TARGET): $(CORE_OBJECTS) $(ENGINE_OBJECTS) $(WINDOWS_RESOURCE_OBJECT)
	mkdir -p "$(@D)"
	$(CXX) $^ $(LDFLAGS) -o "$@"

$(TEST_TARGET): $(CORE_OBJECTS) $(UCI_OBJECT) $(TEST_OBJECTS)
	mkdir -p "$(@D)"
	$(CXX) $^ $(LDFLAGS) -o "$@"

$(TT_BENCH_TARGET): $(OBJ_DIR)/search/tt.o $(TT_BENCH_OBJECT)
	mkdir -p "$(@D)"
	$(CXX) $^ $(LDFLAGS) -o "$@"

$(DATAGEN_TARGET): $(CORE_OBJECTS) $(DATAGEN_OBJECT)
	mkdir -p "$(@D)"
	$(CXX) $^ $(LDFLAGS) -o "$@"

# ---------------------------------------------------------------------------
# Compilation
# ---------------------------------------------------------------------------


$(WINDOWS_RESOURCE_OBJECT): $(WINDOWS_RESOURCE_SOURCE) $(WINDOWS_ICON_SOURCE)
	mkdir -p "$(@D)"
	$(WINDRES) -I"$(PROJECT_ROOT)/resources/windows" -O coff -i "$<" -o "$@"

$(OBJ_DIR)/%.o: %.cpp
	mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c "$<" -o "$@"

$(OBJ_DIR)/tests/%.o: $(PROJECT_ROOT)/tests/%.cpp
	mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c "$<" -o "$@"

# ---------------------------------------------------------------------------
# Maintenance
# ---------------------------------------------------------------------------

clean:
	rm -rf "$(BUILD_ROOT)"

-include $(DEPENDENCIES)