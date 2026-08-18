# MORS - a modern C++23 chess engine
# Copyright (C) 2026 Theodore Magnus Øen
# SPDX-License-Identifier: AGPL-3.0-or-later

SHELL := powershell.exe
.SHELLFLAGS := -NoProfile -Command

CXX ?= g++
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
ENGINE_TARGET := $(BUILD_DIR)/mors-$(ARCH)$(EXE_EXT)

CPPFLAGS := -I.
COMMON_FLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

ifeq ($(ARCH),avx2)
    CPPFLAGS += -DMORS_DUAL_HQ
    ARCH_FLAGS := -mavx2
else ifeq ($(ARCH),hybrid)
    CPPFLAGS += -DMORS_HYBRID -DMORS_DUAL_HQ -DMORS_MAGIC
    ARCH_FLAGS := -mavx2
else ifeq ($(ARCH),pext-avx2)
    CPPFLAGS += -DMORS_PEXT
    ARCH_FLAGS := -mbmi2 -mavx2
else ifeq ($(ARCH),pext)
    CPPFLAGS += -DMORS_PEXT
    ARCH_FLAGS := -mbmi2
else ifeq ($(ARCH),generic)
    CPPFLAGS += -DMORS_MAGIC
    ARCH_FLAGS :=
else
    $(error Unsupported ARCH '$(ARCH)'; expected generic, avx2, hybrid, pext or pext-avx2)
endif

ifeq ($(CONFIG),debug)
    CONFIG_FLAGS := -Og -g3
else ifeq ($(CONFIG),sanitize)
    CONFIG_FLAGS := -O1 -g3 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
    LINK_FLAGS := -fsanitize=address -fsanitize=undefined
else ifeq ($(CONFIG),release)
    CONFIG_FLAGS := -O3 -DNDEBUG
    ifeq ($(LTO),1)
        CONFIG_FLAGS += -flto=auto
        LINK_FLAGS += -flto=auto
    endif
else
    $(error Unsupported CONFIG '$(CONFIG)'; expected debug, sanitize or release)
endif

CXXFLAGS := $(COMMON_FLAGS) $(CONFIG_FLAGS) $(ARCH_FLAGS) -MMD -MP
LDFLAGS := $(LINK_FLAGS)

CORE_SOURCES := \
    chess/bitboard.cpp \
    chess/attacks.cpp \
    chess/zobrist.cpp \
    chess/position.cpp \
    chess/movegen.cpp \
    chess/perft.cpp \
    eval/nnue/network.cpp \
    eval/nnue/worker.cpp \
    search/search.cpp \
    search/see.cpp \
    search/time.cpp \
    search/tt.cpp

CORE_OBJECTS := $(addprefix $(OBJ_DIR)/,$(CORE_SOURCES:.cpp=.o))
PERFT_OBJECT := $(OBJ_DIR)/tools/perft_main.o
TT_BENCH_OBJECT := $(OBJ_DIR)/tools/tt_bench.o
UCI_OBJECT := $(OBJ_DIR)/uci.o
ENGINE_OBJECTS := $(OBJ_DIR)/main.o $(UCI_OBJECT)
TEST_OBJECTS := $(OBJ_DIR)/tests/attacks_tests.o $(OBJ_DIR)/tests/zobrist_tests.o $(OBJ_DIR)/tests/score_tests.o $(OBJ_DIR)/tests/see_tests.o $(OBJ_DIR)/tests/tt_tests.o $(OBJ_DIR)/tests/nnue_tests.o $(OBJ_DIR)/tests/search_tests.o $(OBJ_DIR)/tests/time_tests.o $(OBJ_DIR)/tests/uci_tests.o $(OBJ_DIR)/tests/movegen_tests.o
DEPENDENCIES := $(CORE_OBJECTS:.o=.d) $(ENGINE_OBJECTS:.o=.d) $(PERFT_OBJECT:.o=.d) $(TT_BENCH_OBJECT:.o=.d) $(TEST_OBJECTS:.o=.d)

.DEFAULT_GOAL := all

.PHONY: all build release debug sanitize test test-run uci perft ttbench ttbench-run clean

all: release

build: $(ENGINE_TARGET) $(PERFT_TARGET)

release:
	$(MAKE) CONFIG=release ARCH=$(ARCH) build

debug:
	$(MAKE) CONFIG=debug ARCH=$(ARCH) build

sanitize:
	$(MAKE) CONFIG=sanitize ARCH=$(ARCH) test-run

test:
	$(MAKE) CONFIG=debug ARCH=$(ARCH) test-run

test-run: $(TEST_TARGET)
	& '$(TEST_TARGET)'

uci: $(ENGINE_TARGET)
	& '$(ENGINE_TARGET)'

perft:
	$(MAKE) CONFIG=release ARCH=$(ARCH) build
	& '$(BUILD_ROOT)/release-$(ARCH)/mors-perft-$(ARCH)$(EXE_EXT)' 5

ttbench:
	$(MAKE) CONFIG=release ARCH=$(ARCH) ttbench-run

ttbench-run: $(TT_BENCH_TARGET)
	& '$(TT_BENCH_TARGET)'

$(PERFT_TARGET): $(CORE_OBJECTS) $(PERFT_OBJECT)
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $^ $(LDFLAGS) -o $@

$(ENGINE_TARGET): $(CORE_OBJECTS) $(ENGINE_OBJECTS)
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $^ $(LDFLAGS) -o $@

$(TEST_TARGET): $(CORE_OBJECTS) $(UCI_OBJECT) $(TEST_OBJECTS)
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $^ $(LDFLAGS) -o $@

$(TT_BENCH_TARGET): $(OBJ_DIR)/search/tt.o $(TT_BENCH_OBJECT)
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $^ $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: %.cpp
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/tests/%.o: $(PROJECT_ROOT)/tests/%.cpp
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	if (Test-Path -LiteralPath '$(BUILD_ROOT)') { Remove-Item -LiteralPath '$(BUILD_ROOT)' -Recurse -Force }

-include $(DEPENDENCIES)
