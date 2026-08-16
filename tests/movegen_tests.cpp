// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/perft.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

struct PerftCase final {
    std::string_view name;
    std::string_view fen;
    std::array<std::uint64_t, 5> nodes;
    int max_depth;
};

bool run_case(const PerftCase& test) {
    auto parsed = mros::Position::from_fen(test.fen);
    if (!parsed) {
        std::cerr << "FAIL " << test.name << ": " << parsed.error() << '\n';
        return false;
    }

    mros::Position position = std::move(*parsed);
    const std::string original = position.fen();

    for (int depth = 1; depth <= test.max_depth; ++depth) {
        const std::uint64_t actual = mros::perft(position, depth);
        const std::uint64_t expected = test.nodes[std::size_t(depth - 1)];
        if (actual != expected) {
            std::cerr << "FAIL " << test.name << " depth " << depth
                      << ": expected " << expected << ", got " << actual << '\n';
            return false;
        }
        if (position.fen() != original) {
            std::cerr << "FAIL " << test.name << ": make/unmake changed position\n";
            return false;
        }
    }

    std::cout << "PASS " << test.name << '\n';
    return true;
}

} // namespace

bool run_attacks_tests();

int main() {
    mros::initialize_attacks();

    constexpr std::array<PerftCase, 5> CASES = {{
        {
            "start position",
            mros::START_FEN,
            {20, 400, 8'902, 197'281, 4'865'609},
            5
        },
        {
            "kiwipete",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
            {48, 2'039, 97'862, 4'085'603, 193'690'690},
            4
        },
        {
            "en-passant and checks",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            {14, 191, 2'812, 43'238, 674'624},
            4
        },
        {
            "castling and promotions",
            "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
            {6, 264, 9'467, 422'333, 15'833'292},
            4
        },
        {
            "promotion tactics",
            "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
            {44, 1'486, 62'379, 2'103'487, 89'941'194},
            4
        }
    }};

    bool passed = run_attacks_tests();
    for (const PerftCase& test : CASES)
        passed &= run_case(test);

    return passed ? 0 : 1;
}
