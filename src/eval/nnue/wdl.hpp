// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "chess/types.hpp"

namespace mors {
class Position;
}

namespace mors::nnue {

struct WdlTriplet final {
    int win = 0;
    int draw = 0;
    int loss = 0;
};

// Converts the P2-H32 search scale to displayed centipawns and calibrated
// UCI win/draw/loss permille.  The model is shared with MagnusChessX's
// current P2-H32 implementation.
[[nodiscard]] int score_to_cp(Value score, const Position& position) noexcept;
[[nodiscard]] WdlTriplet score_to_wdl(
    Value score,
    const Position& position
) noexcept;

} // namespace mors::nnue
