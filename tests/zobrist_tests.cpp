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

    auto dead_ep = Position::from_fen(
        "8/2p5/1k6/2p1rpKp/P6P/2R1P1P1/P7/8 w - h6 0 40"
    );
    auto dead_ep_none = Position::from_fen(
        "8/2p5/1k6/2p1rpKp/P6P/2R1P1P1/P7/8 w - - 0 40"
    );
    auto live_ep = Position::from_fen(
        "4k3/8/8/6Pp/8/8/8/4K3 w - h6 0 1"
    );
    auto live_ep_none = Position::from_fen(
        "4k3/8/8/6Pp/8/8/8/4K3 w - - 0 1"
    );
    if (!dead_ep || !dead_ep_none || !live_ep || !live_ep_none
        || dead_ep->ep_square() != SQ_NONE
        || dead_ep->key() != dead_ep_none->key()
        || live_ep->ep_square() != H6
        || live_ep->key() == live_ep_none->key()) {
        std::cerr << "FAIL zobrist: canonical en-passant FEN key semantics\n";
        return false;
    }

    auto dead_push = Position::from_fen(
        "4k3/7p/8/8/8/8/8/4K3 b - - 0 1"
    );
    auto live_push = Position::from_fen(
        "4k3/7p/8/6P1/8/8/8/4K3 b - - 0 1"
    );
    if (!dead_push || !live_push) {
        std::cerr << "FAIL zobrist: en-passant push FENs must parse\n";
        return false;
    }

    StateInfo dead_state;
    dead_push->do_move(Move::normal(H7, H5), dead_state);
    StateInfo live_state;
    live_push->do_move(Move::normal(H7, H5), live_state);
    if (dead_push->ep_square() != SQ_NONE || live_push->ep_square() != H6) {
        std::cerr << "FAIL zobrist: double push en-passant canonicalization\n";
        return false;
    }

    auto dead_rebuilt = Position::from_fen(dead_push->fen());
    auto live_rebuilt = Position::from_fen(live_push->fen());
    if (!dead_rebuilt || !live_rebuilt
        || dead_rebuilt->key() != dead_push->key()
        || live_rebuilt->key() != live_push->key()) {
        std::cerr << "FAIL zobrist: canonical en-passant incremental key\n";
        return false;
    }

    std::cout << "PASS zobrist\n";
    return true;
}
