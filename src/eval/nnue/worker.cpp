// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "worker.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace mors::nnue {
namespace {

using Accumulator = std::array<std::int16_t, P2H32::WIDTH>;

inline constexpr std::uint16_t NO_FEATURE = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t FEATURE_PLANES = 11;
inline constexpr std::size_t MIRROR_NB = 2;

struct Feature final {
    std::uint16_t coarse = NO_FEATURE;
    std::uint16_t fine = NO_FEATURE;
};

static_assert(sizeof(Feature) == 4);

struct Perspective final {
    std::uint16_t coarse_base = 0;
    std::uint16_t fine_base = 0;
    std::uint8_t coarse_bucket = 0;
    std::uint8_t fine_bucket = 0;
    std::uint8_t square_xor = 0;
};

struct PerspectiveDiff final {
    std::array<Feature, 2> removed{};
    std::array<Feature, 2> added{};
    std::uint8_t removed_count = 0;
    std::uint8_t added_count = 0;
    bool refresh = false;
};

using MoveDiff = std::array<PerspectiveDiff, COLOR_NB>;

struct IndexList final {
    std::array<std::uint16_t, 64> values{};
    std::size_t count = 0;

    void push(std::uint16_t value) noexcept {
        assert(count < values.size());
        values[count++] = value;
    }
};

[[nodiscard]] constexpr int nonking_index(PieceType type) noexcept {
    switch (type) {
    case PAWN:   return 0;
    case KNIGHT: return 1;
    case BISHOP: return 2;
    case ROOK:   return 3;
    case QUEEN:  return 4;
    default:     return -1;
    }
}

[[nodiscard]] constexpr auto perspective_table() noexcept {
    std::array<std::array<Perspective, SQUARE_NB>, COLOR_NB> table{};
    for (int color = WHITE; color <= BLACK; ++color) {
        for (int square = A1; square <= H8; ++square) {
            const int vertical_xor = color == BLACK ? 56 : 0;
            const int relative = square ^ vertical_xor;
            const bool mirror = (relative & 7) >= 4;
            const int canonical = relative ^ (mirror ? 7 : 0);
            const int file = canonical & 7;
            const int rank = canonical >> 3;
            const int coarse_bucket = (rank / 2) * 4 + file;
            const int fine_bucket = rank * 4 + file;

            table[static_cast<std::size_t>(color)][static_cast<std::size_t>(square)] = {
                .coarse_base = static_cast<std::uint16_t>(coarse_bucket * 10 * 64),
                .fine_base = static_cast<std::uint16_t>(fine_bucket * 11 * 64),
                .coarse_bucket = static_cast<std::uint8_t>(coarse_bucket),
                .fine_bucket = static_cast<std::uint8_t>(fine_bucket),
                .square_xor = static_cast<std::uint8_t>(vertical_xor ^ (mirror ? 7 : 0))
            };
        }
    }
    return table;
}

inline constexpr auto PERSPECTIVES = perspective_table();

[[nodiscard]] Perspective perspective_of(
    const Position& position,
    Color perspective
) noexcept {
    return PERSPECTIVES[static_cast<std::size_t>(perspective)]
                       [static_cast<std::size_t>(position.king_square(perspective))];
}

[[nodiscard]] Feature feature_of(
    Color perspective,
    const Perspective& transform,
    Piece piece,
    Square square
) noexcept {
    if (piece == NO_PIECE)
        return {};

    const PieceType type = type_of(piece);
    const int relative_color = color_of(piece) == perspective ? 0 : 1;
    const int canonical = static_cast<int>(square) ^ transform.square_xor;

    if (type == KING) {
        return {
            .coarse = NO_FEATURE,
            .fine = relative_color == 1
                ? static_cast<std::uint16_t>(transform.fine_base + 10 * 64 + canonical)
                : NO_FEATURE
        };
    }

    const int piece_index = nonking_index(type);
    assert(piece_index >= 0);
    const int plane = relative_color * 5 + piece_index;
    return {
        .coarse = static_cast<std::uint16_t>(
            transform.coarse_base + plane * 64 + canonical
        ),
        .fine = static_cast<std::uint16_t>(
            transform.fine_base + plane * 64 + canonical
        )
    };
}

void append_feature(
    MoveDiff& diff,
    const std::array<Perspective, COLOR_NB>& transforms,
    Piece piece,
    Square square,
    bool added
) noexcept {
    if (piece == NO_PIECE)
        return;

    for (int color = WHITE; color <= BLACK; ++color) {
        PerspectiveDiff& side = diff[static_cast<std::size_t>(color)];
        if (side.refresh)
            continue;

        const Feature feature = feature_of(
            static_cast<Color>(color),
            transforms[static_cast<std::size_t>(color)],
            piece,
            square
        );
        if (feature.coarse == NO_FEATURE && feature.fine == NO_FEATURE)
            continue;

        if (added) {
            assert(side.added_count < side.added.size());
            side.added[side.added_count++] = feature;
        } else {
            assert(side.removed_count < side.removed.size());
            side.removed[side.removed_count++] = feature;
        }
    }
}

[[nodiscard]] MoveDiff make_move_diff(const Position& position, Move move) noexcept {
    MoveDiff diff{};
    const std::array<Perspective, COLOR_NB> transforms{
        perspective_of(position, WHITE),
        perspective_of(position, BLACK)
    };

    const Square from = move.from();
    const Square to = move.to();
    const Piece moving = position.piece_on(from);
    assert(moving != NO_PIECE);
    const Color moving_color = color_of(moving);

    if (type_of(moving) == KING)
        diff[static_cast<std::size_t>(moving_color)].refresh = true;

    if (move.type() == EN_PASSANT) {
        const Square captured_square = to - pawn_push(moving_color);
        append_feature(
            diff,
            transforms,
            position.piece_on(captured_square),
            captured_square,
            false
        );
    } else if (move.type() != CASTLING && position.piece_on(to) != NO_PIECE) {
        append_feature(diff, transforms, position.piece_on(to), to, false);
    }

    append_feature(diff, transforms, moving, from, false);
    const Piece placed = move.type() == PROMOTION
        ? make_piece(moving_color, move.promotion_type())
        : moving;
    append_feature(diff, transforms, placed, to, true);

    if (move.type() == CASTLING) {
        const bool king_side = file_of(to) == FILE_G;
        const Square rook_from = relative_square(
            moving_color,
            king_side ? H1 : A1
        );
        const Square rook_to = relative_square(
            moving_color,
            king_side ? F1 : D1
        );
        const Piece rook = make_piece(moving_color, ROOK);
        append_feature(diff, transforms, rook, rook_from, false);
        append_feature(diff, transforms, rook, rook_to, true);
    }
    return diff;
}

[[nodiscard]] constexpr Bitboard mirror_horizontal(Bitboard board) noexcept {
    board = ((board >> 1) & 0x5555'5555'5555'5555ULL)
          | ((board & 0x5555'5555'5555'5555ULL) << 1);
    board = ((board >> 2) & 0x3333'3333'3333'3333ULL)
          | ((board & 0x3333'3333'3333'3333ULL) << 2);
    board = ((board >> 4) & 0x0F0F'0F0F'0F0F'0F0FULL)
          | ((board & 0x0F0F'0F0F'0F0F'0F0FULL) << 4);
    return board;
}

[[nodiscard]] constexpr Bitboard canonicalize(
    Bitboard board,
    std::uint8_t square_xor
) noexcept {
    if ((square_xor & 56U) != 0)
        board = std::byteswap(board);
    if ((square_xor & 7U) != 0)
        board = mirror_horizontal(board);
    return board;
}

[[nodiscard]] std::array<Bitboard, FEATURE_PLANES> feature_planes(
    const Position& position,
    Color perspective,
    const Perspective& transform
) noexcept {
    std::array<Bitboard, FEATURE_PLANES> planes{};
    for (int relative_color = 0; relative_color < 2; ++relative_color) {
        const Color actual = relative_color == 0 ? perspective : ~perspective;
        for (int piece_index = 0; piece_index < 5; ++piece_index) {
            const PieceType type = static_cast<PieceType>(PAWN + piece_index);
            planes[static_cast<std::size_t>(relative_color * 5 + piece_index)] =
                canonicalize(position.pieces(actual, type), transform.square_xor);
        }
    }
    planes[10] = canonicalize(position.pieces(~perspective, KING), transform.square_xor);
    return planes;
}

template<std::size_t Width, std::size_t AccumulatorOffset>
void apply_many_rows(
    Accumulator& cache_accumulator,
    Accumulator& output,
    std::span<const std::int16_t> weights,
    const IndexList& added,
    const IndexList& removed
) noexcept {
#if defined(__AVX2__)
    constexpr std::size_t LANES = 16;
    constexpr std::size_t REGISTERS = 8;
    constexpr std::size_t TILE = LANES * REGISTERS;
    static_assert(Width % TILE == 0);

    for (std::size_t offset = 0; offset < Width; offset += TILE) {
        __m256i values[REGISTERS]{};
        for (std::size_t reg = 0; reg < REGISTERS; ++reg) {
            values[reg] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                cache_accumulator.data() + AccumulatorOffset + offset + reg * LANES
            ));
        }

        for (std::size_t item = 0; item < removed.count; ++item) {
            const std::int16_t* row = weights.data()
                + static_cast<std::size_t>(removed.values[item]) * Width + offset;
            for (std::size_t reg = 0; reg < REGISTERS; ++reg) {
                values[reg] = _mm256_sub_epi16(
                    values[reg],
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + reg * LANES))
                );
            }
        }
        for (std::size_t item = 0; item < added.count; ++item) {
            const std::int16_t* row = weights.data()
                + static_cast<std::size_t>(added.values[item]) * Width + offset;
            for (std::size_t reg = 0; reg < REGISTERS; ++reg) {
                values[reg] = _mm256_add_epi16(
                    values[reg],
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + reg * LANES))
                );
            }
        }

        for (std::size_t reg = 0; reg < REGISTERS; ++reg) {
            auto* cache_destination = reinterpret_cast<__m256i*>(
                cache_accumulator.data() + AccumulatorOffset + offset + reg * LANES
            );
            auto* output_destination = reinterpret_cast<__m256i*>(
                output.data() + AccumulatorOffset + offset + reg * LANES
            );
            _mm256_storeu_si256(cache_destination, values[reg]);
            _mm256_storeu_si256(output_destination, values[reg]);
        }
    }
#else
    constexpr std::size_t TILE = 32;
    static_assert(Width % TILE == 0);
    for (std::size_t offset = 0; offset < Width; offset += TILE) {
        std::array<std::int32_t, TILE> values{};
        for (std::size_t lane = 0; lane < TILE; ++lane)
            values[lane] = cache_accumulator[AccumulatorOffset + offset + lane];

        for (std::size_t item = 0; item < removed.count; ++item) {
            const std::int16_t* row = weights.data()
                + static_cast<std::size_t>(removed.values[item]) * Width + offset;
            for (std::size_t lane = 0; lane < TILE; ++lane)
                values[lane] -= row[lane];
        }
        for (std::size_t item = 0; item < added.count; ++item) {
            const std::int16_t* row = weights.data()
                + static_cast<std::size_t>(added.values[item]) * Width + offset;
            for (std::size_t lane = 0; lane < TILE; ++lane)
                values[lane] += row[lane];
        }

        for (std::size_t lane = 0; lane < TILE; ++lane) {
            const auto value = static_cast<std::int16_t>(values[lane]);
            cache_accumulator[AccumulatorOffset + offset + lane] = value;
            output[AccumulatorOffset + offset + lane] = value;
        }
    }
#endif
}

template<std::size_t Width, std::size_t AccumulatorOffset, int AddCount, int SubCount>
void apply_incremental_segment(
    Accumulator& destination,
    const Accumulator& source,
    std::span<const std::int16_t> weights,
    const std::array<Feature, 2>& added,
    const std::array<Feature, 2>& removed,
    bool fine
) noexcept {
    const auto index_of = [fine](const Feature& feature) noexcept {
        return fine ? feature.fine : feature.coarse;
    };

    std::array<const std::int16_t*, 2> add_rows{};
    std::array<const std::int16_t*, 2> sub_rows{};
    for (int item = 0; item < AddCount; ++item) {
        const std::uint16_t index = index_of(added[static_cast<std::size_t>(item)]);
        if (index != NO_FEATURE)
            add_rows[static_cast<std::size_t>(item)] = weights.data()
                + static_cast<std::size_t>(index) * Width;
    }
    for (int item = 0; item < SubCount; ++item) {
        const std::uint16_t index = index_of(removed[static_cast<std::size_t>(item)]);
        if (index != NO_FEATURE)
            sub_rows[static_cast<std::size_t>(item)] = weights.data()
                + static_cast<std::size_t>(index) * Width;
    }

#if defined(__AVX2__)
    for (std::size_t offset = 0; offset < Width; offset += 16) {
        __m256i value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
            source.data() + AccumulatorOffset + offset
        ));
        for (const std::int16_t* row : add_rows) {
            if (row != nullptr)
                value = _mm256_add_epi16(value, _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(row + offset)
                ));
        }
        for (const std::int16_t* row : sub_rows) {
            if (row != nullptr)
                value = _mm256_sub_epi16(value, _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(row + offset)
                ));
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(
            destination.data() + AccumulatorOffset + offset
        ), value);
    }
#else
    for (std::size_t offset = 0; offset < Width; ++offset) {
        std::int32_t value = source[AccumulatorOffset + offset];
        for (const std::int16_t* row : add_rows)
            if (row != nullptr) value += row[offset];
        for (const std::int16_t* row : sub_rows)
            if (row != nullptr) value -= row[offset];
        destination[AccumulatorOffset + offset] = static_cast<std::int16_t>(value);
    }
#endif
}

