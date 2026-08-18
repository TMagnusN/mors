// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "bitboard.hpp"

#include <array>
#include <cassert>

#if defined(MORS_PEXT)
#include "detail/pext.hpp"
#endif
#if defined(MORS_DUAL_HQ)
#include "detail/dual_hq.hpp"
#endif
#if defined(MORS_MAGIC)
#include "detail/generated_magics.hpp"
#endif

#if defined(MORS_HYBRID) && (!defined(MORS_DUAL_HQ) || !defined(MORS_MAGIC))
#error "MORS_HYBRID requires both MORS_DUAL_HQ and MORS_MAGIC"
#endif
#if defined(MORS_PEXT) && (defined(MORS_DUAL_HQ) || defined(MORS_MAGIC))
#error "MORS_PEXT is a standalone slider backend"
#endif

namespace mors {

struct SliderAttacks final {
    Bitboard bishop;
    Bitboard rook;
};

void initialize_attacks();

[[nodiscard]] Bitboard pawn_attacks(Color color, Square square) noexcept;
[[nodiscard]] Bitboard knight_attacks(Square square) noexcept;
[[nodiscard]] Bitboard king_attacks(Square square) noexcept;

namespace detail {

[[nodiscard]] bool attacks_ready() noexcept;

#if defined(MORS_MAGIC)

extern std::array<Bitboard, BISHOP_ATTACK_TABLE_SIZE> bishop_magic_attacks;
extern std::array<Bitboard, ROOK_ATTACK_TABLE_SIZE> rook_magic_attacks;

[[nodiscard]] inline std::size_t magic_index(
    const GeneratedMagic& magic,
    Bitboard occupied
) noexcept {
    return magic.offset + (((occupied & magic.mask) * magic.magic) >> magic.shift);
}

template<PieceType Type>
[[nodiscard]] inline Bitboard magic_attacks(Square square, Bitboard occupied) noexcept {
    static_assert(Type == BISHOP || Type == ROOK);

    if constexpr (Type == BISHOP) {
        const GeneratedMagic& magic = BISHOP_MAGICS[square];
        return bishop_magic_attacks[magic_index(magic, occupied)];
    } else {
        const GeneratedMagic& magic = ROOK_MAGICS[square];
        return rook_magic_attacks[magic_index(magic, occupied)];
    }
}

#endif

} // namespace detail

template<PieceType Type>
[[nodiscard]] inline Bitboard sliding_attacks(Square square, Bitboard occupied) noexcept {
    static_assert(Type == BISHOP || Type == ROOK || Type == QUEEN);
    assert(detail::attacks_ready() && is_ok(square));

#if defined(MORS_PEXT)
    if constexpr (Type == BISHOP)
        return detail::bishop_pext_entries[square].lookup(occupied);
    else if constexpr (Type == ROOK)
        return detail::rook_pext_entries[square].lookup(occupied);
    else
        return detail::bishop_pext_entries[square].lookup(occupied)
             | detail::rook_pext_entries[square].lookup(occupied);
#elif defined(MORS_HYBRID)
    if constexpr (Type == BISHOP)
        return detail::magic_attacks<BISHOP>(square, occupied);
    else if constexpr (Type == ROOK)
        return detail::magic_attacks<ROOK>(square, occupied);
    else {
        const auto [bishop, rook] = detail::dual_hq_entries[square].attacks(occupied);
        return bishop | rook;
    }
#elif defined(MORS_DUAL_HQ)
    const auto [bishop, rook] = detail::dual_hq_entries[square].attacks(occupied);
    if constexpr (Type == BISHOP)
        return bishop;
    else if constexpr (Type == ROOK)
        return rook;
    else
        return bishop | rook;
#elif defined(MORS_MAGIC)
    if constexpr (Type == BISHOP) {
        return detail::magic_attacks<BISHOP>(square, occupied);
    } else if constexpr (Type == ROOK) {
        return detail::magic_attacks<ROOK>(square, occupied);
    } else {
        return sliding_attacks<BISHOP>(square, occupied)
             | sliding_attacks<ROOK>(square, occupied);
    }
#else
#error "Select MORS_PEXT, MORS_DUAL_HQ or MORS_MAGIC"
#endif
}

[[nodiscard]] inline Bitboard bishop_attacks(Square square, Bitboard occupied) noexcept {
    return sliding_attacks<BISHOP>(square, occupied);
}

[[nodiscard]] inline Bitboard rook_attacks(Square square, Bitboard occupied) noexcept {
    return sliding_attacks<ROOK>(square, occupied);
}

[[nodiscard]] inline SliderAttacks slider_attacks(Square square, Bitboard occupied) noexcept {
    assert(detail::attacks_ready() && is_ok(square));
#if defined(MORS_DUAL_HQ)
    const auto [bishop, rook] = detail::dual_hq_entries[square].attacks(occupied);
    return {bishop, rook};
#else
    return {
        sliding_attacks<BISHOP>(square, occupied),
        sliding_attacks<ROOK>(square, occupied)
    };
#endif
}

[[nodiscard]] inline Bitboard queen_attacks(Square square, Bitboard occupied) noexcept {
    return sliding_attacks<QUEEN>(square, occupied);
}

[[nodiscard]] Bitboard attacks(Piece piece, Square square, Bitboard occupied) noexcept;

// Full rank/file/diagonal through two aligned squares; includes both endpoints.
[[nodiscard]] Bitboard line_bb(Square first, Square second) noexcept;

// Squares strictly between two aligned squares; excludes both endpoints.
[[nodiscard]] Bitboard between_bb(Square first, Square second) noexcept;

// Excludes first and includes second. Useful for single-check evasion masks.
[[nodiscard]] inline Bitboard segment_bb(Square first, Square second) noexcept {
    const Bitboard between = between_bb(first, second);
    return between || line_bb(first, second) ? between | square_bb(second) : EMPTY_BB;
}

[[nodiscard]] inline bool aligned(Square first, Square second, Square third) noexcept {
    return (line_bb(first, second) & square_bb(third)) != 0;
}

namespace detail {

[[nodiscard]] Bitboard reference_bishop_attacks(Square square, Bitboard occupied) noexcept;
[[nodiscard]] Bitboard reference_rook_attacks(Square square, Bitboard occupied) noexcept;

} // namespace detail

} // namespace mors
