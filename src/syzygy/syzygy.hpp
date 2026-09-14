// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "chess/move_list.hpp"
#include <optional>
#include <string_view>

namespace mors {
class Position;
namespace syzygy {

struct Options final {
    int probe_limit = 7;
    int probe_depth = 1;
    bool rule50 = true;
};

enum class Wdl : int { Loss = -2, BlessedLoss = -1, Draw = 0, CursedWin = 1, Win = 2 };

struct RootProbe final {
    MoveList moves;
    Wdl wdl = Wdl::Draw;
    bool used_dtz = false;
};

// Fathom owns process-global mappings. Initialize/free only while all search
// workers are idle; probes may run concurrently after initialization.
[[nodiscard]] bool init(std::string_view path);
void shutdown() noexcept;
[[nodiscard]] int max_pieces() noexcept;
[[nodiscard]] int effective_limit(const Options& options) noexcept;
[[nodiscard]] std::optional<Wdl> probe_wdl(const Position& position, const Options& options) noexcept;
[[nodiscard]] std::optional<RootProbe> probe_root(
    const Position& position, const Options& options, bool has_repeated = false) noexcept;
[[nodiscard]] Value score(Wdl wdl, int ply, bool rule50) noexcept;

} // namespace syzygy
} // namespace mors