template<int AddCount, int SubCount>
void apply_incremental_counts(
    Accumulator& destination,
    const Accumulator& source,
    const Network& network,
    const std::array<Feature, 2>& added,
    const std::array<Feature, 2>& removed
) noexcept {
    apply_incremental_segment<P2H32::COARSE_WIDTH, 0, AddCount, SubCount>(
        destination,
        source,
        network.coarse_weights(),
        added,
        removed,
        false
    );
    apply_incremental_segment<
        P2H32::FINE_WIDTH,
        P2H32::COARSE_WIDTH,
        AddCount,
        SubCount
    >(
        destination,
        source,
        network.fine_weights(),
        added,
        removed,
        true
    );
}

void apply_incremental(
    Accumulator& destination,
    const Accumulator& source,
    const Network& network,
    const PerspectiveDiff& diff,
    bool forward
) noexcept {
    assert(!diff.refresh);
    const auto& added = forward ? diff.added : diff.removed;
    const auto& removed = forward ? diff.removed : diff.added;
    const std::uint8_t add_count = forward ? diff.added_count : diff.removed_count;
    const std::uint8_t sub_count = forward ? diff.removed_count : diff.added_count;

    switch ((static_cast<unsigned>(add_count) << 2) | sub_count) {
    case 0x0: apply_incremental_counts<0, 0>(destination, source, network, added, removed); break;
    case 0x1: apply_incremental_counts<0, 1>(destination, source, network, added, removed); break;
    case 0x2: apply_incremental_counts<0, 2>(destination, source, network, added, removed); break;
    case 0x4: apply_incremental_counts<1, 0>(destination, source, network, added, removed); break;
    case 0x5: apply_incremental_counts<1, 1>(destination, source, network, added, removed); break;
    case 0x6: apply_incremental_counts<1, 2>(destination, source, network, added, removed); break;
    case 0x8: apply_incremental_counts<2, 0>(destination, source, network, added, removed); break;
    case 0x9: apply_incremental_counts<2, 1>(destination, source, network, added, removed); break;
    case 0xA: apply_incremental_counts<2, 2>(destination, source, network, added, removed); break;
    default: assert(false); break;
    }
}

