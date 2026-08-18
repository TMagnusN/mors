// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../bitboard.hpp"
#include "generated_magics.hpp"

#include <array>
#include <cstdint>
#include <immintrin.h>

namespace mors::detail {

struct PextEntry final {
    Bitboard occupancy_mask = 0;
    Bitboard attack_mask = 0;
    const std::uint16_t* attacks = nullptr;

    [[nodiscard]] inline Bitboard lookup(Bitboard occupied) const noexcept {
        const std::uint64_t index = _pext_u64(occupied, occupancy_mask);
        return _pdep_u64(attacks[index], attack_mask);
    }
};

extern const std::array<PextEntry, SQUARE_NB> bishop_pext_entries;
extern const std::array<PextEntry, SQUARE_NB> rook_pext_entries;

extern std::array<std::uint16_t, BISHOP_ATTACK_TABLE_SIZE> bishop_pext_attacks;
extern std::array<std::uint16_t, ROOK_ATTACK_TABLE_SIZE> rook_pext_attacks;

} // namespace mors::detail
