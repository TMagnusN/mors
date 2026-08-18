// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "movegen.hpp"

namespace mors {

[[nodiscard]] std::uint64_t perft(Position& position, int depth) noexcept;

[[nodiscard]] std::vector<std::pair<Move, std::uint64_t>> perft_divide(
    Position& position,
    int depth
);

[[nodiscard]] std::string move_to_uci(Move move);

} // namespace mors
