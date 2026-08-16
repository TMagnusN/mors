// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cassert>
#include <compare>
#include <cstdint>
#include <type_traits>

#include "types.hpp"

namespace mros {

enum MoveType : std::uint8_t {
    NORMAL,
    PROMOTION,
    EN_PASSANT,
    CASTLING
};

class Move final {
public:
    constexpr Move() noexcept = default;

    [[nodiscard]] static constexpr Move normal(Square from, Square to) noexcept {
        return encode(from, to, NORMAL, 0);
    }

    [[nodiscard]] static constexpr Move promotion(
        Square from,
        Square to,
        PieceType promoted
    ) noexcept {
        assert(promoted >= KNIGHT && promoted <= QUEEN);
        return encode(from, to, PROMOTION, std::uint16_t(promoted - KNIGHT));
    }

    [[nodiscard]] static constexpr Move en_passant(Square from, Square to) noexcept {
        return encode(from, to, EN_PASSANT, 0);
    }

    [[nodiscard]] static constexpr Move castling(Square from, Square to) noexcept {
        return encode(from, to, CASTLING, 0);
    }

    [[nodiscard]] constexpr Square from() const noexcept {
        return Square((value_ >> 6) & 0x3F);
    }

    [[nodiscard]] constexpr Square to() const noexcept {
        return Square(value_ & 0x3F);
    }

    [[nodiscard]] constexpr MoveType type() const noexcept {
        return MoveType(value_ >> 14);
    }

    [[nodiscard]] constexpr PieceType promotion_type() const noexcept {
        assert(type() == PROMOTION);
        return PieceType(KNIGHT + ((value_ >> 12) & 0x3));
    }

    [[nodiscard]] constexpr std::uint16_t raw() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_none() const noexcept { return value_ == 0; }

    constexpr auto operator<=>(const Move&) const noexcept = default;

private:
    explicit constexpr Move(std::uint16_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr Move encode(
        Square from,
        Square to,
        MoveType type,
        std::uint16_t payload
    ) noexcept {
        assert(is_ok(from) && is_ok(to) && from != to);
        const std::uint32_t encoded = std::uint32_t(to)
                                    | (std::uint32_t(from) << 6)
                                    | (std::uint32_t(payload) << 12)
                                    | (std::uint32_t(type) << 14);
        return Move(std::uint16_t(encoded));
    }

    std::uint16_t value_ = 0;
};

static_assert(sizeof(Move) == 2);
static_assert(std::is_trivially_copyable_v<Move>);

} // namespace mros
