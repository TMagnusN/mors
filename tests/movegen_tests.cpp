// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/perft.hpp"

#include <algorithm>
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
    auto parsed = mors::Position::from_fen(test.fen);
    if (!parsed) {
        std::cerr << "FAIL " << test.name << ": " << parsed.error() << '\n';
        return false;
    }

    mors::Position position = std::move(*parsed);
    const std::string original = position.fen();

    for (int depth = 1; depth <= test.max_depth; ++depth) {
        const std::uint64_t actual = mors::perft(position, depth);
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

bool cross_check_legal_moves(mors::Position& position, int depth) {
    mors::MoveList actual;
    mors::generate_legal(position, actual);

    mors::MoveList noisy;
    mors::generate_legal_noisy(position, noisy);

    mors::MoveList pseudo;
    mors::generate_pseudo_legal(position, pseudo);

    std::array<std::uint16_t, mors::MAX_MOVES> actual_raw{};
    std::array<std::uint16_t, mors::MAX_MOVES> reference_raw{};
    std::array<std::uint16_t, mors::MAX_MOVES> noisy_raw{};
    std::array<std::uint16_t, mors::MAX_MOVES> expected_noisy_raw{};
    std::size_t reference_size = 0;
    std::size_t expected_noisy_size = 0;

    for (std::size_t index = 0; index < actual.size(); ++index) {
        actual_raw[index] = actual[index].raw();
        const mors::Move move = actual[index];
        const bool is_noisy = move.type() == mors::PROMOTION
                           || move.type() == mors::EN_PASSANT
                           || position.piece_on(move.to()) != mors::NO_PIECE;
        if (is_noisy)
            expected_noisy_raw[expected_noisy_size++] = move.raw();
    }
    for (std::size_t index = 0; index < noisy.size(); ++index)
        noisy_raw[index] = noisy[index].raw();

    const mors::Color us = position.side_to_move();
    const mors::Color them = ~us;
    for (const mors::Move move : pseudo) {
        mors::StateInfo state;
        position.do_move(move, state);
        const bool legal = !position.is_square_attacked(position.king_square(us), them);
        position.undo_move(move, state);

        if (legal)
            reference_raw[reference_size++] = move.raw();
    }

    std::sort(actual_raw.begin(), actual_raw.begin() + actual.size());
    std::sort(reference_raw.begin(), reference_raw.begin() + reference_size);
    if (actual.size() != reference_size
        || !std::equal(actual_raw.begin(), actual_raw.begin() + actual.size(),
                       reference_raw.begin())) {
        std::cerr << "FAIL movegen cross-check: " << position.fen()
                  << " direct=" << actual.size()
                  << " reference=" << reference_size << '\n';
        return false;
    }

    std::sort(noisy_raw.begin(), noisy_raw.begin() + noisy.size());
    std::sort(
        expected_noisy_raw.begin(),
        expected_noisy_raw.begin() + expected_noisy_size
    );
    if (noisy.size() != expected_noisy_size
        || !std::equal(
            noisy_raw.begin(),
            noisy_raw.begin() + noisy.size(),
            expected_noisy_raw.begin()
        )) {
        std::cerr << "FAIL noisy movegen cross-check: " << position.fen()
                  << " noisy=" << noisy.size()
                  << " expected=" << expected_noisy_size << '\n';
        return false;
    }

    if (depth == 0)
        return true;

    for (const mors::Move move : actual) {
        mors::StateInfo state;
        position.do_move(move, state);
        const bool passed = cross_check_legal_moves(position, depth - 1);
        position.undo_move(move, state);
        if (!passed)
            return false;
    }
    return true;
}

bool run_movegen_cross_checks() {
    struct CrossCheckCase final {
        std::string_view fen;
        int depth;
    };

    constexpr std::array<CrossCheckCase, 4> CASES = {{
        {mors::START_FEN, 2},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10", 1},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", 2},
        {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", 1}
    }};

    for (const CrossCheckCase& test : CASES) {
        auto parsed = mors::Position::from_fen(test.fen);
        if (!parsed || !cross_check_legal_moves(*parsed, test.depth))
            return false;
    }

    std::cout << "PASS movegen cross-check\n";
    return true;
}

} // namespace

bool run_attacks_tests();
bool run_zobrist_tests();
bool run_score_tests();
bool run_see_tests();
bool run_tt_tests();
bool run_nnue_tests();
bool run_search_tests();
bool run_time_tests();
bool run_uci_tests();

int main() {
    mors::initialize_attacks();

    constexpr std::array<PerftCase, 5> CASES = {{
        {
            "start position",
            mors::START_FEN,
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
    passed &= run_zobrist_tests();
    passed &= run_score_tests();
    passed &= run_see_tests();
    passed &= run_tt_tests();
    passed &= run_nnue_tests();
    passed &= run_search_tests();
    passed &= run_time_tests();
    passed &= run_uci_tests();
    passed &= run_movegen_cross_checks();
    for (const PerftCase& test : CASES)
        passed &= run_case(test);

    return passed ? 0 : 1;
}
