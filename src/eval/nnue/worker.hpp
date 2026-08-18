// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/position.hpp"
#include "network.hpp"

#include <cstddef>
#include <memory>

namespace mors::nnue {

class Worker final {
public:
    static constexpr std::size_t CAPACITY = 256;

    Worker();
    ~Worker();

    Worker(Worker&&) noexcept;
    Worker& operator=(Worker&&) noexcept;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // push() is called immediately before Position::do_move(); pop() is
    // called immediately after Position::undo_move().
    void reset() noexcept;
    void push(const Position& position, Move move) noexcept;
    void pop() noexcept;

    [[nodiscard]] Value evaluate(const Position& position, const Network& network) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

static_assert(
    Worker::CAPACITY >= static_cast<std::size_t>(MAX_PLY + 16),
    "NNUE worker must retain sixteen states beyond the search ply limit"
);

// Slow, stateless oracle used to verify the lazy and SIMD worker paths.
[[nodiscard]] Value evaluate_reference(
    const Position& position,
    const Network& network
) noexcept;

[[nodiscard]] const char* worker_backend() noexcept;

} // namespace mors::nnue
