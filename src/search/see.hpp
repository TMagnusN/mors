// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/position.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace mros {

using SeeValue = std::int32_t;

inline constexpr std::array<SeeValue, PIECE_TYPE_NB> SEE_PIECE_VALUES{
    0,   // NO_PIECE_TYPE
    100, // PAWN
    320, // KNIGHT
    330, // BISHOP
    500, // ROOK
    900, // QUEEN
    0,   // KING is handled through capture legality, not material value
    0    // reserved PieceType value
};

[[nodiscard]] constexpr SeeValue see_piece_value(PieceType type) noexcept {
    assert(type < PIECE_TYPE_NB);
    return SEE_PIECE_VALUES[static_cast<std::size_t>(type)];
}

// Returns whether the static exchange sequence on move.to() is worth at least
// threshold from the current side-to-move perspective. The move must belong to
// the position; Search normally calls this only with already legal moves.
[[nodiscard]] bool see_ge(
    const Position& position,
    Move move,
    SeeValue threshold
) noexcept;

} // namespace mros
