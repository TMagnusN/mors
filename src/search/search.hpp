// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/move.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>

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

    // Called synchronously after each fully completed iterative-deepening
    // depth. An interrupted partial iteration is never reported.
    std::function<void(const SearchResult&)> iteration_callback{};
};

struct SearchStats final {
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
    std::uint64_t qsearch_see_prunes = 0;
    std::uint64_t qsearch_lmp_prunes = 0;
    int seldepth = 0;
};

struct SearchResult final {
    Move best_move{};
    Value value = VALUE_NONE;
    Depth completed_depth = 0;
    bool stopped = false;
    SearchStats stats{};

    std::array<Move, MAX_PLY> principal_variation{};
    std::size_t pv_length = 0;
};

// Runs an iterative-deepening PVS search through the current single-worker
// coordinator. The root position is copied into worker-local make/unmake and
// NNUE state, and the TT generation advances exactly once per call. The table
// must have been resized to a non-zero size and the network must be valid.
[[nodiscard]] SearchResult search(
    Position& position,
    TranspositionTable& table,
    const nnue::Network& network,
    const SearchLimits& limits = {}
);

} // namespace mors
