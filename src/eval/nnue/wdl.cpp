// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "wdl.hpp"

#include "chess/position.hpp"
#include "search/score.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace mors::nnue {
namespace {

constexpr int MATERIAL_MAX = 78;
constexpr int CP_MAX_RAW = 32'768;
constexpr double CP_BASE = 104.0;
constexpr double CP_MATERIAL = 12.0;
constexpr double CP_ENDGAME_DISCOUNT = 6.0;
constexpr double CP_MIN_DENOMINATOR = 96.0;
constexpr double CP_MAX_DENOMINATOR = 128.0;

constexpr std::array<double, 4> WDL_A{
    2.37727934,
    -12.88614270,
    28.77964720,
    82.47173049
};
constexpr std::array<double, 4> WDL_B{
    -0.44227540,
    4.20132633,
    -11.59643736,
    32.47824518
};
constexpr int WDL_PLY_CLAMP = 240;
constexpr double WDL_PLY_DIVISOR = 64.0;
constexpr double WDL_CP_CLAMP = 2'000.0;
constexpr int WDL_LOW_MATERIAL_THRESHOLD = 4;
constexpr double WDL_LOW_MATERIAL_A_BONUS = 130.0;
constexpr double WDL_LOW_MATERIAL_B_BONUS = 22.0;

[[nodiscard]] constexpr double horner(
    const std::array<double, 4>& coefficients,
    double x
) noexcept {
    return ((coefficients[0] * x + coefficients[1]) * x + coefficients[2])
         * x + coefficients[3];
}

[[nodiscard]] int game_ply(const Position& position) noexcept {
    const int fullmove = std::max(1, int(position.fullmove_number()));
    return (fullmove - 1) * 2 + (position.side_to_move() == BLACK ? 1 : 0);
}

[[nodiscard]] int material_units(const Position& position) noexcept {
    const int units =
          std::popcount(position.pieces(PAWN))
        + 3 * std::popcount(position.pieces(KNIGHT))
        + 3 * std::popcount(position.pieces(BISHOP))
        + 5 * std::popcount(position.pieces(ROOK))
        + 9 * std::popcount(position.pieces(QUEEN));
    return std::clamp(units, 0, MATERIAL_MAX);
}

[[nodiscard]] int phase_units(const Position& position) noexcept {
    return std::popcount(position.pieces(KNIGHT))
         + std::popcount(position.pieces(BISHOP))
         + 2 * std::popcount(position.pieces(ROOK))
         + 4 * std::popcount(position.pieces(QUEEN));
}

[[nodiscard]] double cp_denominator(const Position& position) noexcept {
    const double material = static_cast<double>(material_units(position))
                          / static_cast<double>(MATERIAL_MAX);
    double denominator = CP_BASE + CP_MATERIAL * material;
    if (phase_units(position) < 10)
        denominator -= CP_ENDGAME_DISCOUNT;
    return std::clamp(
        denominator,
        CP_MIN_DENOMINATOR,
        CP_MAX_DENOMINATOR
    );
}

[[nodiscard]] WdlTriplet ordinary_wdl(
    int cp,
    const Position& position
) noexcept {
    const int units = material_units(position);
    const double material = static_cast<double>(units)
                          / static_cast<double>(MATERIAL_MAX);
    const double ply = static_cast<double>(
        std::min(WDL_PLY_CLAMP, game_ply(position))
    ) / WDL_PLY_DIVISOR;
    const double low_material = 1.0 - material;

    double a = horner(WDL_A, ply);
    double b = horner(WDL_B, ply);
    if (units <= WDL_LOW_MATERIAL_THRESHOLD) {
        a += WDL_LOW_MATERIAL_A_BONUS * low_material;
        b += WDL_LOW_MATERIAL_B_BONUS * low_material;
    }

    const double x = std::clamp(
        static_cast<double>(cp),
        -WDL_CP_CLAMP,
        WDL_CP_CLAMP
    );
    const double slope = std::max(1.0, b);
    const double win = 1.0 / (1.0 + std::exp((a - x) / slope));
    const double loss = 1.0 / (1.0 + std::exp((a + x) / slope));

    int win_i = std::clamp(
        static_cast<int>(std::round(1'000.0 * win)),
        0,
        1'000
    );
    int loss_i = std::clamp(
        static_cast<int>(std::round(1'000.0 * loss)),
        0,
        1'000
    );
    if (win_i + loss_i > 1'000) {
        const int total = win_i + loss_i;
        win_i = win_i * 1'000 / total;
        loss_i = 1'000 - win_i;
    }
    return {win_i, 1'000 - win_i - loss_i, loss_i};
}

} // namespace

int score_to_cp(Value score, const Position& position) noexcept {
    const std::int64_t raw = score;
    const std::int64_t absolute = raw >= 0 ? raw : -raw;
    const double clipped = static_cast<double>(
        std::min<std::int64_t>(absolute, CP_MAX_RAW)
    );
    const int cp = static_cast<int>(
        std::round(clipped * 100.0 / cp_denominator(position))
    );
    return raw >= 0 ? cp : -cp;
}

WdlTriplet score_to_wdl(Value score, const Position& position) noexcept {
    if (is_win(score))
        return {1'000, 0, 0};
    if (is_loss(score))
        return {0, 0, 1'000};
    return ordinary_wdl(score_to_cp(score, position), position);
}

} // namespace mors::nnue
