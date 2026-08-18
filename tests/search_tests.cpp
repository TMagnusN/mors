// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/movegen.hpp"
#include "eval/nnue/network.hpp"
#include "search/score.hpp"
#include "search/search.hpp"
#include "search/tt.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using namespace mors;

[[nodiscard]] std::filesystem::path network_path() {
    constexpr std::array<std::string_view, 3> CANDIDATES{
        "../networks/mors-p2h32.nnue",
        "networks/mors-p2h32.nnue",
        "../../networks/mors-p2h32.nnue"
    };
    for (const std::string_view candidate : CANDIDATES) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return std::filesystem::path(candidate);
    }
    return {};
}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL search: " << message << '\n';
    return condition;
}

[[nodiscard]] bool move_is_legal(Position& position, Move move) {
    MoveList legal;
    generate_legal(position, legal);
    for (const Move candidate : legal)
        if (candidate == move) return true;
    return false;
}

bool test_terminal_nodes(const nnue::Network& network) {
    constexpr std::string_view CHECKMATE =
        "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1";
    constexpr std::string_view STALEMATE =
        "7k/5Q2/5K2/8/8/8/8/8 b - - 0 1";
    constexpr std::string_view MATE_IN_ONE =
        "7k/8/5KQ1/8/8/8/8/8 w - - 0 1";

    auto checkmate = Position::from_fen(CHECKMATE);
    auto stalemate = Position::from_fen(STALEMATE);
    auto mate = Position::from_fen(MATE_IN_ONE);
    if (!expect(checkmate.has_value() && stalemate.has_value() && mate.has_value(),
                "terminal FENs must parse")) {
        return false;
    }

    TranspositionTable table(1);
    const SearchLimits limits{.max_depth = 2};
    const SearchResult checkmate_result = search(*checkmate, table, network, limits);
    table.clear();
    const SearchResult stalemate_result = search(*stalemate, table, network, limits);
    table.clear();
    const SearchResult mate_result = search(*mate, table, network, limits);

    if (!expect(checkmate_result.value == mated_in(0),
                "root checkmate must use the root-relative mate score")
        || !expect(checkmate_result.best_move.is_none(),
                   "checkmate has no best move")
        || !expect(stalemate_result.value == VALUE_DRAW,
                   "stalemate must be a draw")
        || !expect(stalemate_result.best_move.is_none(),
                   "stalemate has no best move")
        || !expect(mate_result.value == mate_in(1),
                   "mate in one must have exact distance")
        || !expect(!mate_result.best_move.is_none(),
                   "mate in one must return a move")
        || !expect(move_is_legal(*mate, mate_result.best_move),
                   "mate move must be legal")) {
        return false;
    }

    StateInfo state;
    mate->do_move(mate_result.best_move, state);
    MoveList replies;
    generate_legal(*mate, replies);
    const bool child_in_check = mate->is_square_attacked(
        mate->king_square(mate->side_to_move()),
        ~mate->side_to_move(),
        mate->pieces()
    );
    mate->undo_move(mate_result.best_move, state);
    return expect(replies.empty() && child_in_check,
                  "reported mate move must actually checkmate");
}

bool test_pvs_and_restoration(const nnue::Network& network) {
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "start position must parse"))
        return false;

    Position& position = *parsed;
    const std::string original_fen = position.fen();
    const Key original_key = position.key();
    TranspositionTable table(4);
    const SearchResult result = search(
        position,
        table,
        network,
        SearchLimits{.max_depth = 3}
    );

    if (!expect(result.completed_depth == 3, "depth-three iteration must complete")
        || !expect(is_valid_value(result.value), "PVS result must be a valid value")
        || !expect(!result.best_move.is_none(), "PVS must return a root move")
        || !expect(result.pv_length >= 1, "PVS must return a principal variation")
        || !expect(result.stats.nodes > result.stats.qnodes,
                   "main-search nodes must be counted separately")
        || !expect(result.stats.pvs_researches > 0,
                   "test position must exercise a PVS full-window re-search")
        || !expect(result.stats.aspiration_searches >= 2,
                   "depths after the first must use aspiration windows")
        || !expect(result.stats.aspiration_searches
                       == 2 + result.stats.aspiration_researches,
                   "each aspiration failure must cause exactly one root re-search")
        || !expect(result.stats.static_eval_cache_hits > 0,
                   "iterative deepening must reuse cached static evaluations")
        || !expect(position.key() == original_key && position.fen() == original_fen,
                   "search must restore the root position")) {
        return false;
    }

    Position pv_position = position;
    for (std::size_t index = 0; index < result.pv_length; ++index) {
        const Move move = result.principal_variation[index];
        if (!expect(move_is_legal(pv_position, move), "every PV move must be legal"))
            return false;
        StateInfo state;
        pv_position.do_move(move, state);
    }
    return true;
}

