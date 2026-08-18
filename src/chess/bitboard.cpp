// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "bitboard.hpp"

#include <string>

namespace mors {

std::string pretty(Bitboard bb) {
    std::string result;
    result.reserve(18 * 9);

    for (int rank = RANK_8; rank >= RANK_1; --rank) {
        result += char('1' + rank);
        result += "  ";

        for (int file = FILE_A; file <= FILE_H; ++file) {
            const Square sq = make_square(File(file), Rank(rank));
            result += contains(bb, sq) ? "X " : ". ";
        }

        result += '\n';
    }

    result += "\n   a b c d e f g h\n";
    return result;
}

} // namespace mors
