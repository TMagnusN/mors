// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "perft.hpp"

#include <cassert>
#include <string>

namespace mors {

std::uint64_t perft(Position& position, int depth) noexcept {
    assert(depth >= 0 && position.is_consistent());
    if (depth == 0)
        return 1;

    MoveList moves;
    generate_legal(position, moves);
    if (depth == 1)
        return moves.size();

    std::uint64_t nodes = 0;
    for (const Move move : moves) {
        StateInfo state;
        position.do_move(move, state);
        nodes += perft(position, depth - 1);
        position.undo_move(move, state);
    }
    return nodes;
}

std::vector<std::pair<Move, std::uint64_t>> perft_divide(Position& position, int depth) {
    assert(depth >= 1 && position.is_consistent());
    MoveList moves;
    generate_legal(position, moves);

    std::vector<std::pair<Move, std::uint64_t>> result;
    result.reserve(moves.size());

    for (const Move move : moves) {
        StateInfo state;
        position.do_move(move, state);
        result.emplace_back(move, perft(position, depth - 1));
        position.undo_move(move, state);
    }

    return result;
}

std::string move_to_uci(Move move) {
    assert(!move.is_none());
    std::string result;
    result.reserve(5);

    result += char('a' + file_of(move.from()));
    result += char('1' + rank_of(move.from()));
    result += char('a' + file_of(move.to()));
    result += char('1' + rank_of(move.to()));

    if (move.type() == PROMOTION) {
        constexpr char PROMOTION_CHARS[PIECE_TYPE_NB] = {
            '\0', '\0', 'n', 'b', 'r', 'q', '\0', '\0'
        };
        result += PROMOTION_CHARS[move.promotion_type()];
    }

    return result;
}

} // namespace mors
