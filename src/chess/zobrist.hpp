// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cassert>
#include <cstdint>

#include "types.hpp"

namespace mros::zobrist {

inline constexpr Key SEED = 114514ULL;

struct Tables final {
    std::array<std::array<Key, SQUARE_NB>, PIECE_NB> piece_square{};
    std::array<Key, FILE_NB> en_passant{};
    std::array<Key, CASTLING_RIGHT_NB> castling{};
    Key side = 0;
};

extern const Tables KEYS;

[[nodiscard]] inline Key piece_square(Piece piece, Square square) noexcept {
    assert(is_ok(piece) && is_ok(square));
    return KEYS.piece_square[piece][square];
}

[[nodiscard]] inline Key en_passant(File file) noexcept {
    assert(is_ok(file));
    return KEYS.en_passant[file];
}

[[nodiscard]] inline Key castling(CastlingRights rights) noexcept {
    assert(std::uint8_t(rights) < CASTLING_RIGHT_NB);
    return KEYS.castling[std::uint8_t(rights)];
}

[[nodiscard]] inline Key side() noexcept {
    return KEYS.side;
}

} // namespace mros::zobrist
