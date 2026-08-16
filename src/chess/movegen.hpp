// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "move_list.hpp"
#include "position.hpp"

namespace mros {

// Generates moves that obey piece movement rules. Castling path safety is
// checked here; king safety for all other moves is checked by generate_legal.
void generate_pseudo_legal(const Position& position, MoveList& moves) noexcept;

// Correctness-first legal generator. It deliberately uses make/unmake as the
// final king-safety oracle; its public contract remains suitable for replacing
// the internals with pin/check masks later.
void generate_legal(Position& position, MoveList& moves) noexcept;

} // namespace mros
