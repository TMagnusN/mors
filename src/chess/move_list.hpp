// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cassert>
#include <cstddef>

#include "move.hpp"

namespace mros {

inline constexpr std::size_t MAX_MOVES = 256;

class MoveList final {
public:
    using iterator = Move*;
    using const_iterator = const Move*;

    constexpr void push(Move move) noexcept {
        assert(size_ < moves_.size());
        moves_[size_++] = move;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr Move operator[](std::size_t index) const noexcept {
        assert(index < size_);
        return moves_[index];
    }

    [[nodiscard]] constexpr iterator begin() noexcept { return moves_.data(); }
    [[nodiscard]] constexpr iterator end() noexcept { return moves_.data() + size_; }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return moves_.data(); }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return moves_.data() + size_; }

private:
    std::array<Move, MAX_MOVES> moves_{};
    std::size_t size_ = 0;
};

} // namespace mros
