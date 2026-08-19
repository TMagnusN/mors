// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "move_list.hpp"
#include "position.hpp"

namespace mors {

// Generates moves that obey piece movement rules. Castling path safety is
// checked here; king safety for all other moves is checked by generate_legal.
void generate_pseudo_legal(const Position& position, MoveList& moves) noexcept;

// Generates legal moves directly from check, pin, and evasion masks. Only the
// en-passant edge case uses make/unmake as a final king-safety oracle.
void generate_legal(Position& position, MoveList& moves) noexcept;

// Generates only legal captures and promotions. This is the tactical subset
// consumed by non-check quiescence nodes.
void generate_legal_noisy(Position& position, MoveList& moves) noexcept;

} // namespace mors
