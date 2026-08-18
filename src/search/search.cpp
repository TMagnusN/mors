// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "search.hpp"

#include "chess/movegen.hpp"
#include "eval/nnue/network.hpp"
#include "eval/nnue/worker.hpp"
#include "score.hpp"
#include "see.hpp"
#include "tt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mors {
namespace {

inline constexpr int TT_MOVE_SCORE = 1'000'000;
inline constexpr int GOOD_NOISY_SCORE = 100'000;
inline constexpr int BAD_NOISY_SCORE = -100'000;
inline constexpr Value INITIAL_ASPIRATION_DELTA = 16;
inline constexpr Depth RFP_MAX_DEPTH = 6;
inline constexpr Value RFP_MARGIN_PER_DEPTH = 100;
inline constexpr Value QS_SEE_MARGIN = 74;
inline constexpr Value QS_SEE_GAP_DIVISOR = 8;
inline constexpr std::size_t QS_LMP_MOVE_LIMIT = 2;
inline constexpr Bitboard ONE_SQUARE_COLOR = 0xAA55'AA55'AA55'AA55ULL;

struct PvTable final {
    std::array<std::array<Move, MAX_PLY>, MAX_PLY + 1> moves{};
    std::array<std::size_t, MAX_PLY + 1> lengths{};
};

struct Context final {
    Context(
        Position& current_position,
        TranspositionTable& current_table,
        const nnue::Network& current_network,
        const SearchLimits& current_limits
    )
        : position(current_position),
          table(current_table),
          network(current_network),
          limits(current_limits),
          pv(std::make_unique<PvTable>()) {
        keys.reserve(current_limits.prior_keys.size() + MAX_PLY + 1);
        keys.insert(
            keys.end(),
            current_limits.prior_keys.begin(),
            current_limits.prior_keys.end()
        );
        root_key_index = keys.size();
        keys.push_back(position.key());
        evaluator.reset();
    }

    Position& position;
    TranspositionTable& table;
    const nnue::Network& network;
    const SearchLimits& limits;
    nnue::Worker evaluator;
    std::unique_ptr<PvTable> pv;
    std::vector<Key> keys;
    std::size_t root_key_index = 0;
    SearchStats stats{};
    bool stopped = false;
    std::chrono::steady_clock::time_point soft_deadline =
        limits.start_time + limits.soft_time;
    std::chrono::steady_clock::time_point hard_deadline =
        limits.start_time + limits.hard_time;
};

class MoveGuard final {
public:
    MoveGuard(Context& context, Move move) noexcept
        : context_(context), move_(move) {
        context_.evaluator.push(context_.position, move_);
        context_.position.do_move(move_, state_);
        context_.keys.push_back(context_.position.key());
        context_.table.prefetch(context_.position.key());
    }

    ~MoveGuard() {
        context_.keys.pop_back();
        context_.position.undo_move(move_, state_);
        context_.evaluator.pop();
    }

