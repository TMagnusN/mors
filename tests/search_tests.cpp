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

bool test_persistent_quiet_history(const nnue::Network& network) {
    auto position = Position::from_fen(START_FEN);
    if (!expect(position.has_value(), "history persistence FEN must parse"))
        return false;
    TranspositionTable table(4);
    SearchThreadPool pool(table, network);
    bool completed_ok = true;
    const auto run = [&] {
        // Remove TT reuse as a confounder: only worker history survives.
        table.clear();
        SearchResult completed;
        pool.start(*position, SearchLimits{.max_depth = 5},
                   [&](const SearchResult& result, std::string_view error) {
                       completed = result;
                       completed_ok &= error.empty() && result.completed_depth == 5;
                   });
        pool.wait();
        return completed;
    };
    const auto same_search = [](const SearchResult& left, const SearchResult& right) {
        return left.value == right.value && left.best_move == right.best_move
            && left.stats.nodes == right.stats.nodes && left.pv_length == right.pv_length
            && left.principal_variation == right.principal_variation;
    };
    const SearchResult cold = run();
    const SearchResult warm = run();
    if (!expect(completed_ok && !same_search(cold, warm),
                "consecutive jobs must retain learned history even after clearing TT")) {
        return false;
    }

    TranspositionTable fresh_table(4);
    SearchThreadPool fresh_pool(fresh_table, network);
    SearchResult fresh;
    fresh_pool.start(*position, SearchLimits{.max_depth = 5},
                     [&](const SearchResult& result, std::string_view error) {
                         fresh = result;
                         completed_ok &= error.empty();
                     });
    fresh_pool.wait();
    if (!expect(completed_ok && same_search(cold, fresh),
                "independent worker pools must not share quiet history")) {
        return false;
    }

    const auto generation = table.generation();
    pool.clear();
    if (!expect(pool.size() == 1 && table.generation() == generation
                    && table.probe(position->key()).hit,
                "clearing worker history must preserve pool size and TT contents")) {
        return false;
    }
    const SearchResult reset = run();
    if (!expect(completed_ok && same_search(cold, reset),
                "new-game history reset must reproduce a fresh search")) {
        return false;
    }
    pool.resize(2);
    pool.start(*position, SearchLimits{.max_depth = MAX_PLY, .max_nodes = 1'000});
    pool.wait();
    pool.clear();
    if (!expect(pool.size() == 2, "history reset must preserve all SMP workers"))
        return false;
    pool.resize(1);
    const SearchResult resized = run();
    return expect(completed_ok && same_search(cold, resized),
                  "resizing workers must rebuild their private history");
}

bool test_numa_reconfiguration(const nnue::Network& network) {
    TranspositionTable table(2);
    SearchThreadPool pool(table, network, 2, numa::Policy::None);
    for (const auto invalid : {std::size_t{0}, MAX_SEARCH_THREADS + 1}) {
        bool rejected = false;
        try { pool.resize(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!expect(rejected && pool.size() == 2, "failed resize must preserve the pool")) return false;
    }
    bool rejected_network = false;
    try { pool.refresh_network(nnue::Network{}); }
    catch (const std::invalid_argument&) { rejected_network = true; }
    if (!expect(rejected_network, "invalid network must preserve active replicas")) return false;
    auto position = Position::from_fen(START_FEN);
    auto replacement = network.clone();
    for (const auto policy : {numa::Policy::Auto, numa::Policy::None, numa::Policy::Auto}) {
        pool.set_numa_policy(policy);
        pool.refresh_network(replacement);
        pool.clear();
        if (!expect(pool.numa_policy() == policy && pool.size() == 2,
                    "policy changes preserve worker count")) return false;
        bool completed = false;
        pool.start(*position, SearchLimits{.max_depth = 3},
            [&](const SearchResult& result, std::string_view error) {
                completed = error.empty() && result.completed_depth == 3
                    && move_is_legal(*position, result.best_move);
            });
        pool.wait();
        if (!expect(completed, "reconfigured workers must search with valid network data")) return false;
    }
    return true;
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
    limits.use_see_pruning = false; // Preserve the pre-SEE reference search.
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

bool test_main_search_see(const nnue::Network& network) {
    constexpr std::array positions{
        START_FEN,
        std::string_view{"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10"},
        std::string_view{"4k3/8/8/8/8/8/4r3/3QK3 w - - 0 1"},
        std::string_view{"4k3/P7/8/3pP3/8/8/8/4K3 w - d6 0 1"}
    };
    std::uint64_t quiet_prunes = 0;
    std::uint64_t noisy_prunes = 0;
    for (const auto fen : positions) {
        auto position = Position::from_fen(fen);
        if (!expect(position.has_value(), "main SEE FEN must parse"))
            return false;
        const auto original = position->fen();
        const auto original_key = position->key();
        TranspositionTable table(8);
        for (const bool enabled : {false, true}) {
            table.clear();
            SearchLimits limits{.max_depth = 6};
            limits.use_see_pruning = enabled;
            const auto result = search(*position, table, network, limits);
            if (!expect(result.completed_depth == 6 && !result.stopped,
                        "SEE comparison must complete both searches")
                || !expect(is_valid_value(result.value) && move_is_legal(*position, result.best_move),
                           "SEE comparison must return a valid score and legal move")
                || !expect(position->fen() == original && position->key() == original_key,
                           "SEE comparison must preserve the root")
                || !expect(result.stats.see_prunes == result.stats.see_quiet_prunes
                                                       + result.stats.see_noisy_prunes,
                           "SEE categories must sum to the total")
                || !expect(enabled || result.stats.see_prunes == 0,
                           "disabling main SEE must suppress all main SEE pruning"))
                return false;
            Position pv = *position;
            for (std::size_t index = 0; index < result.pv_length; ++index) {
                const Move move = result.principal_variation[index];
                if (!expect(move_is_legal(pv, move), "SEE search PV must remain legal"))
                    return false;
                StateInfo state;
                pv.do_move(move, state);
            }
            quiet_prunes += result.stats.see_quiet_prunes;
            noisy_prunes += result.stats.see_noisy_prunes;
            std::cout << "SEE " << (enabled ? "on" : "off")
                      << " nodes=" << result.stats.nodes
                      << " quiet=" << result.stats.see_quiet_prunes
                      << " noisy=" << result.stats.see_noisy_prunes << '\n';
        }
        table.clear();
        const auto root_only = search(*position, table, network, SearchLimits{.max_depth = 1});
        if (!expect(root_only.stats.see_prunes == 0,
                    "root moves must never undergo main SEE pruning"))
            return false;
    }
    return expect(quiet_prunes > 0 && noisy_prunes > 0,
                  "comparison positions must exercise quiet and noisy SEE pruning");
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

bool test_qsearch_tt(const nnue::Network& network) {
    auto root = Position::from_fen(START_FEN);
    TranspositionTable table(8);
    const SearchLimits limits{.max_depth = 1};
    const SearchResult cold = search(*root, table, network, limits);
    if (!expect(cold.completed_depth == 1 && !cold.best_move.is_none(),
                "qsearch TT cold search must complete"))
        return false;

    Position child = *root;
    StateInfo state;
    child.do_move(cold.best_move, state);
    const TTProbe probe = table.probe(child.key());
    if (!expect(probe.hit && probe.data.depth == DEPTH_QS,
                "depth-one search must store its qsearch child at QS depth")
        || !expect(probe.data.bound == BOUND_EXACT
                       && is_valid_value(probe.data.value)
                       && is_eval_value(probe.data.static_eval),
                   "PV qsearch must store an exact score and raw eval"))
        return false;

    // Main search can leave a quiet TT move that is not in the noisy list.
    // Reuse the entry, but do not search this quiet in a non-check qsearch.
    MoveList legal;
    generate_legal(child, legal);
    TTData data = probe.data;
    data.move = legal[0]; // Starting-position replies are all quiet.
    data.depth = 4;
    probe.writer.write(data, true);
    const SearchResult warm = search(*root, table, network, limits);
    if (!expect(warm.completed_depth == 1 && warm.value == cold.value
                    && warm.best_move == cold.best_move,
                "warm qsearch must preserve the root score and move")
        || !expect(warm.stats.tt_cutoffs > 0,
                   "depth-one TT cutoffs must come from qsearch")
        || !expect(warm.stats.static_eval_cache_hits > 1,
                   "warm search must reuse raw eval beyond the root")
        || !expect(root->fen() == START_FEN,
                   "qsearch TT reuse must restore the root"))
        return false;

    table.clear();
    const SearchResult repeated = search(*root, table, network, limits);
    return expect(repeated.value == cold.value && repeated.best_move == cold.best_move
                      && repeated.stats.nodes == cold.stats.nodes,
                  "clearing TT must reproduce a fresh qsearch");
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

    TranspositionTable reference_table(4);
    const SearchResult reference = search(*parsed, reference_table, network,
        SearchLimits{.max_depth = 3, .use_see_pruning = false});

    SearchResult completed;
    std::string completion_error;
    std::size_t completion_count = 0;
    pool.start(
        *parsed,
        SearchLimits{.max_depth = 3, .use_see_pruning = false},
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
                       && completed.stats.nodes == reference.stats.nodes
                       && completed.best_move == reference.best_move
                       && completed.value == reference.value
                       && completed.principal_variation == reference.principal_variation,
                   "persistent main worker must match a fresh standalone search")
        || !expect(table.generation() == 1,
                   "one pool job must advance TT generation exactly once")
        || !expect(parsed->key() == original_key
                       && parsed->fen() == original_fen,
                   "pool search must leave the caller root untouched")) {
        return false;
    }

    pool.resize(4);
    if (!expect(pool.size() == 4, "thread pool must grow while idle"))
        return false;

    completion_error.clear();
    pool.start(
        *parsed,
        SearchLimits{
            .max_depth = MAX_PLY,
            .max_nodes = 1'000
        },
        [&](const SearchResult& result, std::string_view error) {
            completed = result;
            completion_error = error;
            ++completion_count;
        }
    );
    pool.wait();
    if (!expect(completion_count == 2 && completion_error.empty(),
                "Lazy SMP job must report one successful completion")
        || !expect(completed.stopped && completed.stats.nodes == 1'000,
                   "all workers must share one exact global node budget")
        || !expect(table.generation() == 2,
                   "multi-worker jobs must advance TT generation only once")
        || !expect(parsed->key() == original_key
                       && parsed->fen() == original_fen,
                   "Lazy SMP workers must use isolated root copies")) {
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
    bool rejected_busy_clear = false;
    try {
        pool.clear();
    } catch (const std::logic_error&) {
        rejected_busy_clear = true;
    }
    pool.request_stop();
    pool.wait();

    if (!expect(rejected_busy_clear,
                "thread pool must reject history reset during an active job")
        || !expect(rejected_busy_resize,
                "thread pool must reject resize during an active job")
        || !expect(stopped_completion && stopped_result,
                   "pool-owned cancellation must report one normal completion")
        || !expect(table.generation() == 3,
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
    return expect(table.generation() == 4,
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

    const bool passed = test_numa_reconfiguration(*loaded)
                     && test_persistent_quiet_history(*loaded)
                     && test_null_move_round_trip(*loaded)
                     && test_terminal_nodes(*loaded)
                     && test_pvs_and_restoration(*loaded)
                     && test_main_search_see(*loaded)
                     && test_reverse_futility_pruning(*loaded)
                     && test_qsearch_tt(*loaded)
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
