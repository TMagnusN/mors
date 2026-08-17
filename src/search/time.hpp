// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "search.hpp"

#include <cstdint>

namespace mros {
class Position;
}

namespace mros::timeman {

inline constexpr std::int64_t DEFAULT_MOVE_OVERHEAD_MS = 10;
inline constexpr std::int64_t MIN_MOVE_OVERHEAD_MS = 0;
inline constexpr std::int64_t MAX_MOVE_OVERHEAD_MS = 5'000;

struct GoParams final {
    Depth depth = 0;
    std::uint64_t nodes = 0;
    std::int64_t movetime = 0;
    std::int64_t wtime = 0;
    std::int64_t btime = 0;
    std::int64_t winc = 0;
    std::int64_t binc = 0;
    int movestogo = 0;
    bool infinite = false;
};

// Converts raw UCI clocks into an optimal depth-boundary budget and a hard
// safety ceiling. The formula is adapted from the author's MagnusChessX time
// manager; search-specific instability adjustments can be layered on later.
class TimeManager final {
public:
    void new_game() noexcept;
    void set_move_overhead_ms(std::int64_t value) noexcept;
    [[nodiscard]] std::int64_t move_overhead_ms() const noexcept;

    [[nodiscard]] bool build_limits(
        const Position& position,
        const GoParams& params,
        SearchLimits& limits
    ) noexcept;

private:
    double original_time_adjust_ = -1.0;
    std::int64_t move_overhead_ms_ = DEFAULT_MOVE_OVERHEAD_MS;
};

} // namespace mros::timeman