    MoveGuard(const MoveGuard&) = delete;
    MoveGuard& operator=(const MoveGuard&) = delete;

private:
    Context& context_;
    Move move_;
    StateInfo state_{};
};

[[nodiscard]] bool begin_node(Context& context, int ply, bool qnode) noexcept {
    assert(ply >= 0 && ply <= MAX_PLY);
    if (context.limits.stop != nullptr
        && context.limits.stop->load(std::memory_order_relaxed)) {
        context.stopped = true;
        return false;
    }
    if (context.stats.nodes >= context.limits.max_nodes) {
        context.stopped = true;
        return false;
    }
    // Clock reads are substantially more expensive than an atomic stop flag.
    // Poll the hard deadline every 64 nodes; the root of a fresh search is
    // always checked because the node counter starts at zero.
    if (context.limits.hard_time.count() > 0
        && (context.stats.nodes & 63U) == 0
        && std::chrono::steady_clock::now() >= context.hard_deadline) {
        context.stopped = true;
        return false;
    }

    ++context.stats.nodes;
    if (qnode)
        ++context.stats.qnodes;
    context.stats.seldepth = std::max(context.stats.seldepth, ply);
    return true;
}

[[nodiscard]] bool in_check(const Position& position) noexcept {
    const Color side = position.side_to_move();
    return position.is_square_attacked(
        position.king_square(side),
        ~side,
        position.pieces()
    );
}

[[nodiscard]] bool has_insufficient_material(const Position& position) noexcept {
    if ((position.pieces(PAWN)
       | position.pieces(ROOK)
       | position.pieces(QUEEN)) != EMPTY_BB) {
        return false;
    }

    const Bitboard knights = position.pieces(KNIGHT);
    const Bitboard bishops = position.pieces(BISHOP);
    const int minor_count = std::popcount(knights | bishops);
    if (minor_count <= 1)
        return true;

    // Any number of bishops confined to one square color cannot mate.
    return knights == EMPTY_BB
        && ((bishops & ONE_SQUARE_COLOR) == EMPTY_BB
            || (bishops & ~ONE_SQUARE_COLOR) == EMPTY_BB);
}

[[nodiscard]] bool is_repetition(const Context& context) noexcept {
    assert(!context.keys.empty());
    const Key current = context.keys.back();
    const std::size_t current_index = context.keys.size() - 1;
    const std::size_t reversible = static_cast<std::size_t>(
        context.position.halfmove_clock()
    );
    const std::size_t first = current_index > reversible
        ? current_index - reversible
        : 0;

    int matches_before_root = 0;
    for (std::size_t index = current_index; index-- > first;) {
        if (context.keys[index] != current)
            continue;

        // A cycle created inside this search is enough to terminate the line.
        if (index >= context.root_key_index)
            return true;

        // Before the root, require two earlier occurrences for threefold.
        if (++matches_before_root >= 2)
            return true;
    }
    return false;
}

[[nodiscard]] bool is_draw(const Context& context, int ply) noexcept {
    if (ply == 0)
        return false;
    return context.position.halfmove_clock() >= 100
        || has_insufficient_material(context.position)
        || is_repetition(context);
}

[[nodiscard]] bool is_capture(const Position& position, Move move) noexcept {
    return move.type() == EN_PASSANT
        || (move.type() != CASTLING
            && position.piece_on(move.to()) != NO_PIECE);
}

[[nodiscard]] bool gives_check(const Position& position, Move move) noexcept {
    const Color us = position.side_to_move();
    const Square from = move.from();
    const Square to = move.to();
    const Piece moving_piece = position.piece_on(from);
    assert(is_ok(moving_piece) && color_of(moving_piece) == us);

    Bitboard occupied = position.pieces();
    clear_square(occupied, from);
    if (move.type() == EN_PASSANT)
        clear_square(occupied, to - pawn_push(us));
    set_square(occupied, to);

    Bitboard pawns = position.pieces(us, PAWN);
    Bitboard knights = position.pieces(us, KNIGHT);
    Bitboard bishops = position.pieces(us, BISHOP);
    Bitboard rooks = position.pieces(us, ROOK);
    Bitboard queens = position.pieces(us, QUEEN);
    Bitboard kings = position.pieces(us, KING);

    const Bitboard from_bb = square_bb(from);
    switch (type_of(moving_piece)) {
    case PAWN:   pawns &= ~from_bb; break;
    case KNIGHT: knights &= ~from_bb; break;
    case BISHOP: bishops &= ~from_bb; break;
    case ROOK:   rooks &= ~from_bb; break;
    case QUEEN:  queens &= ~from_bb; break;
    case KING:   kings &= ~from_bb; break;
    default: assert(false); break;
    }

    const PieceType destination_type = move.type() == PROMOTION
        ? move.promotion_type()
        : type_of(moving_piece);
    switch (destination_type) {
    case PAWN:   set_square(pawns, to); break;
    case KNIGHT: set_square(knights, to); break;
    case BISHOP: set_square(bishops, to); break;
    case ROOK:   set_square(rooks, to); break;
    case QUEEN:  set_square(queens, to); break;
    case KING:   set_square(kings, to); break;
    default: assert(false); break;
    }

    if (move.type() == CASTLING) {
        const bool king_side = file_of(to) == FILE_G;
        const Square rook_from = relative_square(us, king_side ? H1 : A1);
        const Square rook_to = relative_square(us, king_side ? F1 : D1);
        clear_square(rooks, rook_from);
        set_square(rooks, rook_to);
        clear_square(occupied, rook_from);
        set_square(occupied, rook_to);
    }

    const Square king = position.king_square(~us);
    const SliderAttacks sliders = slider_attacks(king, occupied);
    const bool checking = (pawn_attacks(~us, king) & pawns)
        || (knight_attacks(king) & knights)
        || (sliders.bishop & (bishops | queens))
        || (sliders.rook & (rooks | queens))
        || (king_attacks(king) & kings);

#ifndef NDEBUG
    Position reference = position;
    StateInfo state;
    reference.do_move(move, state);
    assert(checking == in_check(reference));
#endif

    return checking;
}

[[nodiscard]] int move_order_score(
    const Position& position,
    Move move,
    Move tt_move
) noexcept {
    if (!tt_move.is_none() && move == tt_move)
        return TT_MOVE_SCORE;

    const bool capture = is_capture(position, move);
    const bool promotion = move.type() == PROMOTION;
    if (!capture && !promotion)
        return 0;

    const Piece moving_piece = position.piece_on(move.from());
    int victim_value = 0;
    if (move.type() == EN_PASSANT) {
        victim_value = see_piece_value(PAWN);
    } else {
        const Piece captured = position.piece_on(move.to());
        if (captured != NO_PIECE)
            victim_value = see_piece_value(type_of(captured));
    }

    int promotion_gain = 0;
    if (promotion) {
        promotion_gain = see_piece_value(move.promotion_type())
                       - see_piece_value(PAWN);
    }

    const int material_order = 16 * (victim_value + promotion_gain)
                             - see_piece_value(type_of(moving_piece));
    return (see_ge(position, move, 0) ? GOOD_NOISY_SCORE : BAD_NOISY_SCORE)
         + material_order;
}

void order_moves(
    const Position& position,
    MoveList& moves,
    Move tt_move = {},
    std::array<int, MAX_MOVES>* ordered_scores = nullptr
) noexcept {
    struct ScoredMove final {
        Move move;
        int score;
    };

    std::array<ScoredMove, MAX_MOVES> scored{};
    for (std::size_t index = 0; index < moves.size(); ++index) {
        scored[index] = {
            .move = moves[index],
            .score = move_order_score(position, moves[index], tt_move)
        };
    }
    // Move lists are small. Stable insertion sort avoids allocating from the
    // recursive search hot path and preserves movegen order for equal scores.
    for (std::size_t index = 1; index < moves.size(); ++index) {
        const ScoredMove item = scored[index];
        std::size_t destination = index;
        while (destination > 0
               && item.score > scored[destination - 1].score) {
            scored[destination] = scored[destination - 1];
            --destination;
        }
        scored[destination] = item;
    }
    std::transform(
        scored.begin(),
        scored.begin() + static_cast<std::ptrdiff_t>(moves.size()),
        moves.begin(),
        [](const ScoredMove& item) noexcept { return item.move; }
    );
    if (ordered_scores != nullptr) {
        std::transform(
            scored.begin(),
            scored.begin() + static_cast<std::ptrdiff_t>(moves.size()),
            ordered_scores->begin(),
            [](const ScoredMove& item) noexcept { return item.score; }
        );
    }
}

[[nodiscard]] bool contains_move(const MoveList& moves, Move move) noexcept {
    return move.is_none()
        || std::find(moves.begin(), moves.end(), move) != moves.end();
}

void clear_pv(Context& context, int ply) noexcept {
    assert(ply >= 0 && ply <= MAX_PLY);
    context.pv->lengths[static_cast<std::size_t>(ply)] =
        static_cast<std::size_t>(ply);
}

void update_pv(Context& context, int ply, Move move) noexcept {
    assert(ply >= 0 && ply < MAX_PLY);
    const std::size_t row = static_cast<std::size_t>(ply);
    const std::size_t child = row + 1;
    context.pv->moves[row][row] = move;

    const std::size_t child_length = context.pv->lengths[child];
    assert(child_length >= child && child_length <= MAX_PLY);
    std::copy(
        context.pv->moves[child].begin() + static_cast<std::ptrdiff_t>(child),
        context.pv->moves[child].begin()
            + static_cast<std::ptrdiff_t>(child_length),
        context.pv->moves[row].begin() + static_cast<std::ptrdiff_t>(child)
    );
    context.pv->lengths[row] = child_length;
}

[[nodiscard]] Value qsearch(
    Context& context,
    Value alpha,
    Value beta,
    int ply
) noexcept;

[[nodiscard]] Value pvs(
    Context& context,
    Depth depth,
    Value alpha,
    Value beta,
    int ply,
    bool pv_node
) noexcept {
    assert(depth >= 0);
    assert(alpha < beta);
    assert(ply >= 0 && ply <= MAX_PLY);

    if (depth == 0)
        return qsearch(context, alpha, beta, ply);

    clear_pv(context, ply);

    if (!begin_node(context, ply, false))
        return VALUE_NONE;

    const bool checked = in_check(context.position);
    MoveList moves;
    generate_legal(context.position, moves);

    if (moves.empty())
        return checked ? mated_in(ply) : VALUE_DRAW;
    if (is_draw(context, ply))
        return VALUE_DRAW;
    if (ply == MAX_PLY)
        return context.evaluator.evaluate(context.position, context.network);

    alpha = std::max(alpha, mated_in(ply));
    beta = std::min(beta, mate_in(ply + 1));
    if (alpha >= beta)
        return alpha;
    const Value original_alpha = alpha;

    TTProbe probe = context.table.probe(context.position.key());
    bool tt_hit = probe.hit;
    Move tt_move{};
    if (tt_hit) {
        ++context.stats.tt_hits;
        if (!contains_move(moves, probe.data.move)) {
            tt_hit = false;
        } else {
            tt_move = probe.data.move;
            const Value tt_value = value_from_tt(
                probe.data.value,
                ply,
                context.position.halfmove_clock()
            );
            const bool depth_ok = probe.data.depth >= depth;
            const bool bound_ok =
                   probe.data.bound == BOUND_EXACT
                || (probe.data.bound == BOUND_LOWER && tt_value >= beta)
                || (probe.data.bound == BOUND_UPPER && tt_value <= alpha);

            if (!pv_node
                && tt_value != VALUE_NONE
                && depth_ok
                && bound_ok) {
                ++context.stats.tt_cutoffs;
                return tt_value;
            }
        }
    }

    Value raw_static_eval = VALUE_NONE;
    if (!checked) {
        if (tt_hit && is_eval_value(probe.data.static_eval)) {
            raw_static_eval = probe.data.static_eval;
            ++context.stats.static_eval_cache_hits;
        } else {
            raw_static_eval = context.evaluator.evaluate(
                context.position,
                context.network
            );
            assert(is_eval_value(raw_static_eval));

            // Populate an evaluation-only entry on a true miss. Do not replace
            // an existing searched entry merely because it predates cached
            // static evaluations; the normal node write below will refresh it.
            if (!probe.hit) {
                probe.writer.write({
                    .move = {},
                    .value = VALUE_NONE,
                    .static_eval = raw_static_eval,
                    .depth = DEPTH_UNSEARCHED,
                    .bound = BOUND_NONE,
                    .pv = false
                });
            }
        }

        // Reverse futility pruning is restricted to ordinary non-PV windows.
        // A fail-hard beta return avoids presenting the heuristic estimate as
        // an exact score to the parent.
        const std::int64_t rfp_margin =
            static_cast<std::int64_t>(RFP_MARGIN_PER_DEPTH) * depth;
        const std::int64_t rfp_threshold =
            static_cast<std::int64_t>(beta) + rfp_margin;
        if (!pv_node
            && depth <= RFP_MAX_DEPTH
            && is_eval_value(beta)
            && rfp_threshold <= VALUE_EVAL_MAX
            && raw_static_eval >= rfp_threshold) {
            ++context.stats.rfp_cutoffs;
            return beta;
        }
    }

    order_moves(context.position, moves, tt_move);

    Value best_value = -VALUE_INFINITE;
    Move best_move{};
    std::size_t move_count = 0;

    for (const Move move : moves) {
        Value score = VALUE_NONE;
        {
            MoveGuard guard(context, move);

            if (move_count == 0) {
                const Value child = pvs(
                    context,
                    depth - 1,
                    -beta,
                    -alpha,
                    ply + 1,
                    pv_node
                );
                if (child == VALUE_NONE)
                    return VALUE_NONE;
                score = -child;
            } else {
                const Value probe_value = pvs(
                    context,
                    depth - 1,
                    -alpha - 1,
                    -alpha,
                    ply + 1,
                    false
                );
                if (probe_value == VALUE_NONE)
                    return VALUE_NONE;
                score = -probe_value;

                if (score > alpha && score < beta) {
                    ++context.stats.pvs_researches;
                    const Value child = pvs(
                        context,
                        depth - 1,
                        -beta,
                        -alpha,
                        ply + 1,
                        pv_node
                    );
                    if (child == VALUE_NONE)
                        return VALUE_NONE;
                    score = -child;
                }
            }
        }
        ++move_count;

        if (score > best_value) {
            best_value = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            update_pv(context, ply, move);
        }
        if (alpha >= beta)
            break;
    }

    assert(best_value != -VALUE_INFINITE && !best_move.is_none());
    const Bound bound = best_value >= beta                 ? BOUND_LOWER
                      : best_value <= original_alpha       ? BOUND_UPPER
                                                          : BOUND_EXACT;
    probe.writer.write({
        .move = best_move,
        .value = value_to_tt(best_value, ply),
        .static_eval = raw_static_eval,
        .depth = depth,
        .bound = bound,
        .pv = pv_node
    });
    return best_value;
}

[[nodiscard]] Value qsearch(
    Context& context,
    Value alpha,
    Value beta,
    int ply
) noexcept {
    assert(alpha < beta);
    assert(ply >= 0 && ply <= MAX_PLY);
    clear_pv(context, ply);

    if (!begin_node(context, ply, true))
        return VALUE_NONE;

    const bool checked = in_check(context.position);
    MoveList moves;
    generate_legal(context.position, moves);

    if (moves.empty())
        return checked ? mated_in(ply) : VALUE_DRAW;
    if (is_draw(context, ply))
        return VALUE_DRAW;
    if (ply == MAX_PLY)
        return context.evaluator.evaluate(context.position, context.network);

    alpha = std::max(alpha, mated_in(ply));
    beta = std::min(beta, mate_in(ply + 1));
    if (alpha >= beta)
        return alpha;

    Value best_value = -VALUE_INFINITE;
    Value stand_pat = VALUE_NONE;
    if (!checked) {
        stand_pat = context.evaluator.evaluate(
            context.position,
            context.network
        );
        best_value = stand_pat;
        if (stand_pat >= beta)
            return stand_pat;
        alpha = std::max(alpha, stand_pat);
    }

    std::array<int, MAX_MOVES> ordered_scores{};
    order_moves(context.position, moves, {}, &ordered_scores);
    std::size_t noisy_move_count = 0;
    for (std::size_t move_index = 0; move_index < moves.size(); ++move_index) {
        const Move move = moves[move_index];
        if (!checked
            && !is_capture(context.position, move)
            && move.type() != PROMOTION) {
            continue;
        }

        ++noisy_move_count;
        if (!checked) {
            // Reckless-style qsearch LMP: after the first two ordered noisy
            // moves, stop on the first continuation that does not give check.
            // Checked nodes remain exhaustive because MORS does not yet have
            // Reckless's staged evasion picker and search-stack loss state.
            if (noisy_move_count > QS_LMP_MOVE_LIMIT
                && !gives_check(context.position, move)) {
                ++context.stats.qsearch_lmp_prunes;
                break;
            }

            // Convert the score gap to a SEE policy explicitly. The gap is
            // deliberately damped like Reckless rather than
            // demanding that material alone bridge the full alpha gap.
            const SeeValue see_threshold = static_cast<SeeValue>(
                (static_cast<std::int64_t>(alpha)
                 - static_cast<std::int64_t>(stand_pat))
                / QS_SEE_GAP_DIVISOR
                - QS_SEE_MARGIN
            );
            // Ordering already classified every noisy move with SEE >= 0.
            // Reuse that result whenever it proves this looser threshold.
            const bool ordered_good = ordered_scores[move_index] > 0;
            const bool passes_see = (ordered_good && see_threshold <= 0)
                || see_ge(context.position, move, see_threshold);
            if (!passes_see) {
                ++context.stats.qsearch_see_prunes;
                continue;
            }
        }

        Value score = VALUE_NONE;
        {
            MoveGuard guard(context, move);
            const Value child = qsearch(context, -beta, -alpha, ply + 1);
            if (child == VALUE_NONE)
                return VALUE_NONE;
            score = -child;
        }

        if (score > best_value)
            best_value = score;
        if (score > alpha) {
            alpha = score;
            update_pv(context, ply, move);
        }
        if (alpha >= beta)
            return best_value;
    }

    assert(best_value != -VALUE_INFINITE);
    return best_value;
}

} // namespace

