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
                           || (move.type() != mors::CASTLING
                               && position.piece_on(move.to()) != mors::NO_PIECE);
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

bool expect_chess960(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL Chess960: " << message << '\n';
    return condition;
}

bool has_castling_move(
    mors::Position& position,
    mors::Square king_from,
    mors::Square rook_from
) {
    mors::MoveList moves;
    mors::generate_legal(position, moves);
    return std::find(
        moves.begin(),
        moves.end(),
        mors::Move::castling(king_from, rook_from)
    ) != moves.end();
}

bool run_chess960_tests() {
    using namespace mors;

    bool passed = true;
    auto shredder = Position::from_fen(
        "bbqnnrkr/pppppppp/8/8/8/8/PPPPPPPP/BBQNNRKR w HFhf - 0 1",
        true
    );
    passed &= expect_chess960(shredder.has_value(), "Shredder-FEN must parse");
    if (!shredder)
        return false;
    passed &= expect_chess960(
        shredder->castling_rights() == ANY_CASTLING,
        "Shredder-FEN rights"
    );
    passed &= expect_chess960(
        shredder->castling_rook_square(WHITE_KING_SIDE) == H1
            && shredder->castling_rook_square(WHITE_QUEEN_SIDE) == F1
            && shredder->castling_rook_square(BLACK_KING_SIDE) == H8
            && shredder->castling_rook_square(BLACK_QUEEN_SIDE) == F8,
        "dynamic rook origins"
    );
    passed &= expect_chess960(
        shredder->fen() ==
            "bbqnnrkr/pppppppp/8/8/8/8/PPPPPPPP/BBQNNRKR w HFhf - 0 1",
        "Shredder-FEN round trip"
    );

    auto xfen = Position::from_fen(
        "nqbnrkrb/pppppppp/8/8/8/8/PPPPPPPP/NQBNRKRB w KQkq - 0 1",
        true
    );
    passed &= expect_chess960(
        xfen && xfen->castling_rook_square(WHITE_KING_SIDE) == G1
             && xfen->castling_rook_square(WHITE_QUEEN_SIDE) == E1
             && xfen->fen().contains(" w GEge "),
        "X-FEN rook discovery"
    );

    passed &= expect_chess960(
        !Position::from_fen("4k3/8/8/8/8/8/8/4K3 w H - 0 1", true),
        "missing castling rook must be rejected"
    );
    passed &= expect_chess960(
        !Position::from_fen("4k3/8/8/8/8/8/8/4K2R w HH - 0 1", true),
        "duplicate castling side must be rejected"
    );

    struct CastleCase final {
        std::string_view fen;
        Square king_from;
        Square rook_from;
        Square king_to;
        Square rook_to;
    };
    constexpr std::array<CastleCase, 6> CASTLES{{
        {"4k3/8/8/8/8/8/8/R5KR w AH - 0 1", G1, H1, G1, F1},
        {"4k3/8/8/8/8/8/8/4KR2 w F - 0 1", E1, F1, G1, F1},
        {"4k3/8/8/8/8/8/8/4K1R1 w G - 0 1", E1, G1, G1, F1},
        {"4k3/8/8/8/8/8/8/5K1R w H - 0 1", F1, H1, G1, F1},
        {"4k3/8/8/8/8/8/8/2RK4 w C - 0 1", D1, C1, C1, D1},
        {"4k3/8/8/8/8/8/8/3RK3 w D - 0 1", E1, D1, C1, D1}
    }};

    for (const CastleCase& test : CASTLES) {
        auto parsed = Position::from_fen(test.fen, true);
        if (!expect_chess960(parsed.has_value(), "edge castle FEN"))
            return false;
        Position& position = *parsed;
        const std::string before_fen = position.fen();
        const Key before_key = position.key();
        const Move move = Move::castling(test.king_from, test.rook_from);
        passed &= expect_chess960(
            has_castling_move(position, test.king_from, test.rook_from),
            "edge castle must be generated"
        );
        StateInfo state;
        position.do_move(move, state);
        passed &= expect_chess960(
            position.piece_on(test.king_to) == W_KING
                && position.piece_on(test.rook_to) == W_ROOK
                && position.castling_rights() == NO_CASTLING
                && position.is_consistent(),
            "edge castle make"
        );
        position.undo_move(move, state);
        passed &= expect_chess960(
            position.fen() == before_fen
                && position.key() == before_key
                && position.is_consistent(),
            "edge castle unmake"
        );
    }

    auto rook_blocker =
        Position::from_fen("4k3/8/8/8/8/8/8/rR4K1 w B - 0 1", true);
    passed &= expect_chess960(
        rook_blocker && !has_castling_move(*rook_blocker, G1, B1),
        "rook movement must not expose the king"
    );

    auto dynamic_rights =
        Position::from_fen("4k3/8/8/8/8/8/8/1R2K1R1 w BG - 0 1", true);
    if (!expect_chess960(dynamic_rights.has_value(), "dynamic rights FEN"))
        return false;
    const std::string rights_fen = dynamic_rights->fen();
    const Key rights_key = dynamic_rights->key();
    StateInfo rights_state;
    dynamic_rights->do_move(Move::normal(G1, G2), rights_state);
    passed &= expect_chess960(
        dynamic_rights->castling_rights() == WHITE_QUEEN_SIDE
            && dynamic_rights->is_consistent(),
        "moving a dynamic rook must clear only its right"
    );
    dynamic_rights->undo_move(Move::normal(G1, G2), rights_state);
    passed &= expect_chess960(
        dynamic_rights->fen() == rights_fen
            && dynamic_rights->key() == rights_key,
        "dynamic rook right must restore on unmake"
    );

    auto captured_right =
        Position::from_fen("1r2k3/8/8/8/8/8/8/1R2K1R1 b BG - 0 1", true);
    if (!expect_chess960(captured_right.has_value(), "captured right FEN"))
        return false;
    StateInfo capture_state;
    captured_right->do_move(Move::normal(B8, B1), capture_state);
    passed &= expect_chess960(
        captured_right->castling_rights() == WHITE_KING_SIDE
            && captured_right->is_consistent(),
        "capturing a dynamic rook must clear its right"
    );
    captured_right->undo_move(Move::normal(B8, B1), capture_state);

    auto black_stationary =
        Position::from_fen("r5kr/8/8/8/8/8/8/4K3 b ah - 0 1", true);
    passed &= expect_chess960(
        black_stationary && has_castling_move(*black_stationary, G8, H8),
        "black stationary-king castle must be generated"
    );


    auto first_rook =
        Position::from_fen("4k3/8/8/8/8/8/8/4KR1R w F - 0 1", true);
    auto second_rook =
        Position::from_fen("4k3/8/8/8/8/8/8/4KR1R w H - 0 1", true);
    passed &= expect_chess960(
        first_rook && second_rook && first_rook->key() != second_rook->key(),
        "Zobrist key must include the castling rook origin"
    );

    const auto expect_perft = [&passed](
        std::string_view fen,
        bool chess960,
        int depth,
        std::uint64_t expected,
        const char* message
    ) {
        auto parsed = Position::from_fen(fen, chess960);
        const bool current = parsed && perft(*parsed, depth) == expected;
        passed &= expect_chess960(current, message);
    };
    expect_perft(
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        false, 4, 314'346, "classical castling perft d4"
    );
    expect_perft(
        "4k3/8/8/8/8/8/8/R5KR w AH - 0 1",
        true, 4, 16'219, "stationary-king perft d4"
    );
    expect_perft(
        "4k3/8/8/8/8/8/8/2RK4 w C - 0 1",
        true, 4, 6'173, "king-rook-swap perft d4"
    );
    expect_perft(
        "nqbnrkrb/pppppppp/8/8/8/8/PPPPPPPP/NQBNRKRB w KQkq - 0 1",
        true, 3, 8'934, "X-FEN benchmark perft d3"
    );

    if (passed)
        std::cout << "PASS Chess960 compatibility\n";
    return passed;
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
bool run_datagen_tests();

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
    passed &= run_datagen_tests();
    passed &= run_chess960_tests();
    passed &= run_movegen_cross_checks();
    for (const PerftCase& test : CASES)
        passed &= run_case(test);

    return passed ? 0 : 1;
}