bool test_reverse_futility_pruning(const nnue::Network& network) {
    auto parsed = Position::from_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10"
    );
    if (!expect(parsed.has_value(), "RFP exercise FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    TranspositionTable table(8);
    const SearchResult result = search(
        *parsed,
        table,
        network,
        SearchLimits{.max_depth = 4}
    );

    return expect(result.completed_depth == 4, "RFP exercise search must complete")
        && expect(result.stats.rfp_cutoffs > 0,
                  "non-PV shallow nodes must exercise reverse futility pruning")
        && expect(!result.best_move.is_none()
                      && move_is_legal(*parsed, result.best_move),
                  "RFP exercise must retain a legal root move")
        && expect(parsed->fen() == original_fen,
                  "RFP exercise must restore the root position");
}

bool test_tt_reuse(const nnue::Network& network) {
    auto parsed = Position::from_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10"
    );
    if (!expect(parsed.has_value(), "TT-reuse FEN must parse"))
        return false;

    TranspositionTable table(8);
    const SearchLimits limits{.max_depth = 3};
    const SearchResult first = search(*parsed, table, network, limits);
    const SearchResult second = search(*parsed, table, network, limits);

    return expect(first.completed_depth == 3 && second.completed_depth == 3,
                  "both TT searches must complete")
        && expect(first.value == second.value, "TT reuse must preserve the score")
        && expect(first.best_move == second.best_move,
                  "TT reuse must preserve the best move")
        && expect(second.stats.tt_hits > 0, "second search must hit the TT")
        && expect(second.stats.tt_cutoffs > 0, "second search must use TT bounds");
}

bool test_node_limit(const nnue::Network& network) {
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "node-limit FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    TranspositionTable table(1);
    const SearchResult result = search(
        *parsed,
        table,
        network,
        SearchLimits{.max_depth = 8, .max_nodes = 1}
    );

    return expect(result.stopped, "node limit must stop search")
        && expect(result.completed_depth == 0,
                  "an incomplete first iteration must not be published")
        && expect(result.value == VALUE_NONE,
                  "no completed iteration must retain the none sentinel")
        && expect(result.stats.nodes == 1, "hard node limit must be exact")
        && expect(parsed->fen() == original_fen,
                  "aborted search must restore the position");
}

bool test_cooperative_stop_and_deadline(const nnue::Network& network) {
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "timed-search FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    TranspositionTable table(1);
    std::atomic_bool stop = true;
    SearchLimits stopped_limits{.max_depth = 8};
    stopped_limits.stop = &stop;
    const SearchResult pre_stopped = search(
        *parsed,
        table,
        network,
        stopped_limits
    );
    if (!expect(pre_stopped.stopped && pre_stopped.stats.nodes == 0,
                "a pre-set stop flag must cancel before the first node")
        || !expect(parsed->fen() == original_fen,
                   "cooperative stop must restore the root position")) {
        return false;
    }

    stop.store(false, std::memory_order_relaxed);
    table.clear();
    SearchLimits timed_limits{.max_depth = MAX_PLY};
    timed_limits.stop = &stop;
    timed_limits.start_time = std::chrono::steady_clock::now();
    timed_limits.soft_time = std::chrono::milliseconds(5);
    timed_limits.hard_time = std::chrono::milliseconds(20);
    const auto started = std::chrono::steady_clock::now();
    const SearchResult timed = search(*parsed, table, network, timed_limits);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    return expect(timed.stopped, "time budget must stop iterative deepening")
        && expect(elapsed < std::chrono::seconds(1),
                  "hard deadline must prevent an unbounded overrun")
        && expect(parsed->fen() == original_fen,
                  "timed stop must restore the root position");
}

bool test_draw_rules(const nnue::Network& network) {
    auto insufficient = Position::from_fen(
        "8/8/8/8/8/2k5/8/2K5 w - - 0 1"
    );
    auto rule50 = Position::from_fen(
        "7k/8/8/8/8/8/6R1/6K1 w - - 100 1"
    );
    if (!expect(insufficient.has_value() && rule50.has_value(),
                "draw-rule FENs must parse")) {
        return false;
    }

    TranspositionTable table(1);
    const SearchLimits limits{.max_depth = 2};
    const SearchResult insufficient_result = search(*insufficient, table, network, limits);
    table.clear();
    const SearchResult rule50_result = search(*rule50, table, network, limits);

    return expect(insufficient_result.value == VALUE_DRAW,
                  "insufficient material must draw")
        && expect(rule50_result.value == VALUE_DRAW,
                  "rule-50 position must draw")
        && expect(!rule50_result.best_move.is_none(),
                  "a drawable root with legal moves still needs a best move");
}

} // namespace

bool run_search_tests() {
    const std::filesystem::path path = network_path();
    if (!expect(!path.empty(), "mors-p2h32.nnue must be available"))
        return false;

    auto loaded = mors::nnue::Network::load(path);
    if (!expect(loaded.has_value(), "search network must load")) {
        if (!loaded)
            std::cerr << "  " << loaded.error() << '\n';
        return false;
    }

    const bool passed = test_terminal_nodes(*loaded)
                     && test_pvs_and_restoration(*loaded)
                     && test_reverse_futility_pruning(*loaded)
                     && test_tt_reuse(*loaded)
                     && test_node_limit(*loaded)
                     && test_cooperative_stop_and_deadline(*loaded)
                     && test_draw_rules(*loaded);
    if (passed)
        std::cout << "PASS iterative PVS search\n";
    return passed;
}
