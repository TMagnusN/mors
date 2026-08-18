// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/movegen.hpp"
#include "chess/zobrist.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

bool verify_incremental_keys(std::string_view fen, mors::MoveType required_type) {
    auto parsed = mors::Position::from_fen(fen);
    if (!parsed) {
        std::cerr << "FAIL zobrist: invalid test FEN: " << parsed.error() << '\n';
        return false;
    }

    mors::Position position = std::move(*parsed);
    const mors::Key original_key = position.key();
    const std::string original_fen = position.fen();
    mors::MoveList moves;
    mors::generate_legal(position, moves);

    bool found_required_type = false;
    for (const mors::Move move : moves) {
        found_required_type |= move.type() == required_type;
        mors::StateInfo state;
        position.do_move(move, state);

        auto rebuilt = mors::Position::from_fen(position.fen());
        if (!rebuilt || rebuilt->key() != position.key()) {
            std::cerr << "FAIL zobrist: incremental key differs from full rebuild\n";
            return false;
        }

        position.undo_move(move, state);
        if (position.key() != original_key || position.fen() != original_fen) {
            std::cerr << "FAIL zobrist: undo did not restore key and position\n";
            return false;
        }
    }

    if (!found_required_type) {
        std::cerr << "FAIL zobrist: required move type was not generated\n";
        return false;
    }
    return true;
}

} // namespace

bool run_zobrist_tests() {
    using namespace mors;
    static_assert(zobrist::SEED == Key{114514});

    constexpr std::array<std::pair<std::string_view, MoveType>, 4> CASES = {{
        {START_FEN, NORMAL},
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", CASTLING},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", EN_PASSANT},
        {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", PROMOTION}
    }};

    for (const auto& [fen, required_type] : CASES) {
        if (!verify_incremental_keys(fen, required_type))
            return false;
    }

    auto white = Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    auto black = Position::from_fen("4k3/8/8/8/8/8/8/4K3 b - - 0 1");
    auto clocks = Position::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 99 42");
    if (!white || !black || !clocks
        || white->key() == black->key()
        || white->key() != clocks->key()) {
        std::cerr << "FAIL zobrist: side/clocks key semantics\n";
        return false;
    }

    std::cout << "PASS zobrist\n";
    return true;
}
