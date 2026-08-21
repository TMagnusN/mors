// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/position.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace mors::tools {

// Binary-compatible with bulletformat 1.8 ChessBoard. The record is stored
// side-to-move relative even though the generator accepts an absolute white
// result, matching ChessBoard::from_raw and the trainer's direct .data loader.
struct BulletChessBoard final {
    std::uint64_t occupancy = 0;
    std::array<std::uint8_t, 16> pieces{};
    std::int16_t score = 0;
    std::uint8_t result = 1;
    std::uint8_t king_square = 0;
    std::uint8_t opponent_king_square = 0;
    std::array<std::uint8_t, 3> extra{};
};

static_assert(sizeof(BulletChessBoard) == 32);
static_assert(alignof(BulletChessBoard) == alignof(std::uint64_t));

// 0 = black win, 1 = draw, 2 = white win.
[[nodiscard]] inline BulletChessBoard encode_bullet_record(
    const Position& position,
    int score_from_side_to_move,
    std::uint8_t white_result
) noexcept {
    BulletChessBoard record;
    const Color side = position.side_to_move();
    std::array<std::uint8_t, SQUARE_NB> relative_pieces{};

    for (int square_index = 0; square_index < SQUARE_NB; ++square_index) {
        const Square square = Square(square_index);
        const Piece piece = position.piece_on(square);
        if (piece == NO_PIECE)
            continue;

        const Square relative = relative_square(side, square);
        const std::uint8_t relative_color =
            color_of(piece) == side ? 0U : 8U;
        const std::uint8_t bullet_piece = static_cast<std::uint8_t>(
            int(type_of(piece)) - int(PAWN)
        );
        relative_pieces[static_cast<std::size_t>(relative)] =
            relative_color | bullet_piece;
        record.occupancy |= std::uint64_t{1} << relative;
    }

    std::size_t piece_index = 0;
    for (std::size_t square = 0; square < SQUARE_NB; ++square) {
        if ((record.occupancy & (std::uint64_t{1} << square)) == 0)
            continue;
        const std::uint8_t piece = relative_pieces[square];
        record.pieces[piece_index / 2] |= static_cast<std::uint8_t>(
            piece << (4 * (piece_index & 1))
        );
        ++piece_index;
    }

    record.score = static_cast<std::int16_t>(std::clamp(
        score_from_side_to_move,
        int(std::numeric_limits<std::int16_t>::min()),
        int(std::numeric_limits<std::int16_t>::max())
    ));
    white_result = std::min<std::uint8_t>(white_result, 2U);
    record.result = side == WHITE
        ? white_result
        : static_cast<std::uint8_t>(2U - white_result);

    record.king_square = static_cast<std::uint8_t>(
        relative_square(side, position.king_square(side))
    );
    const Square relative_opponent_king = relative_square(
        side,
        position.king_square(~side)
    );
    record.opponent_king_square = static_cast<std::uint8_t>(
        relative_opponent_king ^ 56
    );
    return record;
}

} // namespace mors::tools
