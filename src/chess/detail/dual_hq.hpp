// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../bitboard.hpp"

#include <array>
#include <cstdint>
#include <immintrin.h>
#include <utility>

namespace mors::detail {

struct alignas(32) DualHqEntry final {
    // Four AVX2 lanes: file, diagonal, unused, antidiagonal.
    std::array<Bitboard, 4> masks{};
    Bitboard origin = 0;
    Bitboard reversed_origin = 0;
    const std::uint8_t* rank_attacks_lookup = nullptr;
    unsigned rank_shift = 0;

    [[nodiscard]] inline std::pair<Bitboard, Bitboard>
    attacks(Bitboard occupied) const noexcept {
        const __m256i reverse_control = _mm256_setr_epi8(
            7, 6, 5, 4, 3, 2, 1, 0,
            15, 14, 13, 12, 11, 10, 9, 8,
            7, 6, 5, 4, 3, 2, 1, 0,
            15, 14, 13, 12, 11, 10, 9, 8
        );
        const auto reverse_bytes = [reverse_control](__m256i value) noexcept {
            return _mm256_shuffle_epi8(value, reverse_control);
        };

        const __m256i mask = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(masks.data())
        );
        const __m256i occupancy =
            _mm256_set1_epi64x(static_cast<long long>(occupied));
        const __m256i masked = _mm256_and_si256(occupancy, mask);
        const __m256i forward = _mm256_sub_epi64(
            masked,
            _mm256_set1_epi64x(static_cast<long long>(origin))
        );
        const __m256i backward = reverse_bytes(_mm256_sub_epi64(
            reverse_bytes(masked),
            _mm256_set1_epi64x(static_cast<long long>(reversed_origin))
        ));
        const __m256i rays = _mm256_and_si256(
            _mm256_xor_si256(forward, backward),
            mask
        );

        // Combining the 128-bit halves maps the four lanes to [file, bishop].
        const __m128i file_bishop = _mm_or_si128(
            _mm256_castsi256_si128(rays),
            _mm256_extracti128_si256(rays, 1)
        );

        const unsigned rank_occupancy =
            unsigned((occupied >> (rank_shift + 1U)) & 0x3FU);
        const Bitboard rank =
            Bitboard(rank_attacks_lookup[rank_occupancy]) << rank_shift;
        const Bitboard bishop = Bitboard(_mm_extract_epi64(file_bishop, 1));
        const Bitboard rook = Bitboard(_mm_cvtsi128_si64(file_bishop)) + rank;

        return {bishop, rook};
    }
};

static_assert(alignof(DualHqEntry) == 32);
static_assert(sizeof(DualHqEntry) % 32 == 0);

extern const std::array<DualHqEntry, SQUARE_NB> dual_hq_entries;

} // namespace mors::detail
