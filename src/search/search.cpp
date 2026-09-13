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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <map>
#include <sstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mors {
namespace {

inline constexpr int TT_MOVE_SCORE = 1'000'000;
inline constexpr int GOOD_NOISY_SCORE = 100'000;
inline constexpr int KILLER_MOVE_SCORE = 80'000;
inline constexpr int COUNTER_MOVE_SCORE = 70'000;
inline constexpr int BAD_NOISY_SCORE = -100'000;
inline constexpr Value INITIAL_ASPIRATION_DELTA = 16;
inline constexpr Depth RFP_MAX_DEPTH = 9;
inline constexpr Value RFP_MARGIN_PER_DEPTH = 100;
inline constexpr Depth LMP_MAX_DEPTH = 4;
inline constexpr std::size_t LMP_BASE_MOVE_LIMIT = 3;
inline constexpr Depth FFP_MAX_DEPTH = 8;
inline constexpr Value FFP_MARGIN_PER_DEPTH = 80;
inline constexpr int FFP_HISTORY_MULTIPLIER = 55;
inline constexpr int FFP_HISTORY_DIVISOR = 1'024;
inline constexpr Value FFP_BASE_OFFSET = 40;
inline constexpr int QUIET_HISTORY_MAX = 8'192;
inline constexpr Depth LMR_MIN_DEPTH = 2;
inline constexpr std::size_t LMR_MIN_MOVE_COUNT = 3;
inline constexpr Depth NMP_MIN_DEPTH = 3;
inline constexpr Depth NMP_VERIFICATION_DEPTH = 10;
inline constexpr Value NMP_BASE_EVAL_MARGIN = 220;
inline constexpr Value NMP_MARGIN_PER_DEPTH = 20;
inline constexpr Value NMP_MIN_EVAL_MARGIN = 30;
inline constexpr Value NMP_EVAL_GAP_PER_REDUCTION = 200;
inline constexpr Depth NMP_MAX_GAP_REDUCTION = 3;
inline constexpr Depth IIR_MIN_DEPTH = 4;
inline constexpr Depth SE_MIN_DEPTH = 4;
inline constexpr Depth SE_TT_DEPTH_MARGIN = 2;
inline constexpr Value SE_MARGIN_PER_DEPTH = 2;
inline constexpr Value QS_SEE_MARGIN = 74;
inline constexpr Value QS_SEE_GAP_DIVISOR = 8;
inline constexpr std::size_t QS_LMP_MOVE_LIMIT = 2;
inline constexpr Bitboard ONE_SQUARE_COLOR = 0xAA55'AA55'AA55'AA55ULL;

struct PvTable final {
    std::array<std::array<Move, MAX_PLY>, MAX_PLY + 1> moves{};
    std::array<std::size_t, MAX_PLY + 1> lengths{};
};

using QuietHistory = std::array<
    std::array<std::array<std::int16_t, 64>, 64>,
    COLOR_NB
>;
using KillerMoves = std::array<std::array<Move, 2>, MAX_PLY>;
using CounterMoves = std::array<std::array<Move, 64>, 64>;

using PieceToHistory = std::array<std::array<std::int16_t, SQUARE_NB>, PIECE_NB>;
using ContinuationHistory =
    std::array<std::array<PieceToHistory, SQUARE_NB>, PIECE_NB>;
using NoisyHistory = std::array<
    std::array<std::array<std::int16_t, PIECE_TYPE_NB>, SQUARE_NB>, PIECE_NB>;

// Each pool worker owns one instance across jobs. Position/path state stays
// in Context; learned history is rebuilt only when the pool is cleared/resized.
struct ThreadData final {
    QuietHistory quiet_history{};
    ContinuationHistory continuation_history{};
    NoisyHistory noisy_history{};
};

struct WorkerControl final {
    std::atomic<std::uint64_t>* published_nodes = nullptr;
    std::atomic<std::uint64_t>* shared_node_count = nullptr;
    std::atomic_bool* shared_stop = nullptr;
    std::uint64_t shared_node_limit =
        std::numeric_limits<std::uint64_t>::max();
};

struct Context final {
    Context(
        Position& current_position,
        TranspositionTable& current_table,
        const nnue::Network& current_network,
        ThreadData& current_thread_data,
        const SearchLimits& current_limits,
        WorkerControl* current_worker_control = nullptr
    )
        : position(current_position),
          table(current_table),
          network(current_network),
          thread_data(current_thread_data),
          limits(current_limits),
          worker_control(current_worker_control),
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
    ThreadData& thread_data;
    const SearchLimits& limits;
    WorkerControl* worker_control = nullptr;
    nnue::Worker evaluator;
    std::unique_ptr<PvTable> pv;
    std::vector<Key> keys;
    std::size_t root_key_index = 0;
    std::size_t repetition_floor = 0;
    KillerMoves killer_moves{};
    CounterMoves counter_moves{};
    std::array<Move, MAX_PLY> path_moves{};
    std::array<Piece, MAX_PLY> path_pieces{};
    int nmp_min_ply = 0;
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

class NullMoveGuard final {
public:
    explicit NullMoveGuard(Context& context) noexcept
        : context_(context), old_repetition_floor_(context.repetition_floor) {
        context_.position.do_null_move(state_);
        context_.keys.push_back(context_.position.key());
        context_.repetition_floor = context_.keys.size() - 1;
        context_.table.prefetch(context_.position.key());
    }

    ~NullMoveGuard() {
        context_.keys.pop_back();
        context_.position.undo_null_move(state_);
        context_.repetition_floor = old_repetition_floor_;
    }

