// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/move.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mors {

inline constexpr std::size_t DEFAULT_TT_SIZE_MB = 16;

namespace detail {
struct TTCluster;
}

struct TTData final {
    Move move{};
    Value value = VALUE_NONE;
    Value static_eval = VALUE_NONE;
    Depth depth = DEPTH_UNSEARCHED;
    Bound bound = BOUND_NONE;
    bool pv = false;
};

class TranspositionTable;

class TTWriter final {
public:
    TTWriter() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return cluster_ != nullptr; }
    void write(const TTData& data, bool force = false) const noexcept;

private:
    friend class TranspositionTable;

    TTWriter(
        detail::TTCluster* cluster,
        std::uint8_t slot,
        std::uint16_t signature,
        std::uint16_t expected_signature,
        std::uint8_t generation
    ) noexcept;

    detail::TTCluster* cluster_ = nullptr;
    std::uint16_t signature_ = 0;
    std::uint16_t expected_signature_ = 0;
    std::uint8_t slot_ = 0;
    std::uint8_t generation_ = 0;
};

struct TTProbe final {
    bool hit = false;
    TTData data{};
    TTWriter writer{};
};

class TranspositionTable final {
public:
    TranspositionTable() noexcept;
    explicit TranspositionTable(std::size_t megabytes);
    ~TranspositionTable();

    TranspositionTable(const TranspositionTable&) = delete;
    TranspositionTable& operator=(const TranspositionTable&) = delete;
    TranspositionTable(TranspositionTable&&) = delete;
    TranspositionTable& operator=(TranspositionTable&&) = delete;

    void resize(std::size_t megabytes);
    void clear() noexcept;
    void new_search() noexcept;

    [[nodiscard]] TTProbe probe(Key key) noexcept;
    void prefetch(Key key) const noexcept;

    [[nodiscard]] int hashfull() const noexcept;
    [[nodiscard]] std::size_t cluster_count() const noexcept { return cluster_count_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] std::uint8_t generation() const noexcept {
        return generation_.load(std::memory_order_relaxed);
    }

private:
    // Probe/write/hashfull/new_search support concurrent search workers.
    // Writers and probe results are still invalidated by resize/clear, so the
    // engine must stop and join every worker before changing table storage.
    [[nodiscard]] std::size_t index(Key key) const noexcept;

    std::unique_ptr<detail::TTCluster[]> table_;
    std::size_t cluster_count_ = 0;
    std::atomic<std::uint8_t> generation_{0};
};

} // namespace mors