[[nodiscard]] constexpr std::int32_t screlu(std::int32_t value) noexcept {
    const std::int32_t clipped = std::clamp(value, 0, P2H32::QA);
    return clipped * clipped;
}

[[nodiscard]] std::int64_t dot_pair_scalar(
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
[[nodiscard]] std::int64_t dot_pair_avx2(
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
#endif

[[nodiscard]] Value forward(
    const Position& position,
    const Network& network,
    const Accumulator& white,
    const Accumulator& black,
    bool use_native_backend
) noexcept {
    const Color side = position.side_to_move();
    const Accumulator& stm = side == WHITE ? white : black;
    const Accumulator& nstm = side == WHITE ? black : white;

    const int phase_units =
        popcount(position.pieces(KNIGHT))
        + popcount(position.pieces(BISHOP))
        + 2 * popcount(position.pieces(ROOK))
        + 4 * popcount(position.pieces(QUEEN));
    static constexpr std::array<std::uint8_t, 25> PHASE_BUCKETS{
        3, 3, 3, 3, 3, 3,
        2, 2, 2, 2, 2, 2,
        1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0
    };
    const int phase = PHASE_BUCKETS[static_cast<std::size_t>(
        std::clamp(phase_units, 0, 24)
    )];

    const Square relative_king = relative_square(side, position.king_square(side));
    const int file = static_cast<int>(file_of(relative_king));
    const int canonical_file = file < 4 ? file : 7 - file;
    const int coarse_bucket = (static_cast<int>(rank_of(relative_king)) / 2) * 4
                            + canonical_file;
    const std::size_t bucket = static_cast<std::size_t>(
        phase * static_cast<int>(P2H32::COARSE_BUCKETS) + coarse_bucket
    );

    const std::size_t base = bucket * 2 * P2H32::WIDTH;
    const std::int16_t* stm_weights = network.output_weights().data() + base;
    const std::int16_t* nstm_weights = stm_weights + P2H32::WIDTH;

#if defined(__AVX2__)
    std::int64_t output = use_native_backend
        ? dot_pair_avx2(stm, stm_weights, nstm, nstm_weights)
        : dot_pair_scalar(stm, stm_weights, nstm, nstm_weights);
#else
    (void)use_native_backend;
    std::int64_t output = dot_pair_scalar(stm, stm_weights, nstm, nstm_weights);
#endif
    output /= P2H32::QA;
    output += network.output_biases()[bucket];
    const std::int64_t scale = network.scale();
    const std::int64_t denominator = static_cast<std::int64_t>(P2H32::QA) * P2H32::QB;
    const std::int64_t clamp_product =
        static_cast<std::int64_t>(VALUE_EVAL_MAX) * denominator;
    const std::int64_t safe_magnitude = clamp_product / scale;
    if (output > safe_magnitude)
        return VALUE_EVAL_MAX;
    if (output < -safe_magnitude)
        return -VALUE_EVAL_MAX;

    output *= scale;
    output /= denominator;
    return static_cast<Value>(std::clamp<std::int64_t>(
        output,
        -static_cast<std::int64_t>(VALUE_EVAL_MAX),
        static_cast<std::int64_t>(VALUE_EVAL_MAX)
    ));
}

void rebuild_reference(
    const Position& position,
    Color perspective,
    const Network& network,
    Accumulator& accumulator
) noexcept {
    std::copy(network.coarse_biases().begin(),
              network.coarse_biases().end(),
              accumulator.begin());
    std::copy(network.fine_biases().begin(),
              network.fine_biases().end(),
              accumulator.begin() + static_cast<std::ptrdiff_t>(P2H32::COARSE_WIDTH));

    const Perspective transform = perspective_of(position, perspective);
    Bitboard occupied = position.pieces();
    while (occupied != EMPTY_BB) {
        const Square square = pop_lsb(occupied);
        const Feature feature = feature_of(
            perspective,
            transform,
            position.piece_on(square),
            square
        );

        if (feature.coarse != NO_FEATURE) {
            const std::int16_t* row = network.coarse_weights().data()
                + static_cast<std::size_t>(feature.coarse) * P2H32::COARSE_WIDTH;
            for (std::size_t index = 0; index < P2H32::COARSE_WIDTH; ++index) {
                accumulator[index] = static_cast<std::int16_t>(
                    static_cast<std::int32_t>(accumulator[index]) + row[index]
                );
            }
        }
        if (feature.fine != NO_FEATURE) {
            const std::int16_t* row = network.fine_weights().data()
                + static_cast<std::size_t>(feature.fine) * P2H32::FINE_WIDTH;
            for (std::size_t index = 0; index < P2H32::FINE_WIDTH; ++index) {
                const std::size_t destination = P2H32::COARSE_WIDTH + index;
                accumulator[destination] = static_cast<std::int16_t>(
                    static_cast<std::int32_t>(accumulator[destination]) + row[index]
                );
            }
        }
    }
}

} // namespace

struct Worker::Impl final {
    struct alignas(64) State final {
        alignas(64) std::array<Accumulator, COLOR_NB> accumulators{};
        MoveDiff diff{};
        std::uint8_t computed_mask = 0;
    };

    struct alignas(64) RefreshEntry final {
        alignas(64) Accumulator accumulator{};
        std::array<Bitboard, FEATURE_PLANES> planes{};
        bool initialized = false;
    };

    std::array<State, Worker::CAPACITY> states{};
    // Mirroring changes the feature coordinate system even when the fine bucket
    // is unchanged, so mirrored accumulators require distinct Finny entries.
    std::array<
        std::array<std::array<RefreshEntry, P2H32::FINE_BUCKETS>, MIRROR_NB>,
        COLOR_NB
    > cache{};
    std::size_t state_count = 1;
    const Network* network = nullptr;
    const std::int16_t* network_weights = nullptr;

    void bind(const Network& current) noexcept {
        const std::int16_t* const current_weights = current.coarse_weights().data();
        if (network == &current && network_weights == current_weights)
            return;
        network = &current;
        network_weights = current_weights;
        for (std::size_t index = 0; index < state_count; ++index)
            states[index].computed_mask = 0;
        for (auto& side : cache)
            for (auto& orientation : side)
                for (RefreshEntry& entry : orientation)
                    entry.initialized = false;
    }

    void refresh(
        const Position& position,
        Color perspective,
        Accumulator& output
    ) noexcept {
        assert(network != nullptr);
        const Perspective transform = perspective_of(position, perspective);
        const std::size_t mirror = (transform.square_xor & 7U) != 0 ? 1U : 0U;
        RefreshEntry& entry = cache[static_cast<std::size_t>(perspective)]
                                   [mirror]
                                   [static_cast<std::size_t>(transform.fine_bucket)];

        if (!entry.initialized) {
            std::copy(network->coarse_biases().begin(),
                      network->coarse_biases().end(),
                      entry.accumulator.begin());
            std::copy(network->fine_biases().begin(),
                      network->fine_biases().end(),
                      entry.accumulator.begin()
                          + static_cast<std::ptrdiff_t>(P2H32::COARSE_WIDTH));
            entry.planes.fill(EMPTY_BB);
            entry.initialized = true;
        }

        const auto current = feature_planes(position, perspective, transform);
        IndexList coarse_added;
        IndexList coarse_removed;
        IndexList fine_added;
        IndexList fine_removed;

        for (std::size_t plane = 0; plane < FEATURE_PLANES; ++plane) {
            Bitboard removed = entry.planes[plane] & ~current[plane];
            Bitboard added = current[plane] & ~entry.planes[plane];

            while (removed != EMPTY_BB) {
                const Square square = pop_lsb(removed);
                if (plane < 10) {
                    coarse_removed.push(static_cast<std::uint16_t>(
                        transform.coarse_base + plane * 64 + square
                    ));
                }
                fine_removed.push(static_cast<std::uint16_t>(
                    transform.fine_base + plane * 64 + square
                ));
            }
            while (added != EMPTY_BB) {
                const Square square = pop_lsb(added);
                if (plane < 10) {
                    coarse_added.push(static_cast<std::uint16_t>(
                        transform.coarse_base + plane * 64 + square
                    ));
                }
                fine_added.push(static_cast<std::uint16_t>(
                    transform.fine_base + plane * 64 + square
                ));
            }
            entry.planes[plane] = current[plane];
        }

        apply_many_rows<P2H32::COARSE_WIDTH, 0>(
            entry.accumulator,
            output,
            network->coarse_weights(),
            coarse_added,
            coarse_removed
        );
        apply_many_rows<P2H32::FINE_WIDTH, P2H32::COARSE_WIDTH>(
            entry.accumulator,
            output,
            network->fine_weights(),
            fine_added,
            fine_removed
        );
    }

    [[nodiscard]] const Accumulator& ensure(
        const Position& position,
        Color perspective
    ) noexcept {
        assert(network != nullptr);
        const std::uint8_t mask = static_cast<std::uint8_t>(
            1U << static_cast<unsigned>(perspective)
        );
        const std::size_t current = state_count - 1;
        if ((states[current].computed_mask & mask) != 0)
            return states[current].accumulators[static_cast<std::size_t>(perspective)];

        std::size_t boundary = current;
        bool found_computed = false;
        for (std::size_t index = current;; --index) {
            if ((states[index].computed_mask & mask) != 0) {
                boundary = index;
                found_computed = true;
                break;
            }
            if (index > 0
                && states[index].diff[static_cast<std::size_t>(perspective)].refresh) {
                boundary = index;
                break;
            }
            if (index == 0) {
                boundary = 0;
                break;
            }
        }

        if (found_computed) {
            for (std::size_t index = boundary + 1; index <= current; ++index) {
                apply_incremental(
                    states[index].accumulators[static_cast<std::size_t>(perspective)],
                    states[index - 1].accumulators[static_cast<std::size_t>(perspective)],
                    *network,
                    states[index].diff[static_cast<std::size_t>(perspective)],
                    true
                );
                states[index].computed_mask = static_cast<std::uint8_t>(
                    states[index].computed_mask | mask
                );
            }
        } else {
            refresh(
                position,
                perspective,
                states[current].accumulators[static_cast<std::size_t>(perspective)]
            );
            states[current].computed_mask = static_cast<std::uint8_t>(
                states[current].computed_mask | mask
            );

            for (std::size_t index = current; index > boundary; --index) {
                apply_incremental(
                    states[index - 1].accumulators[static_cast<std::size_t>(perspective)],
                    states[index].accumulators[static_cast<std::size_t>(perspective)],
                    *network,
                    states[index].diff[static_cast<std::size_t>(perspective)],
                    false
                );
                states[index - 1].computed_mask = static_cast<std::uint8_t>(
                    states[index - 1].computed_mask | mask
                );
            }
        }
        return states[current].accumulators[static_cast<std::size_t>(perspective)];
    }
};

Worker::Worker() : impl_(std::make_unique<Impl>()) {
    assert(reinterpret_cast<std::uintptr_t>(impl_.get()) % alignof(Impl) == 0);
}
Worker::~Worker() = default;
Worker::Worker(Worker&&) noexcept = default;
Worker& Worker::operator=(Worker&&) noexcept = default;

void Worker::reset() noexcept {
    impl_->state_count = 1;
    impl_->states[0].diff = {};
    impl_->states[0].computed_mask = 0;
}

void Worker::push(const Position& position, Move move) noexcept {
    assert(impl_->state_count < CAPACITY);
    Impl::State& next = impl_->states[impl_->state_count++];
    next.diff = make_move_diff(position, move);
    next.computed_mask = 0;
}

void Worker::pop() noexcept {
    assert(impl_->state_count > 1);
    --impl_->state_count;
}

Value Worker::evaluate(const Position& position, const Network& network) noexcept {
    assert(network.valid());
    impl_->bind(network);
    const Accumulator& white = impl_->ensure(position, WHITE);
    const Accumulator& black = impl_->ensure(position, BLACK);
    return forward(position, network, white, black, true);
}

std::size_t Worker::size() const noexcept {
    return impl_->state_count;
}

Value evaluate_reference(const Position& position, const Network& network) noexcept {
    assert(network.valid());
    alignas(64) Accumulator white{};
    alignas(64) Accumulator black{};
    rebuild_reference(position, WHITE, network, white);
    rebuild_reference(position, BLACK, network, black);
    return forward(position, network, white, black, false);
}

const char* worker_backend() noexcept {
#if defined(__AVX2__)
    return "avx2";
#else
    return "scalar";
#endif
}

} // namespace mors::nnue
