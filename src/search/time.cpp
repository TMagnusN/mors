// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "time.hpp"

#include "chess/position.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mors::timeman {
namespace {

[[nodiscard]] int game_ply(const Position& position) noexcept {
    const int fullmove = std::max(1, int(position.fullmove_number()));
    return std::max(
        0,
        (fullmove - 1) * 2 + (position.side_to_move() == BLACK ? 1 : 0)
    );
}

[[nodiscard]] std::chrono::milliseconds milliseconds(std::int64_t value) noexcept {
    return std::chrono::milliseconds(std::max<std::int64_t>(0, value));
}

} // namespace

void TimeManager::new_game() noexcept {
    original_time_adjust_ = -1.0;
}

void TimeManager::set_move_overhead_ms(std::int64_t value) noexcept {
    const std::int64_t clamped = std::clamp(
        value,
        MIN_MOVE_OVERHEAD_MS,
        MAX_MOVE_OVERHEAD_MS
    );
    if (move_overhead_ms_ == clamped)
        return;
    move_overhead_ms_ = clamped;
    original_time_adjust_ = -1.0;
}

std::int64_t TimeManager::move_overhead_ms() const noexcept {
    return move_overhead_ms_;
}

bool TimeManager::build_limits(
    const Position& position,
    const GoParams& params,
    SearchLimits& limits
) noexcept {
    limits.max_depth = params.depth > 0 ? params.depth : MAX_PLY;
    limits.max_nodes = params.nodes > 0
        ? params.nodes
        : std::numeric_limits<std::uint64_t>::max();
    limits.soft_time = {};
    limits.hard_time = {};

    if (params.movetime > 0) {
        limits.soft_time = milliseconds(params.movetime);
        limits.hard_time = milliseconds(params.movetime);
        return true;
    }

    const Color side = position.side_to_move();
    const std::int64_t remaining = side == WHITE ? params.wtime : params.btime;
    const std::int64_t increment = side == WHITE ? params.winc : params.binc;

    if (!params.infinite && remaining > 0) {
        const int ply = game_ply(position);
        const std::int64_t safe_remaining = std::max<std::int64_t>(1, remaining);
        const std::int64_t safe_increment = std::max<std::int64_t>(0, increment);

        std::int64_t centi_mtg = params.movestogo > 0
            ? std::min<std::int64_t>(std::int64_t(params.movestogo) * 100, 5'000)
            : 5'051;
        if (remaining < 1'000) {
            centi_mtg = std::max<std::int64_t>(
                1,
                static_cast<std::int64_t>(double(remaining) * 5.051)
            );
        }

        const std::int64_t overhead = move_overhead_ms_;
        const std::int64_t time_left = std::max<std::int64_t>(
            1,
            safe_remaining
                + (safe_increment * (centi_mtg - 100)
                   - overhead * (200 + centi_mtg)) / 100
        );

        double optimal_scale = 1.0;
        double maximum_scale = 1.0;
        if (params.movestogo == 0) {
            if (original_time_adjust_ < 0.0) {
                original_time_adjust_ =
                    0.3128 * std::log10(double(time_left)) - 0.4354;
            }

            const double log_time_seconds =
                std::log10(double(safe_remaining) / 1'000.0);
            const double optimal_constant = std::min(
                0.0032116 + 0.000321123 * log_time_seconds,
                0.00508017
            );
            const double maximum_constant = std::max(
                3.3977 + 3.03950 * log_time_seconds,
                2.94761
            );

            optimal_scale = std::min(
                0.0121431
                    + std::pow(double(ply) + 2.94693, 0.461073)
                        * optimal_constant,
                0.213035 * double(safe_remaining) / double(time_left)
            ) * original_time_adjust_;
            maximum_scale = std::min(
                6.67704,
                maximum_constant + double(ply) / 11.9847
            );
        } else {
            const double moves_to_go = double(centi_mtg) / 100.0;
            optimal_scale = std::min(
                (0.88 + double(ply) / 116.4) / moves_to_go,
                0.88 * double(safe_remaining) / double(time_left)
            );
            maximum_scale = 1.3 + 0.11 * moves_to_go;
        }

        std::int64_t optimal = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(optimal_scale * double(time_left))
        );
        const std::int64_t maximum = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(std::min(
                0.825179 * double(safe_remaining) - double(overhead),
                maximum_scale * double(optimal)
            )) - 10
        );
        optimal = std::min(optimal, maximum);

        limits.soft_time = milliseconds(optimal);
        limits.hard_time = milliseconds(maximum);
    }

    const bool has_non_time_limit = params.depth > 0 || params.nodes > 0;
    if (!params.infinite
        && !has_non_time_limit
        && limits.soft_time.count() == 0
        && limits.hard_time.count() == 0) {
        // A GUI can report zero after clock rounding in bullet. Always return
        // a move instead of treating this as a malformed command.
        if (remaining <= 0) {
            limits.soft_time = std::chrono::milliseconds(1);
            limits.hard_time = std::chrono::milliseconds(1);
            return true;
        }
        return false;
    }
    return true;
}

} // namespace mors::timeman