    NullMoveGuard(const NullMoveGuard&) = delete;
    NullMoveGuard& operator=(const NullMoveGuard&) = delete;

private:
    Context& context_;
    std::size_t old_repetition_floor_ = 0;
    StateInfo state_{};
};

[[nodiscard]] bool begin_node(Context& context, int ply, bool qnode) noexcept {
    assert(ply >= 0 && ply <= MAX_PLY);
    if (context.limits.stop != nullptr
        && context.limits.stop->load(std::memory_order_relaxed)) {
        context.stopped = true;
        return false;
    }
    if (context.worker_control == nullptr
        && context.stats.nodes >= context.limits.max_nodes) {
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

    if (context.worker_control != nullptr
        && context.worker_control->shared_node_count != nullptr) {
        std::atomic<std::uint64_t>& shared_nodes =
            *context.worker_control->shared_node_count;
        std::uint64_t node_count =
            shared_nodes.load(std::memory_order_relaxed);
        while (node_count < context.worker_control->shared_node_limit
               && !shared_nodes.compare_exchange_weak(
                   node_count,
                   node_count + 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
        if (node_count >= context.worker_control->shared_node_limit) {
            if (context.worker_control->shared_stop != nullptr) {
                context.worker_control->shared_stop->store(
                    true,
                    std::memory_order_release
                );
            }
            context.stopped = true;
            return false;
        }
    }

    ++context.stats.nodes;
    if (context.worker_control != nullptr
        && context.worker_control->published_nodes != nullptr
        && (context.stats.nodes & 63U) == 0) {
        context.worker_control->published_nodes->store(
            context.stats.nodes,
            std::memory_order_relaxed
        );
    }
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

[[nodiscard]] bool has_non_pawn_material(
    const Position& position,
    Color side
) noexcept {
    return (position.pieces(side, KNIGHT)
          | position.pieces(side, BISHOP)
          | position.pieces(side, ROOK)
          | position.pieces(side, QUEEN)) != EMPTY_BB;
}

[[nodiscard]] Value nmp_eval_margin(Depth depth) noexcept {
    return std::max(
        NMP_MIN_EVAL_MARGIN,
        NMP_BASE_EVAL_MARGIN - NMP_MARGIN_PER_DEPTH * depth
    );
}

[[nodiscard]] Depth null_move_reduction(
    Depth depth,
    Value static_eval,
    Value beta
) noexcept {
    assert(depth >= NMP_MIN_DEPTH && static_eval >= beta);
    const Value eval_gap = static_eval - beta;
    const Depth gap_reduction = std::min(
        NMP_MAX_GAP_REDUCTION,
        static_cast<Depth>(eval_gap / NMP_EVAL_GAP_PER_REDUCTION)
    );
    return 3 + depth / 3 + gap_reduction;
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
    const std::size_t first_repetition_index = std::max(
        first,
        context.repetition_floor
    );

    int matches_before_root = 0;
    for (std::size_t index = current_index;
         index-- > first_repetition_index;) {
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

    // Castling changes two pieces and its Chess960 overlaps make an ad-hoc
    // occupancy transform needlessly fragile. It is rare, so use the canonical
    // make-move path as the exact oracle.
    if (move.type() == CASTLING) {
        Position after = position;
        StateInfo state;
        after.do_move(move, state);
        return in_check(after);
    }

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

[[nodiscard]] bool is_killer_move(
    const Context& context,
    Move move,
    int ply
) noexcept {
    const auto& killers = context.killer_moves[static_cast<std::size_t>(ply)];
    return killers[0] == move || killers[1] == move;
}

[[nodiscard]] bool is_counter_move(
    const Context& context,
    Move move,
    int ply
) noexcept {
    if (ply == 0)
        return false;
    const Move previous =
        context.path_moves[static_cast<std::size_t>(ply - 1)];
    return !previous.is_none()
        && context.counter_moves[static_cast<std::size_t>(previous.from())]
                                [static_cast<std::size_t>(previous.to())]
            == move;
}

int see_history(Context& context, Move move, int ply, bool quiet) noexcept;

[[nodiscard]] int move_order_score(
    Context& context,
    Move move,
    Move tt_move,
    int ply
) noexcept {
    const Position& position = context.position;
    if (!tt_move.is_none() && move == tt_move)
        return TT_MOVE_SCORE;

    const bool capture = is_capture(position, move);
    const bool promotion = move.type() == PROMOTION;
    if (!capture && !promotion) {
        if (is_killer_move(context, move, ply))
            return KILLER_MOVE_SCORE;
        if (is_counter_move(context, move, ply))
            return COUNTER_MOVE_SCORE;
        // Reuse butterfly + one- and two-ply continuation history.
        return see_history(context, move, ply, true);
    }

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
         + material_order + see_history(context, move, ply, false);
}

void order_moves(
    Context& context,
    MoveList& moves,
    int ply,
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
            .score = move_order_score(
                context,
                moves[index],
                tt_move,
                ply
            )
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

void update_history_entry(std::int16_t& entry, int bonus) noexcept {
    bonus = std::clamp(bonus, -QUIET_HISTORY_MAX, QUIET_HISTORY_MAX);
    const int current = entry;
    entry = static_cast<std::int16_t>(
        current + bonus - current * std::abs(bonus) / QUIET_HISTORY_MAX
    );
}

// The moving piece is recorded before make_move, including for promotions.
// Stop at a null move: synthetic positions must not train continuations.
template<typename Visitor>
void visit_continuations(Context& context, Move move, int ply, Visitor visit) noexcept {
    const Piece piece = context.position.piece_on(move.from());
    for (int distance = 1; distance <= 2 && distance <= ply; ++distance) {
        const auto previous_ply = static_cast<std::size_t>(ply - distance);
        const Move previous = context.path_moves[previous_ply];
        const Piece previous_piece = context.path_pieces[previous_ply];
        if (previous.is_none() || previous_piece == NO_PIECE)
            break;
        visit(context.thread_data.continuation_history
            [previous_piece][previous.to()][piece][move.to()]);
    }
}

std::int16_t& noisy_history_entry(Context& context, Move move) noexcept {
    const Piece piece = context.position.piece_on(move.from());
    const PieceType captured = move.type() == EN_PASSANT
        ? PAWN : type_of(context.position.piece_on(move.to()));
    return context.thread_data.noisy_history[piece][move.to()][captured];
}

int see_history(Context& context, Move move, int ply, bool quiet) noexcept {
    if (!quiet)
        return noisy_history_entry(context, move);
    int history = context.thread_data.quiet_history
        [context.position.side_to_move()][move.from()][move.to()];
    visit_continuations(context, move, ply,
        [&](std::int16_t& entry) noexcept { history += entry; });
    return history;
}

void update_quiet_history(Context& context, Move move, int ply, int bonus) noexcept {
    update_history_entry(context.thread_data.quiet_history
        [context.position.side_to_move()][move.from()][move.to()], bonus);
    visit_continuations(context, move, ply,
        [&](std::int16_t& entry) noexcept { update_history_entry(entry, bonus); });
}

[[nodiscard]] Depth late_move_reduction(
    Depth depth,
    std::size_t move_count,
    int history,
    bool priority_quiet
) noexcept {
    assert(depth >= LMR_MIN_DEPTH);
    assert(move_count >= LMR_MIN_MOVE_COUNT);

    Depth reduction = 1;
    reduction += depth >= 4 && move_count >= 5 ? 1 : 0;
    reduction += depth >= 6 && move_count >= 8 ? 1 : 0;
    reduction += depth >= 8 && move_count >= 12 ? 1 : 0;
    reduction += history < 0 ? 1 : 0;
    reduction -= history > 2'048 || priority_quiet ? 1 : 0;
    return std::clamp(reduction, Depth{0}, depth - 1);
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
    bool pv_node,
    bool allow_null = true,
    Move excluded_move = {}
) noexcept {
    assert(depth >= 0);
    assert(alpha < beta);
    assert(ply >= 0 && ply <= MAX_PLY);

    if (depth == 0)
        return qsearch(context, alpha, beta, ply);

    const bool excluded_search = !excluded_move.is_none();
    if (!excluded_search)
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
    Value tt_value = VALUE_NONE;
    if (tt_hit) {
        ++context.stats.tt_hits;
        if (!contains_move(moves, probe.data.move)) {
            tt_hit = false;
        } else {
            tt_value = value_from_tt(
                probe.data.value,
                ply,
                context.position.halfmove_clock()
            );
            if (!excluded_search) {
                tt_move = probe.data.move;
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
            if (!excluded_search && !probe.hit) {
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
        if (!excluded_search
            && !pv_node
            && depth <= RFP_MAX_DEPTH
            && is_eval_value(beta)
            && rfp_threshold <= VALUE_EVAL_MAX
            && raw_static_eval >= rfp_threshold) {
            ++context.stats.rfp_cutoffs;
            return beta;
        }
    }

    // Reckless-style NMP: require a static-eval cushion, then search a
    // dynamically reduced null position. The reduction grows with depth and
    // with the eval gap; only deep fail-highs pay for verification.
    const std::int64_t nmp_threshold = static_cast<std::int64_t>(beta)
        + nmp_eval_margin(depth);
    if (!excluded_search
        && !pv_node
        && !checked
        && allow_null
        && depth >= NMP_MIN_DEPTH
        && is_eval_value(beta)
        && is_eval_value(raw_static_eval)
        && nmp_threshold <= VALUE_EVAL_MAX
        && raw_static_eval >= nmp_threshold
        && ply >= context.nmp_min_ply
        && has_non_pawn_material(
            context.position,
            context.position.side_to_move()
        )) {
        const Depth reduction = null_move_reduction(
            depth,
            raw_static_eval,
            beta
        );
        const Depth null_depth = std::max(Depth{0}, depth - reduction);
        context.path_moves[static_cast<std::size_t>(ply)] = {};
        context.path_pieces[static_cast<std::size_t>(ply)] = NO_PIECE;
        ++context.stats.nmp_searches;

        Value null_score = VALUE_NONE;
        {
            NullMoveGuard guard(context);
            const Value child = pvs(
                context,
                null_depth,
                -beta,
                -beta + 1,
                ply + 1,
                false,
                false
            );
            if (child == VALUE_NONE)
                return VALUE_NONE;
            null_score = -child;
        }

        if (null_score >= beta && is_eval_value(null_score)) {
            if (context.nmp_min_ply > 0
                || depth < NMP_VERIFICATION_DEPTH) {
                ++context.stats.nmp_cutoffs;
                return beta;
            }

            ++context.stats.nmp_verifications;
            const int old_nmp_min_ply = context.nmp_min_ply;
            context.nmp_min_ply = ply + std::max(
                1,
                3 * static_cast<int>(null_depth) / 4
            );
            const Value verified = pvs(
                context,
                null_depth,
                beta - 1,
                beta,
                ply,
                false,
                false
            );
            context.nmp_min_ply = old_nmp_min_ply;
            if (verified == VALUE_NONE)
                return VALUE_NONE;
            if (verified >= beta) {
                ++context.stats.nmp_cutoffs;
                return beta;
            }
        }
    }

    // Internal iterative reduction: without a legal TT move, the node has no
    // trustworthy search-derived ordering hint. Spend one ply less until a
    // later iteration supplies one.
    if (!excluded_search
        && depth >= IIR_MIN_DEPTH
        && tt_move.is_none()) {
        --depth;
        ++context.stats.iir_reductions;
    }

    order_moves(context, moves, ply, tt_move);

    Depth singular_extension = 0;
    const bool singular_candidate = !excluded_search
                                 && ply > 0
                                 && depth >= SE_MIN_DEPTH
                                 && !tt_move.is_none()
                                 && is_eval_value(tt_value)
                                 && probe.data.depth + SE_TT_DEPTH_MARGIN >= depth
                                 && (probe.data.bound == BOUND_LOWER
                                     || probe.data.bound == BOUND_EXACT);
    if (singular_candidate) {
        const Value singular_beta = clamp_eval(
            static_cast<std::int64_t>(tt_value)
            - static_cast<std::int64_t>(SE_MARGIN_PER_DEPTH) * depth
        );
        const Depth verification_depth = std::max(
            Depth{1},
            (depth - 1) / 2
        );
        ++context.stats.singular_searches;
        const Value alternatives = pvs(
            context,
            verification_depth,
            singular_beta - 1,
            singular_beta,
            ply,
            false,
            false,
            tt_move
        );
        if (alternatives == VALUE_NONE)
            return VALUE_NONE;
        if (alternatives < singular_beta) {
            singular_extension = 1;
            ++context.stats.singular_extensions;
        } else if (singular_beta >= beta) {
            ++context.stats.singular_multicut_cutoffs;
            return beta;
        } else if (tt_value >= beta) {
            // The TT move predicts a fail-high, but the excluded search found
            // another move near its score. It is therefore not singular and
            // does not deserve the full nominal depth.
            singular_extension = -1;
            ++context.stats.singular_reductions;
        }
    }

    Value best_value = -VALUE_INFINITE;
    Move best_move{};
    std::size_t move_count = 0;
    std::size_t quiet_move_count = 0;
    std::array<Move, MAX_MOVES> searched_quiets{};
    std::size_t searched_quiet_count = 0;
    std::array<Move, MAX_MOVES> searched_noisies{};
    std::size_t searched_noisy_count = 0;
    bool skip_quiets = false;
    const bool lmp_node = !excluded_search
                       && !pv_node
                       && !checked
                       && depth <= LMP_MAX_DEPTH
                       && is_eval_value(alpha)
                       && is_eval_value(beta);
    const std::size_t lmp_move_limit = LMP_BASE_MOVE_LIMIT
        + static_cast<std::size_t>(depth * depth);

    for (const Move move : moves) {
        if (move == excluded_move)
            continue;

        const bool quiet = !is_capture(context.position, move)
                        && move.type() != PROMOTION;
        if (quiet)
            ++quiet_move_count;

        const int history = quiet
            ? context.thread_data.quiet_history[
                  static_cast<std::size_t>(context.position.side_to_move())
              ][static_cast<std::size_t>(move.from())]
               [static_cast<std::size_t>(move.to())]
            : 0;

        bool checking = false;
        bool checking_known = false;

        // Forward futility pruning: once a history-adjusted upper estimate
        // cannot reach alpha, the remaining ordinary quiets are no better by
        // move order.  Keep walking the list for tactical noisies, castling,
        // and direct checks instead of terminating the move loop.
        const bool ffp_candidate = !excluded_search
                                && !pv_node
                                && !checked
                                && quiet
                                && move.type() != CASTLING
                                && move != tt_move
                                && depth <= FFP_MAX_DEPTH
                                && move_count > 0
                                && is_eval_value(alpha)
                                && is_eval_value(raw_static_eval)
                                && is_eval_value(best_value);
        if (ffp_candidate && skip_quiets) {
            checking = gives_check(context.position, move);
            checking_known = true;
            if (!checking) {
                ++context.stats.ffp_prunes;
                continue;
            }
        }

        if (ffp_candidate && !skip_quiets) {
            const std::int64_t futility_value =
                static_cast<std::int64_t>(raw_static_eval)
                + static_cast<std::int64_t>(FFP_MARGIN_PER_DEPTH) * depth
                + static_cast<std::int64_t>(FFP_HISTORY_MULTIPLIER)
                    * history / FFP_HISTORY_DIVISOR
                - FFP_BASE_OFFSET;
            if (futility_value <= alpha) {
                checking = gives_check(context.position, move);
                checking_known = true;
                if (!checking) {
                    // The skipped quiet still contributes its conservative
                    // upper estimate.  Without this, the node could store a
                    // much lower searched score as an overconfident TT upper
                    // bound even though the pruned move may score higher.
                    best_value = std::max(
                        best_value,
                        clamp_eval(futility_value)
                    );
                    skip_quiets = true;
                    ++context.stats.ffp_prunes;
                    continue;
                }
            }
        }

        // Quiet history makes the depth-squared prefix meaningful. Preserve
        // tactically exceptional quiets and give successful moves more room.
        if (lmp_node
            && quiet
            && move.type() != CASTLING
            && move != tt_move
            && quiet_move_count > lmp_move_limit
                + static_cast<std::size_t>(std::max(history, 0) / 2'048)
            && is_eval_value(best_value)) {
            checking = gives_check(context.position, move);
            checking_known = true;
            if (!checking) {
                ++context.stats.lmp_prunes;
                continue;
            }
        }

        // Experimental Reckless-style main-search SEE policy. Require a real
        // searched move before pruning (best_value may be a futility estimate).
        // Excluded searches retain the existing MORS singular-search guards.
        if (context.limits.use_see_pruning
            && !excluded_search && ply > 0 && move_count > 0
            && is_valid_value(best_value) && !is_loss(best_value)
            && (!checked || !quiet)) {
            const int history_score = see_history(context, move, ply, quiet);
            const SeeValue threshold = std::min(0, quiet
                ? -12 * depth * depth + 56 * depth - 27 * history_score / 1024 + 27
                : -7 * depth * depth - 36 * depth - 39 * history_score / 1024 + 14);
            if (!see_ge(context.position, move, threshold)) {
                ++context.stats.see_prunes;
                if (quiet)
                    ++context.stats.see_quiet_prunes;
                else
                    ++context.stats.see_noisy_prunes;
                continue;
            }
        }

        Depth reduction = 0;
        if (!excluded_search
            && !checked
            && quiet
            && move.type() != CASTLING
            && move != tt_move
            && depth >= LMR_MIN_DEPTH
            && move_count + 1 >= LMR_MIN_MOVE_COUNT) {
            if (!checking_known)
                checking = gives_check(context.position, move);
            if (!checking) {
                reduction = late_move_reduction(
                    depth,
                    move_count + 1,
                    history,
                    is_killer_move(context, move, ply)
                        || is_counter_move(context, move, ply)
                );
            }
        }

        context.path_moves[static_cast<std::size_t>(ply)] = move;
        context.path_pieces[static_cast<std::size_t>(ply)] =
            context.position.piece_on(move.from());
        const Depth extension = move == tt_move ? singular_extension : 0;
        const Depth child_depth = depth - 1 + extension;
        Value score = VALUE_NONE;
        {
            MoveGuard guard(context, move);

            if (move_count == 0) {
                const Value child = pvs(
                    context,
                    child_depth,
                    -beta,
                    -alpha,
                    ply + 1,
                    pv_node
                );
                if (child == VALUE_NONE)
                    return VALUE_NONE;
                score = -child;
            } else {
                if (reduction > 0)
                    ++context.stats.lmr_searches;
                Value probe_value = pvs(
                    context,
                    child_depth - reduction,
                    -alpha - 1,
                    -alpha,
                    ply + 1,
                    false
                );
                if (probe_value == VALUE_NONE)
                    return VALUE_NONE;
                score = -probe_value;

                // A reduced fail-high is only a hint. Verify it at the full
                // depth before allowing it to affect alpha or cut the node.
                if (reduction > 0 && score > alpha) {
                    ++context.stats.lmr_researches;
                    probe_value = pvs(
                        context,
                        child_depth,
                        -alpha - 1,
                        -alpha,
                        ply + 1,
                        false
                    );
                    if (probe_value == VALUE_NONE)
                        return VALUE_NONE;
                    score = -probe_value;
                }

                if (score > alpha && score < beta) {
                    ++context.stats.pvs_researches;
                    const Value child = pvs(
                        context,
                        child_depth,
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
        if (quiet)
            searched_quiets[searched_quiet_count++] = move;
        else
            searched_noisies[searched_noisy_count++] = move;

        if (score > best_value) {
            best_value = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            if (!excluded_search)
                update_pv(context, ply, move);
        }
        if (alpha >= beta) {
            if (!excluded_search) {
                const int bonus = std::min(2'048, 32 * depth * depth);
                if (!quiet)
                    update_history_entry(noisy_history_entry(context, move), bonus);
                for (std::size_t index = 0; index < searched_noisy_count; ++index) {
                    if (searched_noisies[index] != move)
                        update_history_entry(
                            noisy_history_entry(context, searched_noisies[index]), -bonus / 2);
                }
            }
            if (!excluded_search && quiet) {
                const int bonus = std::min(2'048, 32 * depth * depth);
                update_quiet_history(context, move, ply, bonus);
                for (std::size_t index = 0;
                     index + 1 < searched_quiet_count;
                     ++index) {
                    update_quiet_history(
                        context,
                        searched_quiets[index],
                        ply,
                        -bonus / 2
                    );
                }

                auto& killers = context.killer_moves[
                    static_cast<std::size_t>(ply)
                ];
                if (killers[0] != move) {
                    killers[1] = killers[0];
                    killers[0] = move;
                }
                if (ply > 0) {
                    const Move previous = context.path_moves[
                        static_cast<std::size_t>(ply - 1)
                    ];
                    if (!previous.is_none()) {
                        context.counter_moves[
                            static_cast<std::size_t>(previous.from())
                        ][static_cast<std::size_t>(previous.to())] = move;
                    }
                }
            }
            break;
        }
    }

    if (excluded_search && move_count == 0)
        return original_alpha;

    assert(best_value != -VALUE_INFINITE && !best_move.is_none());
    const Bound bound = best_value >= beta                 ? BOUND_LOWER
                      : best_value <= original_alpha       ? BOUND_UPPER
                                                          : BOUND_EXACT;
    if (!excluded_search) {
        probe.writer.write({
            .move = best_move,
            .value = value_to_tt(best_value, ply),
            .static_eval = raw_static_eval,
            .depth = depth,
            .bound = bound,
            .pv = pv_node
        });
    }
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
    const bool pv_node = beta - alpha > 1;
    clear_pv(context, ply);

    if (!begin_node(context, ply, true))
        return VALUE_NONE;

    const bool checked = in_check(context.position);
    MoveList moves;
    if (checked)
        generate_legal(context.position, moves);
    else
        generate_legal_noisy(context.position, moves);

    if (checked && moves.empty())
        return mated_in(ply);
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
        // A main-search entry may contain a legal quiet move absent from our
        // noisy list. Validate it without discarding its score or raw eval.
        if (!contains_move(moves, probe.data.move)) {
            MoveList legal;
            generate_legal(context.position, legal);
            tt_hit = contains_move(legal, probe.data.move);
        }
        if (tt_hit) {
            if (contains_move(moves, probe.data.move))
                tt_move = probe.data.move;
            const Value tt_value = value_from_tt(
                probe.data.value, ply, context.position.halfmove_clock());
            const bool bound_ok = probe.data.bound == BOUND_EXACT
                || (probe.data.bound == BOUND_LOWER && tt_value >= beta)
                || (probe.data.bound == BOUND_UPPER && tt_value <= alpha);
            // Keep PV nodes searchable so a cached score cannot truncate PV.
            if (!pv_node && tt_value != VALUE_NONE
                && probe.data.depth >= DEPTH_QS && bound_ok) {
                ++context.stats.tt_cutoffs;
                return tt_value;
            }
        }
    }

    Value best_value = -VALUE_INFINITE;
    Move best_move{};
    Value stand_pat = VALUE_NONE;
    if (!checked) {
        if (tt_hit && is_eval_value(probe.data.static_eval)) {
            stand_pat = probe.data.static_eval;
            ++context.stats.static_eval_cache_hits;
        } else {
            stand_pat = context.evaluator.evaluate(context.position, context.network);
        }
        best_value = stand_pat;
        if (stand_pat >= beta) {
            probe.writer.write({
                .move = {},
                .value = value_to_tt(stand_pat, ply),
                .static_eval = stand_pat,
                .depth = DEPTH_QS,
                .bound = BOUND_LOWER,
                .pv = pv_node
            });
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
    }

    std::array<int, MAX_MOVES> ordered_scores{};
    order_moves(context, moves, ply, tt_move, &ordered_scores);
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
            // TT priority is not evidence of a non-losing exchange.
            const bool ordered_good = move != tt_move && ordered_scores[move_index] > 0;
            const bool passes_see = (ordered_good && see_threshold <= 0)
                || see_ge(context.position, move, see_threshold);
            if (!passes_see) {
                ++context.stats.qsearch_see_prunes;
                continue;
            }
        }

        context.path_moves[static_cast<std::size_t>(ply)] = move;
        context.path_pieces[static_cast<std::size_t>(ply)] =
            context.position.piece_on(move.from());
        Value score = VALUE_NONE;
        {
            MoveGuard guard(context, move);
            const Value child = qsearch(context, -beta, -alpha, ply + 1);
            if (child == VALUE_NONE)
                return VALUE_NONE;
            score = -child;
        }

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

    assert(best_value != -VALUE_INFINITE);
    // Interrupted children return above, before publishing a partial bound.
    probe.writer.write({
        .move = best_move,
        .value = value_to_tt(best_value, ply),
        .static_eval = stand_pat,
        .depth = DEPTH_QS,
        .bound = best_value >= beta ? BOUND_LOWER
               : best_value <= original_alpha ? BOUND_UPPER : BOUND_EXACT,
        .pv = pv_node
    });
    return best_value;
}

// Owns the mutable state for one independent search lane. The root position is
// copied before a job starts so future Lazy SMP workers never share make/unmake
// state, NNUE accumulator state, histories, PV storage, or node counters.
class SearchWorker final {
public:
    SearchWorker(
        TranspositionTable& table,
        const nnue::Network& network,
        ThreadData& thread_data
    ) noexcept
        : table_(table), network_(network), thread_data_(thread_data) {}

    [[nodiscard]] SearchResult run(
        const Position& root_position,
        const SearchLimits& limits,
        const std::function<void(const SearchResult&)>& iteration_callback,
        std::size_t worker_index = 0,
        WorkerControl* worker_control = nullptr
    ) {
        Position position = root_position;
        Context context(
            position,
            table_,
            network_,
            thread_data_,
            limits,
            worker_control
        );
        SearchResult result;

        for (Depth depth = 1; depth <= limits.max_depth; ++depth) {
            Value alpha = -VALUE_INFINITE;
            Value beta = VALUE_INFINITE;
            Value delta = INITIAL_ASPIRATION_DELTA
                + static_cast<Value>(worker_index % 8);

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
            result.stats = context.stats;

            if (worker_control != nullptr
                && worker_control->published_nodes != nullptr) {
                worker_control->published_nodes->store(
                    context.stats.nodes,
                    std::memory_order_relaxed
                );
            }
            if (iteration_callback)
                iteration_callback(result);

            if (limits.soft_time.count() > 0
                && std::chrono::steady_clock::now() >= context.soft_deadline) {
                context.stopped = true;
                break;
            }
        }

        result.stopped = context.stopped;
        result.stats = context.stats;
        if (worker_control != nullptr
            && worker_control->published_nodes != nullptr) {
            worker_control->published_nodes->store(
                context.stats.nodes,
                std::memory_order_relaxed
            );
        }
        return result;
    }

private:
    TranspositionTable& table_;
    const nnue::Network& network_;
    ThreadData& thread_data_;
};

// Coordinates state shared by a root search. Only this layer advances the TT
// generation and selects which worker may report iterations; helper workers
// will receive an empty callback when Lazy SMP is introduced.
class SearchCoordinator final {
public:
    SearchCoordinator(
        TranspositionTable& table,
        const nnue::Network& network,
        const SearchLimits& limits,
        SearchWorker& main_worker
    ) noexcept
        : table_(table),
          network_(network),
          limits_(limits),
          main_worker_(main_worker) {}

    [[nodiscard]] SearchResult run(const Position& root_position) {
        if (!network_.valid())
            throw std::invalid_argument("search requires a valid NNUE network");
        if (table_.cluster_count() == 0)
            throw std::invalid_argument("search requires a non-empty transposition table");
        if (limits_.max_depth < 1 || limits_.max_depth > MAX_PLY)
            throw std::invalid_argument("search depth is outside the supported range");

        table_.new_search();
        return main_worker_.run(
            root_position,
            limits_,
            limits_.iteration_callback
        );
    }

private:
    TranspositionTable& table_;
    const nnue::Network& network_;
    const SearchLimits& limits_;
    SearchWorker& main_worker_;
};

} // namespace

class SearchThreadPool::Impl final {
public:
    Impl(
        TranspositionTable& table, const nnue::Network& network,
        std::size_t thread_count, numa::Policy policy,
        numa::Topology topology, bool allow_binding = true
    ) : table_(table), network_(network), topology_(std::move(topology)), policy_(policy),
        bindings_() {
        if (thread_count < 1 || thread_count > MAX_SEARCH_THREADS)
            throw std::invalid_argument("thread count is outside the supported range");
        bindings_ = numa::plan(topology_, thread_count,
            allow_binding ? policy : numa::Policy::None, numa::process_rotation());
        try {
            start_threads(thread_count);
            refresh_network(network_);
        } catch (...) {
            stop_threads();
            throw;
        }
    }

    ~Impl() {
        request_stop();
        wait();
        stop_threads();
    }

    [[nodiscard]] std::unique_ptr<Impl> replacement(std::size_t count, numa::Policy policy) {
        require_idle();
        return create(table_, network_, count, policy, topology_);
    }

    static std::unique_ptr<Impl> create(
        TranspositionTable& table, const nnue::Network& network,
        std::size_t count, numa::Policy policy, const numa::Topology& topology
    ) {
        try {
            return std::make_unique<Impl>(table, network, count, policy, topology);
        } catch (const numa::BindingError& error) {
            auto pool = std::make_unique<Impl>(table, network, count, policy, topology, false);
            pool->binding_diagnostic_ = error.what();
            return pool;
        }
    }

    void check_idle() const { require_idle(); }

    [[nodiscard]] numa::Policy policy() const noexcept { return policy_; }

    [[nodiscard]] std::string configuration() const {
        std::ostringstream output;
        output << "info string Available processors: " << topology_.describe() << '\n'
               << "info string NUMA policy: " << (policy_ == numa::Policy::Auto ? "auto" : "none")
               << (bindings_.empty() ? " (OS scheduling)\n" : " (CPU binding enabled)\n");
        if (!topology_.diagnostic.empty())
            output << "info string " << topology_.diagnostic << '\n';
        if (!binding_diagnostic_.empty())
            output << "info string NUMA fallback: " << binding_diagnostic_ << '\n';
        if (replicas_.empty()) {
            output << "info string Network replicas: 1 shared (OS placement)\n";
        } else {
            for (const auto& replica : replicas_) {
                std::size_t workers = 0;
                for (const auto& binding : bindings_)
                    workers += binding.node == replica.node;
                output << "info string NUMA node " << replica.node << ": " << workers
                       << " workers, 1 NNUE replica (preferred placement)\n";
            }
        }
        return output.str();
    }

    void refresh_network(const nnue::Network& network) {
        require_idle();
        if (!network.valid()) throw std::invalid_argument("invalid NNUE network");
        std::vector<Replica> replacements;
        std::map<unsigned, std::size_t> leaders;
        for (std::size_t i = 0; i < bindings_.size(); ++i)
            leaders.try_emplace(bindings_[i].node, i);
        for (const auto& [node, leader] : leaders) {
            (void)leader;
            replacements.push_back(Replica{node, {}});
        }
        if (!replacements.empty()) {
            run_on_workers([&](std::size_t index) {
                for (auto& replica : replacements)
                    if (leaders.at(replica.node) == index)
                        replica.network = network.clone(replica.node);
            });
        }
        replicas_.swap(replacements);
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return threads_.size();
    }

    void clear() {
        require_idle();
        // Stage on the owning, bound workers. Publish only after all allocations
        // succeed; a failed clear keeps every worker's learned history intact.
        std::vector<std::unique_ptr<ThreadData>> replacements(threads_.size());
        run_on_workers([&](std::size_t index) {
            replacements[index] = std::make_unique<ThreadData>();
        });
        thread_data_.swap(replacements);
    }

    void start(
        const Position& root_position,
        const SearchLimits& limits,
        CompletionCallback completion_callback
    ) {
        if (!network_.valid())
            throw std::invalid_argument("search requires a valid NNUE network");
        if (table_.cluster_count() == 0)
            throw std::invalid_argument("search requires a non-empty transposition table");
        if (limits.max_depth < 1 || limits.max_depth > MAX_PLY)
            throw std::invalid_argument("search depth is outside the supported range");

        {
            const std::lock_guard lock(mutex_);
            if (active_)
                throw std::logic_error("thread pool already has an active search");
            if (maintenance_ || restarting_ || shutting_down_ || threads_.empty())
                throw std::logic_error("thread pool is not ready");

            const bool initially_stopped =
                limits.stop != nullptr
                && limits.stop->load(std::memory_order_acquire);
            stop_requested_.store(initially_stopped, std::memory_order_release);
            job_.emplace(
                root_position,
                limits,
                stop_requested_,
                threads_.size(),
                std::move(completion_callback)
            );
            table_.new_search();
            active_ = true;
            ++job_generation_;
        }
        job_cv_.notify_all();
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    void wait() {
        std::unique_lock lock(mutex_);
        idle_cv_.wait(lock, [this] { return !active_; });
    }

    [[nodiscard]] bool searching() const {
        const std::lock_guard lock(mutex_);
        return active_;
    }

private:
    struct alignas(64) WorkerSharedState final {
        std::atomic<std::uint64_t> nodes{0};
    };

    struct Job final {
        Job(
            const Position& root_position,
            const SearchLimits& requested_limits,
            std::atomic_bool& job_stop,
            std::size_t requested_worker_count,
            CompletionCallback requested_completion
        )
            : root(root_position),
              prior_keys(requested_limits.prior_keys.size()),
              limits(requested_limits),
              completion(std::move(requested_completion)),
              worker_states(
                  std::make_unique<WorkerSharedState[]>(
                      requested_worker_count
                  )
              ),
              worker_count(requested_worker_count),
              remaining_workers(requested_worker_count) {
            if (!prior_keys.empty()) {
                std::copy(
                    requested_limits.prior_keys.begin(),
                    requested_limits.prior_keys.end(),
                    prior_keys.begin()
                );
            }
            limits.prior_keys = std::span<const Key>(prior_keys);
            limits.stop = &job_stop;
        }

        Position root;
        std::vector<Key> prior_keys;
        SearchLimits limits;
        CompletionCallback completion;
        std::unique_ptr<WorkerSharedState[]> worker_states;
        std::atomic<std::uint64_t> shared_node_count{0};
        SearchResult main_result;
        std::string error;
        std::size_t worker_count = 0;
        std::size_t arrived_workers = 0;
        std::size_t remaining_workers = 0;
        bool main_finished = false;
    };

    [[nodiscard]] static std::uint64_t total_nodes(
        const Job& job
    ) noexcept {
        if (job.limits.max_nodes
            != std::numeric_limits<std::uint64_t>::max()) {
            return job.shared_node_count.load(std::memory_order_relaxed);
        }

        std::uint64_t nodes = 0;
        for (std::size_t index = 0; index < job.worker_count; ++index) {
            nodes += job.worker_states[index].nodes.load(
                std::memory_order_relaxed
            );
        }
        return nodes;
    }

    struct Replica final {
        unsigned node;
        nnue::Network network;
    };

    void require_idle() const {
        const std::lock_guard lock(mutex_);
        if (active_ || maintenance_ || restarting_ || shutting_down_)
            throw std::logic_error("thread pool must be idle for reconfiguration");
    }

    void run_on_workers(std::function<void(std::size_t)> task) {
        std::unique_lock lock(mutex_);
        if (active_ || maintenance_ || restarting_ || shutting_down_)
            throw std::logic_error("thread pool must be idle for maintenance");
        if (threads_.empty()) throw std::logic_error("thread pool is empty");
        maintenance_task_ = std::move(task);
        maintenance_error_ = nullptr;
        maintenance_remaining_ = threads_.size();
        maintenance_ = true;
        ++job_generation_;
        job_cv_.notify_all();
        idle_cv_.wait(lock, [this] { return maintenance_remaining_ == 0; });
        maintenance_ = false;
        maintenance_task_ = {};
        if (maintenance_error_) std::rethrow_exception(maintenance_error_);
    }

    void start_threads(std::size_t thread_count) {
        thread_data_.resize(thread_count);
        threads_.reserve(thread_count);
        for (std::size_t index = 0; index < thread_count; ++index)
            threads_.emplace_back(&Impl::worker_loop, this, index);
        std::unique_lock lock(mutex_);
        idle_cv_.wait(lock, [this, thread_count] { return initialized_workers_ == thread_count; });
        if (initialization_error_) std::rethrow_exception(initialization_error_);
    }

    void stop_threads() {
        std::vector<std::thread> retiring;
        {
            const std::lock_guard lock(mutex_);
            restarting_ = true;
            shutting_down_ = true;
            retiring.swap(threads_);
        }
        job_cv_.notify_all();
        for (std::thread& thread : retiring)
            if (thread.joinable())
                thread.join();
        {
            const std::lock_guard lock(mutex_);
            shutting_down_ = false;
            restarting_ = false;
            job_generation_ = 0;
        }
    }

    void worker_loop(std::size_t worker_index) {
        std::exception_ptr initialization_error;
        try {
            if (!bindings_.empty()) numa::bind_current_thread(bindings_[worker_index]);
            thread_data_[worker_index] = std::make_unique<ThreadData>();
        } catch (...) {
            initialization_error = std::current_exception();
        }
        {
            const std::lock_guard lock(mutex_);
            if (initialization_error && !initialization_error_)
                initialization_error_ = initialization_error;
            ++initialized_workers_;
        }
        idle_cv_.notify_all();
        if (initialization_error) return;
        std::uint64_t observed_generation = 0;

        while (true) {
            Job* job = nullptr;
            {
                std::unique_lock lock(mutex_);
                job_cv_.wait(lock, [this, observed_generation] {
                    return shutting_down_
                        || job_generation_ != observed_generation;
                });
                if (shutting_down_)
                    return;

                observed_generation = job_generation_;
                if (maintenance_) {
                    lock.unlock();
                    std::exception_ptr error;
                    try {
                        maintenance_task_(worker_index);
                    } catch (...) {
                        error = std::current_exception();
                    }
                    lock.lock();
                    if (error && !maintenance_error_) maintenance_error_ = error;
                    --maintenance_remaining_;
                    idle_cv_.notify_all();
                    continue;
                }
                assert(active_);
                assert(job_.has_value());
                assert(worker_index < job_->worker_count);
                ++job_->arrived_workers;
                if (job_->arrived_workers == job_->worker_count) {
                    start_cv_.notify_all();
                } else {
                    start_cv_.wait(lock, [this, observed_generation] {
                        return shutting_down_
                            || job_generation_ != observed_generation
                            || (job_.has_value()
                                && job_->arrived_workers
                                    == job_->worker_count);
                    });
                    if (shutting_down_)
                        return;
                }
                job = &*job_;
            }

            SearchLimits worker_limits = job->limits;
            worker_limits.iteration_callback = {};
            WorkerControl worker_control{
                .published_nodes =
                    &job->worker_states[worker_index].nodes,
                .shared_node_count =
                    job->limits.max_nodes
                        == std::numeric_limits<std::uint64_t>::max()
                    ? nullptr
                    : &job->shared_node_count,
                .shared_stop = &stop_requested_,
                .shared_node_limit = job->limits.max_nodes
            };

            std::function<void(const SearchResult&)> iteration_callback;
            if (worker_index == 0 && job->limits.iteration_callback) {
                iteration_callback =
                    [this, job](const SearchResult& iteration) {
                        SearchResult aggregate = iteration;
                        aggregate.stats.nodes = total_nodes(*job);
                        job->limits.iteration_callback(aggregate);
                    };
            }

            SearchResult result;
            std::string error;
            try {
                const nnue::Network* selected_network = &network_;
                if (!bindings_.empty()) {
                    const unsigned node = bindings_[worker_index].node;
                    for (const auto& replica : replicas_)
                        if (replica.node == node) {
                            selected_network = &replica.network;
                            break;
                        }
                }
                SearchWorker worker(table_, *selected_network, *thread_data_[worker_index]);
                result = worker.run(
                    job->root,
                    worker_limits,
                    iteration_callback,
                    worker_index,
                    &worker_control
                );
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "unknown search failure";
            }

            CompletionCallback completion;
            SearchResult completed_result;
            std::string completion_error;
            bool finalize = false;

            {
                const std::lock_guard lock(mutex_);
                assert(job_generation_ == observed_generation);
                assert(job_.has_value());
                if (!error.empty() && job_->error.empty()) {
                    job_->error = "worker "
                        + std::to_string(worker_index)
                        + ": " + error;
                    stop_requested_.store(true, std::memory_order_release);
                }
                if (worker_index == 0) {
                    job_->main_result = result;
                    job_->main_finished = true;
                    stop_requested_.store(true, std::memory_order_release);
                }

                assert(job_->remaining_workers > 0);
                --job_->remaining_workers;
                if (job_->remaining_workers == 0) {
                    assert(job_->main_finished);
                    job_->main_result.stats.nodes = total_nodes(*job_);
                    completed_result = job_->main_result;
                    completion = std::move(job_->completion);
                    completion_error = std::move(job_->error);
                    finalize = true;
                }
            }

            if (!finalize)
                continue;

            try {
                if (completion)
                    completion(completed_result, completion_error);
            } catch (...) {
                // Completion callbacks are external reporting hooks. A faulty
                // reporter must not strand the persistent pool in busy state.
            }

            {
                const std::lock_guard lock(mutex_);
                assert(job_generation_ == observed_generation);
                job_.reset();
                active_ = false;
            }
            idle_cv_.notify_all();
        }
    }

    TranspositionTable& table_;
    const nnue::Network& network_;
    mutable std::mutex mutex_;
    std::condition_variable job_cv_;
    std::condition_variable start_cv_;
    std::condition_variable idle_cv_;
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<ThreadData>> thread_data_;
    numa::Topology topology_;
    numa::Policy policy_;
    std::vector<numa::Cpu> bindings_;
    std::vector<Replica> replicas_;
    std::string binding_diagnostic_;
    std::size_t initialized_workers_ = 0;
    std::exception_ptr initialization_error_;
    std::function<void(std::size_t)> maintenance_task_;
    std::exception_ptr maintenance_error_;
    std::size_t maintenance_remaining_ = 0;
    bool maintenance_ = false;
    std::optional<Job> job_;
    std::atomic_bool stop_requested_{false};
    std::uint64_t job_generation_ = 0;
    bool active_ = false;
    bool restarting_ = false;
    bool shutting_down_ = false;
};

SearchThreadPool::SearchThreadPool(
    TranspositionTable& table,
    const nnue::Network& network,
    std::size_t thread_count,
    numa::Policy policy
)
    : impl_(Impl::create(table, network, thread_count, policy, numa::Topology::detect())) {}

SearchThreadPool::~SearchThreadPool() = default;

void SearchThreadPool::resize(std::size_t thread_count) {
    impl_->check_idle();
    if (thread_count != impl_->size())
        impl_ = impl_->replacement(thread_count, impl_->policy());
}

void SearchThreadPool::set_numa_policy(numa::Policy policy) {
    impl_->check_idle();
    if (policy != impl_->policy())
        impl_ = impl_->replacement(impl_->size(), policy);
}

numa::Policy SearchThreadPool::numa_policy() const { return impl_->policy(); }

std::string SearchThreadPool::configuration() const { return impl_->configuration(); }

void SearchThreadPool::refresh_network(const nnue::Network& network) {
    impl_->refresh_network(network);
}

std::size_t SearchThreadPool::size() const {
    return impl_->size();
}

void SearchThreadPool::clear() {
    impl_->clear();
}

void SearchThreadPool::start(
    const Position& root_position,
    const SearchLimits& limits,
    CompletionCallback completion_callback
) {
    impl_->start(root_position, limits, std::move(completion_callback));
}

void SearchThreadPool::request_stop() noexcept {
    impl_->request_stop();
}

void SearchThreadPool::wait() {
    impl_->wait();
}

bool SearchThreadPool::searching() const {
    return impl_->searching();
}

SearchResult search(
    Position& position,
    TranspositionTable& table,
    const nnue::Network& network,
    const SearchLimits& limits
) {
    auto thread_data = std::make_unique<ThreadData>();
    SearchWorker main_worker(table, network, *thread_data);
    SearchCoordinator coordinator(
        table,
        network,
        limits,
        main_worker
    );
    return coordinator.run(position);
}

} // namespace mors
