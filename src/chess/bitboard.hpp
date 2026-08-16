// MROS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <string>

#include "types.hpp"

namespace mros {

using Bitboard = std::uint64_t;

inline constexpr Bitboard EMPTY_BB = 0;
inline constexpr Bitboard FULL_BB  = ~Bitboard{0};

inline constexpr Bitboard FILE_A_BB = 0x0101010101010101ULL;
inline constexpr Bitboard FILE_B_BB = FILE_A_BB << 1;
inline constexpr Bitboard FILE_C_BB = FILE_A_BB << 2;
inline constexpr Bitboard FILE_D_BB = FILE_A_BB << 3;
inline constexpr Bitboard FILE_E_BB = FILE_A_BB << 4;
inline constexpr Bitboard FILE_F_BB = FILE_A_BB << 5;
inline constexpr Bitboard FILE_G_BB = FILE_A_BB << 6;
inline constexpr Bitboard FILE_H_BB = FILE_A_BB << 7;

inline constexpr Bitboard RANK_1_BB = 0xFFULL;
inline constexpr Bitboard RANK_2_BB = RANK_1_BB << 8;
inline constexpr Bitboard RANK_3_BB = RANK_1_BB << 16;
inline constexpr Bitboard RANK_4_BB = RANK_1_BB << 24;
inline constexpr Bitboard RANK_5_BB = RANK_1_BB << 32;
inline constexpr Bitboard RANK_6_BB = RANK_1_BB << 40;
inline constexpr Bitboard RANK_7_BB = RANK_1_BB << 48;
inline constexpr Bitboard RANK_8_BB = RANK_1_BB << 56;

[[nodiscard]] constexpr Bitboard square_bb(Square sq) noexcept {
    assert(is_ok(sq));
    return Bitboard{1} << sq;
}

[[nodiscard]] constexpr Bitboard file_bb(File file) noexcept {
    assert(is_ok(file));
    return FILE_A_BB << file;
}

[[nodiscard]] constexpr Bitboard file_bb(Square sq) noexcept {
    return file_bb(file_of(sq));
}

[[nodiscard]] constexpr Bitboard rank_bb(Rank rank) noexcept {
    assert(is_ok(rank));
    return RANK_1_BB << (8 * rank);
}

[[nodiscard]] constexpr Bitboard rank_bb(Square sq) noexcept {
    return rank_bb(rank_of(sq));
}

[[nodiscard]] constexpr bool contains(Bitboard bb, Square sq) noexcept {
    return (bb & square_bb(sq)) != 0;
}

constexpr void set_square(Bitboard& bb, Square sq) noexcept { bb |= square_bb(sq); }
constexpr void clear_square(Bitboard& bb, Square sq) noexcept { bb &= ~square_bb(sq); }

[[nodiscard]] constexpr int popcount(Bitboard bb) noexcept { return std::popcount(bb); }
[[nodiscard]] constexpr bool has_single_bit(Bitboard bb) noexcept { return std::has_single_bit(bb); }
[[nodiscard]] constexpr bool more_than_one(Bitboard bb) noexcept { return (bb & (bb - 1)) != 0; }

[[nodiscard]] constexpr Square lsb(Bitboard bb) noexcept {
    assert(bb != 0);
    return Square(std::countr_zero(bb));
}

[[nodiscard]] constexpr Square msb(Bitboard bb) noexcept {
    assert(bb != 0);
    return Square(63 - std::countl_zero(bb));
}

constexpr Square pop_lsb(Bitboard& bb) noexcept {
    assert(bb != 0);
    const Square sq = lsb(bb);
    bb &= bb - 1;
    return sq;
}

template<Direction D>
[[nodiscard]] constexpr Bitboard shift(Bitboard bb) noexcept {
    if constexpr (D == NORTH)
        return bb << 8;
    else if constexpr (D == SOUTH)
        return bb >> 8;
    else if constexpr (D == EAST)
        return (bb & ~FILE_H_BB) << 1;
    else if constexpr (D == WEST)
        return (bb & ~FILE_A_BB) >> 1;
    else if constexpr (D == NORTH_EAST)
        return (bb & ~FILE_H_BB) << 9;
    else if constexpr (D == NORTH_WEST)
        return (bb & ~FILE_A_BB) << 7;
    else if constexpr (D == SOUTH_EAST)
        return (bb & ~FILE_H_BB) >> 7;
    else if constexpr (D == SOUTH_WEST)
        return (bb & ~FILE_A_BB) >> 9;
    else
        return EMPTY_BB;
}

template<Color C>
[[nodiscard]] constexpr Bitboard pawn_attacks(Bitboard pawns) noexcept {
    if constexpr (C == WHITE)
        return shift<NORTH_WEST>(pawns) | shift<NORTH_EAST>(pawns);
    else
        return shift<SOUTH_WEST>(pawns) | shift<SOUTH_EAST>(pawns);
}

template<Color C>
[[nodiscard]] constexpr Square frontmost_square(Bitboard bb) noexcept {
    return C == WHITE ? msb(bb) : lsb(bb);
}

template<Color C>
[[nodiscard]] constexpr Square backmost_square(Bitboard bb) noexcept {
    return C == WHITE ? lsb(bb) : msb(bb);
}

[[nodiscard]] std::string pretty(Bitboard bb);

static_assert(sizeof(Bitboard) == 8);

} // namespace mros
