// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "network.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace mors::nnue::detail {

using Accumulator = std::array<std::int16_t, P2H32::WIDTH>;

[[nodiscard]] constexpr std::int32_t screlu(std::int32_t value) noexcept {
    const std::int32_t clipped = std::clamp(value, 0, P2H32::QA);
    return clipped * clipped;
}

[[nodiscard]] inline std::int64_t dot_pair_scalar(
    const Accumulator& first,
    const std::int16_t* first_weights,
    const Accumulator& second,
    const std::int16_t* second_weights
) noexcept {
    std::int64_t total = 0;
    for (std::size_t index = 0; index < P2H32::WIDTH; ++index) {
        total += static_cast<std::int64_t>(screlu(first[index])) * first_weights[index];
        total += static_cast<std::int64_t>(screlu(second[index])) * second_weights[index];
    }
    return total;
}

#if defined(__AVX2__)
[[nodiscard]] inline std::int64_t dot_pair_avx2_wide(
    const Accumulator& first,
    const std::int16_t* first_weights,
    const Accumulator& second,
    const std::int16_t* second_weights
) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i clip = _mm256_set1_epi16(static_cast<std::int16_t>(P2H32::QA));
    __m256i total64 = _mm256_setzero_si256();

    const std::array<const std::int16_t*, 2> values{first.data(), second.data()};
    const std::array<const std::int16_t*, 2> weights{first_weights, second_weights};

    for (std::size_t offset = 0; offset < P2H32::WIDTH; offset += 16) {
        for (std::size_t side = 0; side < 2; ++side) {
            const __m256i raw = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(values[side] + offset)
            );
            const __m256i clipped = _mm256_min_epi16(
                _mm256_max_epi16(raw, zero),
                clip
            );
            const __m256i raw_weights = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(weights[side] + offset)
            );

            const __m256i value32_low = _mm256_cvtepi16_epi32(
                _mm256_castsi256_si128(clipped)
            );
            const __m256i value32_high = _mm256_cvtepi16_epi32(
                _mm256_extracti128_si256(clipped, 1)
            );
            const __m256i weight32_low = _mm256_cvtepi16_epi32(
                _mm256_castsi256_si128(raw_weights)
            );
            const __m256i weight32_high = _mm256_cvtepi16_epi32(
                _mm256_extracti128_si256(raw_weights, 1)
            );
            const __m256i product_low = _mm256_mullo_epi32(
                _mm256_mullo_epi32(value32_low, value32_low),
                weight32_low
            );
            const __m256i product_high = _mm256_mullo_epi32(
                _mm256_mullo_epi32(value32_high, value32_high),
                weight32_high
            );

            total64 = _mm256_add_epi64(total64, _mm256_cvtepi32_epi64(
                _mm256_castsi256_si128(product_low)
            ));
            total64 = _mm256_add_epi64(total64, _mm256_cvtepi32_epi64(
                _mm256_extracti128_si256(product_low, 1)
            ));
            total64 = _mm256_add_epi64(total64, _mm256_cvtepi32_epi64(
                _mm256_castsi256_si128(product_high)
            ));
            total64 = _mm256_add_epi64(total64, _mm256_cvtepi32_epi64(
                _mm256_extracti128_si256(product_high, 1)
            ));
        }
    }

    alignas(32) std::array<std::int64_t, 4> lanes{};
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes.data()), total64);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}
// x*w fits in int16_t; each int32 lane receives 2*WIDTH/8 terms.
// Widen before the horizontal sum, which can exceed int32_t.
[[nodiscard]] inline std::int64_t dot_pair_avx2_narrow(
    const Accumulator& first,
    const std::int16_t* first_weights,
    const Accumulator& second,
    const std::int16_t* second_weights
) noexcept {
    static_assert(P2H32::WIDTH % 16 == 0);
    static_assert(P2H32::QA * P2H32::FAST_OUTPUT_WEIGHT_MAX <= 32'767);
    static_assert(std::int64_t{P2H32::QA} * P2H32::QA
                  * P2H32::FAST_OUTPUT_WEIGHT_MAX * (2 * P2H32::WIDTH / 8)
                  <= 2'147'483'647);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i clip = _mm256_set1_epi16(static_cast<std::int16_t>(P2H32::QA));
    __m256i first_sum = zero;
    __m256i second_sum = zero;
    for (std::size_t offset = 0; offset < P2H32::WIDTH; offset += 16) {
        const __m256i x = _mm256_min_epi16(clip, _mm256_max_epi16(zero,
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(first.data() + offset))));
        const __m256i y = _mm256_min_epi16(clip, _mm256_max_epi16(zero,
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(second.data() + offset))));
        const __m256i wx = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(first_weights + offset));
        const __m256i wy = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(second_weights + offset));
        first_sum = _mm256_add_epi32(first_sum,
            _mm256_madd_epi16(_mm256_mullo_epi16(x, wx), x));
        second_sum = _mm256_add_epi32(second_sum,
            _mm256_madd_epi16(_mm256_mullo_epi16(y, wy), y));
    }
    const __m256i sum = _mm256_add_epi32(first_sum, second_sum);
    const __m256i sum64 = _mm256_add_epi64(
        _mm256_cvtepi32_epi64(_mm256_castsi256_si128(sum)),
        _mm256_cvtepi32_epi64(_mm256_extracti128_si256(sum, 1)));
    alignas(32) std::array<std::int64_t, 4> lanes{};
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes.data()), sum64);
    return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}
#endif

} // namespace mors::nnue::detail