SearchResult search(
    Position& position,
    TranspositionTable& table,
    const nnue::Network& network,
    const SearchLimits& limits
) {
    if (!network.valid())
        throw std::invalid_argument("search requires a valid NNUE network");
    if (table.cluster_count() == 0)
        throw std::invalid_argument("search requires a non-empty transposition table");
    if (limits.max_depth < 1 || limits.max_depth > MAX_PLY)
        throw std::invalid_argument("search depth is outside the supported range");

    table.new_search();
    Context context(position, table, network, limits);
    SearchResult result;

    for (Depth depth = 1; depth <= limits.max_depth; ++depth) {
        Value alpha = -VALUE_INFINITE;
        Value beta = VALUE_INFINITE;
        Value delta = INITIAL_ASPIRATION_DELTA;

        if (depth > 1 && result.value != VALUE_NONE) {
            alpha = std::max(-VALUE_INFINITE, result.value - delta);
            beta = std::min(VALUE_INFINITE, result.value + delta);
        }

        Value value = VALUE_NONE;
        while (true) {
            if (depth > 1)
                ++context.stats.aspiration_searches;

            value = pvs(context, depth, alpha, beta, 0, true);
            if (value == VALUE_NONE)
                break;

            if (value <= alpha) {
                ++context.stats.aspiration_researches;
                alpha = std::max(-VALUE_INFINITE, alpha - delta);
            } else if (value >= beta) {
                ++context.stats.aspiration_researches;
                beta = std::min(VALUE_INFINITE, beta + delta);
            } else {
                break;
            }

            // Grow gradually so ordinary score drift keeps a tight root
            // window, while tactical swings still converge quickly.
            delta += delta / 2;
        }
        if (value == VALUE_NONE)
            break;

        result.value = value;
        result.completed_depth = depth;
        result.pv_length = context.pv->lengths[0];
        assert(result.pv_length <= result.principal_variation.size());
        std::copy_n(
            context.pv->moves[0].begin(),
            result.pv_length,
            result.principal_variation.begin()
        );
        result.best_move = result.pv_length != 0
            ? result.principal_variation[0]
            : Move{};

        if (limits.soft_time.count() > 0
            && std::chrono::steady_clock::now() >= context.soft_deadline) {
            context.stopped = true;
            break;
        }
    }

    result.stopped = context.stopped;
    result.stats = context.stats;
    return result;
}

} // namespace mors
