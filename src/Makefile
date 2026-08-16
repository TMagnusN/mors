# MROS - a modern C++23 chess engine
# Copyright (C) 2026 Theodore Magnus Øen
# SPDX-License-Identifier: AGPL-3.0-or-later

SHELL := powershell.exe
.SHELLFLAGS := -NoProfile -Command

CXX ?= g++
ARCH ?= generic
CONFIG ?= release

PROJECT_ROOT := ..
BUILD_ROOT := $(PROJECT_ROOT)/build
BUILD_DIR := $(BUILD_ROOT)/$(CONFIG)-$(ARCH)
OBJ_DIR := $(BUILD_DIR)/obj

EXE_EXT := .exe
PERFT_TARGET := $(BUILD_DIR)/mros-perft-$(ARCH)$(EXE_EXT)
TEST_TARGET := $(BUILD_DIR)/mros-tests-$(ARCH)$(EXE_EXT)

CPPFLAGS := -I.
COMMON_FLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

ifeq ($(ARCH),avx2)
    CPPFLAGS += -DMROS_DUAL_HQ
    ARCH_FLAGS := -mavx2
else ifeq ($(ARCH),hybrid)
    CPPFLAGS += -DMROS_HYBRID -DMROS_DUAL_HQ -DMROS_MAGIC
    ARCH_FLAGS := -mavx2
else ifeq ($(ARCH),generic)
    CPPFLAGS += -DMROS_MAGIC
    ARCH_FLAGS :=
else
    $(error Unsupported ARCH '$(ARCH)'; expected generic, avx2 or hybrid)
endif

ifeq ($(CONFIG),debug)
    CONFIG_FLAGS := -Og -g3
else ifeq ($(CONFIG),sanitize)
    CONFIG_FLAGS := -O1 -g3 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
    LINK_FLAGS := -fsanitize=address -fsanitize=undefined
else ifeq ($(CONFIG),release)
    CONFIG_FLAGS := -O3 -DNDEBUG
else
    $(error Unsupported CONFIG '$(CONFIG)'; expected debug, sanitize or release)
endif

CXXFLAGS := $(COMMON_FLAGS) $(CONFIG_FLAGS) $(ARCH_FLAGS) -MMD -MP
LDFLAGS := $(LINK_FLAGS)

CORE_SOURCES := \
    chess/bitboard.cpp \
    chess/attacks.cpp \
    chess/position.cpp \
    chess/movegen.cpp \
    chess/perft.cpp

CORE_OBJECTS := $(addprefix $(OBJ_DIR)/,$(CORE_SOURCES:.cpp=.o))
PERFT_OBJECT := $(OBJ_DIR)/tools/perft_main.o
TEST_OBJECTS := $(OBJ_DIR)/tests/attacks_tests.o $(OBJ_DIR)/tests/movegen_tests.o
DEPENDENCIES := $(CORE_OBJECTS:.o=.d) $(PERFT_OBJECT:.o=.d) $(TEST_OBJECTS:.o=.d)

.DEFAULT_GOAL := all

.PHONY: all build release debug sanitize test test-run perft clean

all: release

build: $(PERFT_TARGET)

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

perft:
	$(MAKE) CONFIG=release ARCH=$(ARCH) build
	& '$(BUILD_ROOT)/release-$(ARCH)/mros-perft-$(ARCH)$(EXE_EXT)' 5

$(PERFT_TARGET): $(CORE_OBJECTS) $(PERFT_OBJECT)
	New-Item -ItemType Directory -Force -Path '$(@D)' | Out-Null
	$(CXX) $^ $(LDFLAGS) -o $@

$(TEST_TARGET): $(CORE_OBJECTS) $(TEST_OBJECTS)
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
