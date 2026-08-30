// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "chess/movegen.hpp"
#include "eval/nnue/network.hpp"
#include "eval/nnue/worker.hpp"
#include "search/score.hpp"
#include "search/search.hpp"
#include "search/tt.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace mors;

[[nodiscard]] std::filesystem::path network_path() {
    constexpr std::array<std::string_view, 3> CANDIDATES{
        "../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue",
        "networks/mors-p2h32-s14400M-o3183M-c+frc.mnue",
        "../../networks/mors-p2h32-s14400M-o3183M-c+frc.mnue"
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

bool test_null_move_round_trip(const nnue::Network& network) {
    auto parsed = Position::from_fen(
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"
    );
    if (!expect(parsed.has_value(), "null-move FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    const Key original_key = parsed->key();
    nnue::Worker worker;
    const Value original_eval = worker.evaluate(*parsed, network);

    StateInfo state;
    parsed->do_null_move(state);
    const bool null_ok = expect(parsed->side_to_move() == WHITE,
                                "null move must toggle side")
        && expect(parsed->ep_square() == SQ_NONE,
                  "null move must clear en-passant")
        && expect(parsed->key() != original_key,
                  "null move must change the position key")
        && expect(parsed->is_consistent(),
                  "null position must remain internally consistent")
        && expect(worker.evaluate(*parsed, network)
                      == nnue::evaluate_reference(*parsed, network),
                  "NNUE accumulator must remain valid across a null move");

    parsed->undo_null_move(state);
    return null_ok
        && expect(parsed->fen() == original_fen && parsed->key() == original_key,
                  "undo null must restore the exact position")
        && expect(worker.evaluate(*parsed, network) == original_eval,
                  "undo null must preserve the NNUE accumulator");
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
    std::vector<Depth> reported_depths;
    std::uint64_t previous_reported_nodes = 0;
    bool reported_nodes_increase = true;
    bool root_unchanged_during_callback = true;
    SearchLimits limits{.max_depth = 3};
    limits.iteration_callback = [&](const SearchResult& iteration) {
        reported_depths.push_back(iteration.completed_depth);
        if (iteration.stats.nodes <= previous_reported_nodes)
            reported_nodes_increase = false;
        previous_reported_nodes = iteration.stats.nodes;
        root_unchanged_during_callback &=
            position.key() == original_key
            && position.fen() == original_fen;
    };
    const SearchResult result = search(position, table, network, limits);

    if (!expect(result.completed_depth == 3, "depth-three iteration must complete")
        || !expect(result.best_move == Move::normal(E2, E4),
                   "worker refactor must preserve the depth-three best move")
        || !expect(result.pv_length >= 3
                       && result.principal_variation[0] == Move::normal(E2, E4)
                       && result.principal_variation[1] == Move::normal(E7, E5)
                       && result.principal_variation[2] == Move::normal(G1, F3),
                   "worker refactor must preserve the depth-three PV")
        || !expect(result.stats.nodes == 351,
                   "worker refactor must preserve the depth-three node count")
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
        || !expect(reported_depths == std::vector<Depth>({1, 2, 3}),
                   "each completed depth must be reported exactly once")
        || !expect(reported_nodes_increase,
                   "iteration reports must carry cumulative node counts")
        || !expect(root_unchanged_during_callback,
                   "iteration callbacks must observe the untouched root position")
        || !expect(table.generation() == 1,
                   "the coordinator must advance TT generation once per search")
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
        SearchLimits{.max_depth = 6}
    );

    return expect(result.completed_depth == 6, "selective-search exercise must complete")
        && expect(result.stats.rfp_cutoffs > 0,
                  "non-PV shallow nodes must exercise reverse futility pruning")
        && expect(result.stats.ffp_prunes > 0,
                  "non-PV quiets must exercise forward futility pruning")
        && expect(result.stats.lmp_prunes > 0,
                  "non-PV shallow nodes must exercise late-move pruning")
        && expect(result.stats.lmr_searches > 0,
                  "late quiets must exercise reduced-depth searches")
        && expect(result.stats.lmr_researches <= result.stats.lmr_searches,
                  "only reduced searches may require full-depth verification")
        && expect(result.stats.nmp_searches > 0,
                  "non-PV eval fail-highs must exercise null-move probes")
        && expect(result.stats.nmp_cutoffs <= result.stats.nmp_searches,
                  "NMP cutoffs must come from null-move probes")
        && expect(result.stats.iir_reductions > 0,
                  "IIR should reduce deep nodes without a TT move")
        && expect(result.stats.singular_searches > 0,
                  "deep TT moves must exercise singular verification")
        && expect(result.stats.singular_extensions > 0,
                  "singular verification must extend a proven TT move")
        && expect(result.stats.singular_extensions
                      <= result.stats.singular_searches,
                  "singular extensions must come from verification searches")
        && expect(result.stats.singular_reductions
                      <= result.stats.singular_searches,
                  "singular reductions must come from verification searches")
        && expect(result.stats.singular_multicut_cutoffs
                      <= result.stats.singular_searches,
                  "singular multi-cut cutoffs must come from verification searches")
        && expect(result.stats.qsearch_see_prunes > 0,
                  "qsearch must exercise threshold SEE pruning")
        && expect(result.stats.qsearch_lmp_prunes > 0,
                  "qsearch must exercise noisy late-move pruning")
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
        && expect(second.stats.tt_cutoffs > 0, "second search must use TT bounds")
        && expect(table.generation() == 2,
                  "two coordinated searches must advance generation exactly twice");
}

bool test_persistent_thread_pool(const nnue::Network& network) {
    auto parsed = Position::from_fen(START_FEN);
    if (!expect(parsed.has_value(), "thread-pool root FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    const Key original_key = parsed->key();
    TranspositionTable table(4);
    SearchThreadPool pool(table, network);
    if (!expect(pool.size() == 1, "thread pool must start with one worker"))
        return false;

    pool.resize(4);
    if (!expect(pool.size() == 4, "thread pool must grow while idle"))
        return false;

    SearchResult completed;
    std::string completion_error;
    std::size_t completion_count = 0;
    pool.start(
        *parsed,
        SearchLimits{.max_depth = 3},
        [&](const SearchResult& result, std::string_view error) {
            completed = result;
            completion_error = error;
            ++completion_count;
        }
    );
    if (!expect(pool.searching(), "start must publish the active job synchronously"))
        return false;
    pool.wait();

    if (!expect(!pool.searching(), "wait must observe an idle pool")
        || !expect(completion_count == 1 && completion_error.empty(),
                   "main worker must report one successful completion")
        || !expect(completed.completed_depth == 3
                       && completed.stats.nodes == 351
                       && completed.best_move == Move::normal(E2, E4),
                   "persistent main worker must preserve fixed search output")
        || !expect(table.generation() == 1,
                   "one pool job must advance TT generation exactly once")
        || !expect(parsed->key() == original_key
                       && parsed->fen() == original_fen,
                   "pool search must leave the caller root untouched")) {
        return false;
    }

    pool.resize(1);
    if (!expect(pool.size() == 1, "thread pool must shrink while idle"))
        return false;

    SearchLimits long_limits{.max_depth = MAX_PLY};
    bool stopped_completion = false;
    bool stopped_result = false;
    pool.start(
        *parsed,
        long_limits,
        [&](const SearchResult& result, std::string_view error) {
            stopped_completion = error.empty();
            stopped_result = result.stopped;
        }
    );

    bool rejected_busy_resize = false;
    try {
        pool.resize(2);
    } catch (const std::logic_error&) {
        rejected_busy_resize = true;
    }
    pool.request_stop();
    pool.wait();

    if (!expect(rejected_busy_resize,
                "thread pool must reject resize during an active job")
        || !expect(stopped_completion && stopped_result,
                   "pool-owned cancellation must report one normal completion")
        || !expect(table.generation() == 2,
                   "stopped pool jobs must still advance TT generation once")) {
        return false;
    }

    {
        SearchThreadPool destructor_pool(table, network, 2);
        destructor_pool.start(
            *parsed,
            SearchLimits{.max_depth = MAX_PLY}
        );
    }
    return expect(table.generation() == 3,
                  "pool destruction must stop and join an active job");
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

bool test_null_move_material_gate(const nnue::Network& network) {
    auto parsed = Position::from_fen(
        "8/8/8/3k4/3P4/3K4/8/8 w - - 0 1"
    );
    if (!expect(parsed.has_value(), "pawn-ending FEN must parse"))
        return false;

    const std::string original_fen = parsed->fen();
    TranspositionTable table(2);
    const SearchResult result = search(
        *parsed,
        table,
        network,
        SearchLimits{.max_depth = 5}
    );
    return expect(result.completed_depth == 5,
                  "pawn-ending search must complete")
        && expect(result.stats.nmp_searches == 0,
                  "NMP must stay disabled without non-pawn material")
        && expect(parsed->fen() == original_fen,
                  "pawn-ending search must restore the root position");
}

} // namespace

bool run_search_tests() {
    const std::filesystem::path path = network_path();
    if (!expect(!path.empty(), "mors-p2h32-s14400M-o3183M-c+frc.mnue must be available"))
        return false;

    auto loaded = mors::nnue::Network::load(path);
    if (!expect(loaded.has_value(), "search network must load")) {
        if (!loaded)
            std::cerr << "  " << loaded.error() << '\n';
        return false;
    }

    const bool passed = test_null_move_round_trip(*loaded)
                     && test_terminal_nodes(*loaded)
                     && test_pvs_and_restoration(*loaded)
                     && test_reverse_futility_pruning(*loaded)
                     && test_tt_reuse(*loaded)
                     && test_persistent_thread_pool(*loaded)
                     && test_node_limit(*loaded)
                     && test_cooperative_stop_and_deadline(*loaded)
                     && test_draw_rules(*loaded)
                     && test_null_move_material_gate(*loaded);
    if (passed)
        std::cout << "PASS iterative PVS search\n";
    return passed;
}
