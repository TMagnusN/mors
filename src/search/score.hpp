// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/types.hpp"

#include <cassert>
#include <cstdint>

namespace mros {

[[nodiscard]] constexpr bool is_valid_value(Value value) noexcept {
    return value >= -VALUE_MATE && value <= VALUE_MATE;
}

[[nodiscard]] constexpr bool is_eval_value(Value value) noexcept {
    return value >= -VALUE_EVAL_MAX && value <= VALUE_EVAL_MAX;
}

[[nodiscard]] constexpr bool is_win(Value value) noexcept {
    return value >= VALUE_TB_WIN_IN_MAX_PLY && value <= VALUE_MATE;
}

[[nodiscard]] constexpr bool is_loss(Value value) noexcept {
    return value <= -VALUE_TB_WIN_IN_MAX_PLY && value >= -VALUE_MATE;
}

[[nodiscard]] constexpr bool is_decisive(Value value) noexcept {
    return is_win(value) || is_loss(value);
}

[[nodiscard]] constexpr bool is_mate_value(Value value) noexcept {
    return (value >= VALUE_MATE_IN_MAX_PLY && value <= VALUE_MATE)
        || (value <= -VALUE_MATE_IN_MAX_PLY && value >= -VALUE_MATE);
}

[[nodiscard]] constexpr Value clamp_eval(std::int64_t value) noexcept {
    if (value > VALUE_EVAL_MAX)
        return VALUE_EVAL_MAX;
    if (value < -VALUE_EVAL_MAX)
        return -VALUE_EVAL_MAX;
    return static_cast<Value>(value);
}

[[nodiscard]] constexpr Value mate_in(int ply) noexcept {
    assert(ply >= 0 && ply <= MAX_PLY);
    return VALUE_MATE - ply;
}

[[nodiscard]] constexpr Value mated_in(int ply) noexcept {
    assert(ply >= 0 && ply <= MAX_PLY);
    return -VALUE_MATE + ply;
}

[[nodiscard]] constexpr Value value_to_tt(Value value, int ply) noexcept {
    assert(is_valid_value(value));
    assert(ply >= 0 && ply <= MAX_PLY);

    const Value stored = is_win(value)  ? value + ply
                       : is_loss(value) ? value - ply
                                        : value;
    assert(is_valid_value(stored));
    return stored;
}

// TT keys intentionally ignore the halfmove clock. A decisive score whose
// position-relative distance exceeds the remaining reversible plies is not a
// reliable forced result in the current rule-50 state, so demote it to the
// strongest ordinary value while preserving its sign.
[[nodiscard]] constexpr Value value_from_tt(
    Value value,
    int ply,
    std::uint16_t halfmove_clock
) noexcept {
    if (value == VALUE_NONE)
        return VALUE_NONE;

    assert(is_valid_value(value));
    assert(ply >= 0 && ply <= MAX_PLY);

    const int remaining = halfmove_clock < 100
        ? 100 - static_cast<int>(halfmove_clock)
        : 0;

    if (value >= VALUE_MATE_IN_MAX_PLY) {
        if (VALUE_MATE - value > remaining)
            return VALUE_EVAL_MAX;
    } else if (value >= VALUE_TB_WIN_IN_MAX_PLY) {
        if (VALUE_TB - value > remaining)
            return VALUE_EVAL_MAX;
    } else if (value <= -VALUE_MATE_IN_MAX_PLY) {
        if (VALUE_MATE + value > remaining)
            return -VALUE_EVAL_MAX;
    } else if (value <= -VALUE_TB_WIN_IN_MAX_PLY) {
        if (VALUE_TB + value > remaining)
            return -VALUE_EVAL_MAX;
    }

    return is_win(value)  ? value - ply
         : is_loss(value) ? value + ply
                          : value;
}

} // namespace mros
