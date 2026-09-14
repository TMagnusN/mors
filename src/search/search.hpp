// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/move.hpp"
#include "platform/numa.hpp"
#include "syzygy/syzygy.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <string>

namespace mors {

class Position;
class TranspositionTable;

namespace nnue {
class Network;
}

struct SearchResult;

struct SearchLimits final {
    Depth max_depth = 1;
    std::uint64_t max_nodes = std::numeric_limits<std::uint64_t>::max();

    // Chronological position keys before the root position. Search-path
    // repetitions are tracked internally from the root onward.
    std::span<const Key> prior_keys{};

    // Cooperative cancellation and time control. A zero duration disables
    // the corresponding deadline. start_time is set when the go command is
    // received so thread-launch latency is charged to the move budget.
    std::atomic_bool* stop = nullptr;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::milliseconds soft_time{};
    std::chrono::milliseconds hard_time{};

    // Toggle only main-search SEE pruning for controlled comparisons.
    // qsearch SEE and history learning remain enabled in both cases.
    bool use_see_pruning = true;

    syzygy::Options syzygy{};

    // Called synchronously after each fully completed iterative-deepening
    // depth. An interrupted partial iteration is never reported.
    std::function<void(const SearchResult&)> iteration_callback{};
};

struct SearchStats final {
    std::uint64_t tb_hits = 0;
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t pvs_researches = 0;
    std::uint64_t aspiration_searches = 0;
    std::uint64_t aspiration_researches = 0;
    std::uint64_t tt_hits = 0;
    std::uint64_t tt_cutoffs = 0;
    std::uint64_t static_eval_cache_hits = 0;
    std::uint64_t rfp_cutoffs = 0;
    std::uint64_t ffp_prunes = 0;
    std::uint64_t lmp_prunes = 0;
    std::uint64_t lmr_searches = 0;
    std::uint64_t lmr_researches = 0;
    std::uint64_t nmp_searches = 0;
    std::uint64_t nmp_cutoffs = 0;
    std::uint64_t nmp_verifications = 0;
    std::uint64_t iir_reductions = 0;
    std::uint64_t singular_searches = 0;
    std::uint64_t singular_extensions = 0;
    std::uint64_t singular_reductions = 0;
    std::uint64_t singular_multicut_cutoffs = 0;
    std::uint64_t see_prunes = 0;
    std::uint64_t see_quiet_prunes = 0;
    std::uint64_t see_noisy_prunes = 0;
    std::uint64_t qsearch_see_prunes = 0;
    std::uint64_t qsearch_lmp_prunes = 0;
    int seldepth = 0;
};

struct SearchResult final {
    Move best_move{};
    Value value = VALUE_NONE;
    Depth completed_depth = 0;
    bool stopped = false;
    bool root_in_tb = false;
    SearchStats stats{};

    std::array<Move, MAX_PLY> principal_variation{};
    std::size_t pv_length = 0;
};

inline constexpr std::size_t MAX_SEARCH_THREADS = 22'528;

// Owns a persistent set of Lazy SMP search threads. Jobs are asynchronous:
// every worker searches an isolated root copy while sharing the TT and stop
// state. The main worker remains authoritative for iteration reports and the
// final result. Quiet, continuation and noisy history are private to each worker and retained across
// jobs. Reconfiguration and start() require the pool to be idle and must be
// serialized by the caller. wait() and request_stop() may accompany a search;
// they must not race destruction or replacement of the pool configuration.
// The pool owns async cancellation after start(); callers stop a job through
// request_stop().
class SearchThreadPool final {
public:
    using CompletionCallback =
        std::function<void(const SearchResult&, std::string_view error)>;

    SearchThreadPool(
        TranspositionTable& table,
        const nnue::Network& network,
        std::size_t thread_count = 1,
        numa::Policy policy = numa::Policy::Auto
    );
    ~SearchThreadPool();

    SearchThreadPool(const SearchThreadPool&) = delete;
    SearchThreadPool& operator=(const SearchThreadPool&) = delete;
    SearchThreadPool(SearchThreadPool&&) = delete;
    SearchThreadPool& operator=(SearchThreadPool&&) = delete;

    void resize(std::size_t thread_count);
    void set_numa_policy(numa::Policy policy);
    [[nodiscard]] numa::Policy numa_policy() const;
    [[nodiscard]] std::string configuration() const;
    // Prepare and publish replicas while idle. The caller must then move the
    // supplied network into the original Network object before the next start.
    // Failure preserves all current replicas and worker history.
    void refresh_network(const nnue::Network& network);
    [[nodiscard]] std::size_t size() const;

    // Rebuild all worker history while preserving the OS threads and TT.
    // The caller clears the TT separately when starting a new game.
    void clear();

    void start(
        const Position& root_position,
        const SearchLimits& limits,
        CompletionCallback completion_callback = {}
    );
    void request_stop() noexcept;
    void wait();
    [[nodiscard]] bool searching() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs an iterative-deepening PVS search through the current single-worker
// coordinator. The root position is copied into worker-local make/unmake and
// NNUE state, and the TT generation advances exactly once per call. The table
// must have been resized to a non-zero size and the network must be valid.
// This standalone entry point starts with fresh history; use SearchThreadPool
// to retain worker history between root searches.
[[nodiscard]] SearchResult search(
    Position& position,
    TranspositionTable& table,
    const nnue::Network& network,
    const SearchLimits& limits = {}
);

} // namespace mors
