// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "syzygy.hpp"
#include "chess/movegen.hpp"
#include "chess/position.hpp"
#include "../../vendor/fathom/tbprobe.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <string>

namespace mors::syzygy {
namespace {
bool probeable(const Position& p, const Options& options) noexcept {
    const int limit = effective_limit(options);
    return limit > 0 && p.castling_rights() == NO_CASTLING
        && std::popcount(p.pieces()) <= limit;
}
unsigned ep(const Position& p) noexcept {
    return p.ep_square() == SQ_NONE ? 0U : static_cast<unsigned>(p.ep_square());
}
PieceType promotion(unsigned value) noexcept {
    switch (value) {
    case TB_PROMOTES_QUEEN: return QUEEN;
    case TB_PROMOTES_ROOK: return ROOK;
    case TB_PROMOTES_BISHOP: return BISHOP;
    case TB_PROMOTES_KNIGHT: return KNIGHT;
    default: return NO_PIECE_TYPE;
    }
}
Move match(TbMove tb, const MoveList& legal) noexcept {
    for (Move m : legal) {
        if (static_cast<unsigned>(m.from()) != TB_MOVE_FROM(tb)
            || static_cast<unsigned>(m.to()) != TB_MOVE_TO(tb)) continue;
        const PieceType promoted = m.type() == PROMOTION ? m.promotion_type() : NO_PIECE_TYPE;
        if (promoted == promotion(TB_MOVE_PROMOTES(tb))) return m;
    }
    return {};
}
} // namespace

bool init(std::string_view path) { return tb_init(std::string(path).c_str()); }
void shutdown() noexcept { tb_free(); }
int max_pieces() noexcept { return static_cast<int>(TB_LARGEST); }
int effective_limit(const Options& options) noexcept {
    return std::min(std::clamp(options.probe_limit, 0, 7), max_pieces());
}

std::optional<Wdl> probe_wdl(const Position& p, const Options& options) noexcept {
    // WDL probing assumes a freshly reset fifty-move counter.
    if (!probeable(p, options) || (options.rule50 && p.halfmove_clock() != 0)) return {};
    const unsigned value = tb_probe_wdl(p.pieces(WHITE), p.pieces(BLACK),
        p.pieces(KING), p.pieces(QUEEN), p.pieces(ROOK), p.pieces(BISHOP),
        p.pieces(KNIGHT), p.pieces(PAWN), 0, 0, ep(p), p.side_to_move() == WHITE);
    if (value == TB_RESULT_FAILED) return {};
    return static_cast<Wdl>(static_cast<int>(value) - 2);
}

std::optional<RootProbe> probe_root(const Position& p, const Options& options, bool repeated) noexcept {
    if (!probeable(p, options) || (options.rule50 && p.halfmove_clock() >= 100)) return {};
    TbRootMoves ranked{};
    int success = tb_probe_root_dtz(p.pieces(WHITE), p.pieces(BLACK),
        p.pieces(KING), p.pieces(QUEEN), p.pieces(ROOK), p.pieces(BISHOP),
        p.pieces(KNIGHT), p.pieces(PAWN), options.rule50 ? p.halfmove_clock() : 0,
        0, ep(p), p.side_to_move() == WHITE, repeated, options.rule50, &ranked);
    const bool used_dtz = success != 0;
    if (!success) {
        // WDL alone cannot guarantee the result with an advanced rule50 clock.
        if (options.rule50 && p.halfmove_clock() != 0) return {};
        success = tb_probe_root_wdl(p.pieces(WHITE), p.pieces(BLACK),
            p.pieces(KING), p.pieces(QUEEN), p.pieces(ROOK), p.pieces(BISHOP),
            p.pieces(KNIGHT), p.pieces(PAWN), 0, 0, ep(p), p.side_to_move() == WHITE,
            options.rule50, &ranked);
    }
    if (!success || ranked.size == 0) return {};
    MoveList legal;
    Position copy = p;
    generate_legal(copy, legal);
    if (ranked.size != legal.size()) return {};
    int best_rank = std::numeric_limits<int>::min();
    for (unsigned i = 0; i < ranked.size; ++i) {
        if (match(ranked.moves[i].move, legal).is_none()) return {};
        best_rank = std::max(best_rank, ranked.moves[i].tbRank);
    }
    RootProbe result;
    result.used_dtz = used_dtz;
    for (unsigned i = 0; i < ranked.size; ++i)
        if (ranked.moves[i].tbRank == best_rank)
            result.moves.push(match(ranked.moves[i].move, legal));
    const int decisive = options.rule50 ? 900 : 1;
    result.wdl = best_rank >= decisive ? Wdl::Win
        : best_rank <= -decisive ? Wdl::Loss
        : best_rank > 0 ? Wdl::CursedWin
        : best_rank < 0 ? Wdl::BlessedLoss : Wdl::Draw;
    return result;
}

Value score(Wdl wdl, int ply, bool rule50) noexcept {
    const int value = static_cast<int>(wdl);
    const int threshold = rule50 ? 1 : 0;
    if (value > threshold) return VALUE_TB - ply;
    if (value < -threshold) return -VALUE_TB + ply;
    return VALUE_DRAW;
}
} // namespace mors::syzygy
